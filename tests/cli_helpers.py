"""Shared location logic for the native CLI binaries.

``minihost_c`` and ``minihost_cpp`` are only present in a standalone CMake
build tree, and where they land depends on the generator: single-config
generators put them at ``build/projects/<name>/<name>``, while the Visual
Studio generator adds a per-config directory and an ``.exe`` suffix. Several
test modules need to find them, so the search lives here rather than being
copied per module and drifting -- a copy that knows only the Unix layout
silently *skips* on Windows rather than failing, which is the worst outcome
for a test whose whole job is to catch drift.
"""

from __future__ import annotations

import os
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent


def find_cli_binary(name: str, env_var: str) -> str | None:
    """Locate a CLI binary via env var or common build locations.

    Among the build-directory candidates the most recently built one wins,
    so a fresh standalone build (e.g. ``build-cli/``) is preferred over a
    stale artifact left in ``build/`` from an earlier configuration.
    """
    env = os.environ.get(env_var)
    if env and os.path.exists(env):
        return env
    candidates = [
        _REPO_ROOT / "build" / "projects" / name / name,
        _REPO_ROOT / "build" / "projects" / name / "Release" / f"{name}.exe",
        _REPO_ROOT / "build-cli" / "projects" / name / name,
        _REPO_ROOT / "build-cli" / "projects" / name / "Release" / f"{name}.exe",
        _REPO_ROOT / "build-desktop" / "projects" / name / name,
    ]
    existing = [c for c in candidates if c.exists()]
    if not existing:
        return None
    return str(max(existing, key=lambda p: p.stat().st_mtime))
