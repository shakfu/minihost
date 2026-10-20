"""Session-wide checks shared by every test module."""

import os

import pytest

_PLUGIN_ENV_VARS = (
    "MINIHOST_TEST_PLUGIN",
    "MINIHOST_TEST_PLUGIN_FX",
    "MINIHOST_TEST_PLUGIN_SYNTH",
)


def pytest_configure(config):
    # An unset variable means "skip the plugin-gated tests". A set one that
    # names a missing path is a misconfiguration: modules that check only
    # whether the variable is set would error, and those that check the path
    # would skip, hiding the mistake. Stop before collection instead.
    missing = [
        f"{var}={os.environ[var]}"
        for var in _PLUGIN_ENV_VARS
        if os.environ.get(var) and not os.path.exists(os.environ[var])
    ]
    if missing:
        raise pytest.UsageError(
            "test plugin path does not exist: " + ", ".join(missing)
        )


# Diagnostic, opt-in: on macOS CI, MIDIClientCreate starts failing with -50
# part-way through the suite but not in a fresh process. Open and close a
# virtual port after collection and after each test, and report the first
# point at which that fails. On failure, also try MIDIClientCreate directly on
# the main thread: minihost creates clients on a worker thread.
_COREMIDI_PROBE = os.environ.get("MINIHOST_COREMIDI_PROBE") == "1"
_probe_state = {"last_ok": None, "first_bad": None, "error": None, "main": None}


def _probe_coremidi():
    import minihost

    minihost.MidiIn.open_virtual("mh-conftest-probe", lambda data: None).close()


def _main_thread_client_status():
    import sys

    if sys.platform != "darwin":
        return None
    import coremidi_loopback

    try:
        coremidi_loopback.Sender().close()
    except RuntimeError as e:
        return str(e)
    return "MIDIClientCreate -> 0"


def _probe_after(label):
    if not _COREMIDI_PROBE or _probe_state["first_bad"]:
        return
    try:
        _probe_coremidi()
    except RuntimeError as e:
        _probe_state["first_bad"] = label
        _probe_state["error"] = str(e)
        _probe_state["main"] = _main_thread_client_status()
    else:
        _probe_state["last_ok"] = label


def pytest_collection_finish(session):
    _probe_after("<collection>")


def pytest_runtest_logfinish(nodeid, location):
    _probe_after(nodeid)


def pytest_terminal_summary(terminalreporter):
    if not _COREMIDI_PROBE:
        return
    tr = terminalreporter
    tr.section("CoreMIDI probe")
    tr.write_line(f"last point with a working client: {_probe_state['last_ok']}")
    tr.write_line(f"first point after which it failed: {_probe_state['first_bad']}")
    tr.write_line(f"error: {_probe_state['error']}")
    tr.write_line(f"main-thread client at that point: {_probe_state['main']}")
