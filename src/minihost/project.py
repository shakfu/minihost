"""Project file loader / renderer for the v2 graph executor.

A project file is a JSON document describing a `GraphV2`: which plugin /
input / output / mix nodes exist, how they're connected, and where the
input audio comes from / output audio goes. Schema v1:

    {
      "minihost_project_version": 1,
      "sample_rate":  48000,
      "block_size":   512,
      "duration_seconds": 5.0,
      "nodes": [
        {"id": "in",  "kind": "input",  "channels": 2,
         "source": "in.wav"},
        {"id": "fx",  "kind": "plugin", "path": "Plug.vst3"},
        {"id": "out", "kind": "output", "channels": 2,
         "sink": "out.wav", "bit_depth": 24}
      ],
      "edges": [
        {"src": "in", "dst": "fx", "dst_port": 0},
        {"src": "fx", "dst": "out", "dst_port": 0}
      ]
    }

Schema notes:
    - Node IDs are user-readable strings; mapped to GraphV2 NodeIds at
      load time.
    - Input nodes have a `source` path (WAV/FLAC/MP3/Vorbis -- whatever
      mh_audio_read supports). All input files must have the project's
      sample_rate.
    - Output nodes have a `sink` path (WAV/FLAC). Optional `bit_depth`
      (default 24).
    - Plugin nodes have a `path`. Optional `state_b64` for persisted
      plugin state. Plugin's I/O channel counts are read from the
      plugin itself; the schema does not duplicate them.
    - Mix nodes have `num_inputs` and `channels`. Optional `gains` array
      of length num_inputs (default all 1.0).
    - `duration_seconds` is optional; when omitted, the render length is
      the maximum length across input source files. If both are missing
      (no input nodes, no duration), loading fails.
    - Optional `layout` object: `{node_id: {"x": float, "y": float}}`.
      Used by the desktop app's node-graph canvas to remember user-
      edited node positions. Missing entries auto-layout; old project
      files without `layout` continue to parse unchanged.

v1 deliberately omits: automation, MIDI routing, plugin sidechain
buses. These are listed in docs/dev/desktop_app_todo.md as follow-ups.
"""

from __future__ import annotations

import base64
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import minihost
from minihost import audio_io


def _np():
    """Lazy-import numpy. The project loader is the only render-path
    code path that needs it; importing at module load breaks the
    numpy-optional invariant exercised by tests/test_numpy_optional.py."""
    import numpy as np
    return np

SCHEMA_VERSION = 1


# -- exceptions -------------------------------------------------------- #

class ProjectError(ValueError):
    """Schema or referenced-file errors while loading a project."""


# -- data classes used internally ------------------------------------- #

@dataclass
class _InputNode:
    id: str
    channels: int
    source: Path
    audio: "Any" = field(repr=False, default=None)  # np.ndarray, lazy-typed


@dataclass
class _OutputNode:
    id: str
    channels: int
    sink: Path
    bit_depth: int = 24


@dataclass
class _PluginNode:
    id: str
    path: Path
    state_b64: str | None = None
    plugin: minihost.Plugin | None = field(repr=False, default=None)


@dataclass
class _MixNode:
    id: str
    num_inputs: int
    channels: int
    gains: list[float] = field(default_factory=list)


@dataclass
class LoadedProject:
    """Result of `load_project`. Holds the built (compiled) GraphV2,
    references to the Plugin objects (so the caller can keep them
    alive), and the per-output sink metadata needed for render."""
    graph: minihost.GraphV2
    sample_rate: int
    block_size: int
    duration_frames: int
    inputs: list[_InputNode]
    outputs: list[_OutputNode]
    plugins: list[_PluginNode]
    # Optional canvas layout. Keyed by node id; values are (x, y)
    # tuples in canvas coordinates. Missing entries auto-layout.
    layout: dict[str, tuple[float, float]] = field(default_factory=dict)


