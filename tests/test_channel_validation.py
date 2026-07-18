"""Regression tests: process methods must reject undersized buffers.

Pre-fix, passing a numpy array with fewer channels than the plugin
required produced UB (the C layer dereferenced past the std::vector of
channel pointers). Now it raises a RuntimeError with a clear message.
"""

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


@pytest.fixture
def plugin():
    return minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)


@skip_if_no_plugin
def test_process_rejects_too_few_input_channels(plugin):
    in_ch = plugin.num_input_channels
    out_ch = plugin.num_output_channels
    if in_ch < 2:
        pytest.skip("plugin has only 1 input channel; cannot test undersize")

    inp = np.zeros((in_ch - 1, 256), dtype=np.float32)
    out = np.zeros((out_ch, 256), dtype=np.float32)
    with pytest.raises(RuntimeError, match=r"Input has .* but plugin requires"):
        plugin.process(inp, out)


@skip_if_no_plugin
def test_process_rejects_too_few_output_channels(plugin):
    in_ch = plugin.num_input_channels
    out_ch = plugin.num_output_channels
    if out_ch < 2:
        pytest.skip("plugin has only 1 output channel; cannot test undersize")

    inp = np.zeros((in_ch, 256), dtype=np.float32)
    out = np.zeros((out_ch - 1, 256), dtype=np.float32)
    with pytest.raises(RuntimeError, match=r"Output has .* but plugin requires"):
        plugin.process(inp, out)


@skip_if_no_plugin
def test_process_rejects_frame_mismatch(plugin):
    inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels, 128), dtype=np.float32)
    with pytest.raises(RuntimeError, match=r"frame counts must match"):
        plugin.process(inp, out)


@skip_if_no_plugin
def test_process_rejects_oversized_block(plugin):
    inp = np.zeros((plugin.num_input_channels, 1024), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels, 1024), dtype=np.float32)
    with pytest.raises(RuntimeError, match=r"exceeds max block size"):
        plugin.process(inp, out)


@skip_if_no_plugin
def test_process_accepts_correct_shape(plugin):
    inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels, 256), dtype=np.float32)
    plugin.process(inp, out)  # should not raise


@skip_if_no_plugin
def test_process_accepts_extra_channels(plugin):
    # User passes more channels than the plugin needs; extras are harmlessly
    # ignored by the C layer.
    inp = np.zeros((plugin.num_input_channels + 2, 256), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels + 2, 256), dtype=np.float32)
    plugin.process(inp, out)


@skip_if_no_plugin
def test_process_midi_validates_shape(plugin):
    inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float32)
    out = np.zeros((max(plugin.num_output_channels - 1, 0), 256), dtype=np.float32)
    if plugin.num_output_channels < 2:
        pytest.skip("need >= 2 output channels for undersize test")
    with pytest.raises(RuntimeError, match=r"Output has"):
        plugin.process_midi(inp, out, [])


@skip_if_no_plugin
def test_chain_process_validates_shape():
    p = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    chain = minihost.PluginChain([p])

    in_ch = chain.num_input_channels
    out_ch = chain.num_output_channels
    if in_ch < 2:
        pytest.skip("chain has 1 input channel; cannot test undersize")

    inp = np.zeros((in_ch - 1, 256), dtype=np.float32)
    out = np.zeros((out_ch, 256), dtype=np.float32)
    with pytest.raises(RuntimeError, match=r"Input has"):
        chain.process(inp, out)
