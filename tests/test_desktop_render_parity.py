"""C++ desktop renderer vs Python renderer parity.

Drives `minihost_desktop --render-project=<path>` headlessly and
compares its output to `minihost.render_project()` over the same
project file. Both paths read the same JSON schema and route audio
through `mh_graph_render_block`; we expect bit-identical output
within 24-bit quantization (~1.2e-7).

Skipped when the desktop binary isn't built (CI / wheel-only runs).
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

import minihost
from minihost import audio_io

REPO_ROOT = Path(__file__).resolve().parent.parent
DESKTOP_BIN = (REPO_ROOT
               / "build" / "projects" / "minihost_desktop"
               / "minihost_desktop.app" / "Contents" / "MacOS"
               / "minihost_desktop")

skip_if_no_desktop = pytest.mark.skipif(
    not DESKTOP_BIN.exists(),
    reason=f"desktop binary not built at {DESKTOP_BIN}",
)


@skip_if_no_desktop
def test_desktop_renderer_matches_python(tmp_path):
    """A noplugin identity graph rendered by both pipelines must
    produce the same WAV within 24-bit quantization error."""
    sr = 48000
    frames = 8192

    in_wav = tmp_path / "in.wav"
    rng = np.random.default_rng(2)
    src = (rng.standard_normal((2, frames)) * 0.1).astype(np.float32)
    audio_io.write_audio(str(in_wav), src, sr, bit_depth=24)

    proj_py  = tmp_path / "p_py.json"
    proj_cpp = tmp_path / "p_cpp.json"
    out_py  = tmp_path / "out_py.wav"
    out_cpp = tmp_path / "out_cpp.wav"

    def make(json_path, sink):
        doc = {
            "minihost_project_version": 1,
            "sample_rate": sr,
            "block_size": 256,
            "nodes": [
                {"id": "in",  "kind": "input",  "channels": 2,
                 "source": str(in_wav)},
                {"id": "out", "kind": "output", "channels": 2,
                 "sink": str(sink), "bit_depth": 24},
            ],
            "edges": [{"src": "in", "dst": "out"}],
        }
        json_path.write_text(json.dumps(doc))

    make(proj_py,  out_py)
    make(proj_cpp, out_cpp)

    # Python render
    minihost.render_project(proj_py)
    assert out_py.exists()

    # C++ headless render
    res = subprocess.run(
        [str(DESKTOP_BIN),
         f"--render-project={proj_cpp}"],
        capture_output=True, text=True, timeout=60,
    )
    assert res.returncode == 0, \
        f"desktop render failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    assert out_cpp.exists()

    py, _  = audio_io.read_audio(str(out_py),  as_=np.ndarray)
    cpp, _ = audio_io.read_audio(str(out_cpp), as_=np.ndarray)
    assert py.shape == cpp.shape
    n = min(py.shape[1], cpp.shape[1])
    maxerr = float(np.max(np.abs(py[:, :n] - cpp[:, :n])))
    # Identity graph through 24-bit round-trip: pipeline-independent
    # difference must be 0. (Any non-zero diff would be a real bug.)
    assert maxerr == 0.0, f"max sample diff {maxerr:.3e} (should be exactly 0)"


@skip_if_no_desktop
def test_desktop_save_roundtrip_matches_python(tmp_path):
    """The C++ saveProjectFile path must round-trip through the
    Python loader. Layout, edges, channels, etc. preserved."""
    in_wav = tmp_path / "in.wav"
    audio_io.write_audio(str(in_wav),
                         np.zeros((2, 512), dtype=np.float32),
                         48000, bit_depth=24)
    proj = tmp_path / "p.json"
    doc = {
        "minihost_project_version": 1,
        "sample_rate": 48000,
        "block_size": 256,
        "nodes": [
            {"id": "in",  "kind": "input",  "channels": 2, "source": str(in_wav)},
            {"id": "out", "kind": "output", "channels": 2,
             "sink": str(tmp_path / "out.wav"), "bit_depth": 24},
        ],
        "edges": [{"src": "in", "dst": "out"}],
        "layout": {
            "in":  {"x": 12.5, "y": 34.0},
            "out": {"x": 260.0, "y": 34.0},
        },
    }
    proj.write_text(json.dumps(doc, indent=2))

    res = subprocess.run(
        [str(DESKTOP_BIN),
         f"--save-roundtrip={proj}"],
        capture_output=True, text=True, timeout=30,
    )
    assert res.returncode == 0, res.stderr

    resaved = tmp_path / "p.resaved.json"
    assert resaved.exists()

    orig    = minihost.load_project(proj)
    resaved_loaded = minihost.load_project(resaved)
    assert orig.layout == resaved_loaded.layout
    assert len(orig.inputs)  == len(resaved_loaded.inputs)
    assert len(orig.outputs) == len(resaved_loaded.outputs)
