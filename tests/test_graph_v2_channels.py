"""Tests for pick_channel and merge_channels nodes in PluginGraph.

These nodes let a graph reshape multi-channel audio without plugins:
extract one channel from a multi-channel source, or interleave several
1-channel sources into a multi-channel signal. They underpin the
desktop app's `channel_split` / `channel_merge` UI helpers.
"""

from __future__ import annotations

import numpy as np
import pytest

import minihost


# -------------------------------------------------------------------- #
# pick_channel                                                          #
# -------------------------------------------------------------------- #

def test_pick_channel_rejects_bad_index_at_add():
    g = minihost.PluginGraph(64, 48000.0)
    with pytest.raises(RuntimeError, match="channel_index"):
        g.add_pick_channel(in_channels=2, channel_index=2)
    with pytest.raises(RuntimeError, match="channel_index"):
        g.add_pick_channel(in_channels=2, channel_index=-1)


def test_pick_channel_rejects_zero_channels():
    g = minihost.PluginGraph(64, 48000.0)
    with pytest.raises(RuntimeError, match="in_channels"):
        g.add_pick_channel(in_channels=0, channel_index=0)


def test_pick_channel_extracts_left_from_stereo():
    F = 32
    g = minihost.PluginGraph(F, 48000.0)
    src = g.add_input(2)
    pick = g.add_pick_channel(in_channels=2, channel_index=0)
    out = g.add_output(1)
    g.connect(src, pick)
    g.connect(pick, out)
    g.compile()

    rng = np.random.default_rng(7)
    in_buf = rng.standard_normal((2, F)).astype(np.float32)
    out_buf = np.zeros((1, F), dtype=np.float32)
    g.render_block([in_buf], [out_buf], F)
    assert np.allclose(out_buf[0], in_buf[0])
    # Right channel should NOT have leaked through.
    assert not np.allclose(out_buf[0], in_buf[1])


def test_pick_channel_extracts_right_from_stereo():
    F = 16
    g = minihost.PluginGraph(F, 48000.0)
    src = g.add_input(2)
    pick = g.add_pick_channel(in_channels=2, channel_index=1)
    out = g.add_output(1)
    g.connect(src, pick)
    g.connect(pick, out)
    g.compile()

    in_buf = np.array(
        [[1.0] * F, [2.0] * F],   # left=1, right=2
        dtype=np.float32,
    )
    out_buf = np.zeros((1, F), dtype=np.float32)
    g.render_block([in_buf], [out_buf], F)
    assert np.allclose(out_buf[0], 2.0)


def test_pick_channel_channel_mismatch_at_connect():
    """A mono source can't feed pick_channel(in_channels=2)."""
    g = minihost.PluginGraph(64, 48000.0)
    mono = g.add_input(1)
    pick = g.add_pick_channel(in_channels=2, channel_index=0)
    with pytest.raises(RuntimeError, match="channel mismatch"):
        g.connect(mono, pick)


# -------------------------------------------------------------------- #
# merge_channels                                                        #
# -------------------------------------------------------------------- #

def test_merge_channels_rejects_zero_out():
    g = minihost.PluginGraph(64, 48000.0)
    with pytest.raises(RuntimeError, match="out_channels"):
        g.add_merge_channels(out_channels=0)


def test_merge_channels_interleaves_two_mono_into_stereo():
    F = 24
    g = minihost.PluginGraph(F, 48000.0)
    left  = g.add_input(1)
    right = g.add_input(1)
    merge = g.add_merge_channels(out_channels=2)
    out   = g.add_output(2)
    g.connect(left,  merge, dst_port=0)
    g.connect(right, merge, dst_port=1)
    g.connect(merge, out)
    g.compile()

    left_buf  = np.array([[1.0] * F], dtype=np.float32)
    right_buf = np.array([[2.0] * F], dtype=np.float32)
    out_buf   = np.zeros((2, F), dtype=np.float32)
    g.render_block([left_buf, right_buf], [out_buf], F)
    assert np.allclose(out_buf[0], 1.0)
    assert np.allclose(out_buf[1], 2.0)


def test_merge_channels_input_ports_are_mono():
    """Each merge_channels input port accepts 1-channel signals; a
    stereo source can't be connected to a port directly."""
    g = minihost.PluginGraph(64, 48000.0)
    stereo = g.add_input(2)
    merge  = g.add_merge_channels(out_channels=2)
    with pytest.raises(RuntimeError, match="channel mismatch"):
        g.connect(stereo, merge, dst_port=0)


def test_pick_then_merge_is_identity():
    """split-then-merge of a stereo signal should reproduce the input."""
    F = 64
    g = minihost.PluginGraph(F, 48000.0)
    src = g.add_input(2)
    pL  = g.add_pick_channel(in_channels=2, channel_index=0)
    pR  = g.add_pick_channel(in_channels=2, channel_index=1)
    mg  = g.add_merge_channels(out_channels=2)
    out = g.add_output(2)
    g.connect(src, pL)
    g.connect(src, pR)
    g.connect(pL, mg, dst_port=0)
    g.connect(pR, mg, dst_port=1)
    g.connect(mg, out)
    g.compile()

    rng = np.random.default_rng(42)
    in_buf  = rng.standard_normal((2, F)).astype(np.float32)
    out_buf = np.zeros((2, F), dtype=np.float32)
    g.render_block([in_buf], [out_buf], F)
    assert np.allclose(out_buf, in_buf)


# -------------------------------------------------------------------- #
# gain / bus via mix(1, channels)                                       #
# -------------------------------------------------------------------- #
# gain and bus aren't separate PluginGraph node kinds -- they are
# 1-input mix nodes from the graph's point of view. These tests
# confirm the existing mix(1, channels) construct meets the gain /
# bus contract; the project-format layer adds the distinct spec
# types and serialization.

def test_mix_of_one_with_gain_acts_as_gain_stage():
    F = 16
    g = minihost.PluginGraph(F, 48000.0)
    src  = g.add_input(2)
    gain = g.add_mix(num_inputs=1, channels=2)
    g.set_mix_gain(gain, 0, 0.25)
    out  = g.add_output(2)
    g.connect(src, gain, dst_port=0)
    g.connect(gain, out)
    g.compile()

    in_buf  = np.full((2, F), 2.0, dtype=np.float32)
    out_buf = np.zeros((2, F), dtype=np.float32)
    g.render_block([in_buf], [out_buf], F)
    assert np.allclose(out_buf, 0.5)


def test_mix_of_one_with_unit_gain_acts_as_bus():
    F = 8
    g = minihost.PluginGraph(F, 48000.0)
    src = g.add_input(2)
    bus = g.add_mix(num_inputs=1, channels=2)   # default gain = 1.0
    out = g.add_output(2)
    g.connect(src, bus, dst_port=0)
    g.connect(bus, out)
    g.compile()

    rng = np.random.default_rng(3)
    in_buf  = rng.standard_normal((2, F)).astype(np.float32)
    out_buf = np.zeros((2, F), dtype=np.float32)
    g.render_block([in_buf], [out_buf], F)
    assert np.allclose(out_buf, in_buf)
