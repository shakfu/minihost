"""Supervised scanning: one bad plugin costs one entry, not the scan.

Probing means instantiating, so a plugin that spins forever or corrupts
its heap on load takes an in-process scan down with it -- five of ~350
installed on the development machine do exactly that. The supervisor
probes each plugin in a child process it is willing to lose.

The tests do not need such a plugin, and could not rely on one being
installed anyway: the worker command is caller-supplied
(``MINIHOST_SCAN_WORKER``), so a fake worker can produce every outcome
on demand -- a good answer, a probe failure, a hang, a crash -- against
empty directories named ``*.vst3``, which is all the scanner's discovery
looks for. That makes the interesting cases deterministic and fast
rather than dependent on which plugins happen to be on the machine.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import pytest

from test_cli_conformance import C_BIN, CPP_BIN, PLUGIN  # same dir

BINARIES = [b for b in (C_BIN, CPP_BIN) if b]

skip_if_no_binaries = pytest.mark.skipif(not BINARIES, reason="no CLI binaries built")

# Each fake plugin's name selects the behaviour the fake worker acts out.
WORKER_SRC = """
import os, sys, time
path = sys.argv[sys.argv.index("--mh-probe-one") + 1]
name = os.path.basename(path)
if name.startswith("hang"):
    time.sleep(600)
elif name.startswith("crash"):
    os.abort()
elif name.startswith("bad"):
    body = '{"ok": false, "error": "fake probe failure"}'
else:
    body = ('{"ok": true, "name": "Fake ' + name + '", "vendor": "Test",'
            ' "version": "1.0", "format": "VST3", "unique_id": "DEADBEEF",'
            ' "num_inputs": 2, "num_outputs": 2,'
            ' "accepts_midi": false, "produces_midi": false}')
# Plugins print to stdout while loading; the markers are what make the
# answer findable in the middle of that, so the fake prints noise too.
print("loading, please wait...")
print("<<<MH_PROBE>>>" + body + "<<<MH_PROBE_END>>>")
"""

PLUGINS = ["good_one.vst3", "bad_one.vst3", "hang_one.vst3", "crash_one.vst3"]


@pytest.fixture
def scan_env(tmp_path: Path):
    """A fake plugin directory, a fake worker, and an isolated cache."""
    plugin_dir = tmp_path / "plugins"
    plugin_dir.mkdir()
    for name in PLUGINS:
        (plugin_dir / name).mkdir()  # discovery looks for *.vst3 directories

    worker = tmp_path / "fake_worker.py"
    worker.write_text(WORKER_SRC)

    env = dict(os.environ)
    env["MINIHOST_CACHE_DIR"] = str(tmp_path / "cache")
    env["MINIHOST_SCAN_WORKER"] = f"{sys.executable} {worker}"
    env["MINIHOST_SCAN_TIMEOUT_MS"] = "1500"
    return plugin_dir, env, tmp_path / "cache" / "plugins.json"


def _scan(binary: str, plugin_dir: Path, env: dict, *extra: str):
    return subprocess.run(
        [binary, "scan", str(plugin_dir), *extra],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )


def _statuses(cache_file: Path) -> dict[str, str]:
    entries = json.loads(cache_file.read_text())["entries"]
    return {Path(k).name: v["status"] for k, v in entries.items()}


@skip_if_no_binaries
@pytest.mark.parametrize("binary", BINARIES)
def test_scan_survives_a_hanging_and_a_crashing_plugin(binary, scan_env):
    """The scan completes, and every plugin is accounted for."""
    plugin_dir, env, cache_file = scan_env

    result = _scan(binary, plugin_dir, env)

    assert result.returncode == 0, result.stderr
    assert _statuses(cache_file) == {
        "good_one.vst3": "ok",
        "bad_one.vst3": "error",
        "hang_one.vst3": "timeout",
        "crash_one.vst3": "crash",
    }


@skip_if_no_binaries
def test_the_good_plugin_is_usable_after_the_bad_ones(scan_env):
    """A scan past a hang and a crash still yields a working entry."""
    plugin_dir, env, cache_file = scan_env

    _scan(BINARIES[0], plugin_dir, env)

    entries = json.loads(cache_file.read_text())["entries"]
    good = next(v for k, v in entries.items() if Path(k).name == "good_one.vst3")
    assert good["desc"]["name"] == "Fake good_one.vst3"
    assert good["desc"]["num_outputs"] == 2


@skip_if_no_binaries
def test_a_rescan_does_not_pay_for_the_bad_plugins_again(scan_env):
    """timeout and crash are remembered like any other outcome."""
    plugin_dir, env, cache_file = scan_env

    _scan(BINARIES[0], plugin_dir, env)
    first = _statuses(cache_file)

    # A second scan re-probes nothing: with a 1.5 s deadline, re-probing the
    # hang alone would take longer than this whole call is allowed.
    result = subprocess.run(
        [BINARIES[0], "scan", str(plugin_dir)],
        capture_output=True,
        text=True,
        timeout=10,
        env=env,
    )

    assert result.returncode == 0
    assert _statuses(cache_file) == first


@skip_if_no_binaries
def test_in_process_scan_still_available(scan_env):
    """--in-process bypasses the worker, so the fake one is not consulted."""
    plugin_dir, env, cache_file = scan_env

    result = _scan(BINARIES[0], plugin_dir, env, "--in-process")

    assert result.returncode == 0
    # Empty directories are not plugins, so probing them fails in-process --
    # the point is that the fake worker's answers are absent.
    assert set(_statuses(cache_file).values()) == {"error"}


@skip_if_no_binaries
@pytest.mark.parametrize("binary", BINARIES)
def test_worker_mode_probes_one_plugin_and_exits(binary, scan_env):
    """The binaries are their own worker, which is what makes the default work."""
    _, env, _ = scan_env
    env = {k: v for k, v in env.items() if k != "MINIHOST_SCAN_WORKER"}

    if not os.path.exists(PLUGIN):
        pytest.skip(f"test plugin not found at {PLUGIN}")

    result = subprocess.run(
        [binary, "--mh-probe-one", PLUGIN],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )

    assert result.returncode == 0
    assert "<<<MH_PROBE>>>" in result.stdout
    payload = result.stdout.split("<<<MH_PROBE>>>")[1].split("<<<MH_PROBE_END>>>")[0]
    assert json.loads(payload)["ok"] is True
