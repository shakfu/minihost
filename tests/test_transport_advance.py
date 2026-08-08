"""Offline renders must advance the host playhead (H5 / H6 in REVIEW.md).

Two separate omissions:

  * `MidiRenderer` never called `set_transport` at all, even though it already
    parses the file's tempo map to place events. Anything tempo-synced -- a
    synced delay, an arpeggiator, an LFO, a step sequencer -- therefore ran at
    its own default tempo with the playhead pinned at sample 0, so rendering a
    MIDI file did not follow that file's tempo.
  * `process_audio(..., bpm=...)` called `set_transport` exactly once, before
    the block loop, with `position_samples=0`. The tempo was right but time
    stood still for the whole render.

Verifying this without a plugin that visibly reacts to host position is the
awkward part. `MH_PlayHead` is written by minihost and read by the plugin, and
nothing reads it back out -- so these tests assert on what minihost *sends*, by
recording the `set_transport` calls, plus a real end-to-end render to prove the
wiring is actually exercised.

H6: `midi` and `sidechain` together used to collect MIDI events and then drop
them, because `mh_process_sidechain` is the only process entry point with no
MIDI parameter. That combination is now rejected.
"""

from __future__ import annotations

import os

import pytest

import minihost
from minihost.render import _seconds_to_beats_and_bpm

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)

TPQ = 480


class _TransportSpy:
    """Wraps a Plugin, recording set_transport calls and forwarding the rest."""

    def __init__(self, plugin):
        self._plugin = plugin
        self.calls: list[dict] = []

    def set_transport(self, **kwargs):
        self.calls.append(kwargs)
        return self._plugin.set_transport(**kwargs)

    def __getattr__(self, name):
        return getattr(self._plugin, name)


# --- the tempo-map inverse -------------------------------------------- #


def test_seconds_to_beats_constant_tempo():
    beats, bpm = _seconds_to_beats_and_bpm(2.0, [(0, 500_000.0)], TPQ)
    assert beats == pytest.approx(4.0)
    assert bpm == pytest.approx(120.0)


def test_seconds_to_beats_across_a_tempo_change():
    """Musical time must follow the tempo map, not wall-clock scaled by one
    tempo -- otherwise anything synced drifts the moment the tempo changes.
    """
    # 120 BPM for 4 beats (2 s), then 240 BPM.
    tempo_map = [(0, 500_000.0), (4 * TPQ, 250_000.0)]
    beats, bpm = _seconds_to_beats_and_bpm(2.0, tempo_map, TPQ)
    assert beats == pytest.approx(4.0)
    assert bpm == pytest.approx(240.0)

    # One further second at 240 BPM is four more beats.
    beats, bpm = _seconds_to_beats_and_bpm(3.0, tempo_map, TPQ)
    assert beats == pytest.approx(8.0)
    assert bpm == pytest.approx(240.0)


def test_seconds_to_beats_handles_an_empty_map():
    beats, bpm = _seconds_to_beats_and_bpm(1.0, [], TPQ)
    assert beats == 0.0
    assert bpm == pytest.approx(120.0)


# --- process_audio ----------------------------------------------------- #


