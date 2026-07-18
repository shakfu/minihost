"""Tests for minihost.PluginGraph (general-DAG executor).

Three families of tests:

1. Topology / validation -- exercised without any plugin, just the
   built-in input / output / mix nodes. Covers channel mismatch,
   cycle detection, missing input ports at compile, post-compile
   mutation, dst-port range, and connect-into-input rejection.

2. Non-plugin numerical parity -- a few simple topologies whose
   output is known a priori from numpy arithmetic. Confirms the
   render-block scheduler and mix node compute the expected
   per-sample values.

3. Plugin parity -- a linear graph (input -> plugin -> output) must
   render the same audio as `Plugin.process_audio` on the same plugin
   with the same input. Skipped when MINIHOST_TEST_PLUGIN is unset.
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


# -------------------------------------------------------------------- #
# 1. Topology / validation                                              #
# -------------------------------------------------------------------- #


def test_compile_empty_graph_fails():
    g = minihost.PluginGraph(64, 48000.0)
    with pytest.raises(RuntimeError, match="no output nodes"):
        g.compile()


def test_unconnected_input_port_fails_compile():
    g = minihost.PluginGraph(64, 48000.0)
    g.add_input(2)
    g.add_output(2)
    # output's input port 0 is unconnected
    with pytest.raises(RuntimeError, match="unconnected"):
        g.compile()


def test_channel_mismatch_rejected_at_connect():
    g = minihost.PluginGraph(64, 48000.0)
    stereo_in = g.add_input(2)
    mono_out = g.add_output(1)
    with pytest.raises(RuntimeError, match="channel mismatch"):
        g.connect(stereo_in, mono_out)


def test_cannot_connect_into_input_node():
    g = minihost.PluginGraph(64, 48000.0)
    a = g.add_input(2)
    b = g.add_input(2)
    with pytest.raises(RuntimeError, match="cannot connect into an input"):
        g.connect(a, b)


def test_dst_port_out_of_range():
    g = minihost.PluginGraph(64, 48000.0)
    src = g.add_input(2)
    mix = g.add_mix(2, 2)
    with pytest.raises(RuntimeError, match="dst_port"):
        g.connect(src, mix, dst_port=5)


def test_self_edge_rejected():
    g = minihost.PluginGraph(64, 48000.0)
    m = g.add_mix(1, 2)
    with pytest.raises(RuntimeError, match="self-edge"):
        g.connect(m, m, dst_port=0)


def test_cycle_detected_at_compile():
    g = minihost.PluginGraph(64, 48000.0)
    m1 = g.add_mix(1, 2)
    m2 = g.add_mix(1, 2)
    out = g.add_output(2)
    g.connect(m1, m2)
    g.connect(m2, m1)  # back-edge
    g.connect(m2, out)
    with pytest.raises(RuntimeError, match="cycle"):
        g.compile()


def test_post_compile_mutation_rejected():
    g = minihost.PluginGraph(64, 48000.0)
    inp = g.add_input(2)
    out = g.add_output(2)
    g.connect(inp, out)
    g.compile()
    assert g.is_compiled
    with pytest.raises(RuntimeError, match="already compiled"):
        g.add_input(1)
    with pytest.raises(RuntimeError, match="already compiled"):
        g.connect(inp, out)


def test_render_before_compile_fails():
    g = minihost.PluginGraph(64, 48000.0)
    inp = g.add_input(2)
    out = g.add_output(2)
    g.connect(inp, out)
    buf_in = np.zeros((2, 8), dtype=np.float32)
    buf_out = np.zeros((2, 8), dtype=np.float32)
    with pytest.raises(RuntimeError, match="render_block failed"):
        g.render_block([buf_in], [buf_out], 8)


# -------------------------------------------------------------------- #
# 2. Non-plugin numerical parity                                        #
# -------------------------------------------------------------------- #


def test_identity_input_to_output_copies_samples():
    """input -> output is a memcpy. Whatever goes in comes out."""
    F = 32
    g = minihost.PluginGraph(F, 48000.0)
    inp = g.add_input(2)
    out = g.add_output(2)
    g.connect(inp, out)
    g.compile()

    rng = np.random.default_rng(0)
    src = rng.standard_normal((2, F)).astype(np.float32)
    dst = np.full((2, F), -7.0, dtype=np.float32)
    g.render_block([src], [dst], F)
    np.testing.assert_array_equal(dst, src)


def test_mix_node_sums_with_per_input_gain():
    """mix(2,2) with gains (1.0, 0.5) on inputs (a, b) emits a + 0.5*b."""
    F = 16
    g = minihost.PluginGraph(F, 48000.0)
    inA = g.add_input(2)
    inB = g.add_input(2)
    mix = g.add_mix(2, 2)
    out = g.add_output(2)
    g.connect(inA, mix, dst_port=0)
    g.connect(inB, mix, dst_port=1)
    g.connect(mix, out)
    g.set_mix_gain(mix, 1, 0.5)
    g.compile()

    a = np.tile(np.array([1.0, 2.0], dtype=np.float32)[:, None], (1, F))
    b = np.tile(np.array([4.0, 8.0], dtype=np.float32)[:, None], (1, F))
    dst = np.zeros((2, F), dtype=np.float32)
    g.render_block([a, b], [dst], F)
    expected = a + 0.5 * b
    np.testing.assert_array_almost_equal(dst, expected, decimal=6)


def test_gain_change_takes_effect_without_recompile():
    F = 8
    g = minihost.PluginGraph(F, 48000.0)
    inA = g.add_input(2)
    inB = g.add_input(2)
    mix = g.add_mix(2, 2)
    out = g.add_output(2)
    g.connect(inA, mix, dst_port=0)
    g.connect(inB, mix, dst_port=1)
    g.connect(mix, out)
    g.compile()

    a = np.ones((2, F), dtype=np.float32)
    b = np.full((2, F), 2.0, dtype=np.float32)
    dst = np.zeros((2, F), dtype=np.float32)

    g.render_block([a, b], [dst], F)
    np.testing.assert_array_almost_equal(dst, a + b, decimal=6)

    g.set_mix_gain(mix, 1, 0.25)
    g.render_block([a, b], [dst], F)
    np.testing.assert_array_almost_equal(dst, a + 0.25 * b, decimal=6)


def test_fan_out_with_mix_reconvergence():
    """One input fanned to both inputs of a mix is summed by the gains."""
    F = 8
    g = minihost.PluginGraph(F, 48000.0)
    src = g.add_input(2)
    mix = g.add_mix(2, 2)
    out = g.add_output(2)
    g.connect(src, mix, dst_port=0)
    g.connect(src, mix, dst_port=1)
    g.connect(mix, out)
    g.set_mix_gain(mix, 0, 0.4)
    g.set_mix_gain(mix, 1, 0.6)
    g.compile()

    s = np.tile(np.arange(F, dtype=np.float32), (2, 1))
    dst = np.zeros((2, F), dtype=np.float32)
    g.render_block([s], [dst], F)
    np.testing.assert_array_almost_equal(dst, s, decimal=6)


# -------------------------------------------------------------------- #
# 3. Plugin parity                                                      #
# -------------------------------------------------------------------- #


@skip_if_no_plugin
def test_linear_plugin_graph_matches_process_audio():
    """input -> plugin -> output (block-by-block) must match
    Plugin.process_audio on the same plugin and input."""
    sr = 48000
    block = 256
    total = block * 4

    p = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=block)
    in_ch = p.num_input_channels
    out_ch = p.num_output_channels
    if in_ch == 0:
        pytest.skip(
            "synth-only plugin (0 input channels) -- use a different parity test"
        )

    rng = np.random.default_rng(123)
    audio_in = rng.standard_normal((in_ch, total)).astype(np.float32) * 0.1

    # Reference: Plugin.process_audio renders the whole buffer. Match
    # block sizes (both the plugin and process_audio) so the call paths
    # tile the input identically.
    ref = minihost.process_audio(
        p, audio_in, block_size=block, compensate_latency=False
    )
    if hasattr(ref, "to_numpy"):
        ref = ref.to_numpy()
    ref = ref[:, :total]

    g = minihost.PluginGraph(block, float(sr))
    in_node = g.add_input(in_ch)
    pl_node = g.add_plugin(p)
    out_node = g.add_output(out_ch)
    g.connect(in_node, pl_node)
    g.connect(pl_node, out_node)
    g.compile()

    actual = np.zeros((out_ch, total), dtype=np.float32)
    in_buf = np.zeros((in_ch, block), dtype=np.float32)
    out_buf = np.zeros((out_ch, block), dtype=np.float32)
    for start in range(0, total, block):
        in_buf[:] = audio_in[:, start : start + block]
        g.render_block([in_buf], [out_buf], block)
        actual[:, start : start + block] = out_buf

    np.testing.assert_allclose(actual, ref, atol=1e-5, rtol=1e-5)


@skip_if_no_plugin
def test_graph_automation_matches_plugin_process_auto():
    """Per-block parameter automation through PluginGraph.set_node_automation
    must match Plugin.process_auto when applied to the same plugin
    over the same input + automation events.
    """
    sr = 48000
    block = 256

    p_ref = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=block)
    in_ch = p_ref.num_input_channels
    out_ch = p_ref.num_output_channels
    if in_ch == 0:
        pytest.skip("synth-only plugin")
    if p_ref.num_params == 0:
        pytest.skip("plugin has no params")

    rng = np.random.default_rng(99)
    in_buf = (rng.standard_normal((in_ch, block)) * 0.05).astype(np.float32)
    # One automation point in this block: param 0 set to 0.7 at frame 50.
    autos = [(50, 0, 0.7)]

    # Reference: Plugin.process_auto on a separate instance.
    out_ref = np.zeros((out_ch, block), dtype=np.float32)
    p_ref.process_auto(in_buf, out_ref, [], autos)

    # Compare against a graph that wraps a separate Plugin instance
    # (each instance is fresh so initial parameter state is identical).
    p_graph = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=block)
    g = minihost.PluginGraph(block, float(sr))
    in_node = g.add_input(in_ch)
    pl_node = g.add_plugin(p_graph)
    out_node = g.add_output(out_ch)
    g.connect(in_node, pl_node)
    g.connect(pl_node, out_node)
    g.compile()
    g.set_node_automation(pl_node, autos)
    out_g = np.zeros((out_ch, block), dtype=np.float32)
    g.render_block([in_buf], [out_g], block)

    np.testing.assert_allclose(out_g, out_ref, atol=1e-5, rtol=1e-5)


@skip_if_no_plugin
def test_plugin_node_keeps_plugin_alive():
    """The graph holds a Python reference to the Plugin so dropping
    the local name does not free the underlying instance."""
    import gc

    g = minihost.PluginGraph(256, 48000.0)
    p = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=256)
    in_ch = p.num_input_channels
    out_ch = p.num_output_channels
    if in_ch == 0:
        pytest.skip("synth-only plugin")
    g.add_input(in_ch)
    g.add_plugin(p)
    g.add_output(out_ch)
    g.connect(0, 1)
    g.connect(1, 2)
    g.compile()
    del p
    gc.collect()

    # If the plugin had been freed, render_block would crash.
    in_buf = np.zeros((in_ch, 256), dtype=np.float32)
    out_buf = np.zeros((out_ch, 256), dtype=np.float32)
    g.render_block([in_buf], [out_buf], 256)
