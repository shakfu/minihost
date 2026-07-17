"""Project file loader / renderer for the v2 graph executor.

A project file is a JSON document describing a `PluginGraph`: which plugin /
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
    - Node IDs are user-readable strings; mapped to PluginGraph NodeIds at
      load time.
    - Input nodes have a `source` path (WAV/FLAC/MP3/Vorbis -- whatever
      mh_audio_read supports). By default an input file must have the
      project's sample_rate (a mismatch is an error). Set optional
      `resample: true` on the input to convert a mismatched file to the
      project rate at load time (via the shared mh_audio_resample).
    - Output nodes have a `sink` path (WAV/FLAC). Optional `bit_depth`
      (default 24).
    - Plugin nodes have a `path`. Optional `state_b64` for persisted
      plugin state. Plugin's I/O channel counts are read from the
      plugin itself; the schema does not duplicate them.
    - Mix nodes have `num_inputs` and `channels`. Optional `gains` array
      of length num_inputs (default all 1.0).
    - MIDI routing uses a second edge class and dedicated node kinds:
        - `midi_input`: an offline MIDI source. Optional `source` path to
          a .mid file (read at the project sample rate); with no source
          the node produces no events (e.g. a desktop live node).
        - `midi_output`: an offline MIDI sink. Optional `sink` path to a
          .mid file; with no sink the node drains and discards.
        - `midi_filter` (`min_note`/`max_note`/`channel_mask`),
          `midi_transpose` (`semitones`), `midi_velocity_curve`
          (`gamma`): per-event MIDI processors.
        - `midi_merge` (`num_inputs`): fan-in for MIDI streams; wire each
          source to a numbered `dst_port`.
      MIDI edges live in the same `edges` list, tagged `"kind": "midi"`
      (audio edges are the default / `"kind": "audio"`). A MIDI edge can
      drive any plugin node that accepts MIDI.
    - `duration_seconds` is optional; when omitted, the render length is
      the maximum length across input source files. If both are missing
      (no input nodes, no duration), loading fails.
    - Optional `layout` object: `{node_id: {"x": float, "y": float}}`.
      Used by the desktop app's node-graph canvas to remember user-
      edited node positions. Missing entries auto-layout; old project
      files without `layout` continue to parse unchanged.

MIDI routing (input/output/filter/transpose/velocity-curve/merge nodes and
`"kind": "midi"` edges) is supported; the schema mirrors what the desktop
app already understands, so projects round-trip between the two. Still
omitted: parameter automation and plugin sidechain buses (tracked in
docs/dev/desktop_app_todo.md).
"""

from __future__ import annotations

import base64
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import minihost
from minihost import audio_io
from minihost.render import midi_file_to_events


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
    # When True, a file whose sample rate differs from the project rate is
    # resampled to the project rate at load time (via the shared
    # mh_audio_resample). When False (default), a mismatch is an error --
    # the project renderer is otherwise strict about input rates.
    resample: bool = False
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
    path: Path | None = None
    state_b64: str | None = None
    # base64-encoded juce::PluginDescription XML. Set for plugins with no
    # usable file path (AudioUnits); loaded via Plugin.from_descriptor.
    descriptor: str | None = None
    plugin: minihost.Plugin | None = field(repr=False, default=None)


@dataclass
class _MixNode:
    id: str
    num_inputs: int
    channels: int
    gains: list[float] = field(default_factory=list)


@dataclass
class _MidiInputNode:
    id: str
    # Offline MIDI source (.mid). Optional: a node authored for the live
    # desktop carries a port_name instead and has no offline source, in
    # which case it produces no events during an offline render.
    source: Path | None = None
    # Absolute (sample_offset, status, data1, data2) events, loaded from
    # `source` at the project sample rate.
    events: list[tuple[int, int, int, int]] = field(
        default_factory=list, repr=False
    )
    node_id: int = -1


@dataclass
class _MidiOutputNode:
    id: str
    # Offline MIDI sink (.mid). Optional: with no sink the node still
    # exists (drains and discards), so a desktop-authored live project
    # loads and renders without error.
    sink: Path | None = None
    # Absolute events captured across the render, written to `sink`.
    captured: list[tuple[int, int, int, int]] = field(
        default_factory=list, repr=False
    )
    node_id: int = -1