@skip_if_no_plugin
def test_process_audio_advances_the_playhead():
    """Pre-fix: one call, position_samples=0, frozen for the whole render."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        spy = _TransportSpy(plugin)
        audio = minihost.AudioBuffer(max(plugin.num_input_channels, 1), 4096)
        minihost.process_audio(spy, audio, bpm=120.0, block_size=512)

        assert len(spy.calls) > 1, "transport must be pushed per block, not once"
        positions = [c["position_samples"] for c in spy.calls]
        assert positions == sorted(positions), "playhead must not go backwards"
        assert positions[0] == 0
        assert positions[-1] > 0, "playhead never advanced past the first block"
        assert all(c["bpm"] == pytest.approx(120.0) for c in spy.calls)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_process_audio_beats_track_the_tempo():
    """At 120 BPM, 48000 samples is one second is two quarter notes."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        spy = _TransportSpy(plugin)
        audio = minihost.AudioBuffer(max(plugin.num_input_channels, 1), 48000)
        minihost.process_audio(spy, audio, bpm=120.0, block_size=512)

        for call in spy.calls:
            expected = (call["position_samples"] / 48000.0) * 2.0
            assert call["position_beats"] == pytest.approx(expected, abs=1e-6)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_process_audio_without_bpm_sets_no_transport():
    """No bpm means no opinion about tempo -- do not fabricate one."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        spy = _TransportSpy(plugin)
        audio = minihost.AudioBuffer(max(plugin.num_input_channels, 1), 2048)
        minihost.process_audio(spy, audio, block_size=512)
        assert spy.calls == []
    finally:
        plugin.close()


# --- MidiRenderer ------------------------------------------------------ #


def _make_midi(tmp_path, bpm=120.0, notes=8):
    mf = minihost.MidiFile()
    mf.ticks_per_quarter = TPQ
    track = mf.add_track()
    mf.add_tempo(track, 0, bpm)
    for i in range(notes):
        mf.add_note_on(track, i * TPQ, 0, 60, 100)
        mf.add_note_off(track, i * TPQ + TPQ // 2, 0, 60)
    path = tmp_path / "t.mid"
    assert mf.save(str(path))
    return str(path)


@skip_if_no_plugin
def test_midi_renderer_advances_the_playhead(tmp_path):
    """Pre-fix: MidiRenderer never called set_transport at all."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        spy = _TransportSpy(plugin)
        renderer = minihost.MidiRenderer(
            spy, _make_midi(tmp_path), block_size=512, tail_seconds=0.5
        )
        while not renderer.is_finished:
            renderer.render_block()

        assert spy.calls, "renderer must drive the transport"
        positions = [c["position_samples"] for c in spy.calls]
        assert positions == sorted(positions)
        assert positions[-1] > positions[0]
    finally:
        plugin.close()


@skip_if_no_plugin
def test_midi_renderer_uses_the_files_tempo(tmp_path):
    """The tempo handed to the plugin must come from the MIDI file, not a
    default -- the renderer already parses the map to place events.
    """
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        spy = _TransportSpy(plugin)
        renderer = minihost.MidiRenderer(
            spy, _make_midi(tmp_path, bpm=90.0), block_size=512, tail_seconds=0.25
        )
        while not renderer.is_finished:
            renderer.render_block()

        assert spy.calls
        assert all(c["bpm"] == pytest.approx(90.0, abs=0.5) for c in spy.calls), (
            f"expected 90 BPM from the file, saw "
            f"{sorted({round(c['bpm'], 2) for c in spy.calls})}"
        )
        # 90 BPM => 1.5 beats per second.
        for call in spy.calls:
            expected = (call["position_samples"] / 48000.0) * 1.5
            assert call["position_beats"] == pytest.approx(expected, rel=1e-3, abs=1e-6)
    finally:
        plugin.close()


# --- H6: MIDI + sidechain ---------------------------------------------- #


@skip_if_no_plugin
def test_midi_with_sidechain_is_rejected_not_silently_dropped():
    """`mh_process_sidechain` has no MIDI parameter, so the sidechain block
    loop collected the events and then discarded them.
    """
    plugin = minihost.Plugin(
        PLUGIN, sample_rate=48000, max_block_size=512, sidechain_channels=2
    )
    try:
        audio = minihost.AudioBuffer(max(plugin.num_input_channels, 1), 1024)
        sidechain = minihost.AudioBuffer(max(plugin.sidechain_channels, 1), 1024)
        with pytest.raises(ValueError, match="midi and sidechain cannot be combined"):
            minihost.process_audio(
                plugin,
                audio,
                midi=[(0, 0x90, 60, 100)],
                sidechain=sidechain,
            )
    finally:
        plugin.close()
