"""A test-plugin env var naming a missing path stops the run (conftest.py)."""

import os
import subprocess
import sys
from pathlib import Path

import pytest

TESTS = Path(__file__).parent


@pytest.mark.parametrize(
    "var", ["MINIHOST_TEST_PLUGIN", "MINIHOST_TEST_PLUGIN_FX"]
)
def test_missing_plugin_path_is_a_usage_error(var):
    env = {**os.environ, var: "/nonexistent/Stale.vst3"}
    r = subprocess.run(
        [sys.executable, "-m", "pytest", "-q", "-p", "no:cacheprovider",
         str(TESTS / "test_api_version.py")],
        env=env, capture_output=True, text=True, timeout=120,
    )
    assert r.returncode == pytest.ExitCode.USAGE_ERROR, r.stdout + r.stderr
    assert f"{var}=/nonexistent/Stale.vst3" in r.stderr
