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
