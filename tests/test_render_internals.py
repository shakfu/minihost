"""Tests for render.py internal functions: _build_tempo_map, _tick_to_seconds, _collect_midi_events."""

import pytest

from minihost._core import MidiFile
from minihost.render import (
    _AUTO_TAIL_SILENT_BLOCKS,
    _AUTO_TAIL_THRESHOLD,
    _build_tempo_map,
    _collect_midi_events,
    _event_to_midi_tuple,
    _is_auto_tail,
    _seconds_to_samples,
    _tick_to_seconds,
)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_midi(tpq=480):
    """Create a MidiFile with the given ticks-per-quarter."""
    mf = MidiFile()
    mf.ticks_per_quarter = tpq
    return mf


# ---------------------------------------------------------------------------
# _build_tempo_map
# ---------------------------------------------------------------------------


class TestBuildTempoMap:
    """Tests for _build_tempo_map."""

    def test_default_tempo_when_no_tempo_events(self):
        """Empty MIDI should default to 120 BPM (500000 us/quarter)."""
        mf = _make_midi()
        mf.add_track()
        tempo_map = _build_tempo_map(mf)
        assert len(tempo_map) == 1
        assert tempo_map[0] == (0, 500_000.0)

    def test_single_tempo_at_tick_zero(self):
        mf = _make_midi()
        t = mf.add_track()
        mf.add_tempo(t, 0, 140.0)
        tempo_map = _build_tempo_map(mf)
        assert len(tempo_map) == 1
        assert tempo_map[0][0] == 0
        # MIDI stores tempo as integer microseconds-per-quarter, so there's
        # rounding from the BPM -> us/q -> BPM -> us/q round-trip.
        expected_us = 60_000_000.0 / 140.0
        assert tempo_map[0][1] == pytest.approx(expected_us, rel=1e-4)

    def test_single_tempo_not_at_zero_inserts_default(self):
        """A tempo event at tick > 0 without one at tick 0 should prepend 120 BPM."""
        mf = _make_midi()
        t = mf.add_track()
        mf.add_tempo(t, 480, 90.0)
        tempo_map = _build_tempo_map(mf)
        assert len(tempo_map) == 2
        assert tempo_map[0] == (0, 500_000.0)  # default 120 BPM
        assert tempo_map[1][0] == 480
        assert tempo_map[1][1] == pytest.approx(60_000_000.0 / 90.0)

    def test_multiple_tempo_changes_sorted(self):
        mf = _make_midi()
        t = mf.add_track()
        # Add out of order
        mf.add_tempo(t, 960, 180.0)
        mf.add_tempo(t, 0, 120.0)
        mf.add_tempo(t, 480, 140.0)
        tempo_map = _build_tempo_map(mf)
        assert len(tempo_map) == 3
        ticks = [t[0] for t in tempo_map]
        assert ticks == [0, 480, 960]

    def test_tempo_across_multiple_tracks(self):
        """Tempo events from different tracks should be merged."""
        mf = _make_midi()
        t0 = mf.add_track()
        t1 = mf.add_track()
        mf.add_tempo(t0, 0, 120.0)
        mf.add_tempo(t1, 960, 60.0)
        tempo_map = _build_tempo_map(mf)
        assert len(tempo_map) == 2
        assert tempo_map[0][0] == 0
        assert tempo_map[1][0] == 960

    def test_very_fast_tempo(self):
        mf = _make_midi()
        t = mf.add_track()
        mf.add_tempo(t, 0, 300.0)
        tempo_map = _build_tempo_map(mf)
        assert tempo_map[0][1] == pytest.approx(60_000_000.0 / 300.0)

    def test_very_slow_tempo(self):
        mf = _make_midi()
        t = mf.add_track()
        mf.add_tempo(t, 0, 20.0)
        tempo_map = _build_tempo_map(mf)
        assert tempo_map[0][1] == pytest.approx(60_000_000.0 / 20.0)

    def test_no_tracks(self):
        """MidiFile with no tracks should still return default tempo."""
        mf = _make_midi()
        tempo_map = _build_tempo_map(mf)
        assert len(tempo_map) == 1
        assert tempo_map[0] == (0, 500_000.0)


# ---------------------------------------------------------------------------
# _tick_to_seconds
# ---------------------------------------------------------------------------