# -- loader ----------------------------------------------------------- #

def load_project(project_path: str | Path) -> LoadedProject:
    """Load a project file and build a compiled `GraphV2` from it.

    Returns a `LoadedProject` with the graph, the resolved input audio
    arrays, and the output sink metadata. Callers typically pass the
    result straight to `render_project_loaded` (which is what
    `render_project` does internally).

    Raises `ProjectError` for any schema or referenced-file issue.
    """
    project_path = Path(project_path)
    if not project_path.exists():
        raise FileNotFoundError(project_path)
    try:
        doc = json.loads(project_path.read_text())
    except json.JSONDecodeError as e:
        raise ProjectError(f"invalid JSON: {e}") from e

    _require_field(doc, "minihost_project_version", int)
    if doc["minihost_project_version"] != SCHEMA_VERSION:
        raise ProjectError(
            f"unsupported project version "
            f"{doc['minihost_project_version']}: this build understands "
            f"version {SCHEMA_VERSION}"
        )

    sr     = _require_field(doc, "sample_rate", (int, float))
    block  = _require_field(doc, "block_size",  int)
    nodes_raw = _require_field(doc, "nodes", list)
    edges_raw = _require_field(doc, "edges", list)
    duration_seconds = doc.get("duration_seconds")
    if duration_seconds is not None and not isinstance(
        duration_seconds, (int, float)
    ):
        raise ProjectError("duration_seconds must be a number")

    project_dir = project_path.parent

    # First pass: parse nodes by kind.
    inputs: list[_InputNode]  = []
    outputs: list[_OutputNode] = []
    plugins: list[_PluginNode] = []
    mixes: list[_MixNode]      = []
    node_by_id: dict[str, tuple[str, Any]] = {}    # id -> (kind, dataclass)

    for raw in nodes_raw:
        nid  = _require_field(raw, "id", str)
        kind = _require_field(raw, "kind", str)
        if nid in node_by_id:
            raise ProjectError(f"duplicate node id {nid!r}")
        if kind == "input":
            n = _InputNode(
                id=nid,
                channels=_require_field(raw, "channels", int),
                source=(project_dir / _require_field(raw, "source", str)).resolve(),
            )
            inputs.append(n)
            node_by_id[nid] = ("input", n)
        elif kind == "output":
            n = _OutputNode(
                id=nid,
                channels=_require_field(raw, "channels", int),
                sink=(project_dir / _require_field(raw, "sink", str)).resolve(),
                bit_depth=int(raw.get("bit_depth", 24)),
            )
            outputs.append(n)
            node_by_id[nid] = ("output", n)
        elif kind == "plugin":
            n = _PluginNode(
                id=nid,
                path=Path(_require_field(raw, "path", str)),
                state_b64=raw.get("state_b64"),
            )
            plugins.append(n)
            node_by_id[nid] = ("plugin", n)
        elif kind == "mix":
            num_inputs = _require_field(raw, "num_inputs", int)
            mn = _MixNode(
                id=nid,
                num_inputs=num_inputs,
                channels=_require_field(raw, "channels", int),
                gains=list(raw.get("gains", [1.0] * num_inputs)),
            )
            if len(mn.gains) != num_inputs:
                raise ProjectError(
                    f"mix node {nid!r}: gains length {len(mn.gains)} "
                    f"does not match num_inputs {num_inputs}"
                )
            mixes.append(mn)
            node_by_id[nid] = ("mix", mn)
        else:
            raise ProjectError(f"unknown node kind {kind!r}")

    if not outputs:
        raise ProjectError("project has no output nodes")

    # Load input audio so we can validate sample rates and decide the
    # render duration before building the graph. numpy is loaded lazily
    # to preserve the numpy-optional invariant for users who never
    # call load_project.
    np = _np() if inputs else None
    for n in inputs:
        if not n.source.exists():
            raise ProjectError(f"input source not found: {n.source}")
        data, file_sr = audio_io.read_audio(n.source, as_=np.ndarray)  # type: ignore[union-attr]
        if int(file_sr) != int(sr):
            raise ProjectError(
                f"input {n.id!r}: file sample rate {file_sr} does not "
                f"match project sample_rate {sr}"
            )
        if data.shape[0] != n.channels:
            raise ProjectError(
                f"input {n.id!r}: file has {data.shape[0]} channels, "
                f"project declares {n.channels}"
            )
        n.audio = np.ascontiguousarray(data, dtype=np.float32)  # type: ignore[union-attr]

    # Compute render length.
    if duration_seconds is not None:
        duration_frames = int(round(float(duration_seconds) * float(sr)))
    elif inputs:
        duration_frames = max(int(n.audio.shape[1]) for n in inputs)
    else:
        raise ProjectError(
            "project has no input nodes and no duration_seconds; "
            "cannot determine render length"
        )

    # Open plugin instances.
    for n in plugins:
        if not n.path.exists():
            raise ProjectError(f"plugin path not found: {n.path}")
        try:
            n.plugin = minihost.Plugin(
                str(n.path),
                sample_rate=int(sr),
                max_block_size=block,
            )
        except Exception as e:
            raise ProjectError(
                f"plugin {n.id!r} failed to open: {e}"
            ) from e
        if n.state_b64:
            n.plugin.set_state(base64.b64decode(n.state_b64))

    # Build the graph.
    g = minihost.GraphV2(block, float(sr))
    id_to_nodeid: dict[str, int] = {}
    for n in inputs:
        id_to_nodeid[n.id] = g.add_input(n.channels)
    for n in plugins:
        id_to_nodeid[n.id] = g.add_plugin(n.plugin)  # type: ignore[arg-type]
    for n in mixes:
        nid = g.add_mix(n.num_inputs, n.channels)
        for i, gv in enumerate(n.gains):
            g.set_mix_gain(nid, i, float(gv))
        id_to_nodeid[n.id] = nid
    for n in outputs:
        id_to_nodeid[n.id] = g.add_output(n.channels)

    for e in edges_raw:
        src = _require_field(e, "src", str)
        dst = _require_field(e, "dst", str)
        dst_port = int(e.get("dst_port", 0))
        if src not in id_to_nodeid:
            raise ProjectError(f"edge references unknown src id {src!r}")
        if dst not in id_to_nodeid:
            raise ProjectError(f"edge references unknown dst id {dst!r}")
        g.connect(id_to_nodeid[src], id_to_nodeid[dst], dst_port=dst_port)

    g.compile()

    # Parse optional layout. Unknown ids are dropped silently;
    # missing-or-malformed entries become auto-layout fallbacks.
    layout: dict[str, tuple[float, float]] = {}
    raw_layout = doc.get("layout")
    if isinstance(raw_layout, dict):
        known_ids = set(node_by_id.keys())
        for nid, pos in raw_layout.items():
            if nid not in known_ids:
                continue
            if not isinstance(pos, dict):
                continue
            x = pos.get("x")
            y = pos.get("y")
            if isinstance(x, (int, float)) and isinstance(y, (int, float)):
                layout[nid] = (float(x), float(y))

    return LoadedProject(
        graph=g,
        sample_rate=int(sr),
        block_size=int(block),
        duration_frames=duration_frames,
        inputs=inputs,
        outputs=outputs,
        plugins=plugins,
        layout=layout,
    )


