"""Regression test: PluginChain / AudioDevice must keep their inputs alive.

Pre-fix, constructing a chain or device from anonymous Plugins (no external
references) left dangling raw pointers once Python collected the inputs.
With nb::keep_alive<1, 2> on the constructors, the inputs are pinned to
the wrapping object.
"""

from __future__ import annotations

import gc
import os

import numpy as np
import pytest

import minihost

PLUGIN = os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


@skip_if_no_plugin
def test_pluginchain_keeps_anonymous_plugins_alive():
    chain = minihost.PluginChain([minihost.Plugin(PLUGIN, sample_rate=48000)])

    # Force a GC sweep -- pre-fix, the anonymous Plugin would be freed here
    # because no Python reference held it.
    gc.collect()

    # Use the chain. Pre-fix, this dereferences a freed C++ Plugin.
    inp = np.zeros((chain.num_input_channels, 256), dtype=np.float32)
    out = np.zeros((chain.num_output_channels, 256), dtype=np.float32)
    chain.process(inp, out)


@skip_if_no_plugin
def test_audiodevice_keeps_anonymous_chain_alive():
    # AudioDevice over an anonymous chain over an anonymous Plugin: the
    # device must keep the chain alive, and the chain must keep the plugin
    # alive. Pre-fix, both intermediate objects were eligible for GC.
    device = minihost.AudioDevice(
        minihost.PluginChain([minihost.Plugin(PLUGIN, sample_rate=48000)]),
        sample_rate=48000,
        buffer_frames=256,
    )
    gc.collect()
    # Touching device properties dereferences the underlying chain/plugin.
    assert device.sample_rate > 0
    assert device.channels > 0


@skip_if_no_plugin
def test_audiodevice_keeps_anonymous_plugin_alive():
    device = minihost.AudioDevice(
        minihost.Plugin(PLUGIN, sample_rate=48000),
        sample_rate=48000,
        buffer_frames=256,
    )
    gc.collect()
    assert device.sample_rate > 0
    assert device.channels > 0
