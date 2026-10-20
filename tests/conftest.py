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
# part-way through the suite but not in a fresh process. After each test, open
# and close a virtual port and report the first test after which that fails.
_COREMIDI_PROBE = os.environ.get("MINIHOST_COREMIDI_PROBE") == "1"
_probe_state = {"last_ok": None, "first_bad": None, "error": None}


def _probe_coremidi():
    import minihost

    minihost.MidiIn.open_virtual("mh-conftest-probe", lambda data: None).close()


def pytest_runtest_logfinish(nodeid, location):
    if not _COREMIDI_PROBE or _probe_state["first_bad"]:
        return
    try:
        _probe_coremidi()
    except RuntimeError as e:
        _probe_state["first_bad"] = nodeid
        _probe_state["error"] = str(e)
    else:
        _probe_state["last_ok"] = nodeid


def pytest_terminal_summary(terminalreporter):
    if not _COREMIDI_PROBE:
        return
    tr = terminalreporter
    tr.section("CoreMIDI probe")
    tr.write_line(f"last test with a working client: {_probe_state['last_ok']}")
    tr.write_line(f"first test after which it failed: {_probe_state['first_bad']}")
    tr.write_line(f"error: {_probe_state['error']}")
