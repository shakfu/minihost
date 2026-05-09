"""Regression test: process must feed all input channels even when inCh != outCh.

Pre-fix, mh_process_midi_io passed a buffer sized to outCh to JUCE's
processBlock. When the plugin was configured with inCh > outCh, the higher
input channels were never delivered. The fix uses a single combined buffer
sized max(inCh + sidechainCh, outCh).

This test does not depend on a particular plugin's channel asymmetry --
instead it covers the buffer plumbing: roundtrip input data through a
configured Plugin and verify the output channels carry sensible data
(pre-fix, the path could SEGV or read garbage when the persistent buffer
was the wrong size).
"""

from __future__ import annotations

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
def test_process_does_not_corrupt_caller_input():
    # The fix uses an internal buffer for processing; caller input arrays
    # should be untouched after process() returns.
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)

    # Use a recognizable input pattern.
    rng = np.random.default_rng(seed=42)
    inp = rng.standard_normal((plugin.num_input_channels, 256)).astype(np.float32)
    inp_copy = inp.copy()
    out = np.zeros((plugin.num_output_channels, 256), dtype=np.float32)

    plugin.process(inp, out)

    # Input array must not have been mutated.
    assert np.array_equal(inp, inp_copy), "input array was mutated by process()"


@skip_if_no_plugin
def test_repeated_process_no_buffer_corruption():
    # Loop many times to catch any per-call buffer mismanagement (the
    # buf.setSize(... avoidReallocating=true) path is exercised here).
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels, 256), dtype=np.float32)

    for _ in range(200):
        plugin.process(inp, out)
        # Output must be finite (no NaN/Inf from stale-buffer reads).
        assert np.isfinite(out).all()


@skip_if_no_plugin
def test_varying_block_sizes():
    # The fix calls buf.setSize(totalCh, nframes, ...) per call. Test that
    # different block sizes within the same plugin lifetime work correctly.
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=1024)

    for n in [64, 256, 1024, 128, 512]:
        inp = np.zeros((plugin.num_input_channels, n), dtype=np.float32)
        out = np.zeros((plugin.num_output_channels, n), dtype=np.float32)
        plugin.process(inp, out)
        assert out.shape == (plugin.num_output_channels, n)
        assert np.isfinite(out).all()


@skip_if_no_plugin
def test_process_auto_chunked_path():
    # mh_process_auto splits at param-change boundaries; exercise the
    # combined-buffer chunk path.
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    inp = np.zeros((plugin.num_input_channels, 512), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels, 512), dtype=np.float32)

    # Param changes at 0, 100, 250, 400 -> creates 4 chunks.
    if plugin.num_params == 0:
        pytest.skip("plugin has no parameters")
    p0 = 0
    changes = [
        (0, p0, 0.25),
        (100, p0, 0.50),
        (250, p0, 0.75),
        (400, p0, 1.00),
    ]
    plugin.process_auto(inp, out, [], changes)
    assert np.isfinite(out).all()
