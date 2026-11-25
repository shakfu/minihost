"""A CLAP plugin that hangs stalls itself, not every other plugin.

The CLAP fixture stalls on request (see projects/test_plugin/clap): CC 119
sleeps inside process() after asking for a restart, CC 118 makes
on_main_thread sleep, and MINIHOST_TEST_CLAP_HANG_INIT makes clap_entry.init
sleep. Each test checks that an unrelated call finishes well inside the
stall. Before the fixes each of them waited the stall out.

The lock-scope tests run in a subprocess with the plugin thread disabled:
only then do two threads service or probe CLAP plugins concurrently.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import threading
import time

import pytest

import minihost

from cli_helpers import find_test_clap

np = pytest.importorskip("numpy")

FX = find_test_clap("MinihostTestFx", "MINIHOST_TEST_CLAP_FX")
SYNTH = find_test_clap("MinihostTestSynth", "MINIHOST_TEST_CLAP_SYNTH")

requires_fixtures = pytest.mark.skipif(
    not (FX and SYNTH and os.path.exists(FX) and os.path.exists(SYNTH)),
    reason="CLAP fixtures not built (make fixtures)",
)

SR = 48000.0
BLOCK = 512
STALL_S = 1.0
STALL_CC = int(STALL_S * 100)  # the fixture sleeps value * 10 ms
FAST_S = 0.5  # an unrelated call must finish inside this
P_LATENCY, LATENCY_MAX = 1, 4096


def _open(path):
    return minihost.Plugin(path, sample_rate=SR, max_block_size=BLOCK)


@requires_fixtures
def test_a_stuck_process_does_not_stall_other_plugins():
    # The stuck block asks for a restart. The plugin thread used to wait for
    # the audio token to do it, and every control call queued behind it.
    stuck, other = _open(FX), _open(FX)
    try:
        chain = minihost.PluginChain([stuck])
        stuck.set_param(P_LATENCY, 64 / LATENCY_MAX)  # restart also re-reads this
        src = np.zeros((2, BLOCK), dtype=np.float32)
        worker = threading.Thread(
            target=chain.process_midi,
            args=(src, np.zeros_like(src), [(0, 0xB0, 119, STALL_CC)]),
        )
        worker.start()
        time.sleep(0.2)

        t0 = time.monotonic()
        other.get_param_info(0)
        other.get_state()
        elapsed = time.monotonic() - t0
        worker.join()

        assert elapsed < FAST_S, (
            f"control call waited {elapsed:.2f}s for another plugin"
        )

        # The deferred restart still happens once process() returns.
        deadline = time.monotonic() + 5.0
        while stuck.latency_samples != 64 and time.monotonic() < deadline:
            stuck.poll_callbacks()
            time.sleep(0.01)
        assert stuck.latency_samples == 64
    finally:
        stuck.close()
        other.close()


_CALLBACK_HANG = r"""
import json, sys, threading, time
import numpy as np
import minihost

fx, cc, stall = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
plugin = minihost.Plugin(fx, sample_rate=48000.0, max_block_size=512)
src = np.zeros((2, 512), dtype=np.float32)
plugin.process_midi(src, np.zeros_like(src), [(0, 0xB0, 118, cc)])

result = {}
def probe():
    time.sleep(0.1)
    t0 = time.monotonic()
    minihost.probe(fx)
    result["probe_s"] = time.monotonic() - t0
t = threading.Thread(target=probe)
t.start()
t0 = time.monotonic()
plugin.poll_callbacks()          # services on this thread: on_main_thread sleeps
result["service_s"] = time.monotonic() - t0
t.join()
plugin.close()
print(json.dumps(result))
"""


_INIT_HANG = r"""
import json, sys, threading, time
import minihost

synth, fx = sys.argv[1], sys.argv[2]
result = {}
def slow():
    t0 = time.monotonic()
    minihost.probe(synth)        # its clap_entry.init sleeps
    result["slow_s"] = time.monotonic() - t0
t = threading.Thread(target=slow)
t.start()
time.sleep(0.1)
t0 = time.monotonic()
minihost.probe(fx)               # a different module
result["fast_s"] = time.monotonic() - t0
t.join()
print(json.dumps(result))
"""


def _run(script, *args, extra_env=None):
    env = dict(os.environ, MINIHOST_MESSAGE_THREAD="0", **(extra_env or {}))
    res = subprocess.run(
        [sys.executable, "-c", script, *map(str, args)],
        capture_output=True,
        text=True,
        timeout=60,
        env=env,
    )
    assert res.returncode == 0, res.stderr
    return json.loads(res.stdout.strip().splitlines()[-1])


@requires_fixtures
def test_a_hung_main_thread_callback_does_not_block_other_threads():
    out = _run(_CALLBACK_HANG, FX, STALL_CC, STALL_S)
    assert out["service_s"] >= STALL_S * 0.9, out  # the stall really happened
    assert out["probe_s"] < FAST_S, out


@requires_fixtures
def test_a_hung_module_init_does_not_block_other_modules():
    out = _run(
        _INIT_HANG,
        SYNTH,
        FX,
        extra_env={
            "MINIHOST_TEST_CLAP_HANG_INIT": f"MinihostTestSynth:{int(STALL_S * 1000)}"
        },
    )
    assert out["slow_s"] >= STALL_S * 0.9, out
    assert out["fast_s"] < FAST_S, out
