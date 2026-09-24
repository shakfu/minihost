"""Tests for the v1 project file loader / renderer (`minihost.project`).

Three families:

1. Schema parsing: required fields, unknown kinds, duplicate ids,
   version mismatch, missing referenced files.

2. Round-trip: save_project -> load_project loads back to an
   equivalent in-memory shape and renders successfully.

3. Render parity: a single-plugin project renders bit-identically to
   `process_audio_to_file` on the same plugin and input audio.
   Skipped when MINIHOST_TEST_PLUGIN is unset.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import pytest

import minihost
from minihost import audio_io

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


# -------------------------------------------------------------------- #
# helpers                                                              #
# -------------------------------------------------------------------- #


def _write_input_wav(
    path: Path, *, channels: int = 2, frames: int = 4096, sr: int = 48000, seed: int = 0
) -> None:
    rng = np.random.default_rng(seed)
    data = (rng.standard_normal((channels, frames)) * 0.1).astype(np.float32)
    audio_io.write_audio(str(path), data, sr, bit_depth=24)


def _identity_project(
    tmp_path: Path, *, sr: int = 48000, block: int = 256, frames: int = 4096
) -> tuple[Path, Path, Path]:
    """A no-plugin project: in.wav -> out.wav. Returns (project_json,
    in_wav, out_wav). The graph is input -> output (identity copy)."""
    in_wav = tmp_path / "in.wav"
    out_wav = tmp_path / "out.wav"
    proj = tmp_path / "project.json"
    _write_input_wav(in_wav, frames=frames, sr=sr)

    doc = {
        "minihost_project_version": 1,
        "sample_rate": sr,
        "block_size": block,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
            {
                "id": "out",
                "kind": "output",
                "channels": 2,
                "sink": str(out_wav),
                "bit_depth": 24,
            },
        ],
        "edges": [
            {"src": "in", "dst": "out"},
        ],
    }
    proj.write_text(json.dumps(doc, indent=2))
    return proj, in_wav, out_wav


# -------------------------------------------------------------------- #
# 1. Schema parsing                                                     #
# -------------------------------------------------------------------- #


def test_missing_file(tmp_path):
    with pytest.raises(FileNotFoundError):
        minihost.load_project(tmp_path / "nope.json")


def test_invalid_json(tmp_path):
    p = tmp_path / "bad.json"
    p.write_text("{not json")
    with pytest.raises(minihost.ProjectError, match="invalid JSON"):
        minihost.load_project(p)


def test_version_mismatch(tmp_path):
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 999,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": [],
                "edges": [],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="unsupported project version"):
        minihost.load_project(p)


def test_missing_required_field(tmp_path):
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                # no sample_rate
                "block_size": 512,
                "nodes": [],
                "edges": [],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="missing required field"):
        minihost.load_project(p)


def test_unknown_node_kind(tmp_path):
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "duration_seconds": 0.1,
                "nodes": [{"id": "foo", "kind": "weird"}],
                "edges": [],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="unknown node kind"):
        minihost.load_project(p)


def test_duplicate_node_id(tmp_path):
    in_wav = tmp_path / "in.wav"
    _write_input_wav(in_wav)
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": [
                    {"id": "x", "kind": "input", "channels": 2, "source": str(in_wav)},
                    {
                        "id": "x",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                    },
                ],
                "edges": [],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="duplicate node id"):
        minihost.load_project(p)


def test_no_output_nodes(tmp_path):
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "duration_seconds": 0.1,
                "nodes": [],
                "edges": [],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="no output nodes"):
        minihost.load_project(p)


def test_no_duration_no_inputs_fails(tmp_path):
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": [
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                    },
                ],
                "edges": [],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="duration_seconds"):
        minihost.load_project(p)


def test_input_source_not_found(tmp_path):
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": [
                    {
                        "id": "in",
                        "kind": "input",
                        "channels": 2,
                        "source": str(tmp_path / "missing.wav"),
                    },
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                    },
                ],
                "edges": [{"src": "in", "dst": "out"}],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="input source not found"):
        minihost.load_project(p)


def test_edge_references_unknown_id(tmp_path):
    in_wav = tmp_path / "in.wav"
    _write_input_wav(in_wav)
    p = tmp_path / "p.json"
    p.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                    },
                ],
                "edges": [{"src": "in", "dst": "ghost"}],
            }
        )
    )
    with pytest.raises(minihost.ProjectError, match="unknown dst id"):
        minihost.load_project(p)


# -------------------------------------------------------------------- #
# 2. Round-trip + identity render                                       #
# -------------------------------------------------------------------- #


def test_identity_render_matches_input(tmp_path):
    """input -> output should reproduce the input bit-for-bit (modulo
    the 24-bit quantization on write/read)."""
    proj, in_wav, out_wav = _identity_project(tmp_path, frames=2048)
    minihost.render_project(proj)
    assert out_wav.exists()

    src, _ = audio_io.read_audio(in_wav, as_=np.ndarray)
    dst, _ = audio_io.read_audio(out_wav, as_=np.ndarray)
    # 24-bit round-trip: ~2^-23 quantization error.
    np.testing.assert_allclose(dst, src, atol=2**-22)


def test_load_project_without_layout(tmp_path):
    """Projects predating the `layout` field must continue to load
    with an empty layout dict (no auto-layout values inserted)."""
    proj, _, _ = _identity_project(tmp_path)
    loaded = minihost.load_project(proj)
    assert loaded.layout == {}


def test_layout_round_trip(tmp_path):
    in_wav = tmp_path / "in.wav"
    _write_input_wav(in_wav, frames=512)
    out_wav = tmp_path / "out.wav"
    proj = tmp_path / "p.json"

    layout = {"in": (12.5, 34.0), "out": (260.0, 34.0)}
    minihost.save_project(
        proj,
        sample_rate=48000,
        block_size=256,
        input_nodes=[{"id": "in", "channels": 2, "source": str(in_wav)}],
        plugin_nodes=[],
        output_nodes=[{"id": "out", "channels": 2, "sink": str(out_wav)}],
        edges=[{"src": "in", "dst": "out"}],
        layout=layout,
    )
    loaded = minihost.load_project(proj)
    assert loaded.layout == layout


def test_layout_drops_unknown_ids(tmp_path):
    """Entries referring to nodes that no longer exist are silently
    dropped (defensive: hand-edited files, schema drift)."""
    in_wav = tmp_path / "in.wav"
    _write_input_wav(in_wav, frames=512)
    out_wav = tmp_path / "out.wav"
    proj = tmp_path / "p.json"
    doc = {
        "minihost_project_version": 1,
        "sample_rate": 48000,
        "block_size": 256,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
            {"id": "out", "kind": "output", "channels": 2, "sink": str(out_wav)},
        ],
        "edges": [{"src": "in", "dst": "out"}],
        "layout": {
            "in": {"x": 10.0, "y": 20.0},
            "ghost": {"x": 99.0, "y": 99.0},
        },
    }
    proj.write_text(json.dumps(doc))
    loaded = minihost.load_project(proj)
    assert loaded.layout == {"in": (10.0, 20.0)}


def test_save_load_round_trip(tmp_path):
    in_wav = tmp_path / "in.wav"
    _write_input_wav(in_wav, frames=2048)
    out_wav = tmp_path / "out.wav"
    proj = tmp_path / "proj.json"

    minihost.save_project(
        proj,
        sample_rate=48000,
        block_size=256,
        input_nodes=[
            {"id": "in", "channels": 2, "source": str(in_wav)},
        ],
        plugin_nodes=[],
        output_nodes=[
            {"id": "out", "channels": 2, "sink": str(out_wav), "bit_depth": 24},
        ],
        edges=[{"src": "in", "dst": "out"}],
    )
    # Load works and yields a renderable graph.
    loaded = minihost.load_project(proj)
    assert loaded.graph.is_compiled
    assert loaded.sample_rate == 48000
    assert loaded.block_size == 256
    assert len(loaded.inputs) == 1
    assert len(loaded.outputs) == 1


# -------------------------------------------------------------------- #
# 3. Plugin render parity                                               #
# -------------------------------------------------------------------- #


@skip_if_no_plugin
def test_single_plugin_project_matches_process_audio_to_file(tmp_path):
    """A project of (input -> plugin -> output) must produce the same
    WAV as `process_audio_to_file(plugin, in.wav, out.wav)` with
    matching block size and no latency compensation."""
    sr = 48000
    block = 256

    p = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=block)
    in_ch = p.num_input_channels
    out_ch = p.num_output_channels
    if in_ch == 0:
        pytest.skip("synth-only plugin")

    in_wav = tmp_path / "in.wav"
    _write_input_wav(in_wav, channels=in_ch, frames=block * 8, sr=sr)
    out_ref = tmp_path / "ref.wav"
    out_v2 = tmp_path / "graph.wav"

    # Reference: existing primitive.
    minihost.process_audio_to_file(
        p,
        str(in_wav),
        str(out_ref),
        block_size=block,
        compensate_latency=False,
    )
    del p  # release before the project loader opens the same path

    proj = tmp_path / "p.json"
    doc = {
        "minihost_project_version": 1,
        "sample_rate": sr,
        "block_size": block,
        "nodes": [
            {"id": "in", "kind": "input", "channels": in_ch, "source": str(in_wav)},
            {"id": "fx", "kind": "plugin", "path": PLUGIN},
            {
                "id": "out",
                "kind": "output",
                "channels": out_ch,
                "sink": str(out_v2),
                "bit_depth": 24,
            },
        ],
        "edges": [
            {"src": "in", "dst": "fx"},
            {"src": "fx", "dst": "out"},
        ],
    }
    proj.write_text(json.dumps(doc))

    minihost.render_project(proj)

    ref, _ = audio_io.read_audio(out_ref, as_=np.ndarray)
    got, _ = audio_io.read_audio(out_v2, as_=np.ndarray)
    # Trim to whichever is shorter -- process_audio_to_file may emit
    # a slightly different total length due to internal tail handling.
    n = min(ref.shape[1], got.shape[1])
    np.testing.assert_allclose(got[:, :n], ref[:, :n], atol=1e-5, rtol=1e-5)


# -------------------------------------------------------------------- #
# Path resolution: every path in a project is relative to the project   #
# file, not to the process working directory.                           #
# -------------------------------------------------------------------- #


def _relative_path_project(tmp_path: Path) -> Path:
    """A project whose every path is written relative to the project file."""
    proj_dir = tmp_path / "project"
    proj_dir.mkdir()
    _write_input_wav(proj_dir / "in.wav", frames=1024)

    doc = {
        "minihost_project_version": 1,
        "sample_rate": 48000,
        "block_size": 256,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": "in.wav"},
            {"id": "fx", "kind": "plugin", "path": "Plug.vst3"},
            {"id": "out", "kind": "output", "channels": 2, "sink": "out.wav"},
        ],
        "edges": [
            {"src": "in", "dst": "fx", "dst_port": 0},
            {"src": "fx", "dst": "out"},
        ],
    }
    proj = proj_dir / "project.json"
    proj.write_text(json.dumps(doc))
    return proj


def test_relative_plugin_path_resolves_against_the_project_dir(tmp_path, monkeypatch):
    """A relative plugin path used to be kept verbatim and opened against the
    process working directory, so the same project loaded a different plugin
    -- or none -- depending on where it was run from."""
    proj = _relative_path_project(tmp_path)

    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(elsewhere)

    with pytest.raises(minihost.ProjectError) as excinfo:
        minihost.load_project(proj)

    message = str(excinfo.value)
    assert "plugin path not found" in message
    # The path it looked for is next to the project file, not next to cwd.
    assert str(proj.parent / "Plug.vst3") in message
    assert str(elsewhere) not in message


def test_relative_plugin_path_is_the_same_from_any_cwd(tmp_path, monkeypatch):
    proj = _relative_path_project(tmp_path)

    expected = f"plugin path not found: {proj.parent / 'Plug.vst3'}"
    for cwd in (proj.parent, tmp_path, Path(os.sep)):
        monkeypatch.chdir(cwd)
        with pytest.raises(minihost.ProjectError) as excinfo:
            minihost.load_project(proj)
        # Not just "the same message everywhere" -- a verbatim relative path
        # is also the same everywhere while meaning a different file.
        assert str(excinfo.value) == expected


def test_absolute_plugin_path_is_left_alone(tmp_path, monkeypatch):
    absolute = tmp_path / "Somewhere" / "Plug.vst3"
    proj_dir = tmp_path / "project"
    proj_dir.mkdir()
    _write_input_wav(proj_dir / "in.wav", frames=1024)
    proj = proj_dir / "project.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 256,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": "in.wav"},
                    {"id": "fx", "kind": "plugin", "path": str(absolute)},
                    {"id": "out", "kind": "output", "channels": 2, "sink": "out.wav"},
                ],
                "edges": [
                    {"src": "in", "dst": "fx", "dst_port": 0},
                    {"src": "fx", "dst": "out"},
                ],
            }
        )
    )

    monkeypatch.chdir(tmp_path)
    with pytest.raises(minihost.ProjectError) as excinfo:
        minihost.load_project(proj)

    assert str(absolute) in str(excinfo.value)


def test_relative_input_and_sink_resolve_against_the_project_dir(tmp_path, monkeypatch):
    """The input and output paths already behaved this way; the plugin path
    now matches them, so pin all three together."""
    proj_dir = tmp_path / "project"
    proj_dir.mkdir()
    _write_input_wav(proj_dir / "in.wav", frames=1024)
    proj = proj_dir / "project.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 256,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": "in.wav"},
                    {"id": "out", "kind": "output", "channels": 2, "sink": "out.wav"},
                ],
                "edges": [{"src": "in", "dst": "out"}],
            }
        )
    )

    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.chdir(elsewhere)

    minihost.render_project(proj)

    assert (proj_dir / "out.wav").exists()
    assert not (elsewhere / "out.wav").exists()


# -------------------------------------------------------------------- #
# Partial-failure cleanup: a failed load closes every plugin it opened. #
# -------------------------------------------------------------------- #


class _FakePlugin:
    """Stands in for minihost.Plugin; opening or restoring state can fail."""

    instances: list["_FakePlugin"] = []

    def __init__(self, path, sample_rate, max_block_size):
        if "bad" in Path(path).name:
            raise RuntimeError("cannot open")
        self.path = path
        self.closed = False
        _FakePlugin.instances.append(self)

    def set_state(self, data):
        if data == b"bad":
            raise RuntimeError("cannot restore")

    def close(self):
        self.closed = True


def _two_plugin_project(tmp_path: Path, second: dict) -> Path:
    for name in ("a.vst3", "bad.vst3"):
        (tmp_path / name).mkdir()
    doc = {
        "minihost_project_version": 1,
        "sample_rate": 48000,
        "block_size": 256,
        "duration_seconds": 0.1,
        "nodes": [
            {"id": "p1", "kind": "plugin", "path": "a.vst3"},
            {"id": "p2", "kind": "plugin", **second},
            {"id": "out", "kind": "output", "channels": 2, "sink": "out.wav"},
        ],
        "edges": [{"src": "p2", "dst": "out"}],
    }
    proj = tmp_path / "project.json"
    proj.write_text(json.dumps(doc))
    return proj


@pytest.mark.parametrize(
    "second, match",
    [
        ({"path": "bad.vst3"}, "failed to open"),
        # base64 of b"bad"
        ({"path": "a.vst3", "state_b64": "YmFk"}, "cannot restore"),
    ],
    ids=["second-open-fails", "state-restore-fails"],
)
def test_failed_load_closes_opened_plugins(tmp_path, monkeypatch, second, match):
    monkeypatch.setattr(_FakePlugin, "instances", [])
    monkeypatch.setattr(minihost, "Plugin", _FakePlugin)
    proj = _two_plugin_project(tmp_path, second)

    with pytest.raises(Exception, match=match):
        minihost.load_project(proj)

    assert _FakePlugin.instances
    assert all(p.closed for p in _FakePlugin.instances)


# -------------------------------------------------------------------- #
# Render geometry: rejected at the schema boundary, not in the graph.   #
# -------------------------------------------------------------------- #


def _patched_identity_project(tmp_path: Path, patch) -> Path:
    proj, _, _ = _identity_project(tmp_path)
    doc = json.loads(proj.read_text())
    patch(doc)
    proj.write_text(json.dumps(doc))
    return proj


def _add_mix(doc, **fields):
    doc["nodes"].append({"id": "mx", "kind": "mix", "channels": 2, **fields})


@pytest.mark.parametrize(
    "patch, match",
    [
        (lambda d: d.update(sample_rate=0), "sample_rate"),
        (lambda d: d.update(sample_rate=-48000), "sample_rate"),
        (lambda d: d.update(block_size=0), "block_size"),
        (lambda d: d.update(block_size=-256), "block_size"),
        (lambda d: d.update(duration_seconds=-1.0), "duration_seconds"),
        (lambda d: d["nodes"][0].update(channels=0), "channels"),
        (lambda d: d["nodes"][1].update(channels=-2), "channels"),
        (lambda d: _add_mix(d, num_inputs=-1), "num_inputs"),
        (lambda d: _add_mix(d, num_inputs=2, gains=[1.0]), "gains length"),
        (lambda d: _add_mix(d, num_inputs=2, gains="ab"), "gains must be"),
    ],
    ids=[
        "zero-sr",
        "negative-sr",
        "zero-block",
        "negative-block",
        "negative-duration",
        "zero-input-channels",
        "negative-output-channels",
        "negative-mix-inputs",
        "short-gains",
        "string-gains",
    ],
)
def test_invalid_render_geometry_is_rejected(tmp_path, patch, match):
    proj = _patched_identity_project(tmp_path, patch)
    with pytest.raises(minihost.ProjectError, match=match):
        minihost.load_project(proj)
