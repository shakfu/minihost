"""Tests for MH_NODE_MIDI_PROCESSOR (filter, transpose, velocity
curve) and MH_NODE_MIDI_MERGE in PluginGraph.

The processor / merge nodes let MIDI streams be reshaped inside the
graph without a plugin: filter by channel + note range, transpose,
remap velocity, or fan multiple sources into one consumer. Pair with
MIDI_INPUT -> ... -> MIDI_OUTPUT for end-to-end coverage that doesn't
require a plugin.
"""

from __future__ import annotations

import numpy as np
import pytest

import minihost


OP_FILTER = 0
OP_TRANSPOSE = 1
OP_VELOCITY_CURVE = 2


def _setup(F=8, channels=1):
    """Common audio scaffold: graph_v2 needs at least one audio output
    for compile to succeed."""
    g = minihost.PluginGraph(F, 48000.0)
    a_in = g.add_input(channels)
    a_out = g.add_output(channels)
    g.connect(a_in, a_out)
    return g, F, channels


# -------------------------------------------------------------------- #
# midi_filter                                                          #
# -------------------------------------------------------------------- #


def test_filter_blocks_notes_outside_range():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    proc = g.add_midi_processor(
        dict(op=OP_FILTER, min_note=60, max_note=72, channel_mask=0xFFFF)
    )
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    events = [
        (0, 0x90, 59, 100),  # B3 -- below range, blocked
        (1, 0x90, 60, 100),  # C4 -- in range, kept
        (2, 0x90, 72, 100),  # C5 -- in range (inclusive upper)
        (3, 0x90, 73, 100),  # above range, blocked
        (4, 0x80, 60, 0),  # note off in range, kept
        (5, 0x80, 80, 0),  # note off out of range, blocked
        (6, 0xB0, 7, 80),  # CC -- not subject to note range
    ]
    g.set_midi_input_events(mi, events)
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    drained = g.get_midi_output_events(mo)
    assert drained == [
        (1, 0x90, 60, 100),
        (2, 0x90, 72, 100),
        (4, 0x80, 60, 0),
        (6, 0xB0, 7, 80),
    ]


def test_filter_channel_mask():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    # Pass channels 0, 5 only (bits 0 and 5).
    proc = g.add_midi_processor(
        dict(op=OP_FILTER, min_note=0, max_note=127, channel_mask=(1 << 0) | (1 << 5))
    )
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    events = [
        (0, 0x90, 60, 100),  # ch 0 -- kept
        (1, 0x91, 60, 100),  # ch 1 -- blocked
        (2, 0x95, 60, 100),  # ch 5 -- kept
        (3, 0xB1, 7, 80),  # ch 1 CC -- blocked
        (4, 0xF0, 0, 0),  # system -- always passes
    ]
    g.set_midi_input_events(mi, events)
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    drained = g.get_midi_output_events(mo)
    assert drained == [
        (0, 0x90, 60, 100),
        (2, 0x95, 60, 100),
        (4, 0xF0, 0, 0),
    ]


# -------------------------------------------------------------------- #
# midi_transpose                                                       #
# -------------------------------------------------------------------- #


def test_transpose_shifts_note_numbers():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    proc = g.add_midi_processor(dict(op=OP_TRANSPOSE, transpose_semitones=12))
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    g.set_midi_input_events(
        mi,
        [
            (0, 0x90, 60, 100),  # C4 -> C5
            (1, 0x80, 60, 0),  # C4 off -> C5 off
            (2, 0xB0, 7, 80),  # CC, unchanged
        ],
    )
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    assert g.get_midi_output_events(mo) == [
        (0, 0x90, 72, 100),
        (1, 0x80, 72, 0),
        (2, 0xB0, 7, 80),
    ]


def test_transpose_drops_events_pushed_out_of_range():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    proc = g.add_midi_processor(dict(op=OP_TRANSPOSE, transpose_semitones=24))
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    g.set_midi_input_events(
        mi,
        [
            (0, 0x90, 100, 100),  # 100 + 24 = 124 -- in range
            (1, 0x90, 110, 100),  # 110 + 24 = 134 -- dropped
            (2, 0x90, 127, 100),  # 127 + 24 = 151 -- dropped
        ],
    )
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    drained = g.get_midi_output_events(mo)
    assert drained == [(0, 0x90, 124, 100)]


# -------------------------------------------------------------------- #
# midi_velocity_curve                                                  #
# -------------------------------------------------------------------- #


def test_velocity_curve_gamma_one_is_identity():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    proc = g.add_midi_processor(dict(op=OP_VELOCITY_CURVE, velocity_gamma=1.0))
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    events = [(0, 0x90, 60, 64), (1, 0x90, 61, 100)]
    g.set_midi_input_events(mi, events)
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    assert g.get_midi_output_events(mo) == events


