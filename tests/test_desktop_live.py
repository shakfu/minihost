"""Desktop live mode with file inputs and recorded file outputs.

Drives `minihost_desktop --live-selftest=<path>`, which runs the
LiveEngine callback without an audio device: two transport takes of the
project length with a Stop between them, or one looped take. Live mode
used to feed file inputs silence and never write file outputs.

Also covers `--render-tail`, which extends an offline render past the
input length.
"""

from __future__ import annotations

import json
import subprocess

import numpy as np

import minihost
from minihost import audio_io

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

SR = 48000
FRAMES = 12000  # not a multiple of the 300-frame callback or 256 block


def _project(tmp_path, src):
    in_wav = tmp_path / "in.wav"
    audio_io.write_audio(str(in_wav), src, SR, bit_depth=32)
    out_wav = tmp_path / "out.wav"
    proj = tmp_path / "p.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": SR,
                "block_size": 256,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(out_wav),
                        "bit_depth": 32,
                    },
                    {"id": "spk", "kind": "device_output", "channels": 2},
                ],
                "edges": [{"src": "in", "dst": "out"}, {"src": "in", "dst": "spk"}],
            }
        )
    )
    return proj, out_wav


def _source():
    rng = np.random.default_rng(7)
    return (rng.standard_normal((2, FRAMES)) * 0.1).astype(np.float32)


def _run(*args):
    res = subprocess.run(
        [str(DESKTOP_BIN), *args], capture_output=True, text=True, timeout=60
    )
    assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"


@skip_if_no_desktop
def test_live_plays_file_inputs_and_records_outputs(tmp_path):
    src = _source()
    proj, out_wav = _project(tmp_path, src)
    _run(f"--live-selftest={proj}")

    # Device output: the file, twice. The second take starts from 0
    # because Stop rewinds.
    device, _ = audio_io.read_audio(
        str(proj.with_suffix(".device.wav")), as_=np.ndarray
    )
    assert device.shape == (2, 2 * FRAMES)
    np.testing.assert_array_equal(device[:, :FRAMES], src)
    np.testing.assert_array_equal(device[:, FRAMES:], src)

    # Recording: Play overwrites the sink, so it holds the second take.
    rec, _ = audio_io.read_audio(str(out_wav), as_=np.ndarray)
    assert rec.shape == src.shape
    np.testing.assert_array_equal(rec, src)


@skip_if_no_desktop
def test_live_loop_repeats_file_inputs(tmp_path):
    src = _source()
    proj, out_wav = _project(tmp_path, src)
    _run(f"--live-selftest={proj}", "--live-loop")

    # One take of 2.5 file lengths: the file wraps twice, and the
    # recording grows with it.
    want = np.tile(src, 3)[:, : FRAMES * 5 // 2]
    device, _ = audio_io.read_audio(
        str(proj.with_suffix(".device.wav")), as_=np.ndarray
    )
    np.testing.assert_array_equal(device, want)
    rec, _ = audio_io.read_audio(str(out_wav), as_=np.ndarray)
    np.testing.assert_array_equal(rec, want)


@skip_if_no_desktop
def test_render_tail_extends_the_render(tmp_path):
    src = _source()
    proj, out_wav = _project(tmp_path, src)
    _run(f"--render-project={proj}", "--render-tail=0.5")

    out, _ = audio_io.read_audio(str(out_wav), as_=np.ndarray)
    assert out.shape == (2, FRAMES + SR // 2)
    np.testing.assert_array_equal(out[:, :FRAMES], src)
    assert not out[:, FRAMES:].any()


# ------------------------------------------------------------- MIDI file --
#
# midi_input -> midi_output, read back from <project>.midi.txt. The
# self-test renders in 300-frame callbacks, plus one stopped callback
# after each take, which carries the Stop note-offs.

BEAT = SR // 2  # 120 BPM
CALLBACK = 300
# (beat, on/off, note). Note 67 is held from beat 1.75 to beat 3.
NOTES = [
    (0, "on", 60),
    (0.5, "off", 60),
    (1, "on", 64),
    (1.5, "off", 64),
    (1.75, "on", 67),
    (3, "off", 67),
]


def _midi_project(tmp_path, duration):
    mf = minihost.MidiFile()
    tpq = mf.ticks_per_quarter
    mf.add_tempo(0, 0, 120.0)
    for beat, kind, note in NOTES:
        tick = int(beat * tpq)
        if kind == "on":
            mf.add_note_on(0, tick, 0, note, 100)
        else:
            mf.add_note_off(0, tick, 0, note, 0)
    mid = tmp_path / "notes.mid"
    mf.save(str(mid))
    # LiveEngine needs an audio output; a silent passthrough supplies one.
    silent = tmp_path / "silent.wav"
    audio_io.write_audio(str(silent), np.zeros((2, 4800), np.float32), SR, bit_depth=24)
    proj = tmp_path / "m.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": SR,
                "block_size": 512,
                "duration_seconds": duration,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": str(silent)},
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                        "bit_depth": 24,
                    },
                    {"id": "mi", "kind": "midi_input", "source": str(mid)},
                    {"id": "mo", "kind": "midi_output"},
                ],
                "edges": [
                    {"src": "in", "dst": "out"},
                    {"src": "mi", "dst": "mo", "kind": "midi"},
                ],
            }
        )
    )
    return proj