class TestTickToSeconds:
    """Tests for _tick_to_seconds."""

    def test_zero_tick(self):
        tempo_map = [(0, 500_000.0)]  # 120 BPM
        assert _tick_to_seconds(0, tempo_map, 480) == 0.0

    def test_one_quarter_note_at_120bpm(self):
        """480 ticks at 120 BPM (500000 us/q) with tpq=480 should be 0.5 seconds."""
        tempo_map = [(0, 500_000.0)]
        result = _tick_to_seconds(480, tempo_map, 480)
        assert result == pytest.approx(0.5)

    def test_two_quarter_notes_at_120bpm(self):
        tempo_map = [(0, 500_000.0)]
        result = _tick_to_seconds(960, tempo_map, 480)
        assert result == pytest.approx(1.0)

    def test_fractional_tick(self):
        """Half a quarter note should be half the time."""
        tempo_map = [(0, 500_000.0)]
        result = _tick_to_seconds(240, tempo_map, 480)
        assert result == pytest.approx(0.25)

    def test_tempo_change_midway(self):
        """First half at 120 BPM, second half at 60 BPM.

        0-480 ticks at 120 BPM = 0.5s
        480-960 ticks at 60 BPM (1000000 us/q) = 1.0s
        Total = 1.5s
        """
        tempo_map = [(0, 500_000.0), (480, 1_000_000.0)]
        result = _tick_to_seconds(960, tempo_map, 480)
        assert result == pytest.approx(1.5)

    def test_tick_exactly_at_tempo_change(self):
        """Tick at exactly the tempo change boundary."""
        tempo_map = [(0, 500_000.0), (480, 1_000_000.0)]
        result = _tick_to_seconds(480, tempo_map, 480)
        assert result == pytest.approx(0.5)

    def test_tick_before_first_tempo_change(self):
        """Tick between start and first tempo change uses initial tempo."""
        tempo_map = [(0, 500_000.0), (960, 1_000_000.0)]
        result = _tick_to_seconds(480, tempo_map, 480)
        assert result == pytest.approx(0.5)

    def test_multiple_tempo_changes(self):
        """Three tempo regions.

        0-480: 120 BPM (500000 us/q) -> 480 ticks = 0.5s
        480-960: 60 BPM (1000000 us/q) -> 480 ticks = 1.0s
        960-1440: 240 BPM (250000 us/q) -> 480 ticks = 0.25s
        Total = 1.75s
        """
        tempo_map = [(0, 500_000.0), (480, 1_000_000.0), (960, 250_000.0)]
        result = _tick_to_seconds(1440, tempo_map, 480)
        assert result == pytest.approx(1.75)

    def test_different_tpq(self):
        """With tpq=96, one quarter note = 96 ticks."""
        tempo_map = [(0, 500_000.0)]  # 120 BPM
        result = _tick_to_seconds(96, tempo_map, 96)
        assert result == pytest.approx(0.5)

    def test_high_resolution_tpq(self):
        """High resolution tpq=960."""
        tempo_map = [(0, 500_000.0)]  # 120 BPM
        result = _tick_to_seconds(960, tempo_map, 960)
        assert result == pytest.approx(0.5)

    def test_tick_well_past_last_tempo_change(self):
        """Tick far beyond the last tempo change should still compute correctly."""
        tempo_map = [(0, 500_000.0), (480, 1_000_000.0)]
        # 0-480 at 120 BPM = 0.5s, then 480-4800 (4320 ticks) at 60 BPM = 9.0s
        result = _tick_to_seconds(4800, tempo_map, 480)
        assert result == pytest.approx(0.5 + 9.0)


# ---------------------------------------------------------------------------
# _collect_midi_events
# ---------------------------------------------------------------------------


