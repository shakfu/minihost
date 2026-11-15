"""A/B Compare: the null test and gapless switching (`minihost_desktop --ab-selftest`)."""

from __future__ import annotations

import subprocess

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

pytestmark = skip_if_no_desktop


def test_ab_compare_selftest():
    res = subprocess.run(
        [str(DESKTOP_BIN), "--ab-selftest"], capture_output=True, text=True, timeout=30
    )
    assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"
    assert "0 failures" in res.stdout
