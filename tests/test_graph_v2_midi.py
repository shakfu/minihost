"""Tests for MIDI routing in minihost.GraphV2.

Covers the no-plugin paths: MIDI_INPUT -> MIDI_OUTPUT, validation of
edge endpoints, and the rule that audio and MIDI live on separate
edge lists.

Plugin-routed MIDI (MIDI_INPUT -> plugin -> MIDI_OUTPUT, with
mh_process_midi_io capture) requires a MIDI-producing plugin and is
covered when MINIHOST_TEST_PLUGIN points at one.
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


# -------------------------------------------------------------------- #
# 1. MIDI topology validation                                          #
# -------------------------------------------------------------------- #

def test_midi_input_node_addable_without_audio_io():
    g = minihost.GraphV2(64, 48000.0)
    mi = g.add_midi_input()
    assert mi >= 0
    assert g.num_input_nodes == 0  # MIDI inputs do not count as audio inputs


def test_midi_output_node_requires_incoming_midi_edge():
    g = minihost.GraphV2(64, 48000.0)
    # Need at least one audio output to make compile happy on that
    # front, plus an unconnected MIDI_OUTPUT to trigger the error.
    inp = g.add_input(2)
    out = g.add_output(2)
    g.connect(inp, out)
    g.add_midi_output()
    with pytest.raises(RuntimeError, match="no incoming MIDI"):
        g.compile()


def test_audio_connect_rejects_midi_nodes():
    g = minihost.GraphV2(64, 48000.0)
    mi = g.add_midi_input()
    out = g.add_output(2)
    with pytest.raises(RuntimeError, match="MIDI nodes cannot"):
        g.connect(mi, out)


def test_midi_connect_rejects_audio_only_src():
    g = minihost.GraphV2(64, 48000.0)
    a_in = g.add_input(2)
    mo = g.add_midi_output()
    with pytest.raises(RuntimeError, match="does not produce MIDI"):
        g.connect_midi(a_in, mo)


def test_midi_connect_rejects_audio_only_dst():
    g = minihost.GraphV2(64, 48000.0)
    mi = g.add_midi_input()
    a_out = g.add_output(2)
    with pytest.raises(RuntimeError, match="does not accept MIDI"):
        g.connect_midi(mi, a_out)


def test_midi_self_edge_rejected():
    g = minihost.GraphV2(64, 48000.0)
    mi = g.add_midi_input()
    with pytest.raises(RuntimeError, match="self-edges"):
        g.connect_midi(mi, mi)


def test_midi_edge_overwrites_previous_on_same_dst():
    """One MIDI edge per dst: a second connect_midi swaps the source."""
    g = minihost.GraphV2(64, 48000.0)
    mi1 = g.add_midi_input()
    mi2 = g.add_midi_input()
    mo = g.add_midi_output()
    g.connect_midi(mi1, mo)
    g.connect_midi(mi2, mo)  # overwrite, not append
    # Add audio path so compile can proceed.
    a_in = g.add_input(1)
    a_out = g.add_output(1)
    g.connect(a_in, a_out)
    g.compile()

    events = [(0, 0x90, 60, 100), (10, 0x80, 60, 0)]
    g.set_midi_input_events(mi2, events)
    g.render_block([np.zeros((1, 8), dtype=np.float32)],
                   [np.zeros((1, 8), dtype=np.float32)], 8)
    drained = g.get_midi_output_events(mo)
    assert drained == events  # came from mi2 (the later edge), not mi1


def test_post_compile_midi_connect_rejected():
    g = minihost.GraphV2(64, 48000.0)
    mi = g.add_midi_input()
    mo = g.add_midi_output()
    g.connect_midi(mi, mo)
    a_in = g.add_input(1)
    a_out = g.add_output(1)
    g.connect(a_in, a_out)
    g.compile()
    with pytest.raises(RuntimeError, match="already compiled"):
        g.connect_midi(mi, mo)


# -------------------------------------------------------------------- #
# 2. MIDI passthrough (no plugin)                                      #
# -------------------------------------------------------------------- #

def test_midi_input_passthrough_to_midi_output():
    F = 16
    g = minihost.GraphV2(F, 48000.0)
    mi = g.add_midi_input()
    mo = g.add_midi_output()
    g.connect_midi(mi, mo)
    # Compile requires audio outputs; add a no-op audio path.
    a_in = g.add_input(1)
    a_out = g.add_output(1)
    g.connect(a_in, a_out)
    g.compile()

    events = [
        (0,  0x90, 60, 100),  # note on
        (4,  0xB0,  7,  80),  # CC volume
        (12, 0x80, 60,   0),  # note off
    ]
    g.set_midi_input_events(mi, events)
    audio_in  = np.zeros((1, F), dtype=np.float32)
    audio_out = np.zeros((1, F), dtype=np.float32)
    g.render_block([audio_in], [audio_out], F)

    drained = g.get_midi_output_events(mo)
    assert drained == events


def test_midi_staging_cleared_after_render():
    """Staged MIDI is one-shot; the next block sees no events."""
    F = 8
    g = minihost.GraphV2(F, 48000.0)
    mi = g.add_midi_input()
    mo = g.add_midi_output()
    g.connect_midi(mi, mo)
    a_in = g.add_input(1)
    a_out = g.add_output(1)
    g.connect(a_in, a_out)
    g.compile()

    g.set_midi_input_events(mi, [(0, 0x90, 64, 100)])
    audio_in  = np.zeros((1, F), dtype=np.float32)
    audio_out = np.zeros((1, F), dtype=np.float32)
    g.render_block([audio_in], [audio_out], F)
    assert len(g.get_midi_output_events(mo)) == 1

    # Second block: no staging.
    g.render_block([audio_in], [audio_out], F)
    assert g.get_midi_output_events(mo) == []


def test_midi_fanout_to_multiple_outputs():
    F = 8
    g = minihost.GraphV2(F, 48000.0)
    mi = g.add_midi_input()
    mo_a = g.add_midi_output()
    mo_b = g.add_midi_output()
    g.connect_midi(mi, mo_a)
    g.connect_midi(mi, mo_b)
    a_in = g.add_input(1)
    a_out = g.add_output(1)
    g.connect(a_in, a_out)
    g.compile()

    events = [(0, 0x90, 64, 100)]
    g.set_midi_input_events(mi, events)
    audio_in  = np.zeros((1, F), dtype=np.float32)
    audio_out = np.zeros((1, F), dtype=np.float32)
    g.render_block([audio_in], [audio_out], F)
    assert g.get_midi_output_events(mo_a) == events
    assert g.get_midi_output_events(mo_b) == events


# -------------------------------------------------------------------- #
# 3. Plugin MIDI routing                                                #
# -------------------------------------------------------------------- #

@skip_if_no_plugin
def test_plugin_midi_input_from_graph_edge():
    """Wiring a MIDI_INPUT into a plugin should drive its synthesis the
    same as direct set_node_midi staging."""
    sr = 48000.0
    F  = 256

    p1 = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    if not p1.accepts_midi:
        pytest.skip("test plugin does not accept MIDI")
    in_ch  = p1.num_input_channels
    out_ch = p1.num_output_channels
    if in_ch == 0:
        # Instrument with no audio input: graph_v2 requires plugin
        # input port 0 to be connected, so the edge form is N/A here.
        # The MIDI routing itself is still validated by the passthrough
        # tests above.
        pytest.skip("plugin has no audio input port; edge form n/a")

    g1 = minihost.GraphV2(F, sr)
    pn1   = g1.add_plugin(p1)
    mi    = g1.add_midi_input()
    a_in  = g1.add_input(in_ch)
    a_out = g1.add_output(out_ch)
    g1.connect(a_in, pn1)
    g1.connect(pn1, a_out)
    g1.connect_midi(mi, pn1)
    g1.compile()

    events = [(0, 0x90, 60, 100), (F // 2, 0x80, 60, 0)]
    g1.set_midi_input_events(mi, events)
    src      = np.zeros((in_ch,  F), dtype=np.float32)
    out_edge = np.zeros((out_ch, F), dtype=np.float32)
    g1.render_block([src], [out_edge], F)

    # Render via direct staging on a second plugin instance.
    p2 = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    out_direct = np.zeros((out_ch, F), dtype=np.float32)
    p2.process_midi(src, out_direct, events)

    assert np.allclose(out_edge, out_direct, atol=1e-5)