def test_velocity_curve_gamma_lt_one_boosts_softer_notes():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    proc = g.add_midi_processor(dict(op=OP_VELOCITY_CURVE, velocity_gamma=0.5))
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    # In: vel 32 (≈0.252). gamma=0.5 -> sqrt(0.252) ≈ 0.502 -> 64.
    g.set_midi_input_events(mi, [(0, 0x90, 60, 32)])
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    drained = g.get_midi_output_events(mo)
    assert len(drained) == 1
    _, status, note, vel = drained[0]
    assert status == 0x90 and note == 60
    assert 60 <= vel <= 68


def test_velocity_curve_preserves_note_off_disguised_as_velocity_zero():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    proc = g.add_midi_processor(dict(op=OP_VELOCITY_CURVE, velocity_gamma=0.5))
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    g.set_midi_input_events(mi, [(0, 0x90, 60, 0)])  # MIDI: vel=0 == note off
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    # Must stay 0; otherwise the note never turns off downstream.
    assert g.get_midi_output_events(mo) == [(0, 0x90, 60, 0)]


# -------------------------------------------------------------------- #
# midi_merge                                                            #
# -------------------------------------------------------------------- #


def test_midi_merge_concatenates_two_sources_sorted_by_sample_offset():
    g, F, ch = _setup()
    mi_a = g.add_midi_input()
    mi_b = g.add_midi_input()
    merge = g.add_midi_merge(num_inputs=2)
    mo = g.add_midi_output()
    g.connect_midi_port(mi_a, merge, 0)
    g.connect_midi_port(mi_b, merge, 1)
    g.connect_midi(merge, mo)
    g.compile()

    g.set_midi_input_events(
        mi_a,
        [
            (0, 0x90, 60, 100),
            (5, 0x80, 60, 0),
        ],
    )
    g.set_midi_input_events(
        mi_b,
        [
            (2, 0x90, 64, 100),
            (7, 0x80, 64, 0),
        ],
    )
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    drained = g.get_midi_output_events(mo)
    assert drained == [
        (0, 0x90, 60, 100),
        (2, 0x90, 64, 100),
        (5, 0x80, 60, 0),
        (7, 0x80, 64, 0),
    ]


def test_midi_merge_rejects_dst_port_out_of_range():
    g, _F, _ch = _setup()
    mi = g.add_midi_input()
    merge = g.add_midi_merge(num_inputs=2)
    with pytest.raises(RuntimeError, match="dst_port"):
        g.connect_midi_port(mi, merge, 2)


def test_midi_merge_compile_requires_every_port_connected():
    g, _F, _ch = _setup()
    mi = g.add_midi_input()
    merge = g.add_midi_merge(num_inputs=3)
    mo = g.add_midi_output()
    g.connect_midi_port(mi, merge, 0)
    g.connect_midi_port(mi, merge, 2)
    g.connect_midi(merge, mo)
    with pytest.raises(RuntimeError, match="port 1.*no incoming"):
        g.compile()


# -------------------------------------------------------------------- #
# Topology validation                                                  #
# -------------------------------------------------------------------- #


def test_midi_processor_compile_requires_input_connected():
    g, _F, _ch = _setup()
    proc = g.add_midi_processor(dict(op=OP_FILTER))
    mo = g.add_midi_output()
    g.connect_midi(proc, mo)
    with pytest.raises(RuntimeError, match="no incoming MIDI"):
        g.compile()


def test_set_midi_processor_params_updates_op():
    g, F, ch = _setup()
    mi = g.add_midi_input()
    proc = g.add_midi_processor(dict(op=OP_TRANSPOSE, transpose_semitones=0))
    mo = g.add_midi_output()
    g.connect_midi(mi, proc)
    g.connect_midi(proc, mo)
    g.compile()

    # Bump the transposition mid-flight.
    g.set_midi_processor_params(proc, dict(op=OP_TRANSPOSE, transpose_semitones=7))
    g.set_midi_input_events(mi, [(0, 0x90, 60, 100)])
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    assert g.get_midi_output_events(mo) == [(0, 0x90, 67, 100)]


def test_processor_chain_filter_then_transpose():
    """Two processors in series: filter to a range, then transpose."""
    g, F, ch = _setup()
    mi = g.add_midi_input()
    flt = g.add_midi_processor(
        dict(op=OP_FILTER, min_note=60, max_note=67, channel_mask=0xFFFF)
    )
    tr = g.add_midi_processor(dict(op=OP_TRANSPOSE, transpose_semitones=-12))
    mo = g.add_midi_output()
    g.connect_midi(mi, flt)
    g.connect_midi(flt, tr)
    g.connect_midi(tr, mo)
    g.compile()

    g.set_midi_input_events(
        mi,
        [
            (0, 0x90, 59, 100),  # filtered out
            (1, 0x90, 60, 100),  # kept -> transposed to 48
            (2, 0x90, 65, 100),  # kept -> 53
            (3, 0x90, 68, 100),  # filtered out
        ],
    )
    g.render_block(
        [np.zeros((ch, F), dtype=np.float32)], [np.zeros((ch, F), dtype=np.float32)], F
    )
    assert g.get_midi_output_events(mo) == [
        (1, 0x90, 48, 100),
        (2, 0x90, 53, 100),
    ]