@dataclass
class _MidiProcessorNode:
    id: str
    # Dict passed straight to PluginGraph.add_midi_processor
    # (op + op-specific fields).
    params: dict


@dataclass
class _MidiMergeNode:
    id: str
    num_inputs: int


@dataclass
class LoadedProject:
    """Result of `load_project`. Holds the built (compiled) PluginGraph,
    references to the Plugin objects (so the caller can keep them
    alive), and the per-output sink metadata needed for render."""
    graph: minihost.PluginGraph
    sample_rate: int
    block_size: int
    duration_frames: int
    inputs: list[_InputNode]
    outputs: list[_OutputNode]
    plugins: list[_PluginNode]
    # MIDI source/sink nodes participating in the offline render. The
    # renderer stages each input's per-block events and drains each
    # output, writing any node that has a `sink` to a .mid file.
    midi_inputs: list[_MidiInputNode] = field(default_factory=list)
    midi_outputs: list[_MidiOutputNode] = field(default_factory=list)
    # Optional canvas layout. Keyed by node id; values are (x, y)
    # tuples in canvas coordinates. Missing entries auto-layout.
    layout: dict[str, tuple[float, float]] = field(default_factory=dict)


# -- loader ----------------------------------------------------------- #

def load_project(project_path: str | Path) -> LoadedProject:
    """Load a project file and build a compiled `PluginGraph` from it.

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
    midi_inputs: list[_MidiInputNode] = []
    midi_outputs: list[_MidiOutputNode] = []
    midi_procs: list[_MidiProcessorNode] = []
    midi_merges: list[_MidiMergeNode] = []
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
                resample=bool(raw.get("resample", False)),
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
            descriptor = raw.get("descriptor")
            if descriptor:
                # Descriptor-based (AudioUnit): path is optional.
                path_val = raw.get("path")
                n = _PluginNode(
                    id=nid,
                    path=Path(path_val) if path_val else None,
                    state_b64=raw.get("state_b64"),
                    descriptor=descriptor,
                )
            else:
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
        elif kind == "midi_input":
            src = raw.get("source")
            mi = _MidiInputNode(
                id=nid,
                source=(project_dir / src).resolve() if src else None,
            )
            midi_inputs.append(mi)
            node_by_id[nid] = ("midi_input", mi)
        elif kind == "midi_output":
            snk = raw.get("sink")
            mo = _MidiOutputNode(
                id=nid,
                sink=(project_dir / snk).resolve() if snk else None,
            )
            midi_outputs.append(mo)
            node_by_id[nid] = ("midi_output", mo)
        elif kind == "midi_filter":
            mp = _MidiProcessorNode(
                id=nid,
                params={
                    "op": 0,  # MH_MIDI_OP_FILTER
                    "min_note": int(raw.get("min_note", 0)),
                    "max_note": int(raw.get("max_note", 127)),
                    "channel_mask": int(raw.get("channel_mask", 0xFFFF)),
                },
            )
            midi_procs.append(mp)
            node_by_id[nid] = ("midi_processor", mp)
        elif kind == "midi_transpose":
            mp = _MidiProcessorNode(
                id=nid,
                params={
                    "op": 1,  # MH_MIDI_OP_TRANSPOSE
                    "transpose_semitones": int(raw.get("semitones", 0)),
                },
            )
            midi_procs.append(mp)
            node_by_id[nid] = ("midi_processor", mp)
        elif kind == "midi_velocity_curve":
            mp = _MidiProcessorNode(
                id=nid,
                params={
                    "op": 2,  # MH_MIDI_OP_VELOCITY_CURVE
                    "velocity_gamma": float(raw.get("gamma", 1.0)),
                },
            )
            midi_procs.append(mp)
            node_by_id[nid] = ("midi_processor", mp)
        elif kind == "midi_merge":
            num_inputs = _require_field(raw, "num_inputs", int)
            mm = _MidiMergeNode(id=nid, num_inputs=num_inputs)
            midi_merges.append(mm)
            node_by_id[nid] = ("midi_merge", mm)
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
            if not n.resample:
                raise ProjectError(
                    f"input {n.id!r}: file sample rate {file_sr} does not "
                    f"match project sample_rate {sr} (set resample=true on "
                    f"the input to convert)"
                )
            data = audio_io.resample(data, int(file_sr), int(sr))
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

    # Load MIDI sources (offline). A midi_input with no `source` produces
    # no events (e.g. a desktop-authored live node); that is allowed.
    for mi in midi_inputs:
        if mi.source is None:
            continue
        if not mi.source.exists():
            raise ProjectError(f"midi_input source not found: {mi.source}")
        try:
            mi.events = midi_file_to_events(str(mi.source), float(sr))
        except Exception as e:
            raise ProjectError(
                f"midi_input {mi.id!r}: failed to read {mi.source}: {e}"
            ) from e

    # Open plugin instances. Descriptor-based nodes (AudioUnits, which have
    # no file path) open via Plugin.from_descriptor; path-based nodes open by
    # path.
    for n in plugins:
        try:
            if n.descriptor:
                pd_xml = base64.b64decode(n.descriptor).decode("utf-8")
                n.plugin = minihost.Plugin.from_descriptor(
                    pd_xml,
                    sample_rate=int(sr),
                    max_block_size=block,
                )
            else:
                if n.path is None or not n.path.exists():
                    raise ProjectError(f"plugin path not found: {n.path}")
                n.plugin = minihost.Plugin(
                    str(n.path),
                    sample_rate=int(sr),
                    max_block_size=block,
                )
        except ProjectError:
            raise
        except Exception as e:
            raise ProjectError(
                f"plugin {n.id!r} failed to open: {e}"
            ) from e
        if n.state_b64:
            n.plugin.set_state(base64.b64decode(n.state_b64))

    # Build the graph.
    g = minihost.PluginGraph(block, float(sr))
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
    for mi in midi_inputs:
        mi.node_id = g.add_midi_input()
        id_to_nodeid[mi.id] = mi.node_id
    for mp in midi_procs:
        id_to_nodeid[mp.id] = g.add_midi_processor(mp.params)
    for mm in midi_merges:
        id_to_nodeid[mm.id] = g.add_midi_merge(mm.num_inputs)
    for mo in midi_outputs:
        mo.node_id = g.add_midi_output()
        id_to_nodeid[mo.id] = mo.node_id

    # Nodes that take MIDI on numbered ports (only midi_merge does); MIDI
    # edges to anything else use the implicit port 0.
    midi_merge_ids = {mm.id for mm in midi_merges}

    for e in edges_raw:
        src = _require_field(e, "src", str)
        dst = _require_field(e, "dst", str)
        dst_port = int(e.get("dst_port", 0))
        ekind = e.get("kind", "audio")
        if src not in id_to_nodeid:
            raise ProjectError(f"edge references unknown src id {src!r}")
        if dst not in id_to_nodeid:
            raise ProjectError(f"edge references unknown dst id {dst!r}")
        if ekind == "audio":
            g.connect(id_to_nodeid[src], id_to_nodeid[dst], dst_port=dst_port)
        elif ekind == "midi":
            if dst in midi_merge_ids:
                g.connect_midi_port(
                    id_to_nodeid[src], id_to_nodeid[dst], dst_port
                )
            else:
                g.connect_midi(id_to_nodeid[src], id_to_nodeid[dst])
        else:
            raise ProjectError(
                f"edge kind must be \"audio\" or \"midi\", got {ekind!r}"
            )

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
        midi_inputs=midi_inputs,
        midi_outputs=midi_outputs,
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
    midi_nodes: list[dict] | None = None,
    edges: list[dict],
    layout: dict[str, tuple[float, float]] | None = None,
) -> None:
    """Write a project JSON file. Caller supplies node and edge dicts
    in the schema format (see module docstring). Atomic write via a
    .tmp file + rename.

    `midi_nodes` is a list of MIDI node dicts; unlike the other node
    lists each entry must carry its own `kind` (one of `midi_input`,
    `midi_output`, `midi_filter`, `midi_transpose`,
    `midi_velocity_curve`, `midi_merge`), since the kinds are
    heterogeneous. MIDI edges go in `edges` tagged `"kind": "midi"`.

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
    for n in (midi_nodes or []):
        if "kind" not in n:
            raise ProjectError("each midi_nodes entry must include a 'kind'")
        doc["nodes"].append(dict(n))
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

    # Per-input cursors into each MIDI source's (sorted) event list, so
    # staging a block is a forward walk rather than a rescan.
    midi_cursors = [0] * len(p.midi_inputs)

    frame = 0
    while frame < frames_total:
        n_frames = min(block, frames_total - frame)
        # Stage audio inputs.
        for buf, node in zip(in_bufs, p.inputs):
            avail = max(0, min(n_frames, node.audio.shape[1] - frame))
            if avail > 0:
                buf[:, :avail] = node.audio[:, frame:frame + avail]
            if avail < n_frames:
                buf[:, avail:n_frames] = 0.0
        # Stage MIDI inputs: events in [frame, frame + n_frames), rebased
        # to a block-local sample offset.
        block_end = frame + n_frames
        for ci, mi in enumerate(p.midi_inputs):
            events = mi.events
            cur = midi_cursors[ci]
            block_events = []
            while cur < len(events) and events[cur][0] < block_end:
                off, status, d1, d2 = events[cur]
                block_events.append((off - frame, status, d1, d2))
                cur += 1
            midi_cursors[ci] = cur
            p.graph.set_midi_input_events(mi.node_id, block_events)
        # Render.
        p.graph.render_block(in_bufs, out_scratch, n_frames)
        # Capture audio outputs.
        for accum, scratch in zip(out_accum, out_scratch):
            accum[:, frame:frame + n_frames] = scratch[:, :n_frames]
        # Drain MIDI outputs, restoring absolute sample offsets.
        for mo in p.midi_outputs:
            for off, status, d1, d2 in p.graph.get_midi_output_events(mo.node_id):
                mo.captured.append((off + frame, status, d1, d2))
        frame += n_frames
        if progress_callback is not None:
            progress_callback(frame, frames_total)

    # Write audio sinks.
    for node, accum in zip(p.outputs, out_accum):
        node.sink.parent.mkdir(parents=True, exist_ok=True)
        audio_io.write_audio(
            str(node.sink), accum, p.sample_rate, bit_depth=node.bit_depth
        )

    # Write MIDI sinks (nodes without a sink drain and discard).
    for mo in p.midi_outputs:
        if mo.sink is None:
            continue
        mo.sink.parent.mkdir(parents=True, exist_ok=True)
        _write_midi_events(mo.sink, mo.captured, p.sample_rate)


