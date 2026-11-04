"""When closing a non-live plugin editor saves state, and when it asks to Save.

``--editor-sync-selftest`` opens a real editor window on the MinihostTestFx
fixture and checks the decision the app makes on close, Save and quit:

- an untouched open leaves the project clean,
- a host-notified parameter change is an edit (written, Save prompt),
- an unreported state change on a plugin whose state encodes unstably is
  noise (written and autosaved, no Save prompt),
- the same change on a stable plugin is an edit.

Needs a display, like the plugin browser selftest (xvfb on headless Linux).
"""

from __future__ import annotations

import os
import platform
import subprocess

import pytest

from cli_helpers import find_test_plugin
from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")
requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX), reason="MinihostTestFx not built"
)
needs_display = pytest.mark.skipif(
    platform.system() == "Linux" and not os.environ.get("DISPLAY"),
    reason="editor sync selftest needs an X display (run under xvfb-run)",
)


@skip_if_no_desktop
@needs_display
@requires_fx
def test_editor_sync_selftest_passes(tmp_path):
    env = dict(os.environ, MINIHOST_DESKTOP_SETTINGS_DIR=str(tmp_path))
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--editor-sync-selftest={FX}"],
        capture_output=True,
        text=True,
        timeout=60,
        env=env,
    )
    assert res.returncode == 0, (
        f"editor sync selftest failed (returncode={res.returncode})\n"
        f"stdout:\n{res.stdout}\nstderr:\n{res.stderr}"
    )
    assert "editor-sync-selftest OK" in res.stderr


@skip_if_no_desktop
def test_editor_sync_selftest_empty_value_exits_2():
    """Malformed invocation is a usage error (exit 2)."""
    res = subprocess.run(
        [str(DESKTOP_BIN), "--editor-sync-selftest="],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert res.returncode == 2, (res.returncode, res.stderr)
