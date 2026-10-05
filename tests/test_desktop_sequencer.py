"""The desktop app's generative sequencer node.

A sequencer drives a midi_output node. Offline renders are captured with
`--render-project --render-midi-log`, live playback with `--live-selftest`
(see test_desktop_live.py). Each line of a log is
"frame status data1 data2".
"""

from __future__ import annotations

import base64
import json
import platform
import subprocess

import numpy as np
import pytest

from minihost import audio_io

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

pytestmark = skip_if_no_desktop

SR = 48000
STEP = SR // 8  # 1/16 at 120 BPM
MINOR = [0, 2, 3, 5, 7, 8, 10]
SEQ = {
    "steps": 8,
    "rate": 0.25,
    "root": 48,
    "scale": "minor",
    "octaves": 2,
    "density": 0.6,
    "gate": 0.5,
    "velocity_min": 60,
    "velocity_max": 100,
    "mutate": 0.0,
    "seed": 42,
    "channel": 3,
}


def _project(tmp_path, duration=2.0, name="s", **seq):
    # LiveEngine needs an audio output; a silent passthrough supplies one.
    silent = tmp_path / "silent.wav"
    audio_io.write_audio(str(silent), np.zeros((2, 480), np.float32), SR, bit_depth=24)
    proj = tmp_path / f"{name}.json"
    proj.write_text(json.dumps({
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 512,
        "bpm": 120,
        "duration_seconds": duration,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": str(silent)},
            {"id": "out", "kind": "output", "channels": 2,
             "sink": str(tmp_path / f"{name}.wav"), "bit_depth": 24},
            {"id": "seq", "kind": "sequencer", **{**SEQ, **seq}},
            {"id": "mo", "kind": "midi_output"},
        ],
        "edges": [{"src": "in", "dst": "out"},
                  {"src": "seq", "dst": "mo", "kind": "midi"}],
    }))
    return proj


def _run(*args, ok=True):
    res = subprocess.run([str(DESKTOP_BIN), *args], capture_output=True, text=True,
                         timeout=60)
    if ok:
        assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"
    return res


def _log(path):
    out = []
    for line in path.read_text().splitlines():
        frame, status, d1, d2 = (int(x) for x in line.split())
        out.append((frame, status, d1, d2))
    return out


def _render(proj):
    _run(f"--render-project={proj}", "--render-midi-log")
    return _log(proj.with_suffix(".render-midi.txt"))


def _ons(events):
    return [e for e in events if e[1] & 0xF0 == 0x90 and e[3] > 0]


def _assert_no_hung_notes(events):
    sounding = set()
    for _, status, note, vel in events:
        if status & 0xF0 == 0x90 and vel > 0:
            sounding.add(note)
        else:
            sounding.discard(note)
    assert not sounding


