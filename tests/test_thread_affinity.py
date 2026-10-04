"""Every control op reaches the plugin on minihost's plugin thread.

JUCE-hosted VST3/AU instances are thread-affine (see runOnMsg in
minihost.cpp). The FX fixture logs any control callback that runs off the
thread that constructed it, when MINIHOST_TEST_AFFINITY_LOG is set. A child
process is used because the fixture reads the variable once per process.
"""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap

import pytest

from cli_helpers import find_test_plugin

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")

pytestmark = pytest.mark.skipif(
    not FX,
    reason="MinihostTestFx not built; set MINIHOST_TEST_PLUGIN_FX or "
    "build with -DMINIHOST_BUILD_TEST_PLUGIN=ON",
)

# Each line runs from the Python main thread, which is never the plugin
# thread. Grouped by the C entry point they reach.
CONTROL_OPS = textwrap.dedent(
    """
    import minihost
    p = minihost.Plugin(FX, sample_rate=48000, max_block_size=256,
                        sidechain_channels=2)
    for i in range(p.num_params):
        p.get_param_info(i)
        p.param_to_text(i, 0.5)
        p.param_from_text(i, "0.5")
        p.set_param(i, p.get_param(i))
        p.begin_param_gesture(i)
        p.end_param_gesture(i)
    p.find_param("Gain")
    p.num_programs
    p.program
    if p.num_programs:  # the VST3 wrapper exposes none for one program
        p.program = 0
        p.get_program_name(0)
    ps = p.get_program_state()
    if ps:
        p.set_program_state(ps)
    p.set_state(p.get_state())
    p.latency_samples
    p.tail_seconds
    p.supports_double
    p.processing_precision
    p.non_realtime = True
    p.non_realtime = False
    p.bypass = p.bypass
    p.num_input_buses, p.num_output_buses
    p.get_bus_info(True, 0)
    p.check_buses_layout([2, 2], [2])
    p.set_track_properties(name="t", colour=0xFF00FF00)
    p.set_transport(120.0)
    p.clear_transport()
    p.morph_apply(p.morph_capture())
    import numpy as np
    buf = np.zeros((2, 256))
    p.process_double(buf, buf.copy())
    p.reset()
    p.sample_rate = 44100.0
    p.poll_callbacks()
    p.close()
    """
)


def test_control_ops_run_on_the_plugin_thread(tmp_path):
    log = tmp_path / "affinity.log"
    env = {**os.environ, "MINIHOST_TEST_AFFINITY_LOG": str(log)}
    script = f"FX = {FX!r}\n" + CONTROL_OPS
    r = subprocess.run(
        [sys.executable, "-c", script],
        env=env,
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert r.returncode == 0, r.stderr
    off_thread = sorted(set(log.read_text().split())) if log.exists() else []
    assert off_thread == [], f"called off the plugin thread: {off_thread}"
