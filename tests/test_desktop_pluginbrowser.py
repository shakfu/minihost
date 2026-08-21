"""Plugin Browser dialog lifetime coverage for the minihost_desktop binary.

The Plugin Browser is launched with ``DialogWindow::LaunchOptions::
launchAsync()``, which enters the modal state with ``deleteWhenDismissed =
true``. The window therefore frees *itself* the moment it is dismissed -- via
the close button or Escape -- and any owning pointer the app kept is left
dangling. Holding it in a ``std::unique_ptr`` meant the next "open the plugin
browser" dereferenced freed memory (segfault) and shutdown double-freed it.

Clicking through the dialog cannot be automated here, but the load-bearing
part -- the window's ownership and lifetime across open / dismiss / reopen --
is driven end-to-end through the binary's ``--plugin-browser-selftest`` mode.
It asserts that opening creates a window, that re-requesting while open reuses
it rather than stacking a second one, that the tracking pointer nulls itself
once the window is dismissed, and that reopening afterwards yields a live,
fully-formed window. Any broken invariant makes the binary exit non-zero.

Note it deliberately does not compare window addresses across a dismissal: the
old window is freed first, so the allocator may legitimately return the same
block, and address identity would prove nothing either way.

Unlike the other desktop selftests this one creates real windows and so needs a
window server rather than merely tolerating one; on headless Linux run it under
``xvfb-run``. Skipped when the desktop binary isn't built, or when no display is
available. CI runs it on Linux only, where xvfb gives a dependable server -- it
is fine to run locally on any platform with a display.
"""

from __future__ import annotations

import os
import platform
import subprocess

import pytest

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

# The selftest maps a real window. On Linux that needs an X server (xvfb is
# enough); macOS and Windows always have a window server available.
needs_display = pytest.mark.skipif(
    platform.system() == "Linux" and not os.environ.get("DISPLAY"),
    reason="plugin browser selftest needs an X display (run under xvfb-run)",
)


@skip_if_no_desktop
@needs_display
def test_plugin_browser_open_dismiss_reopen(tmp_path):
    """Open, dismiss and reopen the browser without dangling or double-free.

    Regression test: reopening after a dismissal used to segfault.
    """
    env = dict(os.environ)
    # Hermetic settings dir -- the mode reads the scan-path/deadman sidecars
    # next to it and must not touch the developer's real plugin library.
    env["MINIHOST_DESKTOP_SETTINGS_DIR"] = str(tmp_path)

    res = subprocess.run(
        [str(DESKTOP_BIN), "--plugin-browser-selftest"],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
    )

    # A crash shows up as a negative/`>128` return code rather than a clean
    # failure exit, so report it distinctly -- that is the original bug.
    assert res.returncode == 0, (
        f"plugin browser selftest failed (returncode={res.returncode})\n"
        f"stdout:\n{res.stdout}\nstderr:\n{res.stderr}"
    )
    assert "plugin-browser-selftest OK" in res.stderr


@skip_if_no_desktop
@needs_display
def test_plugin_browser_selftest_leaves_library_untouched(tmp_path):
    """The selftest must never write the scanned-plugin library.

    It runs outside the GUI app-shell, so ``owns_library_`` stays false and
    shutdown must not persist an empty ``known_plugins.xml`` over a real one.
    """
    env = dict(os.environ)
    env["MINIHOST_DESKTOP_SETTINGS_DIR"] = str(tmp_path)

    subprocess.run(
        [str(DESKTOP_BIN), "--plugin-browser-selftest"],
        capture_output=True,
        text=True,
        timeout=120,
        env=env,
        check=True,
    )

    assert not (tmp_path / "known_plugins.xml").exists(), (
        "selftest wrote known_plugins.xml; it must not persist the library"
    )