def test_render_plays_notes_in_scale_on_the_step_grid(tmp_path):
    ev = _render(_project(tmp_path))
    ons = _ons(ev)
    assert 0 < len(ons) < 32  # 32 steps at density 0.6
    allowed = {48 + 12 * k + d for k in range(2) for d in MINOR}
    for frame, status, note, vel in ons:
        assert frame % STEP == 0
        assert status == 0x92  # channel 3
        assert note in allowed
        assert 60 <= vel <= 100
    offs = {(f, n) for f, s, n, _ in ev if s == 0x82}
    for frame, _, note, _ in ons:
        if frame + STEP // 2 < 2 * SR:
            assert (frame + STEP // 2, note) in offs  # gate 0.5


def test_pattern_repeats_unless_mutated(tmp_path):
    bar = 8 * STEP

    def bars(events):
        ons = _ons(events)
        return [[(f - b * bar, n, v) for f, _, n, v in ons if b * bar <= f < (b + 1) * bar]
                for b in range(4)]

    plain = bars(_render(_project(tmp_path, duration=4.0, name="plain")))
    assert plain[0] and plain[0] == plain[1] == plain[2] == plain[3]
    mutated = bars(_render(_project(tmp_path, duration=4.0, name="mut", mutate=1.0)))
    assert mutated[0] != mutated[1]


def test_seed_changes_the_melody(tmp_path):
    a = _render(_project(tmp_path, name="a"))
    b = _render(_project(tmp_path, name="b", seed=43))
    assert _ons(a) != _ons(b)


def test_live_matches_the_offline_render(tmp_path):
    # Live runs in 300-frame callbacks, the render in 512-frame blocks.
    proj = _project(tmp_path)
    rendered = _render(proj)
    _run(f"--live-selftest={proj}")
    live = _log(proj.with_suffix(".midi.txt"))
    take = 2 * SR
    assert [e for e in live if e[0] < take] == rendered
    # Second take starts from 0 again, after Stop released what sounded.
    second = [(f - take - 300, s, n, v) for f, s, n, v in live if f > take]
    assert _ons(second) == _ons(rendered)
    _assert_no_hung_notes(live)


def test_live_loop_repeats_and_releases_at_the_wrap(tmp_path):
    loop = int(1.01 * SR)
    proj = _project(tmp_path, duration=1.01, gate=1.0, density=1.0)
    _run(f"--live-selftest={proj}", "--live-loop")
    live = _log(proj.with_suffix(".midi.txt"))
    first = [(f, n) for f, _, n, _ in _ons(live) if f < loop]
    second = [(f - loop, n) for f, _, n, _ in _ons(live) if loop <= f < 2 * loop]
    assert first and first == second
    _assert_no_hung_notes(live)
    # gate 1: each note-off shares a frame with the next note-on and
    # comes before it.
    for i, (f, s, _, v) in enumerate(live):
        if s & 0xF0 == 0x90 and v > 0 and i > 0 and live[i - 1][0] == f:
            assert live[i - 1][1] & 0xF0 == 0x80


def test_live_edit_switches_the_melody_without_hung_notes(tmp_path):
    proj = _project(tmp_path)
    _run(f"--live-selftest={proj}", "--live-reseed")
    live = _log(proj.with_suffix(".midi.txt"))
    take = 2 * SR
    # The edit lands before the callback that crosses the midpoint.
    switch = ((take // 2 - 1) // 300) * 300
    a = _ons(_render(proj))
    b = _ons(_render(_project(tmp_path, name="b", seed=43)))
    ons = _ons([e for e in live if e[0] < take])
    assert [e for e in ons if e[0] < switch] == [e for e in a if e[0] < switch]
    assert [e for e in ons if e[0] >= switch] == [e for e in b if switch <= e[0] < take]
    _assert_no_hung_notes(live)


def test_unknown_scale_is_rejected(tmp_path):
    res = _run(f"--render-project={_project(tmp_path, scale='lydian')}", ok=False)
    assert res.returncode != 0
    assert "unknown scale" in res.stderr


def test_save_round_trip_keeps_settings_and_bpm(tmp_path):
    proj = _project(tmp_path, mutate=0.25, seed=4000000000)
    _run(f"--save-roundtrip={proj}")
    doc = json.loads(proj.with_suffix(".resaved.json").read_text())
    assert doc["bpm"] == 120
    node = next(n for n in doc["nodes"] if n["kind"] == "sequencer")
    for k, v in {**SEQ, "mutate": 0.25, "seed": 4000000000}.items():
        assert node[k] == v, k


def test_sequencer_drives_an_instrument_plugin(tmp_path):
    # Regression: a project with no midi_input node ran the legacy
    # receives_midi migration, which wired a second MIDI source to the
    # synth and displaced the sequencer, so the render was silent.
    if platform.system() != "Darwin":
        pytest.skip("stock AU instruments are macOS-only")
    xml = '<PLUGIN name="AUMIDISynth" format="AudioUnit" file="AudioUnit:Synths/aumu,msyn,appl"/>'
    proj = tmp_path / "synth.json"
    out = tmp_path / "synth.wav"
    proj.write_text(json.dumps({
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 512,
        "duration_seconds": 1.0,
        "nodes": [
            {"id": "seq", "kind": "sequencer", **SEQ, "density": 1.0},
            {"id": "synth", "kind": "plugin", "name": "AUMIDISynth",
             "descriptor": base64.b64encode(xml.encode()).decode()},
            {"id": "out", "kind": "output", "channels": 2, "sink": str(out)},
        ],
        "edges": [{"src": "seq", "dst": "synth", "kind": "midi"},
                  {"src": "synth", "dst": "out"}],
    }))
    _run(f"--render-project={proj}")
    audio, _ = audio_io.read_audio(str(out), as_=np.ndarray)
    assert np.abs(audio).max() > 0.01


def test_start_live_fills_missing_plugin_probe(tmp_path):
    # Projects saved before plugin probe data was persisted have none, so
    # an instrument showed a false audio input port. Starting live fills
    # it in from the opened plugin.
    if platform.system() != "Darwin":
        pytest.skip("stock AU instruments are macOS-only")
    xml = '<PLUGIN name="AUMIDISynth" format="AudioUnit" file="AudioUnit:Synths/aumu,msyn,appl"/>'
    proj = tmp_path / "old.json"
    proj.write_text(json.dumps({
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 512,
        "duration_seconds": 0.1,
        "nodes": [
            {"id": "seq", "kind": "sequencer"},
            {"id": "synth", "kind": "plugin", "name": "AUMIDISynth",
             "descriptor": base64.b64encode(xml.encode()).decode()},
            {"id": "out", "kind": "output", "channels": 2,
             "sink": str(tmp_path / "old.wav")},
        ],
        "edges": [{"src": "seq", "dst": "synth", "kind": "midi"},
                  {"src": "synth", "dst": "out"}],
    }))
    _run(f"--live-selftest={proj}")
    nodes = {n["id"]: n for n in
             json.loads(proj.with_suffix(".probed.json").read_text())["nodes"]}
    assert nodes["synth"]["probe"] == {"in_channels": 0, "out_channels": 2,
                                       "accepts_midi": True, "produces_midi": False}
