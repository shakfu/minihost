"""Edits made on a running plugin reach the project document.

Drives `minihost_desktop --live-state-selftest=<path>`: live mode starts
headlessly, a parameter of the running plugin is written, and
`captureLiveState` copies the instance's state into the document, which is
saved as `<project>.captured.json`. The desktop app runs the same capture on
Save, Stop Live and Restart Live, so a knob moved in a live editor window is
kept without a manual Capture State.
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
GAIN = 0  # MinihostTestFx parameter index (TestPluginProcessor.h)


def _project(tmp_path):
    src = np.full((2, 4800), 0.25, np.float32)
    audio_io.write_audio(str(tmp_path / "in.wav"), src, SR, bit_depth=32)
    proj = tmp_path / "p.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": SR,
                "block_size": 256,
                "nodes": [
                    {
                        "id": "in",
                        "kind": "input",
                        "channels": 2,
                        "source": str(tmp_path / "in.wav"),
                    },
                    {"id": "fx", "kind": "plugin", "path": FX},
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / "out.wav"),
                        "bit_depth": 32,
                    },
                    {"id": "spk", "kind": "device_output", "channels": 2},
                ],
                "edges": [
                    {"src": "in", "dst": "fx"},
                    {"src": "fx", "dst": "out"},
                    {"src": "fx", "dst": "spk"},
                ],
            }
        )
    )
    return proj


@skip_if_no_desktop
@requires_fx
def test_live_parameter_edit_is_captured_into_the_document(tmp_path):
    proj = _project(tmp_path)
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--live-state-selftest={proj}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"

    captured = json.loads(proj.with_suffix(".captured.json").read_text())
    (fx,) = [n for n in captured["nodes"] if n["id"] == "fx"]
    state = base64.b64decode(fx["state_b64"])

    # The state restores the written value on a fresh instance.
    p = minihost.Plugin(FX)
    p.set_state(state)
    assert p.get_param(GAIN) == pytest.approx(0.25, abs=1e-6)
