"""C++ desktop renderer vs Python renderer parity.

Drives `minihost_desktop --render-project=<path>` headlessly and
compares its output to `minihost.render_project()` over the same
project file. Both paths read the same JSON schema and route audio
through `mh_graph_render_block`; we expect bit-identical output
within 24-bit quantization (~1.2e-7).

Skipped when the desktop binary isn't built (CI / wheel-only runs).
"""

from __future__ import annotations

import base64
import json
import os
import platform
import subprocess

import numpy as np
import pytest

import minihost
from minihost import audio_io

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop


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

    proj_py = tmp_path / "p_py.json"
    proj_cpp = tmp_path / "p_cpp.json"
    out_py = tmp_path / "out_py.wav"
    out_cpp = tmp_path / "out_cpp.wav"

    def make(json_path, sink):
        doc = {
            "minihost_project_version": 1,
            "sample_rate": sr,
            "block_size": 256,
            "nodes": [
                {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
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
        json_path.write_text(json.dumps(doc))

    make(proj_py, out_py)
    make(proj_cpp, out_cpp)

    # Python render
    minihost.render_project(proj_py)
    assert out_py.exists()

    # C++ headless render
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--render-project={proj_cpp}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert res.returncode == 0, (
        f"desktop render failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    )
    assert out_cpp.exists()

    py, _ = audio_io.read_audio(str(out_py), as_=np.ndarray)
    cpp, _ = audio_io.read_audio(str(out_cpp), as_=np.ndarray)
    assert py.shape == cpp.shape
    n = min(py.shape[1], cpp.shape[1])
    maxerr = float(np.max(np.abs(py[:, :n] - cpp[:, :n])))
    # Identity graph through 24-bit round-trip: pipeline-independent
    # difference must be 0. (Any non-zero diff would be a real bug.)
    assert maxerr == 0.0, f"max sample diff {maxerr:.3e} (should be exactly 0)"


# Stock Apple AU effects (macOS): stable identifiers, and effects -- they do
# NOT accept MIDI, which is exactly what the migration regression needs.
_STOCK_AU_EFFECTS = [
    ("AUBandpass", "AudioUnit:Effects/aufx,bpas,appl"),
    ("AULowpass", "AudioUnit:Effects/aufx,lpas,appl"),
    ("AUDelay", "AudioUnit:Effects/aufx,dely,appl"),
]


@skip_if_no_desktop
def test_desktop_effect_only_no_midi_migration_crash(tmp_path):
    """An effect-only project must load and render, not abort.

    Regression: the legacy receives_midi fan-out defaults to true, so the
    desktop loader would synthesize a MIDI input and try to connect it to
    every plugin -- including effects that don't accept MIDI, which the graph
    compiler rejects (connectMidi throws), aborting the whole load. The fix
    only wires MIDI to plugins whose opened instance reports accepts_midi.

    Uses a stock Apple AU effect (guaranteed on macOS, and guaranteed to not
    accept MIDI) with no receives_midi override, so the migration runs. Also
    guards the exit-abort fix: a plugin-loading render must exit 0 (libminihost's
    redundant message thread is disabled in the desktop app so teardown is
    clean).
    """
    if platform.system() != "Darwin":
        pytest.skip("stock AU effects are macOS-only")

    in_wav = tmp_path / "in.wav"
    out_wav = tmp_path / "out.wav"
    src = (np.random.default_rng(3).standard_normal((2, 4800)) * 0.2).astype(np.float32)
    audio_io.write_audio(str(in_wav), src, 48000, bit_depth=24)

    name, ident = _STOCK_AU_EFFECTS[0]
    pd_xml = f'<PLUGIN name="{name}" format="AudioUnit" file="{ident}"/>'
    desc_b64 = base64.b64encode(pd_xml.encode("utf-8")).decode("ascii")

    proj = tmp_path / "p.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
                    # No receives_midi -> defaults true; effect does not accept MIDI.
                    {
                        "id": "fx",
                        "kind": "plugin",
                        "descriptor": desc_b64,
                        "name": name,
                    },
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(out_wav),
                        "bit_depth": 24,
                    },
                ],
                "edges": [{"src": "in", "dst": "fx"}, {"src": "fx", "dst": "out"}],
            }
        )
    )

    res = subprocess.run(
        [str(DESKTOP_BIN), f"--render-project={proj}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert "connect_midi" not in res.stderr, (
        f"MIDI migration wired MIDI to a non-MIDI plugin:\n{res.stderr}"
    )
    # Clean exit: a plugin-loading render must not abort on teardown.
    assert res.returncode == 0, (
        f"plugin render did not exit cleanly:\nstderr:{res.stderr}"
    )
    assert out_wav.exists(), f"render produced no output:\n{res.stderr}"
    y, _ = audio_io.read_audio(str(out_wav), as_=np.ndarray)
    assert y.shape[1] == 4800
    assert np.isfinite(y).all()


@skip_if_no_desktop
def test_desktop_midi_chain_matches_python(tmp_path):
    """A MIDI-driven instrument render must match Python bit-for-bit.

    The desktop offline renderer streams a midi_input node's .mid `source`
    into the graph per block, mirroring the Python renderer. Regression guard
    for the bug where the desktop dropped MIDI-file input entirely and rendered
    silence (its midi_input was live-only). Needs an instrument that accepts
    MIDI (MINIHOST_TEST_PLUGIN, default Dexed); skipped otherwise.
    """
    plugin = (
        os.environ.get("MINIHOST_TEST_PLUGIN")
        or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
    )
    if not os.path.exists(plugin):
        pytest.skip(f"test plugin not found: {plugin}")
    probe = minihost.Plugin(plugin)
    accepts = probe.accepts_midi
    del probe
    if not accepts:
        pytest.skip(f"{plugin} does not accept MIDI (needs an instrument)")

    # Author a short C-major chord (held 1.5s at 120 BPM).
    mf = minihost.MidiFile()
    t = mf.add_track()
    mf.add_tempo(t, 0, 120.0)
    for pitch in (60, 64, 67):
        mf.add_note_on(t, 0, 0, pitch, 100)
    for pitch in (60, 64, 67):
        mf.add_note_off(t, 1440, 0, pitch, 0)
    mid = tmp_path / "chord.mid"
    assert mf.save(str(mid))

    out_cpp = tmp_path / "cpp.wav"
    out_py = tmp_path / "py.wav"

    def build(sink):
        return {
            "minihost_project_version": 1,
            "sample_rate": 48000,
            "block_size": 512,
            "duration_seconds": 2.0,
            "nodes": [
                {"id": "midi", "kind": "midi_input", "source": str(mid)},
                {"id": "synth", "kind": "plugin", "path": plugin},
                {
                    "id": "out",
                    "kind": "output",
                    "channels": 2,
                    "sink": str(sink),
                    "bit_depth": 24,
                },
            ],
            "edges": [
                {"kind": "midi", "src": "midi", "dst": "synth"},
                {"src": "synth", "dst": "out"},
            ],
        }

    pj_cpp = tmp_path / "cpp.json"
    pj_cpp.write_text(json.dumps(build(out_cpp)))
    pj_py = tmp_path / "py.json"
    pj_py.write_text(json.dumps(build(out_py)))

    res = subprocess.run(
        [str(DESKTOP_BIN), f"--render-project={pj_cpp}"],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert res.returncode == 0, f"desktop render failed:\n{res.stderr}"
    minihost.render_project(pj_py)

    assert out_cpp.exists() and out_py.exists()
    a, _ = audio_io.read_audio(str(out_cpp), as_=np.ndarray)
    b, _ = audio_io.read_audio(str(out_py), as_=np.ndarray)
    # MIDI actually drove the synth (not silence).
    assert float(np.max(np.abs(a))) > 1e-3, (
        f"desktop MIDI render is silent:\n{res.stderr}"
    )
    # And the two renderers agree bit-for-bit.
    n = min(a.shape[1], b.shape[1])
    maxerr = float(np.max(np.abs(a[:, :n] - b[:, :n])))
    assert maxerr == 0.0, f"desktop vs python diff {maxerr:.3e} (should be 0)"


@skip_if_no_desktop
def test_desktop_save_roundtrip_matches_python(tmp_path):
    """The C++ saveProjectFile path must round-trip through the
    Python loader. Layout, edges, channels, etc. preserved."""
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
            {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
            {
                "id": "out",
                "kind": "output",
                "channels": 2,
                "sink": str(tmp_path / "out.wav"),
                "bit_depth": 24,
            },
        ],
        "edges": [{"src": "in", "dst": "out"}],
        "layout": {
            "in": {"x": 12.5, "y": 34.0},
            "out": {"x": 260.0, "y": 34.0},
        },
    }
    proj.write_text(json.dumps(doc, indent=2))

    res = subprocess.run(
        [str(DESKTOP_BIN), f"--save-roundtrip={proj}"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert res.returncode == 0, res.stderr

    resaved = tmp_path / "p.resaved.json"
    assert resaved.exists()

    orig = minihost.load_project(proj)
    resaved_loaded = minihost.load_project(resaved)
    assert orig.layout == resaved_loaded.layout
    assert len(orig.inputs) == len(resaved_loaded.inputs)
    assert len(orig.outputs) == len(resaved_loaded.outputs)
