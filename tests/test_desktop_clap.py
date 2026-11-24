"""CLAP in the desktop app: library scan, offline render, editor windows.

The scan and render tests use the CLAP fixtures (see test_clap.py). The
editor test needs a CLAP plugin with a GUI, which the fixtures lack; it uses
``MINIHOST_TEST_CLAP_EDITOR`` or a TAL-NoiseMaker install, and skips
otherwise. Skipped when the desktop binary is not built.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

import minihost
from minihost import audio_io

from cli_helpers import find_test_clap
from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

np = pytest.importorskip("numpy")

FX = find_test_clap("MinihostTestFx", "MINIHOST_TEST_CLAP_FX")
SYNTH = find_test_clap("MinihostTestSynth", "MINIHOST_TEST_CLAP_SYNTH")

requires_fixtures = pytest.mark.skipif(
    not (FX and SYNTH and os.path.exists(FX) and os.path.exists(SYNTH)),
    reason="CLAP fixtures not built (make fixtures)",
)


def _copy_plugin(src: str, dst_dir: Path) -> None:
    src_path = Path(src)
    if src_path.is_dir():
        shutil.copytree(src_path, dst_dir / src_path.name)
    else:
        shutil.copy2(src_path, dst_dir / src_path.name)


@skip_if_no_desktop
@requires_fixtures
@pytest.mark.parametrize("out_of_process", [False, True])
def test_library_scan_lists_clap_plugins(tmp_path, out_of_process):
    scan_dir = tmp_path / "plugins"
    scan_dir.mkdir()
    _copy_plugin(FX, scan_dir)
    _copy_plugin(SYNTH, scan_dir)
    out_xml = tmp_path / "known.xml"

    argv = [
        str(DESKTOP_BIN),
        f"--scan-plugins={scan_dir}",
        "--scan-format=CLAP",
        f"--scan-out={out_xml}",
    ]
    if out_of_process:
        argv.append("--scan-oop")
    res = subprocess.run(argv, capture_output=True, text=True, timeout=120)
    assert res.returncode == 0, res.stderr

    found = {
        p.get("name"): p for p in ET.parse(out_xml).getroot().findall("PLUGIN")
    }
    assert set(found) == {"MinihostTestFx", "MinihostTestSynth"}, res.stderr
    assert all(p.get("format") == "CLAP" for p in found.values())
    assert found["MinihostTestSynth"].get("isInstrument") == "1"
    assert found["MinihostTestFx"].get("isInstrument") == "0"
    assert (found["MinihostTestFx"].get("numInputs"),
            found["MinihostTestFx"].get("numOutputs")) == ("2", "2")


@skip_if_no_desktop
@requires_fixtures
def test_desktop_renders_a_clap_synth_like_python(tmp_path):
    mf = minihost.MidiFile()
    t = mf.add_track()
    mf.add_tempo(t, 0, 120.0)
    mf.add_note_on(t, 0, 0, 69, 100)
    mf.add_note_off(t, 960, 0, 69, 0)
    mid = tmp_path / "note.mid"
    assert mf.save(str(mid))

    def project(sink: Path) -> Path:
        doc = {
            "minihost_project_version": 1,
            "sample_rate": 48000,
            "block_size": 512,
            "duration_seconds": 1.5,
            "nodes": [
                {"id": "midi", "kind": "midi_input", "source": str(mid)},
                {"id": "synth", "kind": "plugin", "path": SYNTH},
                {"id": "out", "kind": "output", "channels": 2,
                 "sink": str(sink), "bit_depth": 24},
            ],
            "edges": [
                {"kind": "midi", "src": "midi", "dst": "synth"},
                {"src": "synth", "dst": "out"},
            ],
        }
        path = sink.with_suffix(".json")
        path.write_text(json.dumps(doc))
        return path

    out_cpp, out_py = tmp_path / "cpp.wav", tmp_path / "py.wav"
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--render-project={project(out_cpp)}"],
        capture_output=True, text=True, timeout=120,
    )
    assert res.returncode == 0, res.stderr
    minihost.render_project(project(out_py))

    a, _ = audio_io.read_audio(str(out_cpp), as_=np.ndarray)
    b, _ = audio_io.read_audio(str(out_py), as_=np.ndarray)
    assert float(np.max(np.abs(a))) > 0.1, f"desktop render is silent:\n{res.stderr}"
    n = min(a.shape[1], b.shape[1])
    assert float(np.max(np.abs(a[:, :n] - b[:, :n]))) == 0.0


def _editor_plugin() -> str | None:
    env = os.environ.get("MINIHOST_TEST_CLAP_EDITOR")
    if env:
        return env
    if sys.platform == "darwin":
        default = "/Library/Audio/Plug-Ins/CLAP/TAL-NoiseMaker.clap"
        if os.path.exists(default):
            return default
    return None


@skip_if_no_desktop
@pytest.mark.skipif(sys.platform != "darwin", reason="CLAP editors are embedded on macOS only")
def test_clap_editor_window_opens_and_closes(tmp_path):
    plugin = _editor_plugin()
    if plugin is None:
        pytest.skip("no CLAP plugin with a GUI; set MINIHOST_TEST_CLAP_EDITOR")
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--editor-sync-selftest={plugin}"],
        capture_output=True, text=True, timeout=120,
        env=dict(os.environ, MINIHOST_DESKTOP_SETTINGS_DIR=str(tmp_path)),
    )
    assert res.returncode == 0, res.stderr
    assert "editor-sync-selftest: editor=plugin" in res.stderr
    assert "editor-sync-selftest OK" in res.stderr
