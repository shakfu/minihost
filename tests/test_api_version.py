"""Smoke tests for the ABI version surface."""

from __future__ import annotations

import re

import minihost


def test_version_macros_match_function():
    # Header constants and runtime function must agree (the wheel was built
    # against this header). A divergence would mean a wheel/lib mismatch.
    assert minihost.api_version() == minihost.MH_API_VERSION_NUMBER
    assert minihost.api_version_string() == minihost.MH_API_VERSION_STRING


def test_version_number_layout():
    # Layout: MAJOR*10000 + MINOR*100 + PATCH (so 1.2.3 == 10203).
    expected = (
        minihost.MH_API_VERSION_MAJOR * 10000
        + minihost.MH_API_VERSION_MINOR * 100
        + minihost.MH_API_VERSION_PATCH
    )
    assert minihost.MH_API_VERSION_NUMBER == expected


def test_version_string_format():
    assert re.fullmatch(r"\d+\.\d+\.\d+", minihost.MH_API_VERSION_STRING)


def test_version_at_least_1_0_0():
    # We seeded the ABI version at 1.0.0; it must never go backward.
    assert minihost.MH_API_VERSION_NUMBER >= 10000
