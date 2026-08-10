"""Guard that `_core.pyi` keeps describing the native extension.

The package ships `py.typed`, so the stub is the API contract users and
their type checkers see. Nothing in the build ties the stub to the nanobind
module, so a binding added in `_core.cpp` can silently go undeclared -- which
is how `vst3_state_split` / `vst3_state_join` ended up callable at runtime but
invisible to mypy, breaking type checking of `vstpreset.py`.

These tests compare the stub's declarations against the module as actually
imported, so the drift fails here rather than in a downstream user's CI.
"""

import ast
import inspect
from pathlib import Path

import pytest

import minihost
from minihost import _core


def _stub_path() -> Path:
    """Locate `_core.pyi` next to the installed package.

    Resolved via the imported package rather than the repo layout so the
    check also runs against an installed wheel (cibuildwheel runs the test
    suite out-of-tree).
    """
    return Path(minihost.__file__).parent / "_core.pyi"


@pytest.fixture(scope="module")
def stub_tree() -> ast.Module:
    path = _stub_path()
    if not path.is_file():
        pytest.skip(f"_core.pyi not present next to the package ({path})")
    return ast.parse(path.read_text(encoding="utf-8"), filename=str(path))


def _module_level_functions(tree: ast.Module) -> set[str]:
    return {
        node.name
        for node in tree.body
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    }


def _classes(tree: ast.Module) -> set[str]:
    return {node.name for node in tree.body if isinstance(node, ast.ClassDef)}


def _runtime_functions() -> set[str]:
    """Public module-level callables exported by the native extension.

    Classes are excluded (they are checked separately); underscore-prefixed
    names are internal plumbing such as `_message_thread_shutdown`.
    """
    return {
        name
        for name in dir(_core)
        if not name.startswith("_")
        and callable(getattr(_core, name))
        and not inspect.isclass(getattr(_core, name))
    }


def _runtime_classes() -> set[str]:
    return {
        name
        for name in dir(_core)
        if not name.startswith("_") and inspect.isclass(getattr(_core, name))
    }


def test_stub_declares_every_native_function(stub_tree):
    """Every public native helper must have a stub declaration."""
    missing = _runtime_functions() - _module_level_functions(stub_tree)
    assert not missing, (
        "these native functions are exported by _core.cpp but not declared in "
        f"_core.pyi: {sorted(missing)}"
    )


def test_stub_declares_every_native_class(stub_tree):
    """Every public native class must have a stub declaration."""
    missing = _runtime_classes() - _classes(stub_tree)
    assert not missing, (
        "these native classes are exported by _core.cpp but not declared in "
        f"_core.pyi: {sorted(missing)}"
    )


def test_stub_declares_nothing_that_vanished(stub_tree):
    """The stub must not promise functions the extension no longer has.

    A stale declaration is worse than a missing one: it type-checks clean and
    fails with AttributeError at runtime.
    """
    declared = {n for n in _module_level_functions(stub_tree) if not n.startswith("_")}
    phantom = declared - _runtime_functions()
    assert not phantom, (
        "these functions are declared in _core.pyi but do not exist in the "
        f"built extension: {sorted(phantom)}"
    )


def test_native_helpers_used_by_python_modules_exist():
    """Pin the specific native helpers the pure-Python layer reaches for.

    `vstpreset.py` imports `_core` lazily inside functions, so a renamed or
    dropped binding would otherwise only fail when a user writes a preset.
    """
    for name in (
        "vst3_state_split",
        "vst3_state_join",
        "vstpreset_read",
        "vstpreset_write",
        "vstpreset_read_class_id_from_bundle",
    ):
        assert hasattr(_core, name), f"_core is missing {name}"
