"""Tests for MIDI routing in minihost.PluginGraph.

Covers the no-plugin paths: MIDI_INPUT -> MIDI_OUTPUT, validation of
edge endpoints, and the rule that audio and MIDI live on separate
edge lists.

Plugin-routed MIDI (MIDI_INPUT -> plugin -> MIDI_OUTPUT, with
mh_process_midi_io capture) requires a MIDI-producing plugin and is
covered when MINIHOST_TEST_PLUGIN points at one.

Also covers the two plugin-level topologies that had no test at all:
one MIDI source fanned to several instruments whose audio is summed,
and a plugin-to-plugin MIDI edge (MIDI effect -> instrument). The
latter needs a MIDI-emitting plugin named by MINIHOST_TEST_MIDI_FX and
skips without one.
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

MIDI_FX_PLUGIN = os.environ.get("MINIHOST_TEST_MIDI_FX")

skip_if_no_midi_fx = pytest.mark.skipif(
    not MIDI_FX_PLUGIN or not os.path.exists(MIDI_FX_PLUGIN),
    reason="MIDI-emitting plugin not set via MINIHOST_TEST_MIDI_FX",
)


# -------------------------------------------------------------------- #
# 1. MIDI topology validation                                          #
# -------------------------------------------------------------------- #


def test_midi_input_node_addable_without_audio_io():
    g = minihost.PluginGraph(64, 48000.0)
    mi = g.add_midi_input()
    assert mi >= 0
    assert g.num_input_nodes == 0  # MIDI inputs do not count as audio inputs


def test_midi_output_node_requires_incoming_midi_edge():
    g = minihost.PluginGraph(64, 48000.0)
    # Need at least one audio output to make compile happy on that
    # front, plus an unconnected MIDI_OUTPUT to trigger the error.
    inp = g.add_input(2)
    out = g.add_output(2)
    g.connect(inp, out)
    g.add_midi_output()
    with pytest.raises(RuntimeError, match="no incoming MIDI"):
        g.compile()


def test_audio_connect_rejects_midi_nodes():
    g = minihost.PluginGraph(64, 48000.0)
    mi = g.add_midi_input()
    out = g.add_output(2)
    with pytest.raises(RuntimeError, match="MIDI nodes cannot"):
        g.connect(mi, out)


def test_midi_connect_rejects_audio_only_src():
    g = minihost.PluginGraph(64, 48000.0)
    a_in = g.add_input(2)
    mo = g.add_midi_output()
    with pytest.raises(RuntimeError, match="does not produce MIDI"):
        g.connect_midi(a_in, mo)


def test_midi_connect_rejects_audio_only_dst():
    g = minihost.PluginGraph(64, 48000.0)
    mi = g.add_midi_input()
    a_out = g.add_output(2)
    with pytest.raises(RuntimeError, match="does not accept MIDI"):
        g.connect_midi(mi, a_out)


def test_midi_self_edge_rejected():
    g = minihost.PluginGraph(64, 48000.0)
    mi = g.add_midi_input()
    with pytest.raises(RuntimeError, match="self-edges"):
        g.connect_midi(mi, mi)


def test_midi_edge_overwrites_previous_on_same_dst():
    """One MIDI edge per dst: a second connect_midi swaps the source."""
    g = minihost.PluginGraph(64, 48000.0)
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
    g.render_block(
        [np.zeros((1, 8), dtype=np.float32)], [np.zeros((1, 8), dtype=np.float32)], 8
    )
    drained = g.get_midi_output_events(mo)
    assert drained == events  # came from mi2 (the later edge), not mi1


def test_post_compile_midi_connect_rejected():
    g = minihost.PluginGraph(64, 48000.0)
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
    g = minihost.PluginGraph(F, 48000.0)
    mi = g.add_midi_input()
    mo = g.add_midi_output()
    g.connect_midi(mi, mo)
    # Compile requires audio outputs; add a no-op audio path.
    a_in = g.add_input(1)
    a_out = g.add_output(1)
    g.connect(a_in, a_out)
    g.compile()

    events = [
        (0, 0x90, 60, 100),  # note on
        (4, 0xB0, 7, 80),  # CC volume
        (12, 0x80, 60, 0),  # note off
    ]
    g.set_midi_input_events(mi, events)
    audio_in = np.zeros((1, F), dtype=np.float32)
    audio_out = np.zeros((1, F), dtype=np.float32)
    g.render_block([audio_in], [audio_out], F)

    drained = g.get_midi_output_events(mo)
    assert drained == events


def test_midi_staging_cleared_after_render():
    """Staged MIDI is one-shot; the next block sees no events."""
    F = 8
    g = minihost.PluginGraph(F, 48000.0)
    mi = g.add_midi_input()
    mo = g.add_midi_output()
    g.connect_midi(mi, mo)
    a_in = g.add_input(1)
    a_out = g.add_output(1)
    g.connect(a_in, a_out)
    g.compile()

    g.set_midi_input_events(mi, [(0, 0x90, 64, 100)])
    audio_in = np.zeros((1, F), dtype=np.float32)
    audio_out = np.zeros((1, F), dtype=np.float32)
    g.render_block([audio_in], [audio_out], F)
    assert len(g.get_midi_output_events(mo)) == 1

    # Second block: no staging.
    g.render_block([audio_in], [audio_out], F)
    assert g.get_midi_output_events(mo) == []


def test_midi_fanout_to_multiple_outputs():
    F = 8
    g = minihost.PluginGraph(F, 48000.0)
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
    audio_in = np.zeros((1, F), dtype=np.float32)
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
    F = 256

    p1 = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    if not p1.accepts_midi:
        pytest.skip("test plugin does not accept MIDI")
    in_ch = p1.num_input_channels
    out_ch = p1.num_output_channels

    # An instrument reports num_input_ch == 0 and gets no audio input
    # port; compile does not require one to be wired and feeds the
    # plugin silence. This test used to skip on that shape, which meant
    # it never ran against a typical synth -- the graph's plugin MIDI
    # edge went unexercised. Wire the audio input only when there is one.
    g1 = minihost.PluginGraph(F, sr)
    pn1 = g1.add_plugin(p1)
    mi = g1.add_midi_input()
    a_out = g1.add_output(out_ch)
    src = np.zeros((max(in_ch, 1), F), dtype=np.float32)
    graph_inputs = []
    if in_ch > 0:
        a_in = g1.add_input(in_ch)
        g1.connect(a_in, pn1)
        graph_inputs = [src]
    g1.connect(pn1, a_out)
    g1.connect_midi(mi, pn1)
    g1.compile()

    events = [(0, 0x90, 60, 100), (F // 2, 0x80, 60, 0)]
    g1.set_midi_input_events(mi, events)
    out_edge = np.zeros((out_ch, F), dtype=np.float32)
    g1.render_block(graph_inputs, [out_edge], F)

    # Render via direct staging on a second plugin instance.
    p2 = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    out_direct = np.zeros((out_ch, F), dtype=np.float32)
    p2.process_midi(src, out_direct, events)

    # Without this the test passes on silence == silence, which is what
    # it would have done had the MIDI edge delivered nothing.
    assert np.max(np.abs(out_direct)) > 1e-6, "plugin produced nothing for a note-on"
    assert np.allclose(out_edge, out_direct, atol=1e-5)


@skip_if_no_plugin
def test_midi_fanout_drives_several_instruments_and_audio_sums():
    """One MIDI source into two instruments, their audio mixed.

    Fan-out on the MIDI edge list plus fan-in through a mix node. Two
    instances of the same instrument get the same note, so the mix must
    come out as exactly twice one instance's render.
    """
    sr = 48000.0
    F = 256
    events = [(0, 0x90, 60, 100)]

    p1 = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    p2 = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    if not p1.accepts_midi:
        p1.close()
        p2.close()
        pytest.skip("test plugin does not accept MIDI")
    in_ch = p1.num_input_channels
    out_ch = p1.num_output_channels
    src = np.zeros((max(in_ch, 1), F), dtype=np.float32)

    try:
        g = minihost.PluginGraph(F, sr)
        n1 = g.add_plugin(p1)
        n2 = g.add_plugin(p2)
        mi = g.add_midi_input()
        mix = g.add_mix(2, out_ch)
        a_out = g.add_output(out_ch)
        graph_inputs = []
        if in_ch > 0:
            a_in = g.add_input(in_ch)
            g.connect(a_in, n1)
            g.connect(a_in, n2)
            graph_inputs = [src]
        g.connect(n1, mix, 0)
        g.connect(n2, mix, 1)
        g.connect(mix, a_out)
        g.connect_midi(mi, n1)  # same source feeds both instruments
        g.connect_midi(mi, n2)
        g.compile()

        g.set_midi_input_events(mi, events)
        mixed = np.zeros((out_ch, F), dtype=np.float32)
        g.render_block(graph_inputs, [mixed], F)
        g.close()
    finally:
        p1.close()
        p2.close()

    solo_plugin = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    solo = np.zeros((out_ch, F), dtype=np.float32)
    try:
        solo_plugin.process_midi(src, solo, events)
    finally:
        solo_plugin.close()

    assert np.max(np.abs(solo)) > 1e-6, "instrument produced nothing for a note-on"
    assert np.allclose(mixed, solo * 2.0, atol=1e-5)


@skip_if_no_midi_fx
@skip_if_no_plugin
def test_plugin_to_plugin_midi_edge_drives_an_instrument():
    """MIDI effect -> instrument, wired as a MIDI edge between plugins.

    The graph advertises plugin nodes as both MIDI sources (when they
    report produces_midi) and MIDI sinks, but nothing tested the two
    joined together.
    """
    sr = 48000.0
    F = 256
    # Several blocks, not one: a MIDI effect need not answer in the same
    # block it was fed. Chord Prism 2, for instance, emits its
    # transformed note one block later, so a single-block test would
    # only ever see silence.
    blocks = 16
    events = [(0, 0x90, 60, 100)]

    fx = minihost.Plugin(MIDI_FX_PLUGIN, sample_rate=sr, max_block_size=F)
    synth = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    try:
        if not fx.produces_midi:
            pytest.skip("configured MINIHOST_TEST_MIDI_FX reports no MIDI output")
        if synth.num_input_channels > 0:
            pytest.skip("instrument expects audio input; wiring is topology-specific")
        out_ch = synth.num_output_channels
        fx_in_ch = fx.num_input_channels

        g = minihost.PluginGraph(F, sr)
        n_fx = g.add_plugin(fx)
        n_syn = g.add_plugin(synth)
        mi = g.add_midi_input()
        a_out = g.add_output(out_ch)
        graph_inputs = []
        if fx_in_ch > 0:
            a_in = g.add_input(fx_in_ch)
            g.connect(a_in, n_fx)
            graph_inputs = [np.zeros((fx_in_ch, F), dtype=np.float32)]
        g.connect(n_syn, a_out)
        g.connect_midi(mi, n_fx)
        g.connect_midi(n_fx, n_syn)  # the edge under test
        g.compile()

        block = np.zeros((out_ch, F), dtype=np.float32)
        rendered = []
        for index in range(blocks):
            g.set_midi_input_events(mi, events if index == 0 else [])
            g.render_block(graph_inputs, [block], F)
            rendered.append(block.copy())
        through_graph = np.concatenate(rendered, axis=1)
        g.close()
    finally:
        fx.close()
        synth.close()

    # Reference: pump the effect's MIDI output into the instrument by hand.
    fx2 = minihost.Plugin(MIDI_FX_PLUGIN, sample_rate=sr, max_block_size=F)
    synth2 = minihost.Plugin(PLUGIN, sample_rate=sr, max_block_size=F)
    try:
        fx_out = np.zeros((max(fx2.num_output_channels, 1), F), dtype=np.float32)
        fx_in = np.zeros((max(fx2.num_input_channels, 1), F), dtype=np.float32)
        synth_in = np.zeros((max(synth2.num_input_channels, 1), F), dtype=np.float32)
        synth_out = np.zeros((synth2.num_output_channels, F), dtype=np.float32)
        manual = []
        total_emitted = 0
        for index in range(blocks):
            emitted = fx2.process_midi(fx_in, fx_out, events if index == 0 else [])
            total_emitted += len(emitted)
            synth2.process_midi(synth_in, synth_out, emitted)
            manual.append(synth_out.copy())
        by_hand = np.concatenate(manual, axis=1)
    finally:
        fx2.close()
        synth2.close()

    if total_emitted == 0:
        pytest.skip(f"MIDI effect emitted nothing across {blocks} blocks")
    assert np.max(np.abs(by_hand)) > 1e-6, "hand-routed reference is silent"
    assert np.allclose(through_graph, by_hand, atol=1e-5)
