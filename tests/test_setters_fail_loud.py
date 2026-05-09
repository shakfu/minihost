"""Regression tests: set_sample_rate / set_processing_precision must fail
loudly on invalid input rather than silently leaving the plugin misconfigured.
"""

from __future__ import annotations

import math
import os

import pytest

import minihost

PLUGIN = os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


@skip_if_no_plugin
def test_set_sample_rate_rejects_negative():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    with pytest.raises(RuntimeError):
        plugin.sample_rate = -1.0
    # Original rate must still be reported.
    assert plugin.sample_rate == 48000


@skip_if_no_plugin
def test_set_sample_rate_rejects_zero():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    with pytest.raises(RuntimeError):
        plugin.sample_rate = 0.0
    assert plugin.sample_rate == 48000


@skip_if_no_plugin
def test_set_sample_rate_rejects_nan():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    with pytest.raises(RuntimeError):
        plugin.sample_rate = float("nan")
    assert plugin.sample_rate == 48000
    # And not infinite either.
    assert math.isfinite(plugin.sample_rate)


@skip_if_no_plugin
def test_set_processing_precision_rejects_unsupported_double_silently():
    # If the plugin does not support double precision, asking for it must
    # raise via the Python wrapper -- not silently leave the plugin in a
    # mismatched state.
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    if plugin.supports_double:
        pytest.skip("plugin supports double precision; cannot test rejection")
    with pytest.raises(RuntimeError):
        plugin.processing_precision = minihost.MH_PRECISION_DOUBLE
