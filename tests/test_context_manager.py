"""Tests for explicit close() and context-manager support on Plugin / PluginChain."""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


@skip_if_no_plugin
def test_plugin_context_manager_yields_self():
    with minihost.Plugin(PLUGIN, sample_rate=48000) as plugin:
        assert plugin.num_input_channels >= 0
        assert plugin.sample_rate == 48000


@skip_if_no_plugin
def test_plugin_close_is_idempotent():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    plugin.close()
    plugin.close()  # second call must not raise
    plugin.close()  # third too


@skip_if_no_plugin
def test_plugin_operations_after_close_raise_clearly():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    plugin.close()

    inp = np.zeros((2, 256), dtype=np.float32)
    out = np.zeros((2, 256), dtype=np.float32)
    with pytest.raises(RuntimeError):
        plugin.process(inp, out)


@skip_if_no_plugin
def test_plugin_context_manager_closes_on_exit():
    plugin_ref = None
    with minihost.Plugin(PLUGIN, sample_rate=48000) as plugin:
        plugin_ref = plugin
        # Plugin is usable inside the with block.
        inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float32)
        out = np.zeros((plugin.num_output_channels, 256), dtype=np.float32)
        plugin.process(inp, out)

    # After exit, the plugin is closed.
    inp = np.zeros((2, 256), dtype=np.float32)
    out = np.zeros((2, 256), dtype=np.float32)
    with pytest.raises(RuntimeError):
        plugin_ref.process(inp, out)


@skip_if_no_plugin
def test_plugin_context_manager_closes_on_exception():
    plugin_ref = None
    with pytest.raises(ValueError, match="boom"):
        with minihost.Plugin(PLUGIN, sample_rate=48000) as plugin:
            plugin_ref = plugin
            raise ValueError("boom")

    inp = np.zeros((2, 256), dtype=np.float32)
    out = np.zeros((2, 256), dtype=np.float32)
    with pytest.raises(RuntimeError):
        plugin_ref.process(inp, out)


@skip_if_no_plugin
def test_pluginchain_context_manager():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    with minihost.PluginChain([plugin]) as chain:
        assert chain.num_plugins == 1
        inp = np.zeros((chain.num_input_channels, 256), dtype=np.float32)
        out = np.zeros((chain.num_output_channels, 256), dtype=np.float32)
        chain.process(inp, out)


@skip_if_no_plugin
def test_pluginchain_close_does_not_close_member_plugins():
    # The chain's close() releases the chain's own resources but leaves the
    # member Plugin objects alive (they are owned by the caller).
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    chain = minihost.PluginChain([plugin])
    chain.close()
    # Plugin should still work.
    inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels, 256), dtype=np.float32)
    plugin.process(inp, out)
