"""Build-tree lookup of the test-plugin fixtures (cli_helpers.py)."""

import os

import cli_helpers
import pytest


def _bundle(root, build, config, module):
    b = (
        root
        / build
        / "projects/test_plugin/MinihostTestFx_artefacts"
        / config
        / "VST3/MinihostTestFx.vst3"
    )
    (b / "Contents/x86_64-linux").mkdir(parents=True)
    if module:
        (b / "Contents/x86_64-linux/MinihostTestFx.so").write_bytes(b"")
    return b


@pytest.fixture
def root(tmp_path, monkeypatch):
    monkeypatch.setattr(cli_helpers, "_REPO_ROOT", tmp_path)
    monkeypatch.delenv("MINIHOST_TEST_PLUGIN_FX", raising=False)
    return tmp_path


def test_newer_bundle_without_module_is_ignored(root):
    complete = _bundle(root, "build", "Release", module=True)
    empty = _bundle(root, "build-dbg", "Debug", module=False)
    os.utime(complete, (1, 1))
    found = cli_helpers.find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")
    assert found == str(complete), empty


def test_only_incomplete_bundles_gives_none(root):
    _bundle(root, "build", "Release", module=False)
    assert (
        cli_helpers.find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")
        is None
    )


def test_newest_complete_bundle_wins(root):
    old = _bundle(root, "build", "Release", module=True)
    new = _bundle(root, "build-dbg", "Debug", module=True)
    os.utime(old, (1, 1))
    found = cli_helpers.find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")
    assert found == str(new)
