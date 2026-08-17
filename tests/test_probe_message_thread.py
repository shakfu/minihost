"""Probing must not deadlock once the message thread is running.

``mh_probe`` reads a plugin's description, which for an AudioUnit means
instantiating it, which on macOS dispatches to the message thread and
waits. Our message thread services its own task queue and does not pump
JUCE's dispatch loop, so a probe issued from any other thread waited on
a dispatch nothing would ever run. Every other thread-affine entry point
goes through ``runOnMsg``; probing was the one that did not.

The trigger is the message thread being *running*, not the probe itself:
a probe with nothing loaded ran on the calling thread and was fine, which
is why ``minihost info <au> --probe`` worked while ``minihost info <au>``
hung -- the latter loads first (starting the thread), then probes.

Each case runs in a subprocess with a timeout, because the failure mode
is a hang: in-process it would wedge the whole suite rather than fail.
"""

from __future__ import annotations

import os
import platform
import subprocess
import sys

import pytest

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

# Generous: a first load of a large plugin is seconds, a deadlock is
# forever, so anything in between is not a distinction we need to make.
TIMEOUT = 120

_AU_DIRS = (
    "/Library/Audio/Plug-Ins/Components",
    os.path.expanduser("~/Library/Audio/Plug-Ins/Components"),
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)
skip_if_not_macos = pytest.mark.skipif(
    platform.system() != "Darwin", reason="AudioUnits are macOS-only"
)

_SCRIPT = """
import sys
import minihost
open_path, probe_path = sys.argv[1], sys.argv[2]
if open_path:
    # Loading anything starts the message thread; that is the precondition.
    plugin = minihost.Plugin(open_path, sample_rate=48000, max_block_size=512)
print("PROBED:" + minihost.probe(probe_path)["name"])
"""


def _probe_in_subprocess(open_path: str, probe_path: str, env: dict | None = None):
    """Return (timed_out, stdout). Never raises on plugin failure."""
    proc_env = dict(os.environ)
    if env:
        proc_env.update(env)
    try:
        out = subprocess.run(
            [sys.executable, "-c", _SCRIPT, open_path, probe_path],
            capture_output=True,
            text=True,
            timeout=TIMEOUT,
            env=proc_env,
        )
    except subprocess.TimeoutExpired:
        return True, ""
    return False, out.stdout


def _first_audiounit() -> str | None:
    for root in _AU_DIRS:
        if not os.path.isdir(root):
            continue
        for name in sorted(os.listdir(root)):
            if name.endswith(".component"):
                return os.path.join(root, name)
    return None


@skip_if_no_plugin
def test_probe_completes_while_a_plugin_is_open():
    """The general case, on every platform."""
    timed_out, out = _probe_in_subprocess(PLUGIN, PLUGIN)
    assert not timed_out, "probe deadlocked with a plugin open"
    assert "PROBED:" in out


@skip_if_not_macos
@skip_if_no_plugin
def test_probe_of_an_audiounit_completes_while_a_plugin_is_open():
    """The case that actually deadlocked: an AU probe, message thread up."""
    au = _first_audiounit()
    if au is None:
        pytest.skip("no AudioUnit bundles installed")

    # Establish that this particular AU probes at all, with the message
    # thread disabled so the code path under test is not involved. A
    # plugin that cannot be probed either way is not evidence of a
    # regression, so skip rather than fail.
    baseline_timed_out, baseline_out = _probe_in_subprocess(
        "", au, env={"MINIHOST_MESSAGE_THREAD": "0"}
    )
    if baseline_timed_out or "PROBED:" not in baseline_out:
        pytest.skip(f"{os.path.basename(au)} does not probe even single-threaded")

    timed_out, out = _probe_in_subprocess(PLUGIN, au)
    assert not timed_out, (
        f"probing {os.path.basename(au)} deadlocked with the message thread "
        f"running; it probes fine with MINIHOST_MESSAGE_THREAD=0"
    )
    assert "PROBED:" in out
