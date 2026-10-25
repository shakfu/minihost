"""The stale desktop-binary check in `desktop_helpers`."""

from __future__ import annotations

import os
import time

from desktop_helpers import stale_reason


def test_a_binary_older_than_the_sources_is_stale(tmp_path, monkeypatch):
    monkeypatch.delenv("MINIHOST_DESKTOP_BIN", raising=False)
    binary = tmp_path / "minihost_desktop"
    binary.write_bytes(b"")
    os.utime(binary, (0, 0))
    reason = stale_reason(binary)
    assert reason is not None and "make desktop" in reason


def test_a_binary_newer_than_the_sources_is_current(tmp_path, monkeypatch):
    monkeypatch.delenv("MINIHOST_DESKTOP_BIN", raising=False)
    binary = tmp_path / "minihost_desktop"
    binary.write_bytes(b"")
    future = time.time() + 3600
    os.utime(binary, (future, future))
    assert stale_reason(binary) is None


def test_an_explicit_binary_is_trusted(tmp_path, monkeypatch):
    binary = tmp_path / "minihost_desktop"
    binary.write_bytes(b"")
    os.utime(binary, (0, 0))
    monkeypatch.setenv("MINIHOST_DESKTOP_BIN", str(binary))
    assert stale_reason(binary) is None


def test_no_binary_is_not_stale():
    assert stale_reason(None) is None
