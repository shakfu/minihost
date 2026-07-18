"""Conformance tests: the C and C++ front-ends must agree byte-for-byte.

`minihost_c` (pure C) and `minihost_cpp` (C++) are two independent CLI
implementations over the same libminihost C API. They are meant to be
interchangeable, but nothing stopped them drifting apart -- historically they
lagged the library by several releases. This test runs the same data commands
through both binaries and asserts their stdout is identical, so any divergence
fails CI instead of accumulating silently.

Scope: deterministic, plugin-data commands (metadata, parameters, presets,
morph). Human-facing help/usage text is intentionally *not* compared -- the two
CLIs use different argument parsers (hand-rolled vs CLI11) and their usage
strings differ by design. stderr is ignored (plugins log there on load).

The test is skipped unless both binaries and a test plugin are available. Point
it at the binaries with ``MINIHOST_C_BIN`` / ``MINIHOST_CPP_BIN``, or build them
into ``build/`` (as CI's build-cli job does) or ``build-cli/``. The plugin comes
from ``MINIHOST_TEST_PLUGIN`` (default: Dexed).
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parent.parent

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)


def _find_binary(name: str, env_var: str) -> str | None:
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


C_BIN = _find_binary("minihost_c", "MINIHOST_C_BIN")
CPP_BIN = _find_binary("minihost_cpp", "MINIHOST_CPP_BIN")

skip_reason = None
if C_BIN is None or CPP_BIN is None:
    skip_reason = "minihost_c and/or minihost_cpp binary not found (build them first)"
elif not os.path.exists(PLUGIN):
    skip_reason = f"test plugin not found at {PLUGIN}"

pytestmark = pytest.mark.skipif(skip_reason is not None, reason=skip_reason or "")


# Deterministic data commands. Each entry is the argument list following the
# binary; {PLUGIN} is substituted at runtime.
CONFORMANCE_COMMANDS = [
    ["probe", "{PLUGIN}", "-j"],
    ["info", "{PLUGIN}", "--probe", "-j"],
    ["info", "{PLUGIN}", "-j"],
    ["params", "{PLUGIN}", "-j"],
    ["params", "{PLUGIN}", "-V", "-j"],
    ["presets", "{PLUGIN}", "-j"],
    ["morph", "{PLUGIN}", "-t", "0.3", "-j"],
    ["morph", "{PLUGIN}", "-t", "0.0"],
    ["morph", "{PLUGIN}", "-t", "0.75"],
]


def _run(binary: str, args: list[str]) -> subprocess.CompletedProcess:
    resolved = [a.replace("{PLUGIN}", PLUGIN) for a in args]
    return subprocess.run(
        [binary, *resolved],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        timeout=120,
    )


def _cmd_id(args: list[str]) -> str:
    return " ".join(a for a in args if a != "{PLUGIN}")


@pytest.mark.parametrize("args", CONFORMANCE_COMMANDS, ids=_cmd_id)
def test_c_and_cpp_stdout_identical(args):
    c = _run(C_BIN, args)
    cpp = _run(CPP_BIN, args)

    assert c.returncode == 0, f"minihost_c failed ({c.returncode}) for {_cmd_id(args)}"
    assert cpp.returncode == 0, (
        f"minihost_cpp failed ({cpp.returncode}) for {_cmd_id(args)}"
    )

    if c.stdout != cpp.stdout:
        # Produce a readable diff on failure rather than dumping raw bytes.
        import difflib

        c_lines = c.stdout.decode("utf-8", "replace").splitlines()
        cpp_lines = cpp.stdout.decode("utf-8", "replace").splitlines()
        diff = "\n".join(
            difflib.unified_diff(
                c_lines, cpp_lines, "minihost_c", "minihost_cpp", lineterm="", n=2
            )
        )
        # Cap the diff so a large mismatch stays readable.
        diff_head = "\n".join(diff.splitlines()[:40])
        pytest.fail(
            f"stdout differs for '{_cmd_id(args)}' "
            f"(C={len(c.stdout)}b, CPP={len(cpp.stdout)}b):\n{diff_head}"
        )


def test_morph_blend_endpoints_match_across_clis():
    """A spot check that both CLIs interpolate identically at several t."""
    for t in ("0.1", "0.5", "0.9"):
        args = ["morph", "{PLUGIN}", "-t", t, "-j"]
        c = _run(C_BIN, args)
        cpp = _run(CPP_BIN, args)
        assert c.returncode == 0 and cpp.returncode == 0
        assert c.stdout == cpp.stdout, f"morph -t {t} diverges between CLIs"
