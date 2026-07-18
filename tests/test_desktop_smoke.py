"""Headless smoke coverage for the minihost_desktop binary.

Complements ``test_desktop_render_parity.py`` (which covers the two happy
paths: identity render + layout round-trip) with:

- error-path exit codes for the headless entry points (bad/missing input),
- node-kind serialization beyond input/output (mix nodes + gains),
- a multi-node render-parity case (two inputs -> weighted mix -> output)
  against the Python ``render_project`` pipeline.

None of these need a real plugin, so they run wherever a desktop binary is
built. The realtime engine (``LiveEngine``) and plugin editor windows need an
audio device, a display, and a real plugin, so they are exercised by the
``--probe`` mode locally rather than here; see docs/dev/desktop_app_todo.md.

Skipped when the desktop binary isn't built (the default headless / wheel-only
runs). Set ``MINIHOST_DESKTOP_BIN`` to point at a binary in a non-standard
location.
"""

from __future__ import annotations

import json
import subprocess

import numpy as np

import minihost
from minihost import audio_io

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop


def _run(*args: str, timeout: int = 60) -> subprocess.CompletedProcess:
    assert DESKTOP_BIN is not None
    return subprocess.run(
        [str(DESKTOP_BIN), *args],
        capture_output=True,
        text=True,
        timeout=timeout,
    )


# --------------------------------------------------------------------- #
# Error-path exit codes                                                 #
# --------------------------------------------------------------------- #


@skip_if_no_desktop
def test_render_project_missing_file_exits_1(tmp_path):
    """A render against a nonexistent project reports failure (exit 1),
    not a crash and not success."""
    res = _run(f"--render-project={tmp_path / 'nope.json'}", timeout=30)
    assert res.returncode == 1, (res.returncode, res.stderr)


@skip_if_no_desktop
def test_render_project_malformed_json_exits_1(tmp_path):
    """Malformed JSON is a clean load failure, not a crash."""
    bad = tmp_path / "bad.json"
    bad.write_text("{ this is not valid json ")
    res = _run(f"--render-project={bad}", timeout=30)
    assert res.returncode == 1, (res.returncode, res.stderr)


@skip_if_no_desktop
def test_render_project_missing_input_source_exits_1(tmp_path):
    """A structurally valid project whose input file does not exist must
    fail at load (exit 1) rather than render silence."""
    proj = tmp_path / "p.json"
    proj.write_text(
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
                        "source": str(tmp_path / "does_not_exist.wav"),
                    },
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                        "bit_depth": 24,
                    },
                ],
                "edges": [{"src": "in", "dst": "out"}],
            }
        )
    )
    res = _run(f"--render-project={proj}", timeout=30)
    assert res.returncode == 1, (res.returncode, res.stderr)


@skip_if_no_desktop
def test_render_project_empty_value_exits_2(tmp_path):
    """A malformed invocation (no path after the option) is a usage
    error (exit 2), distinct from a load failure (exit 1)."""
    res = _run("--render-project=", timeout=30)
    assert res.returncode == 2, (res.returncode, res.stderr)


@skip_if_no_desktop
def test_save_roundtrip_missing_file_exits_1(tmp_path):
    res = _run(f"--save-roundtrip={tmp_path / 'nope.json'}", timeout=30)
    assert res.returncode == 1, (res.returncode, res.stderr)


# --------------------------------------------------------------------- #
# Node-kind serialization: mix node + gains survive a C++ round-trip    #
# --------------------------------------------------------------------- #


