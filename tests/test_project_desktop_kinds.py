"""The Python project renderer handles every node kind the desktop app writes.

The desktop app and `minihost.render_project` read one schema. These tests
check that Python loads the desktop-only kinds (device and meter nodes,
gain, bus, channel pick/merge, metronome, MIDI clock, sequencer) and, when
the desktop binary is built, that both renderers write bit-identical files.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import numpy as np
import pytest

import minihost
from minihost import _sequencer, audio_io

from cli_helpers import find_test_plugin
from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

REPO = Path(__file__).resolve().parent.parent
EXAMPLES = REPO / "examples" / "projects"
SYNTH = find_test_plugin("MinihostTestSynth", "MINIHOST_TEST_PLUGIN_SYNTH")
requires_synth = pytest.mark.skipif(
    not SYNTH or not os.path.exists(SYNTH), reason="MinihostTestSynth not built"
)
SR = 48000


def _desktop_render(proj: Path) -> None:
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--render-project={proj}"],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"


def _render_both(tmp_path: Path, doc: dict, sinks: list[str]) -> list[tuple]:
    """Render `doc` with Python and with the desktop; return (py, cpp) arrays
    per sink. `doc` names sinks by bare file name; each side gets its own
    directory."""
    pairs = []
    for side in ("py", "cpp"):
        d = tmp_path / side
        d.mkdir()
        proj = d / "p.json"
        proj.write_text(json.dumps(doc))
        if side == "py":
            minihost.render_project(proj)
        else:
            _desktop_render(proj)
    for s in sinks:
        py, _ = audio_io.read_audio(str(tmp_path / "py" / s), as_=np.ndarray)
        cpp, _ = audio_io.read_audio(str(tmp_path / "cpp" / s), as_=np.ndarray)
        pairs.append((py, cpp))
    return pairs


def _routing_doc(src: Path) -> dict:
    # in -> gain 0.5 -> bus -> pick L / pick R -> merge (R, L) -> mix with a
    # silent device input and metronome -> out. A meter and a speaker output
    # tap the merge; a MIDI clock feeds nothing.
    return {
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 256,
        "bpm": 90.0,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": str(src)},
            {"id": "g", "kind": "gain", "channels": 2, "gain": 0.5},
            {"id": "b", "kind": "bus", "channels": 2},
            {"id": "l", "kind": "pick_channel", "in_channels": 2, "channel_index": 0},
            {"id": "r", "kind": "pick_channel", "in_channels": 2, "channel_index": 1},
            {"id": "m", "kind": "merge_channels", "out_channels": 2},
            {"id": "mic", "kind": "device_input", "channels": 2},
            {"id": "click", "kind": "metronome", "channels": 2},
            {"id": "mix", "kind": "mix", "num_inputs": 3, "channels": 2},
            {"id": "meter", "kind": "meter", "channels": 2},
            {"id": "spk", "kind": "device_output", "channels": 2},
            {"id": "clock", "kind": "midi_clock"},
            {
                "id": "out",
                "kind": "output",
                "channels": 2,
                "sink": "out.wav",
                "bit_depth": 32,
            },
        ],
        "edges": [
            {"src": "in", "dst": "g"},
            {"src": "g", "dst": "b"},
            {"src": "b", "dst": "l"},
            {"src": "b", "dst": "r"},
            {"src": "r", "dst": "m", "dst_port": 0},
            {"src": "l", "dst": "m", "dst_port": 1},
            {"src": "m", "dst": "mix", "dst_port": 0},
            {"src": "mic", "dst": "mix", "dst_port": 1},
            {"src": "click", "dst": "mix", "dst_port": 2},
            {"src": "mix", "dst": "out"},
            {"src": "m", "dst": "meter"},
            {"src": "m", "dst": "spk"},
        ],
    }


def _source(tmp_path: Path) -> tuple[Path, np.ndarray]:
    rng = np.random.default_rng(7)
    src = (rng.standard_normal((2, 6000)) * 0.2).astype(np.float32)
    path = tmp_path / "in.wav"
    audio_io.write_audio(str(path), src, SR, bit_depth=32)
    return path, src


def test_routing_kinds_render_in_python(tmp_path):
    src_path, src = _source(tmp_path)
    proj = tmp_path / "p.json"
    proj.write_text(json.dumps(_routing_doc(src_path)))
    p = minihost.render_project(proj)
    assert p.bpm == 90.0
    out, _ = audio_io.read_audio(str(tmp_path / "out.wav"), as_=np.ndarray)
    # Halved and swapped; the device input and metronome are silent offline.
    np.testing.assert_allclose(out, 0.5 * src[::-1], atol=1e-7)


@skip_if_no_desktop
def test_routing_kinds_match_desktop(tmp_path):
    src_path, _ = _source(tmp_path)
    ((py, cpp),) = _render_both(tmp_path, _routing_doc(src_path), ["out.wav"])
    assert py.shape == cpp.shape
    assert np.array_equal(py, cpp)


def _sequencer_doc(synth: str) -> dict:
    # A short rate, a full gate (note-off on the next note-on's sample) and
    # mutation exercise the rounding and hashing paths.
    return {
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 128,
        "bpm": 133.0,
        "duration_seconds": 2.0,
        "nodes": [
            {
                "id": "seq",
                "kind": "sequencer",
                "steps": 12,
                "rate": 1.0 / 6.0,
                "root": 50,
                "scale": "dorian",
                "octaves": 3,
                "density": 0.8,
                "gate": 1.0,
                "velocity_min": 40,
                "velocity_max": 120,
                "mutate": 0.3,
                "seed": 12345,
                "channel": 3,
            },
            {"id": "synth", "kind": "plugin", "path": synth},
            {
                "id": "out",
                "kind": "output",
                "channels": 2,
                "sink": "out.wav",
                "bit_depth": 32,
            },
        ],
        "edges": [
            {"src": "seq", "dst": "synth", "kind": "midi"},
            {"src": "synth", "dst": "out"},
        ],
    }


def _half_sample_doc(synth: str) -> dict:
    # At 128 BPM a 1/16 step is exactly 5625 samples, so with gate 0.5 every
    # note-off falls on k + 0.5: std::llround rounds it up, round() to even.
    doc = _sequencer_doc(synth)
    doc["bpm"] = 128.0
    doc["nodes"][0].update(rate=0.25, gate=0.5, mutate=0.0)
    return doc


@skip_if_no_desktop
@requires_synth
@pytest.mark.parametrize("make", [_sequencer_doc, _half_sample_doc])
def test_sequencer_matches_desktop(tmp_path, make):
    ((py, cpp),) = _render_both(tmp_path, make(SYNTH), ["out.wav"])
    assert np.any(py != 0.0), "the sequencer played no notes"
    assert py.shape == cpp.shape
    assert np.array_equal(py, cpp)


def test_sequencer_events_do_not_depend_on_block_size():
    p = _sequencer.SequencerParams(rate=1.0 / 3.0, gate=1.0, mutate=0.5, seed=99)
    _sequencer.clamp_params(p)
    whole = _sequencer.events(p, SR, 117.0, 0, 5 * SR)
    for block in (1, 64, 1000):
        parts = []
        for s0 in range(0, 5 * SR, block):
            parts += _sequencer.events(p, SR, 117.0, s0, min(s0 + block, 5 * SR))
        assert parts == whole
    assert any(st & 0xF0 == 0x90 for _, st, _, _ in whole)
    # Time order, with a note-off before a note-on at the same sample.
    assert [e[0] for e in whole] == sorted(e[0] for e in whole)


def test_sequencer_rejects_unknown_scale(tmp_path):
    doc = _sequencer_doc("unused.vst3")
    doc["nodes"][0]["scale"] = "lydian"
    proj = tmp_path / "p.json"
    proj.write_text(json.dumps(doc))
    with pytest.raises(minihost.ProjectError, match="unknown scale 'lydian'"):
        minihost.load_project(proj)


def test_bpm_out_of_range_is_rejected(tmp_path):
    src_path, _ = _source(tmp_path)
    doc = _routing_doc(src_path)
    doc["bpm"] = 0
    proj = tmp_path / "p.json"
    proj.write_text(json.dumps(doc))
    with pytest.raises(minihost.ProjectError, match="bpm must be in"):
        minihost.load_project(proj)


def _example_projects(dest: Path) -> list[Path]:
    """Generate the example set in `dest`; return the projects it wrote."""
    subprocess.run(
        [sys.executable, str(EXAMPLES / "make_examples.py"), str(dest)],
        check=True,
        capture_output=True,
        text=True,
        timeout=120,
    )
    return sorted(dest.glob("*.json"))


def _needs_missing_plugin(proj: Path) -> bool:
    try:
        minihost.load_project(proj)
    except minihost.ProjectError as exc:
        msg = str(exc)
        return "failed to open" in msg or "plugin path not found" in msg
    return False


def test_every_example_project_renders_in_python(tmp_path):
    # Two levels down, so the examples' ../../build/out sinks stay in tmp_path.
    dest = tmp_path / "a" / "b"
    dest.mkdir(parents=True)
    rendered = 0
    for proj in _example_projects(dest):
        if _needs_missing_plugin(proj):
            continue
        minihost.render_project(proj)
        rendered += 1
    assert rendered > 0


@skip_if_no_desktop
def test_every_example_project_matches_desktop(tmp_path):
    dest = tmp_path / "a" / "b"
    dest.mkdir(parents=True)
    compared = 0
    for proj in _example_projects(dest):
        if _needs_missing_plugin(proj):
            continue
        doc = json.loads(proj.read_text())
        sinks = [n for n in doc["nodes"] if n["kind"] == "output"]
        minihost.render_project(proj)
        py = {s["id"]: (dest / s["sink"]).read_bytes() for s in sinks}
        _desktop_render(proj)
        for s in sinks:
            assert (dest / s["sink"]).read_bytes() == py[s["id"]], (
                f"{proj.name}: output {s['id']!r} differs"
            )
        compared += 1
    assert compared > 0


def _bypassed_synth_doc(synth: str) -> dict:
    # A bypassed instrument passes its (absent) audio input through: silence.
    doc = _sequencer_doc(synth)
    doc["nodes"][1]["bypass"] = True
    return doc


@requires_synth
def test_bypass_reaches_the_python_graph(tmp_path):
    proj = tmp_path / "p.json"
    proj.write_text(json.dumps(_bypassed_synth_doc(SYNTH)))
    minihost.render_project(proj)
    out, _ = audio_io.read_audio(str(tmp_path / "out.wav"), as_=np.ndarray)
    assert out.shape[1] > 0
    assert not np.any(out)


@skip_if_no_desktop
@requires_synth
def test_bypass_matches_desktop(tmp_path):
    ((py, cpp),) = _render_both(tmp_path, _bypassed_synth_doc(SYNTH), ["out.wav"])
    assert py.shape == cpp.shape
    assert not np.any(cpp)


def test_bypass_must_be_a_boolean(tmp_path):
    proj = tmp_path / "p.json"
    doc = _sequencer_doc("synth.vst3")
    doc["nodes"][1]["bypass"] = "yes"
    proj.write_text(json.dumps(doc))
    with pytest.raises(minihost.ProjectError, match="bypass"):
        minihost.load_project(proj)


# ----- parameter automation ----- #

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")
requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX), reason="MinihostTestFx not built"
)


def _automation_doc(src: Path, lanes: list) -> dict:
    return {
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 512,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": str(src)},
            {"id": "fx", "kind": "plugin", "path": FX},
            {
                "id": "out",
                "kind": "output",
                "channels": 2,
                "sink": "out.wav",
                "bit_depth": 32,
            },
        ],
        "edges": [{"src": "in", "dst": "fx"}, {"src": "fx", "dst": "out"}],
        "automation": lanes,
    }


def _ones(tmp_path: Path, frames: int = 24000) -> Path:
    path = tmp_path / "ones.wav"
    audio_io.write_audio(
        str(path), np.ones((2, frames), dtype=np.float32), SR, bit_depth=32
    )
    return path


_RAMP = [{"node": "fx", "param": "gain", "points": [[0.0, 0.0], [0.25, 1.0]]}]


@requires_fx
def test_automation_ramps_a_parameter(tmp_path):
    proj = tmp_path / "p.json"
    proj.write_text(json.dumps(_automation_doc(_ones(tmp_path), _RAMP)))
    minihost.render_project(proj)
    out, _ = audio_io.read_audio(str(tmp_path / "out.wav"), as_=np.ndarray)
    # One change every 64 samples along 12000 samples; the fixture's gain
    # snaps to steps of 0.01.
    k = np.arange(out.shape[1]) // 64
    expected = np.round(np.minimum(k * 64 / 12000, 1.0) / 0.01) * 0.01
    assert np.allclose(out[0], expected, atol=0.006)
    assert np.all(out[:, :64] == 0.0)
    assert np.all(out[:, 12000:] == 1.0)


@skip_if_no_desktop
@requires_fx
def test_automation_matches_desktop(tmp_path):
    src = _ones(tmp_path)
    lanes = [
        *_RAMP,
        # By index, with a jump (two points at one time) and a point past a
        # block boundary.
        {"node": "fx", "param": 2, "points": [[0.1, 0.0], [0.1, 0.7], [0.3, 0.2]]},
    ]
    ((py, cpp),) = _render_both(tmp_path, _automation_doc(src, lanes), ["out.wav"])
    assert np.any(py != py[0, 0])
    assert py.shape == cpp.shape
    assert np.array_equal(py, cpp)


@pytest.mark.parametrize(
    ("lane", "message"),
    [
        ({"node": "out", "param": 0, "points": [[0, 0]]}, "not a plugin node"),
        ({"node": "fx", "param": -1, "points": [[0, 0]]}, ">= 0"),
        ({"node": "fx", "param": 0, "points": []}, "non-empty"),
        ({"node": "fx", "param": 0, "points": [[0, 2]]}, r"\[0, 1\]"),
        ({"node": "fx", "param": 0, "points": [[-1, 0]]}, "time must be >= 0"),
        ({"node": "fx", "param": 0, "points": [[0]]}, r"\[seconds, value\]"),
    ],
)
def test_automation_lanes_are_checked(tmp_path, lane, message):
    proj = tmp_path / "p.json"
    doc = _automation_doc(tmp_path / "x.wav", [lane])
    doc["nodes"][1]["path"] = "fx.vst3"
    proj.write_text(json.dumps(doc))
    with pytest.raises(minihost.ProjectError, match=message):
        minihost.load_project(proj)


@requires_fx
@pytest.mark.parametrize(
    ("param", "message"),
    [("Nope", "no parameter named 'Nope'"), (100000, "out of range")],
)
def test_automation_param_must_exist(tmp_path, param, message):
    proj = tmp_path / "p.json"
    lane = {"node": "fx", "param": param, "points": [[0, 0]]}
    proj.write_text(json.dumps(_automation_doc(_ones(tmp_path), [lane])))
    with pytest.raises(minihost.ProjectError, match=message):
        minihost.load_project(proj)


@skip_if_no_desktop
@requires_fx
def test_desktop_rejects_an_unknown_automation_param(tmp_path):
    proj = tmp_path / "p.json"
    lane = {"node": "fx", "param": "Nope", "points": [[0, 0]]}
    proj.write_text(json.dumps(_automation_doc(_ones(tmp_path), [lane])))
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--render-project={proj}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert res.returncode != 0
    assert "no parameter named 'Nope'" in res.stdout + res.stderr


@skip_if_no_desktop
def test_desktop_save_keeps_automation(tmp_path):
    proj = tmp_path / "p.json"
    lanes = [{"node": "fx", "param": "Gain", "points": [[0.0, 0.25], [1.5, 0.75]]}]
    doc = _automation_doc(tmp_path / "x.wav", lanes)
    doc["nodes"][1]["path"] = "fx.vst3"
    proj.write_text(json.dumps(doc))
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--save-roundtrip={proj}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert res.returncode == 0, res.stderr
    saved = json.loads((tmp_path / "p.resaved.json").read_text())
    assert saved["automation"] == lanes
