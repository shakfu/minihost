"""Shared helpers for the minihost_desktop headless tests.

Locating the desktop binary is platform-specific (the macOS build is an
``.app`` bundle; Linux/Windows are plain executables) and build-dir specific
(``build`` for the standard build, ``build-desktop-verify`` for an isolated
desktop build). To avoid ever testing a stale binary, the locator returns the
most-recently-modified candidate rather than the first match.
"""

from __future__ import annotations

import os
import platform
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent

# Build directories searched for the binary, in no particular order (the
# newest match wins). `build-desktop` is what `make desktop` produces;
# `build` is the standard build dir when configured with the desktop flags.
_BUILD_DIRS = ["build", "build-desktop"]


def _relative_binary_paths() -> list[str]:
    system = platform.system()
    if system == "Darwin":
        return [
            "projects/minihost_desktop/minihost_desktop.app/"
            "Contents/MacOS/minihost_desktop",
        ]
    if system == "Windows":
        return [
            "projects/minihost_desktop/Release/minihost_desktop.exe",
            "projects/minihost_desktop/minihost_desktop.exe",
        ]
    return ["projects/minihost_desktop/minihost_desktop"]  # Linux / other Unix


def find_desktop_bin() -> Path | None:
    """Locate the desktop binary, preferring the newest one built.

    Honors ``MINIHOST_DESKTOP_BIN``; otherwise searches the standard build
    directories for the platform-specific binary name and returns the
    most-recently-modified match (so a fresh isolated build shadows a stale
    one under ``build/``).
    """
    env = os.environ.get("MINIHOST_DESKTOP_BIN")
    if env:
        p = Path(env)
        return p if p.exists() else None

    candidates = [
        REPO_ROOT / bd / rel
        for bd in _BUILD_DIRS
        for rel in _relative_binary_paths()
    ]
    existing = [c for c in candidates if c.exists()]
    if not existing:
        return None
    return max(existing, key=lambda c: c.stat().st_mtime)


DESKTOP_BIN = find_desktop_bin()

skip_if_no_desktop = pytest.mark.skipif(
    DESKTOP_BIN is None,
    reason="desktop binary not built (set MINIHOST_DESKTOP_BIN to override)",
)