class TestCollectMidiEvents:
    """Tests for _collect_midi_events."""

    def test_empty_midi(self):
        mf = _make_midi()
        mf.add_track()  # empty track
        events = _collect_midi_events(mf)
        assert events == []

    def test_no_tracks(self):
        mf = _make_midi()
        events = _collect_midi_events(mf)
        assert events == []

    def test_single_note(self):
        mf = _make_midi()
        t = mf.add_track()
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_off(t, 480, 0, 60, 0)
        events = _collect_midi_events(mf)
        assert len(events) == 2
        assert events[0]["type"] == "note_on"
        assert events[0]["tick"] == 0
        assert events[0]["pitch"] == 60
        assert events[0]["velocity"] == 100
        assert events[1]["type"] == "note_off"
        assert events[1]["tick"] == 480

    def test_filters_out_tempo_events(self):
        """Tempo events are meta events and should not be collected."""
        mf = _make_midi()
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        mf.add_note_on(t, 0, 0, 60, 100)
        events = _collect_midi_events(mf)
        assert len(events) == 1
        assert events[0]["type"] == "note_on"

    def test_events_from_multiple_tracks(self):
        mf = _make_midi()
        t0 = mf.add_track()
        t1 = mf.add_track()
        mf.add_note_on(t0, 0, 0, 60, 100)
        mf.add_note_on(t1, 240, 0, 64, 80)
        mf.add_note_off(t0, 480, 0, 60, 0)
        mf.add_note_off(t1, 720, 0, 64, 0)
        events = _collect_midi_events(mf)
        assert len(events) == 4
        # Should be sorted by tick
        ticks = [e["tick"] for e in events]
        assert ticks == sorted(ticks)

    def test_control_change_included(self):
        mf = _make_midi()
        t = mf.add_track()
        mf.add_control_change(t, 0, 0, 1, 64)  # CC1 (mod wheel) = 64
        events = _collect_midi_events(mf)
        assert len(events) == 1
        assert events[0]["type"] == "control_change"
        assert events[0]["controller"] == 1
        assert events[0]["value"] == 64

    def test_program_change_included(self):
        mf = _make_midi()
        t = mf.add_track()
        mf.add_program_change(t, 0, 0, 5)
        events = _collect_midi_events(mf)
        assert len(events) == 1
        assert events[0]["type"] == "program_change"
        assert events[0]["program"] == 5

    def test_pitch_bend_included(self):
        """Pitch bend center value. JUCE's addPitchBend treats the input as a
        signed offset (-8192..+8191) mapped to 0..16383, so input 0 -> stored 8192."""
        mf = _make_midi()
        t = mf.add_track()
        mf.add_pitch_bend(t, 0, 0, 0)  # center position
        events = _collect_midi_events(mf)
        assert len(events) == 1
        assert events[0]["type"] == "pitch_bend"
        assert events[0]["value"] == 8192

    def test_zero_duration_notes(self):
        """Note on and off at the same tick (zero duration)."""
        mf = _make_midi()
        t = mf.add_track()
        mf.add_note_on(t, 100, 0, 60, 100)
        mf.add_note_off(t, 100, 0, 60, 0)
        events = _collect_midi_events(mf)
        assert len(events) == 2
        assert events[0]["tick"] == 100
        assert events[1]["tick"] == 100

    def test_events_sorted_across_tracks(self):
        """Events from later tracks but earlier ticks should come first."""
        mf = _make_midi()
        t0 = mf.add_track()
        t1 = mf.add_track()
        mf.add_note_on(t0, 960, 0, 60, 100)
        mf.add_note_on(t1, 0, 0, 64, 80)
        events = _collect_midi_events(mf)
        assert events[0]["tick"] == 0
        assert events[0]["pitch"] == 64
        assert events[1]["tick"] == 960
        assert events[1]["pitch"] == 60

    def test_multi_channel(self):
        mf = _make_midi()
        t = mf.add_track()
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_on(t, 0, 9, 36, 127)  # channel 10 (drums)
        events = _collect_midi_events(mf)
        assert len(events) == 2
        channels = {e["channel"] for e in events}
        assert channels == {0, 9}


# ---------------------------------------------------------------------------
# _event_to_midi_tuple
# ---------------------------------------------------------------------------


class TestEventToMidiTuple:
    """Tests for _event_to_midi_tuple."""

    def test_note_on(self):
        event = {"type": "note_on", "channel": 0, "pitch": 60, "velocity": 100}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0x90, 60, 100)

    def test_note_on_channel_9(self):
        event = {"type": "note_on", "channel": 9, "pitch": 36, "velocity": 127}
        result = _event_to_midi_tuple(event, 42)
        assert result == (42, 0x99, 36, 127)

    def test_note_off(self):
        event = {"type": "note_off", "channel": 0, "pitch": 60, "velocity": 0}
        result = _event_to_midi_tuple(event, 10)
        assert result == (10, 0x80, 60, 0)

    def test_control_change(self):
        event = {"type": "control_change", "channel": 0, "controller": 1, "value": 64}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0xB0, 1, 64)

    def test_program_change(self):
        event = {"type": "program_change", "channel": 0, "program": 5}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0xC0, 5, 0)

    def test_pitch_bend_center(self):
        """Center pitch bend (8192 in 14-bit) -> LSB=0x00, MSB=0x40."""
        event = {"type": "pitch_bend", "channel": 0, "value": 8192}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0xE0, 0x00, 0x40)

    def test_pitch_bend_zero(self):
        """Minimum pitch bend (0 in 14-bit) -> LSB=0, MSB=0."""
        event = {"type": "pitch_bend", "channel": 0, "value": 0}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0xE0, 0, 0)

    def test_pitch_bend_max(self):
        """Maximum pitch bend (16383 in 14-bit) -> LSB=0x7F, MSB=0x7F."""
        event = {"type": "pitch_bend", "channel": 0, "value": 16383}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0xE0, 0x7F, 0x7F)

    def test_unknown_type_returns_none(self):
        event = {"type": "sysex", "channel": 0}
        result = _event_to_midi_tuple(event, 0)
        assert result is None

    def test_default_channel(self):
        """Event without channel key should default to 0."""
        event = {"type": "note_on", "pitch": 60, "velocity": 100}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0x90, 60, 100)