@skip_if_no_desktop
def test_save_roundtrip_preserves_mix_node(tmp_path):
    """A mix node (num_inputs, channels, gains) must survive the C++
    parse -> saveProjectFile path and reload identically via the Python
    loader. Guards node_registry mix (de)serialization."""
    in_wav = tmp_path / "in.wav"
    audio_io.write_audio(
        str(in_wav), np.zeros((2, 512), dtype=np.float32), 48000, bit_depth=24
    )
    proj = tmp_path / "p.json"
    doc = {
        "minihost_project_version": 1,
        "sample_rate": 48000,
        "block_size": 256,
        "nodes": [
            {"id": "a", "kind": "input", "channels": 2, "source": str(in_wav)},
            {"id": "b", "kind": "input", "channels": 2, "source": str(in_wav)},
            {
                "id": "mix",
                "kind": "mix",
                "num_inputs": 2,
                "channels": 2,
                "gains": [0.5, 0.25],
            },
            {
                "id": "out",
                "kind": "output",
                "channels": 2,
                "sink": str(tmp_path / "out.wav"),
                "bit_depth": 24,
            },
        ],
        "edges": [
            {"src": "a", "dst": "mix", "dst_port": 0},
            {"src": "b", "dst": "mix", "dst_port": 1},
            {"src": "mix", "dst": "out"},
        ],
    }
    proj.write_text(json.dumps(doc, indent=2))

    res = _run(f"--save-roundtrip={proj}", timeout=30)
    assert res.returncode == 0, res.stderr

    resaved = tmp_path / "p.resaved.json"
    assert resaved.exists()

    reloaded = json.loads(resaved.read_text())
    mix_nodes = [n for n in reloaded["nodes"] if n.get("kind") == "mix"]
    assert len(mix_nodes) == 1
    mix = mix_nodes[0]
    assert mix["num_inputs"] == 2
    assert mix["channels"] == 2
    assert mix["gains"] == [0.5, 0.25]

    # And the Python loader still accepts the C++-resaved document.
    loaded = minihost.load_project(resaved)
    assert len(loaded.inputs) == 2


# --------------------------------------------------------------------- #
# Multi-node render parity: two inputs -> weighted mix -> output        #
# --------------------------------------------------------------------- #


@skip_if_no_desktop
def test_multinode_mix_render_parity(tmp_path):
    """A two-input weighted-mix graph must render bit-identically through
    the C++ desktop pipeline and the Python render_project pipeline. Both
    route through the same mh_graph_v2 mix node, so any difference is a
    real bug."""
    sr = 48000
    frames = 4096
    rng = np.random.default_rng(7)
    a = (rng.standard_normal((2, frames)) * 0.1).astype(np.float32)
    b = (rng.standard_normal((2, frames)) * 0.1).astype(np.float32)
    wav_a = tmp_path / "a.wav"
    wav_b = tmp_path / "b.wav"
    audio_io.write_audio(str(wav_a), a, sr, bit_depth=24)
    audio_io.write_audio(str(wav_b), b, sr, bit_depth=24)

    def make(json_path, sink):
        json_path.write_text(
            json.dumps(
                {
                    "minihost_project_version": 1,
                    "sample_rate": sr,
                    "block_size": 256,
                    "nodes": [
                        {
                            "id": "a",
                            "kind": "input",
                            "channels": 2,
                            "source": str(wav_a),
                        },
                        {
                            "id": "b",
                            "kind": "input",
                            "channels": 2,
                            "source": str(wav_b),
                        },
                        {
                            "id": "mix",
                            "kind": "mix",
                            "num_inputs": 2,
                            "channels": 2,
                            "gains": [0.5, 0.25],
                        },
                        {
                            "id": "out",
                            "kind": "output",
                            "channels": 2,
                            "sink": str(sink),
                            "bit_depth": 24,
                        },
                    ],
                    "edges": [
                        {"src": "a", "dst": "mix", "dst_port": 0},
                        {"src": "b", "dst": "mix", "dst_port": 1},
                        {"src": "mix", "dst": "out"},
                    ],
                }
            )
        )

    proj_py = tmp_path / "py.json"
    proj_cpp = tmp_path / "cpp.json"
    out_py = tmp_path / "out_py.wav"
    out_cpp = tmp_path / "out_cpp.wav"
    make(proj_py, out_py)
    make(proj_cpp, out_cpp)

    minihost.render_project(proj_py)
    assert out_py.exists()

    res = _run(f"--render-project={proj_cpp}", timeout=60)
    assert res.returncode == 0, (
        f"desktop render failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    )
    assert out_cpp.exists()

    py, _ = audio_io.read_audio(str(out_py), as_=np.ndarray)
    cpp, _ = audio_io.read_audio(str(out_cpp), as_=np.ndarray)
    assert py.shape == cpp.shape
    n = min(py.shape[1], cpp.shape[1])
    maxerr = float(np.max(np.abs(py[:, :n] - cpp[:, :n])))
    assert maxerr == 0.0, f"max sample diff {maxerr:.3e} (should be exactly 0)"


@skip_if_no_desktop
def test_save_roundtrip_preserves_resample_flag(tmp_path):
    """The input `resample` flag must survive the C++ parse -> save path
    and reload via the Python loader."""
    in_wav = tmp_path / "in.wav"
    audio_io.write_audio(
        str(in_wav), np.zeros((2, 512), dtype=np.float32), 44100, bit_depth=24
    )
    proj = tmp_path / "p.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 256,
                "nodes": [
                    {
                        "id": "in",
                        "kind": "input",
                        "channels": 2,
                        "source": str(in_wav),
                        "resample": True,
                    },
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                        "bit_depth": 24,
                    },
                ],
                "edges": [{"src": "in", "dst": "out"}],
            }
        )
    )

    res = _run(f"--save-roundtrip={proj}", timeout=30)
    assert res.returncode == 0, res.stderr
    resaved = tmp_path / "p.resaved.json"
    assert resaved.exists()

    reloaded = json.loads(resaved.read_text())
    in_node = next(n for n in reloaded["nodes"] if n.get("kind") == "input")
    assert in_node.get("resample") is True
    # And the Python loader agrees.
    assert minihost.load_project(resaved).inputs[0].resample is True