# -- helpers ---------------------------------------------------------- #

# MIDI sinks are written at a fixed tempo/resolution. Offline captures
# carry absolute sample offsets, not the source's tempo map, so we pick a
# canonical grid (120 BPM, 480 ticks/quarter) and place events on it; the
# audible timing in samples is preserved, only the notated tempo is
# synthetic.
_MIDI_SINK_BPM = 120.0
_MIDI_SINK_TPQ = 480


def _write_midi_events(
    path: Path,
    events: list[tuple[int, int, int, int]],
    sample_rate: float,
) -> None:
    """Write absolute (sample_offset, status, data1, data2) events to a
    single-track .mid file. Events are assumed sorted by offset (the
    renderer drains them in time order)."""
    mf = minihost.MidiFile()
    mf.ticks_per_quarter = _MIDI_SINK_TPQ
    # A fresh MidiFile already has track 0; write into it directly.
    track = 0
    mf.add_tempo(track, 0, _MIDI_SINK_BPM)
    ticks_per_sec = (_MIDI_SINK_BPM / 60.0) * _MIDI_SINK_TPQ
    for off, status, d1, d2 in events:
        tick = int(round((off / float(sample_rate)) * ticks_per_sec))
        hi = status & 0xF0
        ch = status & 0x0F
        if hi == 0x90 and d2 > 0:
            mf.add_note_on(track, tick, ch, d1, d2)
        elif hi == 0x90:  # note-on with velocity 0 is a note-off
            mf.add_note_off(track, tick, ch, d1, 0)
        elif hi == 0x80:
            mf.add_note_off(track, tick, ch, d1, d2)
        elif hi == 0xB0:
            mf.add_control_change(track, tick, ch, d1, d2)
        elif hi == 0xC0:
            mf.add_program_change(track, tick, ch, d1)
        elif hi == 0xE0:
            mf.add_pitch_bend(track, tick, ch, d1 | (d2 << 7))
        # Other message classes (channel pressure, sysex, ...) are not
        # represented by the MidiFile writer and are dropped.
    mf.save(str(path))


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
