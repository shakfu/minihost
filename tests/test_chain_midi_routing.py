"""MIDI routing through PluginChain.

A chain used to hand MIDI to its first plugin and stop there: the events
a MIDI effect emitted were reported back to the caller and never reached
the plugins behind it, so ``PluginChain([arpeggiator, synth])`` rendered
silence -- the arpeggiator swallowed the notes, the synth was never told
anything. These tests pin the routing that replaced it:

  * MIDI enters the first plugin that accepts it;
  * any plugin reporting ``produces_midi`` replaces the stream for the
    plugins after it, which is what makes MIDI effect -> instrument work;
  * a plugin reporting no MIDI output ends the stream;
  * what ``process_midi`` returns is the MIDI leaving the *last* plugin.

The instrument-behind-a-MIDI-effect cases need a MIDI-emitting plugin
(an arpeggiator, a chorder, a MIDI echo) named by MINIHOST_TEST_MIDI_FX
and skip without one. They were written against Chord Prism 2, which
transposes an incoming C4 up an octave; nothing here depends on the
specific transformation, only that the instrument sounds at all.
"""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost

SYNTH_PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
FX_PLUGIN = (
    os.environ.get("MINIHOST_TEST_FX") or "/Library/Audio/Plug-Ins/VST3/TAL-Filter-2.vst3"
)
MIDI_FX_PLUGIN = os.environ.get("MINIHOST_TEST_MIDI_FX")

skip_if_no_synth = pytest.mark.skipif(
    not os.path.exists(SYNTH_PLUGIN),
    reason=f"instrument not found at {SYNTH_PLUGIN}",
)
skip_if_no_fx = pytest.mark.skipif(
    not os.path.exists(FX_PLUGIN),
    reason=f"effect plugin not found at {FX_PLUGIN}",
)
skip_if_no_midi_fx = pytest.mark.skipif(
    not MIDI_FX_PLUGIN or not os.path.exists(MIDI_FX_PLUGIN),
    reason="MIDI-emitting plugin not set via MINIHOST_TEST_MIDI_FX",
)

BLOCK = 512
BLOCKS = 24
NOTE_ON = [(0, 0x90, 60, 100)]


def _open(path: str) -> minihost.Plugin:
    return minihost.Plugin(path, sample_rate=48000, max_block_size=BLOCK)


def _render(chain: minihost.PluginChain, midi, blocks: int = BLOCKS) -> np.ndarray:
    """Run a note through a chain and return the concatenated output."""
    in_ch = max(chain.num_input_channels, 1)
    out_ch = chain.num_output_channels
    silence = np.zeros((in_ch, BLOCK), dtype=np.float32)
    out = np.zeros((out_ch, BLOCK), dtype=np.float32)
    captured = []
    for index in range(blocks):
        chain.process_midi(silence, out, midi if index == 0 else [])
        captured.append(out.copy())
    return np.concatenate(captured, axis=1)


def _peak(audio: np.ndarray) -> float:
    return float(np.max(np.abs(audio)))


# ---------------------------------------------------------------------------
# the regression: a MIDI effect ahead of an instrument
# ---------------------------------------------------------------------------


@skip_if_no_midi_fx
@skip_if_no_synth
def test_midi_effect_drives_an_instrument_behind_it():
    # The case that was broken: the MIDI effect consumed the note, emitted
    # its own, and the chain dropped it. Output was digital silence.
    fx = _open(MIDI_FX_PLUGIN)
    synth = _open(SYNTH_PLUGIN)
    try:
        if not fx.produces_midi:
            pytest.skip("configured MINIHOST_TEST_MIDI_FX reports no MIDI output")
        with minihost.PluginChain([fx, synth]) as chain:
            audio = _render(chain, NOTE_ON)
    finally:
        fx.close()
        synth.close()

    assert _peak(audio) > 1e-6, "instrument stayed silent behind the MIDI effect"


@skip_if_no_midi_fx
@skip_if_no_synth
def test_chain_matches_feeding_the_effects_output_by_hand():
    # Equivalence: routing inside the chain must produce what a caller
    # gets by pumping the MIDI effect's output into the instrument
    # block by block themselves.
    fx = _open(MIDI_FX_PLUGIN)
    synth = _open(SYNTH_PLUGIN)
    try:
        if not fx.produces_midi:
            pytest.skip("configured MINIHOST_TEST_MIDI_FX reports no MIDI output")

        in_ch = max(fx.num_input_channels, 1)
        silence = np.zeros((in_ch, BLOCK), dtype=np.float32)
        fx_out = np.zeros((max(fx.num_output_channels, 1), BLOCK), dtype=np.float32)
        synth_out = np.zeros((synth.num_output_channels, BLOCK), dtype=np.float32)
        synth_in = np.zeros((max(synth.num_input_channels, 1), BLOCK), dtype=np.float32)

        manual = []
        for index in range(BLOCKS):
            events = fx.process_midi(silence, fx_out, NOTE_ON if index == 0 else [])
            synth.process_midi(synth_in, synth_out, events)
            manual.append(synth_out.copy())
        by_hand = np.concatenate(manual, axis=1)
    finally:
        fx.close()
        synth.close()

    if _peak(by_hand) <= 1e-6:
        pytest.skip("hand-routed reference is silent; nothing to compare")

    fx2 = _open(MIDI_FX_PLUGIN)
    synth2 = _open(SYNTH_PLUGIN)
    try:
        with minihost.PluginChain([fx2, synth2]) as chain:
            through_chain = _render(chain, NOTE_ON)
    finally:
        fx2.close()
        synth2.close()

    assert np.allclose(through_chain, by_hand, atol=1e-4)


