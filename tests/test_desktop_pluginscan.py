"""Headless plugin-scan coverage for the minihost_desktop binary.

The desktop app's plugin picker (File > Open Plugin..., canvas Add Plugin...)
lists the *scanned* plugin library. That library is populated by an in-process
scan that -- prior to this test -- had only ever been compiled, never run
against real plugins. The binary's ``--scan-plugins`` mode is the headless seam
that exercises it: it registers the host formats via
``juce::addDefaultFormatsToManager`` and drives a ``PluginDirectoryScanner`` to
completion, writing the resulting ``KnownPluginList`` to XML.

Two levels of coverage:

- ``test_scan_empty_dir_writes_valid_xml`` always runs. Scanning an empty
  directory (restricted to the VST3 format so no system AudioUnits are pulled
  in) proves the format registers, the scan terminates, and a well-formed XML
  library is written -- with zero plugins to instantiate, so it is fast and
  hermetic.
- ``test_scan_finds_real_plugin`` runs only when ``MINIHOST_TEST_PLUGIN`` points
  at a ``.vst3``. It copies that one plugin into an isolated directory and scans
  it, proving the scanner actually discovers and records a real plugin. This is
  what verifies the format's scanner works end-to-end.

Skipped when the desktop binary isn't built.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop


def _run_scan(scan_dir: Path, out_xml: Path, fmt: str = "VST3",
              out_of_process: bool = False):
    argv = [
        str(DESKTOP_BIN),
        f"--scan-plugins={scan_dir}",
        f"--scan-format={fmt}",
        f"--scan-out={out_xml}",
    ]
    if out_of_process:
        argv.append("--scan-oop")
    return subprocess.run(argv, capture_output=True, text=True, timeout=120)


def _run_scan_default_path(scan_dir: Path, settings_dir: Path,
                           fmt: str = "VST3"):
    """Scan to the library's DEFAULT location, isolated under settings_dir.

    No --scan-out: the scan writes to known_plugins.xml next to the settings
    file -- the same file shutdown() persists -- so this exercises the
    save-on-shutdown path. MINIHOST_DESKTOP_SETTINGS_DIR redirects the app
    data dir (macOS ignores $HOME for it).
    """
    env = dict(os.environ, MINIHOST_DESKTOP_SETTINGS_DIR=str(settings_dir))
    return subprocess.run(
        [str(DESKTOP_BIN), f"--scan-plugins={scan_dir}", f"--scan-format={fmt}"],
        capture_output=True, text=True, timeout=120, env=env,
    )


@skip_if_no_desktop
def test_scan_empty_dir_writes_valid_xml(tmp_path):
    """A completed scan of an empty dir exits 0 and writes parseable XML."""
    scan_dir = tmp_path / "plugins"
    scan_dir.mkdir()
    out_xml = tmp_path / "known.xml"

    res = _run_scan(scan_dir, out_xml)
    assert res.returncode == 0, \
        f"scan failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    assert out_xml.exists(), "scan did not write the output XML"
    # Well-formed and rooted at the KnownPluginList element JUCE emits.
    root = ET.parse(out_xml).getroot()
    assert root.tag == "KNOWNPLUGINS", root.tag
    assert "scan: total found 0" in res.stderr


@skip_if_no_desktop
def test_scan_finds_real_plugin(tmp_path):
    """With a real VST3 in the scanned dir, the library records it."""
    plugin = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not plugin or not plugin.endswith(".vst3"):
        pytest.skip("MINIHOST_TEST_PLUGIN not set to a .vst3")
    src = Path(plugin)
    if not src.exists():
        pytest.skip(f"MINIHOST_TEST_PLUGIN does not exist: {src}")

    # Isolate the one plugin so the scan can't pull in others.
    scan_dir = tmp_path / "plugins"
    scan_dir.mkdir()
    shutil.copytree(src, scan_dir / src.name)
    out_xml = tmp_path / "known.xml"

    res = _run_scan(scan_dir, out_xml)
    assert res.returncode == 0, \
        f"scan failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    root = ET.parse(out_xml).getroot()
    plugins = root.findall("PLUGIN")
    assert plugins, f"scan recorded no plugins for {src.name}:\n{res.stderr}"
    # The recorded plugin should point back at the copy we scanned.
    assert any(src.name in (p.get("file") or "") for p in plugins), \
        [p.get("file") for p in plugins]


@skip_if_no_desktop
def test_scan_oop_finds_real_plugin(tmp_path):
    """The out-of-process scan discovers a real VST3 via the child process.

    Exercises the full handshake: the parent launches a child (a relaunch of
    this same binary with the scanner UID), the child instantiates the plugin
    in its own process, and the descriptions round-trip back over IPC. If the
    handshake were broken, zero plugins would be recorded. Crash *containment*
    (a plugin that kills the child is blacklisted, not fatal) is verified
    manually against a full system scan -- we have no deliberately-crashing
    plugin fixture for a hermetic assertion.
    """
    plugin = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not plugin or not plugin.endswith(".vst3"):
        pytest.skip("MINIHOST_TEST_PLUGIN not set to a .vst3")
    src = Path(plugin)
    if not src.exists():
        pytest.skip(f"MINIHOST_TEST_PLUGIN does not exist: {src}")

    scan_dir = tmp_path / "plugins"
    scan_dir.mkdir()
    shutil.copytree(src, scan_dir / src.name)
    out_xml = tmp_path / "known.xml"

    res = _run_scan(scan_dir, out_xml, out_of_process=True)
    assert res.returncode == 0, \
        f"oop scan failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"
    root = ET.parse(out_xml).getroot()
    plugins = root.findall("PLUGIN")
    assert plugins, \
        f"oop scan recorded no plugins for {src.name}:\n{res.stderr}"
    assert any(src.name in (p.get("file") or "") for p in plugins), \
        [p.get("file") for p in plugins]


@skip_if_no_desktop
def test_scan_default_path_survives_shutdown(tmp_path):
    """A scan to the default library path is not clobbered on shutdown.

    Regression guard: shutdown() persists the app's in-memory KnownPluginList
    to known_plugins.xml. In headless scan mode (and in spawned scan-worker
    children) that member list is empty, so an unconditional save would
    overwrite the freshly-scanned library with nothing. Here we scan to the
    default path (isolated via MINIHOST_DESKTOP_SETTINGS_DIR) and assert the
    library still holds the plugin after the process has fully exited.
    """
    plugin = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not plugin or not plugin.endswith(".vst3"):
        pytest.skip("MINIHOST_TEST_PLUGIN not set to a .vst3")
    src = Path(plugin)
    if not src.exists():
        pytest.skip(f"MINIHOST_TEST_PLUGIN does not exist: {src}")

    scan_dir = tmp_path / "plugins"
    scan_dir.mkdir()
    shutil.copytree(src, scan_dir / src.name)
    settings_dir = tmp_path / "appdata"
    settings_dir.mkdir()

    res = _run_scan_default_path(scan_dir, settings_dir)
    assert res.returncode == 0, \
        f"scan failed:\nstdout:{res.stdout}\nstderr:{res.stderr}"

    library = settings_dir / "known_plugins.xml"
    assert library.exists(), \
        f"library not written at default path:\n{res.stderr}"
    plugins = ET.parse(library).getroot().findall("PLUGIN")
    assert plugins, (
        "library was empty after shutdown -- save-on-shutdown clobbered the "
        f"scanned list:\n{res.stderr}")
