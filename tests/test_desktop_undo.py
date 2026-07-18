"""Undo/redo core coverage for the minihost_desktop binary.

The canvas undo/redo is snapshot-based: each edit records the whole
ProjectDocument, and undo/redo swap it in place. The GUI edit gestures
themselves can't be driven headlessly, but the underlying UndoHistory +
ProjectDocument snapshot/restore is exercised end-to-end through the
binary's ``--undo-selftest`` mode, which asserts the full contract:

- a mutation changes the serialized document,
- undo restores the pre-edit bytes exactly,
- redo re-applies the edit exactly,
- a fresh edit clears the redo stack.

Any broken invariant makes the binary exit non-zero.

Skipped when the desktop binary isn't built.
"""

from __future__ import annotations

import json
import subprocess

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop


def _write_project(path):
    path.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": [
                    {"id": "in", "kind": "input", "channels": 2, "source": "in.wav"},
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": "out.wav",
                        "bit_depth": 24,
                    },
                ],
                "edges": [{"src": "in", "dst": "out"}],
            }
        )
    )


@skip_if_no_desktop
def test_undo_selftest_passes(tmp_path):
    proj = tmp_path / "p.json"
    _write_project(proj)
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--undo-selftest={proj}"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert res.returncode == 0, (
        f"undo self-test failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    )
    assert "undo-selftest OK" in res.stderr


@skip_if_no_desktop
def test_undo_selftest_empty_value_exits_2(tmp_path):
    """Malformed invocation is a usage error (exit 2)."""
    res = subprocess.run(
        [str(DESKTOP_BIN), "--undo-selftest="],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert res.returncode == 2, (res.returncode, res.stderr)
