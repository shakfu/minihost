"""Desktop project loading must agree with the Python loader.

Each case was a silent divergence: the desktop app loaded or rendered the
project differently from `minihost.load_project` and reported nothing.

  * A relative plugin path resolved against the working directory, not the
    project file's directory.
  * A plugin state the plugin refused was ignored, so the render used its
    default parameters.
  * MIDI-file events on the same tick were sorted unstably, so their order
    could differ from Python's.
"""

from __future__ import annotations

import base64
import json
import os
import subprocess

import numpy as np
import pytest

import minihost
from minihost import audio_io

from cli_helpers import find_test_plugin
from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")
requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX), reason="MinihostTestFx not built"
)

SR = 48000


def _run(*args, cwd=None):
    return subprocess.run(
        [str(DESKTOP_BIN), *args],
        capture_output=True,
        text=True,
        timeout=60,
        cwd=cwd,
    )


def _fx_project(tmp_path, plugin_path, **plugin_fields):
    src = np.full((2, 4800), 0.25, np.float32)
    audio_io.write_audio(str(tmp_path / "in.wav"), src, SR, bit_depth=32)
    proj = tmp_path / "p.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": SR,
                "block_size": 512,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": "in.wav"},
                    {
                        "id": "fx",
                        "kind": "plugin",
                        "path": plugin_path,
                        **plugin_fields,
                    },
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": "out.wav",
                        "bit_depth": 32,
                    },
                ],
                "edges": [{"src": "in", "dst": "fx"}, {"src": "fx", "dst": "out"}],
            }
        )
    )
    return proj, src


@skip_if_no_desktop
@requires_fx
def test_relative_plugin_path_resolves_against_the_project(tmp_path):
    rel = os.path.relpath(FX, tmp_path)
    proj, src = _fx_project(tmp_path, rel)
    minihost.load_project(proj)  # Python accepts it

    # Run from a directory deep enough that the path's ".." steps cannot
    # reach "/", where any start directory resolves alike.
    cwd = tmp_path.joinpath(*"abcdefghij")
    cwd.mkdir(parents=True)
    res = _run(f"--render-project={proj}", cwd=cwd)
    assert res.returncode == 0, res.stderr
    out, _ = audio_io.read_audio(str(tmp_path / "out.wav"), as_=np.ndarray)
    np.testing.assert_array_equal(out, src)  # the fixture's default is unity


@skip_if_no_desktop
@requires_fx
def test_rejected_plugin_state_fails_the_render(tmp_path):
    junk = base64.b64encode(b"not a state for this plugin").decode()
    proj, _ = _fx_project(tmp_path, FX, state_b64=junk)
    with pytest.raises(minihost.ProjectError, match="state rejected"):
        minihost.load_project(proj)

    res = _run(f"--render-project={proj}")
    assert res.returncode != 0
    assert "state rejected" in res.stderr + res.stdout
    assert not (tmp_path / "out.wav").exists()


@skip_if_no_desktop
def test_same_tick_midi_events_keep_python_order(tmp_path):
    # Two tracks of note-ons and note-offs, many on one tick: more than the
    # 16 elements std::sort leaves to its stable insertion pass.
    mf = minihost.MidiFile()
    mf.add_tempo(0, 0, 120.0)
    mf.add_track()
    for track in (0, 1):
        for note in range(40, 60):
            mf.add_note_on(track, 0, track, note, 100)
            mf.add_note_off(track, 0, track, note + 30, 0)
    mid = tmp_path / "chord.mid"
    mf.save(str(mid))

    silent = tmp_path / "silent.wav"
    audio_io.write_audio(str(silent), np.zeros((2, 4800), np.float32), SR, bit_depth=24)
    proj = tmp_path / "m.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": SR,
                "block_size": 512,
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
    res = _run(f"--render-project={proj}", "--render-midi-log")
    assert res.returncode == 0, res.stderr

    got = [
        tuple(int(x) for x in line.split())
        for line in proj.with_suffix(".render-midi.txt").read_text().splitlines()
    ]
    want = [tuple(e) for e in minihost.midi_file_to_events(str(mid), SR)]
    assert len(want) == 80
    assert got == want
