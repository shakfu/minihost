"""Native unit tests for C layers with no Python binding.

``make native-tests`` builds and runs the harnesses in ``tests/native/`` under
UBSan: ring-buffer constructor bounds (every in-tree caller passes a
compile-time constant, so there is nothing to drive from Python) and the MIDI
message-length table (asserting what reaches a port needs a real device). See
those files for what the cases cover.

Skipped when no C++ compiler is available (wheel-only test runs).
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]


def _compiler() -> str | None:
    for name in ("c++", "g++", "clang++"):
        if shutil.which(name):
            return name
    return None


@pytest.mark.skipif(shutil.which("make") is None, reason="make not available")
@pytest.mark.skipif(_compiler() is None, reason="no C++ compiler available")
def test_native_harnesses() -> None:
    result = subprocess.run(
        ["make", "native-tests"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        timeout=300,
    )
    assert result.returncode == 0, (
        f"native test harness failed:\n"
        f"--- stdout ---\n{result.stdout}\n--- stderr ---\n{result.stderr}"
    )