@skip_if_no_desktop
def test_resample_render_parity(tmp_path):
    """A 44.1k input rendered into a 48k project with resample=true must be
    bit-identical between the C++ desktop and Python pipelines. Both read
    via mh_audio_read and resample via mh_audio_resample (the same C
    function), so any difference is a real bug."""
    frames = 22050  # 0.5 s at 44.1k
    rng = np.random.default_rng(11)
    src = (rng.standard_normal((2, frames)) * 0.1).astype(np.float32)
    in_wav = tmp_path / "in.wav"
    audio_io.write_audio(str(in_wav), src, 44100, bit_depth=24)

    def make(json_path, sink):
        json_path.write_text(
            json.dumps(
                {
                    "minihost_project_version": 1,
                    "sample_rate": 48000,
                    "block_size": 256,
                    "nodes": [
                        {
                            "id": "in",
                            "kind": "input",
                            "channels": 2,
                            "source": str(in_wav),
                            "resample": True,
                        },
                        {
                            "id": "out",
                            "kind": "output",
                            "channels": 2,
                            "sink": str(sink),
                            "bit_depth": 24,
                        },
                    ],
                    "edges": [{"src": "in", "dst": "out"}],
                }
            )
        )

    proj_py = tmp_path / "py.json"
    proj_cpp = tmp_path / "cpp.json"
    out_py = tmp_path / "out_py.wav"
    out_cpp = tmp_path / "out_cpp.wav"
    make(proj_py, out_py)
    make(proj_cpp, out_cpp)

    minihost.render_project(proj_py)
    assert out_py.exists()

    res = _run(f"--render-project={proj_cpp}", timeout=60)
    assert res.returncode == 0, (
        f"desktop resample render failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    )
    assert out_cpp.exists()

    py, _ = audio_io.read_audio(str(out_py), as_=np.ndarray)
    cpp, _ = audio_io.read_audio(str(out_cpp), as_=np.ndarray)
    assert py.shape == cpp.shape, (py.shape, cpp.shape)
    n = min(py.shape[1], cpp.shape[1])
    maxerr = float(np.max(np.abs(py[:, :n] - cpp[:, :n])))
    assert maxerr == 0.0, f"max sample diff {maxerr:.3e} (should be exactly 0)"