# ---------------------------------------------------------------------------
# _seconds_to_samples
# ---------------------------------------------------------------------------


class TestSecondsToSamples:
    def test_zero(self):
        assert _seconds_to_samples(0.0, 48000.0) == 0

    def test_one_second(self):
        assert _seconds_to_samples(1.0, 48000.0) == 48000

    def test_fractional(self):
        assert _seconds_to_samples(0.5, 44100.0) == 22050

    def test_truncates(self):
        """Should truncate, not round."""
        assert _seconds_to_samples(0.0000001, 48000.0) == 0


# ---------------------------------------------------------------------------
# Integration: tempo map + tick_to_seconds round-trip
# ---------------------------------------------------------------------------


class TestTempoMapIntegration:
    """End-to-end tests using real MidiFile objects through _build_tempo_map and _tick_to_seconds."""

    def test_four_bars_at_120bpm(self):
        """4 bars of 4/4 at 120 BPM = 8 seconds."""
        mf = _make_midi(tpq=480)
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        # 4 bars * 4 beats * 480 ticks = 7680 ticks
        tempo_map = _build_tempo_map(mf)
        result = _tick_to_seconds(7680, tempo_map, 480)
        assert result == pytest.approx(8.0)

    def test_accelerando(self):
        """Start at 60 BPM for 1 bar, then 120 BPM for 1 bar.

        Bar 1: 4 beats * 480 ticks = 1920 ticks at 60 BPM -> 4.0s
        Bar 2: 1920 ticks at 120 BPM -> 2.0s
        Total: 6.0s
        """
        mf = _make_midi(tpq=480)
        t = mf.add_track()
        mf.add_tempo(t, 0, 60.0)
        mf.add_tempo(t, 1920, 120.0)
        tempo_map = _build_tempo_map(mf)
        result = _tick_to_seconds(3840, tempo_map, 480)
        assert result == pytest.approx(6.0)

    def test_collect_events_with_tempo_map(self):
        """Full pipeline: add notes and tempo, collect events, convert to seconds."""
        mf = _make_midi(tpq=480)
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_off(t, 480, 0, 60, 0)
        mf.add_note_on(t, 480, 0, 64, 90)
        mf.add_note_off(t, 960, 0, 64, 0)

        tempo_map = _build_tempo_map(mf)
        events = _collect_midi_events(mf)
        assert len(events) == 4

        # First note starts at 0.0s
        assert _tick_to_seconds(events[0]["tick"], tempo_map, 480) == pytest.approx(0.0)
        # Second note starts at 0.5s (one quarter note at 120 BPM)
        assert _tick_to_seconds(events[2]["tick"], tempo_map, 480) == pytest.approx(0.5)


# ---------------------------------------------------------------------------
# _is_auto_tail and auto-tail constants
# ---------------------------------------------------------------------------


class TestAutoTail:
    """Tests for auto-tail detection helpers."""

    def test_is_auto_tail_string(self):
        assert _is_auto_tail("auto") is True

    def test_is_auto_tail_none(self):
        assert _is_auto_tail(None) is False

    def test_is_auto_tail_float(self):
        assert _is_auto_tail(2.0) is False

    def test_is_auto_tail_zero(self):
        assert _is_auto_tail(0) is False

    def test_threshold_is_sensible(self):
        """Threshold should be a small positive number (~-80 dB)."""
        assert 0 < _AUTO_TAIL_THRESHOLD < 0.01

    def test_silent_blocks_is_positive(self):
        assert _AUTO_TAIL_SILENT_BLOCKS >= 1
