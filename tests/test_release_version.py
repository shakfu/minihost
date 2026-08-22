"""Guards that the release version has exactly one source of truth.

The release version -- what a user means by "which minihost is this" -- lives
in the ``version`` field of ``pyproject.toml`` and nowhere else. Everything
downstream derives from it:

* the Python package, via ``minihost.__version__``
* the CLI binaries and the desktop app, via the CMake-generated
  ``minihost_version.h``
* the CI artifact and archive names, which ``sed`` the same line

The one hand-maintained copy is ``__version__`` in ``src/minihost/__init__.py``
(kept a literal so importing the package costs no metadata lookup). These tests
exist so that copy cannot silently drift; if one fails, fix ``pyproject.toml``
first and propagate, never the other way round.

This is deliberately NOT the same axis as ``MH_API_VERSION_*``, which versions
the C ABI -- see tests/test_api_version.py.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

import pytest

from cli_helpers import find_cli_binary

# Imported lazily by the two tests that need it, NOT at module scope. The
# native-binary checks below are the point of this file in the CLI/sanitizer CI
# jobs, and those jobs install pytest without building the minihost wheel -- a
# module-level `import minihost` would turn that into a collection error and
# take the binary coverage down with it.
try:
    import minihost
except ImportError:  # pragma: no cover - exercised only in package-less CI
    minihost = None  # type: ignore[assignment]

needs_package = pytest.mark.skipif(
    minihost is None, reason="minihost package not installed in this environment"
)

REPO_ROOT = Path(__file__).resolve().parents[1]
PYPROJECT = REPO_ROOT / "pyproject.toml"


def _pyproject_version() -> str:
    """The single source of truth, read the same way the CI job reads it."""
    text = PYPROJECT.read_text(encoding="utf-8")
    match = re.search(r'^version = "([^"]+)"', text, re.MULTILINE)
    assert match, f"no 'version = \"...\"' line in {PYPROJECT}"
    return match.group(1)


@needs_package
def test_package_version_matches_pyproject():
    assert minihost.__version__ == _pyproject_version(), (
        "minihost.__version__ has drifted from pyproject.toml. pyproject.toml "
        "is the source of truth: update it, then mirror it into "
        "src/minihost/__init__.py."
    )


def test_version_string_is_pep440_release():
    assert re.fullmatch(r"\d+\.\d+\.\d+([.\-+].*)?", _pyproject_version())


def test_changelog_documents_current_version():
    changelog = (REPO_ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    version = _pyproject_version()
    assert f"## [{version}]" in changelog, (
        f"CHANGELOG.md has no '## [{version}]' section. A released version "
        "must be described before it ships."
    )


def test_version_header_template_is_versionless():
    """The template must interpolate, never hardcode.

    A literal version escaping into cmake/minihost_version.h.in would defeat
    the whole arrangement, so assert the placeholders are still placeholders.
    """
    template = (REPO_ROOT / "cmake" / "minihost_version.h.in").read_text(
        encoding="utf-8"
    )
    assert '#define MINIHOST_VERSION "@MINIHOST_VERSION@"' in template
    for part in ("MAJOR", "MINOR", "PATCH"):
        assert f"#define MINIHOST_VERSION_{part} @MINIHOST_VERSION_{part}@" in template


@needs_package
def test_python_cli_reports_the_release_version():
    result = subprocess.run(
        [sys.executable, "-m", "minihost.cli", "--version"],
        capture_output=True,
        text=True,
    )
    if result.returncode != 0 and "No module named" in result.stderr:
        pytest.skip("minihost.cli is not runnable as a module in this layout")
    assert result.returncode == 0, result.stderr
    assert f"minihost {_pyproject_version()}" in result.stdout


# The native binaries are only present in a standalone CMake build tree, which
# `make test` (a wheel build) does not produce. Check them when they are there
# rather than skipping the coverage entirely. The locator is shared with
# tests/test_cli_conformance.py so this knows about the Visual Studio layout
# (Release/<name>.exe) too -- a Unix-only path list would *skip* on Windows
# rather than fail, quietly losing the coverage on the one platform where the
# build layout differs.
_NATIVE_CLIS = [
    ("minihost_c", find_cli_binary("minihost_c", "MINIHOST_C_BIN")),
    ("minihost_cpp", find_cli_binary("minihost_cpp", "MINIHOST_CPP_BIN")),
]


@pytest.mark.parametrize("name,binary", _NATIVE_CLIS, ids=[n for n, _ in _NATIVE_CLIS])
def test_native_cli_reports_the_release_version(name: str, binary: str | None):
    if binary is None:
        pytest.skip(f"{name} not built (standalone cmake build only)")
    result = subprocess.run([binary, "--version"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert f"minihost {_pyproject_version()}" in result.stdout
