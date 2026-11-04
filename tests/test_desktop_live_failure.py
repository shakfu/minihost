"""A block that fails to render stops live mode without losing work.

``--live-failure-selftest=<path>`` starts live mode headlessly, records three
blocks, writes a plugin parameter, then makes the next block fail. The
selftest checks that the engine stops and outputs silence, reports the
failure once, and keeps its plugins so their state can be captured. Here the
test checks what reaches disk: the captured state and a finalized take that
holds the three blocks before the failure.
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
BLOCK = 256
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
                "block_size": BLOCK,
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
                ],
                "edges": [
                    {"src": "in", "dst": "fx"},
                    {"src": "fx", "dst": "out"},
                ],
            }
        )
    )
    return proj


@skip_if_no_desktop
@requires_fx
def test_render_failure_keeps_state_and_take(tmp_path):
    proj = _project(tmp_path)
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--live-failure-selftest={proj}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"

    take, sr = audio_io.read_audio(str(tmp_path / "out.wav"), as_=np.ndarray)
    assert sr == SR
    assert take.shape == (2, 3 * BLOCK)
    assert np.any(take != 0.0)

    captured = json.loads(proj.with_suffix(".captured.json").read_text())
    (fx,) = [n for n in captured["nodes"] if n["id"] == "fx"]
    p = minihost.Plugin(FX)
    p.set_state(base64.b64decode(fx["state_b64"]))
    assert p.get_param(GAIN) == pytest.approx(0.25, abs=1e-6)