@skip_if_no_midi_fx
@skip_if_no_synth
def test_routing_survives_an_effect_after_the_instrument():
    # midi_fx -> instrument -> audio effect: the audio stage must not
    # disturb the MIDI that reached the instrument ahead of it.
    fx = _open(MIDI_FX_PLUGIN)
    synth = _open(SYNTH_PLUGIN)
    try:
        if not fx.produces_midi:
            pytest.skip("configured MINIHOST_TEST_MIDI_FX reports no MIDI output")
        if not os.path.exists(FX_PLUGIN):
            pytest.skip(f"effect plugin not found at {FX_PLUGIN}")
        audio_fx = _open(FX_PLUGIN)
        try:
            with minihost.PluginChain([fx, synth, audio_fx]) as chain:
                audio = _render(chain, NOTE_ON)
        finally:
            audio_fx.close()
    finally:
        fx.close()
        synth.close()

    assert _peak(audio) > 1e-6


# ---------------------------------------------------------------------------
# the rules that bound the routing
# ---------------------------------------------------------------------------


@skip_if_no_synth
@skip_if_no_fx
def test_a_plugin_producing_no_midi_ends_the_stream():
    # Documented contract, and the reason MIDI effects must come first:
    # an effect that produces no MIDI terminates the stream, so an
    # instrument placed behind it never hears the note.
    audio_fx = _open(FX_PLUGIN)
    synth = _open(SYNTH_PLUGIN)
    try:
        if audio_fx.produces_midi:
            pytest.skip(f"{FX_PLUGIN} reports MIDI output; not a terminating stage")
        with minihost.PluginChain([audio_fx, synth]) as chain:
            audio = _render(chain, NOTE_ON)
    finally:
        audio_fx.close()
        synth.close()

    assert _peak(audio) <= 1e-6


@skip_if_no_synth
@skip_if_no_fx
def test_midi_out_is_empty_when_the_last_plugin_produces_none():
    # process_midi reports what leaves the chain, so a chain ending in a
    # plugin with produces_midi == 0 reports nothing.
    synth = _open(SYNTH_PLUGIN)
    audio_fx = _open(FX_PLUGIN)
    try:
        if audio_fx.produces_midi:
            pytest.skip(f"{FX_PLUGIN} reports MIDI output")
        silence = np.zeros((max(synth.num_input_channels, 1), BLOCK), dtype=np.float32)
        out = np.zeros((audio_fx.num_output_channels, BLOCK), dtype=np.float32)
        with minihost.PluginChain([synth, audio_fx]) as chain:
            events = chain.process_midi(silence, out, NOTE_ON)
    finally:
        synth.close()
        audio_fx.close()

    assert events == []


@skip_if_no_synth
def test_single_plugin_chain_matches_the_plugin_alone():
    # The common case must be untouched by the routing change: one
    # plugin in a chain behaves exactly like the plugin by itself.
    direct_plugin = _open(SYNTH_PLUGIN)
    chained_plugin = _open(SYNTH_PLUGIN)
    try:
        in_ch = max(direct_plugin.num_input_channels, 1)
        silence = np.zeros((in_ch, BLOCK), dtype=np.float32)
        out_direct = np.zeros((direct_plugin.num_output_channels, BLOCK), dtype=np.float32)

        direct = []
        for index in range(BLOCKS):
            direct_plugin.process_midi(silence, out_direct, NOTE_ON if index == 0 else [])
            direct.append(out_direct.copy())
        direct_audio = np.concatenate(direct, axis=1)

        with minihost.PluginChain([chained_plugin]) as chain:
            chain_audio = _render(chain, NOTE_ON)
    finally:
        direct_plugin.close()
        chained_plugin.close()

    assert _peak(direct_audio) > 1e-6, "instrument produced nothing for a note-on"
    assert np.allclose(chain_audio, direct_audio, atol=1e-4)


@skip_if_no_synth
@skip_if_no_fx
def test_instrument_into_effect_still_hears_midi():
    # The arrangement that always worked -- instrument first, effect
    # second -- must keep working: the chain output has to match the two
    # plugins driven by hand.
    synth = _open(SYNTH_PLUGIN)
    audio_fx = _open(FX_PLUGIN)
    try:
        silence = np.zeros((max(synth.num_input_channels, 1), BLOCK), dtype=np.float32)
        synth_out = np.zeros((synth.num_output_channels, BLOCK), dtype=np.float32)
        fx_out = np.zeros((audio_fx.num_output_channels, BLOCK), dtype=np.float32)

        manual = []
        for index in range(BLOCKS):
            synth.process_midi(silence, synth_out, NOTE_ON if index == 0 else [])
            audio_fx.process(synth_out, fx_out)
            manual.append(fx_out.copy())
        by_hand = np.concatenate(manual, axis=1)
    finally:
        synth.close()
        audio_fx.close()

    synth2 = _open(SYNTH_PLUGIN)
    fx2 = _open(FX_PLUGIN)
    try:
        with minihost.PluginChain([synth2, fx2]) as chain:
            chain_audio = _render(chain, NOTE_ON)
    finally:
        synth2.close()
        fx2.close()

    assert _peak(by_hand) > 1e-6, "instrument into effect produced nothing"
    assert np.allclose(chain_audio, by_hand, atol=1e-4)