# -- save (for symmetry; useful for round-trip tests + the desktop) ---- #

def save_project(
    project_path: str | Path,
    *,
    sample_rate: int,
    block_size: int,
    duration_seconds: float | None = None,
    input_nodes: list[dict],
    output_nodes: list[dict],
    plugin_nodes: list[dict],
    mix_nodes: list[dict] | None = None,
    edges: list[dict],
    layout: dict[str, tuple[float, float]] | None = None,
) -> None:
    """Write a project JSON file. Caller supplies node and edge dicts
    in the schema format (see module docstring). Atomic write via a
    .tmp file + rename.

    `layout` is the optional canvas position map (see module docstring).
    Pass `None` (default) to omit; an empty dict writes an empty
    `layout: {}` for readers that prefer the field to be present.
    """
    doc: dict[str, Any] = {
        "minihost_project_version": SCHEMA_VERSION,
        "sample_rate": int(sample_rate),
        "block_size": int(block_size),
        "nodes": [],
        "edges": list(edges),
    }
    if duration_seconds is not None:
        doc["duration_seconds"] = float(duration_seconds)

    for n in input_nodes:
        doc["nodes"].append({"kind": "input", **n})
    for n in plugin_nodes:
        doc["nodes"].append({"kind": "plugin", **n})
    for n in (mix_nodes or []):
        doc["nodes"].append({"kind": "mix", **n})
    for n in output_nodes:
        doc["nodes"].append({"kind": "output", **n})

    if layout is not None:
        doc["layout"] = {
            nid: {"x": float(x), "y": float(y)}
            for nid, (x, y) in layout.items()
        }

    project_path = Path(project_path)
    tmp = project_path.with_suffix(project_path.suffix + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2) + "\n")
    tmp.replace(project_path)


