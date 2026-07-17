"""Opt-in input resampling in the project renderer.

The project renderer is strict about input sample rates by default: a file
whose rate differs from the project rate is an error. Setting ``resample:
true`` on an input converts it to the project rate at load time (via the
shared mh_audio_resample -- the same C resampler the CLI and desktop use).

These tests cover the Python side of the flag; the C++ desktop mirrors it,
and tests/test_desktop_smoke.py asserts the two render bit-identically.
"""

from __future__ import annotations

import json

import numpy as np
import pytest

import minihost
from minihost import audio_io


def _write_project(path, in_wav, out_wav, *, project_sr, resample):
    node = {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)}
    if resample is not None:
        node["resample"] = resample
    path.write_text(json.dumps({
        "minihost_project_version": 1,
        "sample_rate": project_sr,
        "block_size": 256,
        "nodes": [
            node,
            {"id": "out", "kind": "output", "channels": 2,
             "sink": str(out_wav), "bit_depth": 24},
        ],
        "edges": [{"src": "in", "dst": "out"}],
    }))


def test_mismatch_without_resample_raises(tmp_path):
    in_wav = tmp_path / "in.wav"
    audio_io.write_audio(str(in_wav),
                         np.zeros((2, 4410), dtype=np.float32),
                         44100, bit_depth=24)
    proj = tmp_path / "p.json"
    _write_project(proj, in_wav, tmp_path / "out.wav",
                   project_sr=48000, resample=None)
    with pytest.raises(minihost.ProjectError, match="sample rate"):
        minihost.render_project(proj)


def test_resample_true_converts_and_renders(tmp_path):
    """A 44.1k input with resample=true renders into a 48k project, and the
    output length scales by the rate ratio (48000/44100)."""
    frames = 44100  # 1 second at 44.1k
    in_wav = tmp_path / "in.wav"
    rng = np.random.default_rng(3)
    src = (rng.standard_normal((2, frames)) * 0.1).astype(np.float32)
    audio_io.write_audio(str(in_wav), src, 44100, bit_depth=24)

    out_wav = tmp_path / "out.wav"
    proj = tmp_path / "p.json"
    _write_project(proj, in_wav, out_wav, project_sr=48000, resample=True)

    minihost.render_project(proj)
    assert out_wav.exists()

    out, out_sr = audio_io.read_audio(str(out_wav), as_=np.ndarray)
    assert int(out_sr) == 48000
    expected = round(frames * 48000 / 44100)
    # Resampler frame count can differ by a few samples from the naive
    # ratio; allow a small tolerance.
    assert abs(out.shape[1] - expected) <= 8, (out.shape[1], expected)


def test_resample_flag_round_trips(tmp_path):
    """The resample flag survives a load; default inputs stay False."""
    in_wav = tmp_path / "in.wav"
    audio_io.write_audio(str(in_wav),
                         np.zeros((2, 512), dtype=np.float32),
                         44100, bit_depth=24)
    proj = tmp_path / "p.json"
    _write_project(proj, in_wav, tmp_path / "out.wav",
                   project_sr=48000, resample=True)
    loaded = minihost.load_project(proj)
    assert loaded.inputs[0].resample is True

    # And an input without the field defaults to False.
    proj2 = tmp_path / "p2.json"
    _write_project(proj2, in_wav, tmp_path / "out2.wav",
                   project_sr=44100, resample=None)
    loaded2 = minihost.load_project(proj2)
    assert loaded2.inputs[0].resample is False
