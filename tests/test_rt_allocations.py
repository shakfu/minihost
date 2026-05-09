"""Regression tests: hot audio paths must not allocate after warm-up.

We don't run a malloc interceptor here (that needs a custom allocator hook).
What we can do cheaply is exercise the paths that previously allocated per
call -- mh_process_double and mh_chain_process_auto -- and verify that they
remain stable and correct over many iterations and varying input sizes.
A pre-fix path that allocates per call would still pass these tests, but
they catch the more common follow-on bugs (stale buffer state, off-by-one
in the persistent-vector reuse pattern).
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
def test_process_double_repeated_calls():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float64)
    out = np.zeros((plugin.num_output_channels, 256), dtype=np.float64)

    for _ in range(200):
        plugin.process_double(inp, out)
        assert np.isfinite(out).all()


@skip_if_no_plugin
def test_process_double_varying_sizes():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=1024)
    for n in [64, 256, 1024, 128, 512]:
        inp = np.zeros((plugin.num_input_channels, n), dtype=np.float64)
        out = np.zeros((plugin.num_output_channels, n), dtype=np.float64)
        plugin.process_double(inp, out)
        assert out.shape == (plugin.num_output_channels, n)


@skip_if_no_plugin
def test_process_double_input_not_mutated():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    rng = np.random.default_rng(seed=7)
    inp = rng.standard_normal((plugin.num_input_channels, 256)).astype(np.float64)
    inp_copy = inp.copy()
    out = np.zeros((plugin.num_output_channels, 256), dtype=np.float64)

    plugin.process_double(inp, out)
    assert np.array_equal(inp, inp_copy), "process_double mutated caller input"


@skip_if_no_plugin
def test_chain_process_auto_repeated_chunks():
    # Exercise the chain auto-chunk persistent-vector path with many param
    # changes per block (each forces a chunk boundary).
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    chain = minihost.PluginChain([plugin])
    if chain.get_plugin(0).num_params == 0:
        pytest.skip("plugin has no parameters")

    inp = np.zeros((chain.num_input_channels, 512), dtype=np.float32)
    out = np.zeros((chain.num_output_channels, 512), dtype=np.float32)

    # Many chunks per call: 16 param changes evenly spread across the block.
    changes = [(i * 32, 0, 0, float(i) / 16.0) for i in range(16)]

    for _ in range(50):
        chain.process_auto(inp, out, [], changes)
        assert np.isfinite(out).all()


@skip_if_no_plugin
def test_chain_process_auto_with_midi_events():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    chain = minihost.PluginChain([plugin])

    inp = np.zeros((chain.num_input_channels, 512), dtype=np.float32)
    out = np.zeros((chain.num_output_channels, 512), dtype=np.float32)

    # MIDI events spread across multiple chunks.
    midi = [(0, 0x90, 60, 100), (256, 0x80, 60, 0), (300, 0x90, 64, 100)]
    changes = [(0, 0, 0, 0.5), (256, 0, 0, 0.7)]

    for _ in range(30):
        chain.process_auto(inp, out, midi, changes)
        assert np.isfinite(out).all()
