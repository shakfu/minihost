"""PluginChain dry/wet mix and PluginGraph parallel routing."""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost

# Prefer an effect plugin with matching in/out channels for these tests.
# MINIHOST_TEST_FX overrides; default falls back to a couple of common locations.
FX_PLUGIN = (
    os.environ.get("MINIHOST_TEST_FX")
    or "/Library/Audio/Plug-Ins/VST3/TAL-Filter-2.vst3"
)

skip_if_no_fx = pytest.mark.skipif(
    not os.path.exists(FX_PLUGIN),
    reason=f"effect plugin not found at {FX_PLUGIN}",
)


def _make_chain():
    p = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    return minihost.PluginChain([p]), p


# ---------------------------------------------------------------------------
# PluginChain dry/wet mix
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_chain_default_mix_is_full_wet():
    chain, _p = _make_chain()
    try:
        assert chain.get_mix(0) == pytest.approx(1.0)
    finally:
        chain.close()


@skip_if_no_fx
def test_chain_set_mix_rejects_out_of_range():
    chain, _p = _make_chain()
    try:
        with pytest.raises(RuntimeError, match=r"\[0\.0, 1\.0\]"):
            chain.set_mix(0, -0.1)
        with pytest.raises(RuntimeError, match=r"\[0\.0, 1\.0\]"):
            chain.set_mix(0, 1.5)
    finally:
        chain.close()


@skip_if_no_fx
def test_chain_set_mix_rejects_bad_index():
    chain, _p = _make_chain()
    try:
        with pytest.raises(RuntimeError, match="out of range"):
            chain.set_mix(99, 0.5)
        with pytest.raises(RuntimeError, match="out of range"):
            chain.set_mix(-1, 0.5)
    finally:
        chain.close()


@skip_if_no_fx
def test_chain_mix_zero_yields_dry_input():
    chain, _p = _make_chain()
    try:
        chain.set_mix(0, 0.0)
        inp = np.full((2, 256), 0.25, dtype=np.float32)
        out = np.zeros((2, 256), dtype=np.float32)
        chain.process(inp, out)
        # At full dry, the chain output should exactly equal the input.
        assert np.allclose(out, inp)
    finally:
        chain.close()


@skip_if_no_fx
def test_chain_mix_half_is_average_of_dry_and_wet():
    # Reference wet on a fresh chain
    wet_chain, _ = _make_chain()
    inp = np.full((2, 256), 0.25, dtype=np.float32)
    wet_out = np.zeros((2, 256), dtype=np.float32)
    wet_chain.process(inp, wet_out)
    wet_chain.close()

    # Mix=0.5 on another fresh chain -- output should be (dry + wet) / 2.
    mid_chain, _ = _make_chain()
    mid_chain.set_mix(0, 0.5)
    mid_out = np.zeros((2, 256), dtype=np.float32)
    mid_chain.process(inp, mid_out)
    mid_chain.close()

    expected = 0.5 * inp + 0.5 * wet_out
    assert np.allclose(mid_out, expected, atol=1e-6)


@skip_if_no_fx
def test_chain_get_mix_reflects_clamped_value():
    chain, _p = _make_chain()
    try:
        chain.set_mix(0, 0.3)
        assert chain.get_mix(0) == pytest.approx(0.3, abs=1e-6)
    finally:
        chain.close()


def test_chain_set_mix_rejects_mismatched_channels():
    # Use a synth-style plugin (Dexed: 0 in, 2 out) to exercise the
    # eligibility check. Skip if not available.
    SYNTH = os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
    if not os.path.exists(SYNTH):
        pytest.skip(f"synth plugin not found at {SYNTH}")
    p = minihost.Plugin(SYNTH, sample_rate=48000)
    if p.num_input_channels == p.num_output_channels:
        p.close()
        pytest.skip("synth has matching channel counts; cannot test rejection")
    chain = minihost.PluginChain([p])
    try:
        with pytest.raises(RuntimeError, match="must match"):
            chain.set_mix(0, 0.5)
    finally:
        chain.close()


# ---------------------------------------------------------------------------
# PluginGraph
# ---------------------------------------------------------------------------


def test_graph_create_rejects_bad_channels():
    with pytest.raises(RuntimeError, match="channel counts must be positive"):
        minihost.PluginGraph(0, 2, max_block_size=512, sample_rate=48000.0)
    with pytest.raises(RuntimeError, match="channel counts must be positive"):
        minihost.PluginGraph(2, -1, max_block_size=512, sample_rate=48000.0)


def test_graph_create_rejects_bad_block_size():
    with pytest.raises(RuntimeError, match="max_block_size"):
        minihost.PluginGraph(2, 2, max_block_size=0, sample_rate=48000.0)


def test_graph_create_rejects_bad_sample_rate():
    with pytest.raises(RuntimeError, match="sample_rate"):
        minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=-1.0)


@skip_if_no_fx
def test_graph_empty_produces_silence():
    g = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    try:
        inp = np.full((2, 256), 0.5, dtype=np.float32)
        out = np.full((2, 256), 1.0, dtype=np.float32)  # pre-fill non-zero
        g.process(inp, out)
        # No branches -> output is zeroed.
        assert np.all(out == 0.0)
    finally:
        g.close()


