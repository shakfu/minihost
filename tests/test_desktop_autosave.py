"""Autosave / crash-recovery coverage for the minihost_desktop binary.

The desktop app snapshots the working ProjectDocument to a sidecar
(``autosave.json`` + ``autosave.meta``) on a heartbeat timer so a plugin
crash costs at most a few seconds of unsaved canvas editing. On the next
launch a surviving sidecar (a clean exit deletes it) triggers a recovery
prompt.

The timer, the recovery dialog, and the dirty-flag wiring are GUI-thread
orchestration that can't be driven headlessly, but the load-bearing part --
the sidecar read/write/clear mechanics that could silently corrupt or lose
recovery data -- is exercised end-to-end through the binary's
``--autosave-selftest`` mode. It drives the production writeAutosaveSnapshot
/ parse / clearAutosave helpers against a hermetic settings dir and asserts:

- no sidecar exists before the first write,
- the snapshot creates both sidecar files,
- parsing the sidecar reproduces the document byte-for-byte,
- the meta file records the origin path exactly,
- clearAutosave removes both files.

Any broken invariant makes the binary exit non-zero.

Skipped when the desktop binary isn't built.
"""

from __future__ import annotations

import json
import os
import subprocess

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop


def _write_project(path):
    path.write_text(json.dumps({
        "minihost_project_version": 1,
        "sample_rate": 48000,
        "block_size": 512,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": "in.wav"},
            {"id": "out", "kind": "output", "channels": 2,
             "sink": "out.wav", "bit_depth": 24},
        ],
        "edges": [{"src": "in", "dst": "out"}],
    }))


@skip_if_no_desktop
def test_autosave_selftest_passes(tmp_path):
    """The sidecar lifecycle contract holds end-to-end."""
    proj = tmp_path / "p.json"
    _write_project(proj)
    settings_dir = tmp_path / "settings"
    settings_dir.mkdir()
    env = dict(os.environ, MINIHOST_DESKTOP_SETTINGS_DIR=str(settings_dir))
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--autosave-selftest={proj}"],
        capture_output=True, text=True, timeout=30, env=env,
    )
    assert res.returncode == 0, \
        f"autosave self-test failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    assert "autosave-selftest OK" in res.stderr
    # The self-test clears the sidecar as its final assertion; nothing must
    # be left behind for a subsequent launch to spuriously "recover".
    assert not (settings_dir / "autosave.json").exists()
    assert not (settings_dir / "autosave.meta").exists()


@skip_if_no_desktop
def test_autosave_selftest_empty_value_exits_2(tmp_path):
    """Malformed invocation is a usage error (exit 2)."""
    res = subprocess.run(
        [str(DESKTOP_BIN), "--autosave-selftest="],
        capture_output=True, text=True, timeout=30,
    )
    assert res.returncode == 2, (res.returncode, res.stderr)