# -- renderer -------------------------------------------------------- #

def render_project(
    project_path: str | Path,
    *,
    progress_callback=None,
) -> LoadedProject:
    """Load, render, and write all output sinks. Returns the
    `LoadedProject` for inspection / further use."""
    p = load_project(project_path)
    _render_loaded(p, progress_callback=progress_callback)
    return p


def _render_loaded(p: LoadedProject, *, progress_callback=None) -> None:
    np = _np()
    block = p.block_size
    frames_total = p.duration_frames

    # Pre-allocate scratch buffers (one per input/output node).
    in_bufs:  list = []
    for n in p.inputs:
        in_bufs.append(np.zeros((n.channels, block), dtype=np.float32))
    out_scratch: list = [
        np.zeros((n.channels, block), dtype=np.float32) for n in p.outputs
    ]

    # Accumulate the whole output in memory for v1. Streaming write is
    # an optimisation for later (mh_audio_write doesn't currently
    # expose a streaming API at the Python layer).
    out_accum: list = [
        np.zeros((n.channels, frames_total), dtype=np.float32)
        for n in p.outputs
    ]

    frame = 0
    while frame < frames_total:
        n_frames = min(block, frames_total - frame)
        # Stage inputs.
        for buf, node in zip(in_bufs, p.inputs):
            avail = max(0, min(n_frames, node.audio.shape[1] - frame))
            if avail > 0:
                buf[:, :avail] = node.audio[:, frame:frame + avail]
            if avail < n_frames:
                buf[:, avail:n_frames] = 0.0
        # Render.
        p.graph.render_block(in_bufs, out_scratch, n_frames)
        # Capture outputs.
        for accum, scratch in zip(out_accum, out_scratch):
            accum[:, frame:frame + n_frames] = scratch[:, :n_frames]
        frame += n_frames
        if progress_callback is not None:
            progress_callback(frame, frames_total)

    # Write sinks.
    for node, accum in zip(p.outputs, out_accum):
        node.sink.parent.mkdir(parents=True, exist_ok=True)
        audio_io.write_audio(
            str(node.sink), accum, p.sample_rate, bit_depth=node.bit_depth
        )


# -- helpers ---------------------------------------------------------- #

def _require_field(d: dict, key: str, expected_type) -> Any:
    if not isinstance(d, dict) or key not in d:
        raise ProjectError(f"missing required field {key!r}")
    val = d[key]
    if expected_type is not None and not isinstance(val, expected_type):
        raise ProjectError(
            f"field {key!r} has wrong type "
            f"(expected {expected_type}, got {type(val).__name__})"
        )
    return val