@skip_if_no_fx
def test_graph_add_branch_rejects_channel_mismatch():
    chain, _p = _make_chain()
    try:
        # Graph expects 4-channel I/O; chain is 2-channel.
        g = minihost.PluginGraph(4, 4, max_block_size=512, sample_rate=48000.0)
        with pytest.raises(RuntimeError, match="input channels"):
            g.add_branch(chain)
        g.close()
    finally:
        chain.close()


@skip_if_no_fx
def test_graph_add_branch_rejects_sample_rate_mismatch():
    chain, _p = _make_chain()
    try:
        g = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=44100.0)
        with pytest.raises(RuntimeError, match="sample rate"):
            g.add_branch(chain)
        g.close()
    finally:
        chain.close()


@skip_if_no_fx
def test_graph_single_branch_matches_chain_output():
    # A graph with one unity-gain branch is equivalent to the chain itself
    # (modulo plugin state). Use a fresh chain for the reference render.
    ref_chain, _ = _make_chain()
    inp = np.full((2, 256), 0.1, dtype=np.float32)
    ref = np.zeros((2, 256), dtype=np.float32)
    ref_chain.process(inp, ref)
    ref_chain.close()

    g_chain, _ = _make_chain()
    g = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    g.add_branch(g_chain, gain=1.0)
    out = np.zeros((2, 256), dtype=np.float32)
    g.process(inp, out)
    g.close()
    g_chain.close()

    assert np.allclose(out, ref, atol=1e-6)


@skip_if_no_fx
def test_graph_sums_two_branches():
    inp = np.full((2, 256), 0.1, dtype=np.float32)

    # Two reference chains, processed independently, summed.
    rc1, _ = _make_chain()
    rc2, _ = _make_chain()
    r1 = np.zeros((2, 256), dtype=np.float32)
    r2 = np.zeros((2, 256), dtype=np.float32)
    rc1.process(inp, r1)
    rc2.process(inp, r2)
    rc1.close()
    rc2.close()
    expected = r1 + r2

    gc1, _ = _make_chain()
    gc2, _ = _make_chain()
    g = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    g.add_branch(gc1, gain=1.0)
    g.add_branch(gc2, gain=1.0)
    out = np.zeros((2, 256), dtype=np.float32)
    g.process(inp, out)
    g.close()
    gc1.close()
    gc2.close()

    assert np.allclose(out, expected, atol=1e-6)


@skip_if_no_fx
def test_graph_branch_gain_scales_contribution():
    inp = np.full((2, 256), 0.1, dtype=np.float32)

    # One-branch graph at gain=0.5 vs the same chain at gain=1.0.
    gc1, _ = _make_chain()
    g1 = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    g1.add_branch(gc1, gain=1.0)
    out_full = np.zeros((2, 256), dtype=np.float32)
    g1.process(inp, out_full)
    g1.close()
    gc1.close()

    gc2, _ = _make_chain()
    g2 = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    g2.add_branch(gc2, gain=0.5)
    out_half = np.zeros((2, 256), dtype=np.float32)
    g2.process(inp, out_half)
    g2.close()
    gc2.close()

    assert np.allclose(out_half, 0.5 * out_full, atol=1e-6)


@skip_if_no_fx
def test_graph_muted_branch_skips_processing():
    g_chain, _ = _make_chain()
    g = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    idx = g.add_branch(g_chain, gain=0.0)
    assert g.get_branch_gain(idx) == pytest.approx(0.0)

    inp = np.full((2, 256), 0.5, dtype=np.float32)
    out = np.full((2, 256), 1.0, dtype=np.float32)  # pre-fill non-zero
    g.process(inp, out)
    # Muted branch contributes nothing; output is zero.
    assert np.all(out == 0.0)
    g.close()
    g_chain.close()


@skip_if_no_fx
def test_graph_set_get_branch_gain():
    g_chain, _ = _make_chain()
    g = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    idx = g.add_branch(g_chain)
    assert g.get_branch_gain(idx) == pytest.approx(1.0)
    g.set_branch_gain(idx, 0.25)
    assert g.get_branch_gain(idx) == pytest.approx(0.25)
    with pytest.raises(RuntimeError, match="out of range"):
        g.set_branch_gain(99, 1.0)
    with pytest.raises(RuntimeError, match="out of range"):
        g.get_branch_gain(99)
    g.close()
    g_chain.close()


@skip_if_no_fx
def test_graph_latency_and_tail_are_max_across_branches():
    c1, _ = _make_chain()
    c2, _ = _make_chain()
    g = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
    g.add_branch(c1)
    g.add_branch(c2)
    # Both branches have identical latency/tail; max == single-branch value.
    assert g.latency_samples == max(c1.latency_samples, c2.latency_samples)
    assert g.tail_seconds == pytest.approx(max(c1.tail_seconds, c2.tail_seconds))
    g.close()
    c1.close()
    c2.close()


@skip_if_no_fx
def test_graph_context_manager_closes():
    c, _ = _make_chain()
    with minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0) as g:
        g.add_branch(c)
        assert g.num_branches == 1
    # After context exit, graph is closed; close is idempotent.
    g.close()
    c.close()