def _midi_log(proj):
    events = []
    for line in proj.with_suffix(".midi.txt").read_text().splitlines():
        frame, status, d1, d2 = (int(x) for x in line.split())
        on = status & 0xF0 == 0x90 and d2 > 0
        events.append((frame, "on" if on else "off", d1))
    return events


def _file_events(base, start, end):
    """File events with frames in [start, end), shifted to base."""
    out = []
    for beat, kind, note in NOTES:
        f = int(beat * BEAT)
        if start <= f < end:
            out.append((base + f - start, kind, note))
    return out


@skip_if_no_desktop
def test_live_plays_midi_file_and_releases_held_notes_on_stop(tmp_path):
    proj = _midi_project(tmp_path, 1.25)
    take = int(1.25 * SR)  # ends while note 67 is held
    _run(f"--live-selftest={proj}")

    want = []
    for base in (0, take + CALLBACK):
        want += _file_events(base, 0, take)
        want.append((base + take, "off", 67))  # Stop releases it
    assert _midi_log(proj) == want


@skip_if_no_desktop
def test_live_loops_midi_file_and_releases_held_notes_at_the_wrap(tmp_path):
    loop = int(1.01 * SR)  # wraps mid-callback; note 67 held at the end
    proj = _midi_project(tmp_path, 1.01)
    _run(f"--live-selftest={proj}", "--live-loop")

    total = loop * 5 // 2
    want = []
    for k in range(3):
        want += _file_events(k * loop, 0, min(loop, total - k * loop))
        if k < 2:
            want.append(((k + 1) * loop, "off", 67))
    want.append((total, "off", 64))  # held at Stop
    assert _midi_log(proj) == want


@skip_if_no_desktop
def test_live_midi_loop_wrap_on_a_callback_boundary(tmp_path):
    # 48000 frames is a whole number of 300-frame callbacks, so the wrap
    # falls between chunks. The release lands on the last sample before it.
    proj = _midi_project(tmp_path, 1.0)
    loop = SR
    _run(f"--live-selftest={proj}", "--live-loop")

    total = loop * 5 // 2
    want = []
    for k in range(3):
        want += _file_events(k * loop, 0, min(loop, total - k * loop))
        if k < 2:
            want.append(((k + 1) * loop - 1, "off", 67))
    assert _midi_log(proj) == want


@skip_if_no_desktop
def test_live_recording_longer_than_the_fifo_is_complete(tmp_path):
    # The recorder buffers 2 s. The self-test runs faster than real time,
    # so a 4 s take used to overflow it and lose most of the recording.
    src = (np.random.default_rng(9).standard_normal((2, 4 * SR)) * 0.1).astype(
        np.float32
    )
    proj, out_wav = _project(tmp_path, src)
    _run(f"--live-selftest={proj}")
    rec, _ = audio_io.read_audio(str(out_wav), as_=np.ndarray)
    assert rec.shape == src.shape
    np.testing.assert_array_equal(rec, src)
