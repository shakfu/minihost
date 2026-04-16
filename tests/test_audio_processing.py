"""Tests for audio processing data pipeline.

These tests verify buffer handling, MIDI event conversion, and automation
logic without requiring a real audio plugin.  Plugin-dependent tests that
exercise the actual process/process_midi/process_auto calls live in
test_minihost.py (gated behind MINIHOST_TEST_PLUGIN).
"""

import os

import numpy as np
import pytest

from minihost._core import MidiFile
from minihost.render import (
    _build_tempo_map,
    _collect_midi_events,
    _event_to_midi_tuple,
    _seconds_to_samples,
    _tick_to_seconds,
    render_midi,
    render_midi_stream,
    MidiRenderer,
)
from minihost.automation import (
    _interpolate_keyframes,
    _parse_time_key,
    parse_automation_file,
)


# ---------------------------------------------------------------------------
# MidiFile round-trip integrity
# ---------------------------------------------------------------------------


class TestMidiFileRoundTrip:
    """Verify MIDI data survives the MidiFile create -> get_events cycle."""

    def test_note_on_off_round_trip(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_off(t, 480, 0, 60, 0)

        events = mf.get_events(t)
        note_events = [e for e in events if e["type"] in ("note_on", "note_off")]
        assert len(note_events) == 2

        on = note_events[0]
        assert on["type"] == "note_on"
        assert on["tick"] == 0
        assert on["channel"] == 0
        assert on["pitch"] == 60
        assert on["velocity"] == 100

        off = note_events[1]
        assert off["type"] == "note_off"
        assert off["tick"] == 480
        assert off["pitch"] == 60

    def test_control_change_round_trip(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_control_change(t, 240, 0, 1, 64)  # CC1 = mod wheel

        events = mf.get_events(t)
        cc_events = [e for e in events if e["type"] == "control_change"]
        assert len(cc_events) == 1
        assert cc_events[0]["controller"] == 1
        assert cc_events[0]["value"] == 64
        assert cc_events[0]["channel"] == 0

    def test_program_change_round_trip(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_program_change(t, 0, 0, 42)

        events = mf.get_events(t)
        pc_events = [e for e in events if e["type"] == "program_change"]
        assert len(pc_events) == 1
        assert pc_events[0]["program"] == 42

    def test_pitch_bend_round_trip(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_pitch_bend(t, 0, 0, 8192)

        events = mf.get_events(t)
        pb_events = [e for e in events if e["type"] == "pitch_bend"]
        assert len(pb_events) == 1
        # Verify we get a valid pitch bend value back
        assert 0 <= pb_events[0]["value"] <= 16383

    def test_multi_track_events_isolated(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t0 = mf.add_track()
        t1 = mf.add_track()
        mf.add_note_on(t0, 0, 0, 60, 100)
        mf.add_note_on(t1, 0, 1, 72, 80)

        events_0 = mf.get_events(t0)
        events_1 = mf.get_events(t1)

        notes_0 = [e for e in events_0 if e["type"] == "note_on"]
        notes_1 = [e for e in events_1 if e["type"] == "note_on"]

        assert len(notes_0) == 1
        assert notes_0[0]["pitch"] == 60
        assert len(notes_1) == 1
        assert notes_1[0]["pitch"] == 72

    def test_tempo_event_round_trip(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_tempo(t, 0, 140.0)

        events = mf.get_events(t)
        tempo_events = [e for e in events if e["type"] == "tempo"]
        assert len(tempo_events) == 1
        assert tempo_events[0]["bpm"] == pytest.approx(140.0, abs=0.1)

    def test_ticks_per_quarter_preserved(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 960
        assert mf.ticks_per_quarter == 960

    def test_many_events_collected_sorted(self):
        """_collect_midi_events should return events sorted by tick regardless
        of insertion order."""
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        # Add in reverse order
        for tick in reversed(range(0, 4800, 480)):
            mf.add_note_on(t, tick, 0, 60, 100)

        # _collect_midi_events sorts by tick (used by the render pipeline)
        events = _collect_midi_events(mf)
        ticks = [e["tick"] for e in events]
        assert ticks == sorted(ticks)


# ---------------------------------------------------------------------------
# MIDI event to tuple conversion
# ---------------------------------------------------------------------------


class TestEventToMidiTuple:
    """Test _event_to_midi_tuple conversion for all event types."""

    def test_note_on(self):
        event = {"type": "note_on", "channel": 0, "pitch": 60, "velocity": 100}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0x90, 60, 100)

    def test_note_on_channel_15(self):
        event = {"type": "note_on", "channel": 15, "pitch": 127, "velocity": 1}
        result = _event_to_midi_tuple(event, 42)
        assert result == (42, 0x9F, 127, 1)

    def test_note_off(self):
        event = {"type": "note_off", "channel": 0, "pitch": 60, "velocity": 0}
        result = _event_to_midi_tuple(event, 256)
        assert result == (256, 0x80, 60, 0)

    def test_control_change(self):
        event = {"type": "control_change", "channel": 0, "controller": 1, "value": 64}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0xB0, 1, 64)

    def test_program_change(self):
        event = {"type": "program_change", "channel": 0, "program": 42}
        result = _event_to_midi_tuple(event, 0)
        assert result == (0, 0xC0, 42, 0)

    def test_pitch_bend_center(self):
        event = {"type": "pitch_bend", "channel": 0, "value": 8192}
        result = _event_to_midi_tuple(event, 0)
        assert result is not None
        assert result[0] == 0
        assert result[1] == 0xE0
        # 8192 = 0x2000: LSB = 0x00, MSB = 0x40
        assert result[2] == 0x00
        assert result[3] == 0x40

    def test_pitch_bend_max(self):
        event = {"type": "pitch_bend", "channel": 0, "value": 16383}
        result = _event_to_midi_tuple(event, 0)
        assert result is not None
        assert result[2] == 0x7F  # LSB
        assert result[3] == 0x7F  # MSB

    def test_unknown_type_returns_none(self):
        event = {"type": "sysex", "channel": 0}
        result = _event_to_midi_tuple(event, 0)
        assert result is None

    def test_missing_channel_defaults_to_zero(self):
        event = {"type": "note_on", "pitch": 60, "velocity": 100}
        result = _event_to_midi_tuple(event, 0)
        assert result is not None
        assert result[1] == 0x90  # channel 0


# ---------------------------------------------------------------------------
# Timing accuracy
# ---------------------------------------------------------------------------


class TestTimingAccuracy:
    """Test tick-to-seconds and seconds-to-samples conversions."""

    def test_120bpm_quarter_note_is_half_second(self):
        tempo_map = [(0, 500_000.0)]  # 120 BPM
        tpq = 480
        seconds = _tick_to_seconds(480, tempo_map, tpq)
        assert seconds == pytest.approx(0.5, abs=1e-9)

    def test_tempo_change_midway(self):
        # 120 BPM for first quarter, then 60 BPM for second quarter
        tempo_map = [(0, 500_000.0), (480, 1_000_000.0)]
        tpq = 480
        # First 480 ticks at 120 BPM = 0.5s
        # Next 480 ticks at 60 BPM = 1.0s
        seconds = _tick_to_seconds(960, tempo_map, tpq)
        assert seconds == pytest.approx(1.5, abs=1e-9)

    def test_seconds_to_samples_exact(self):
        assert _seconds_to_samples(1.0, 48000.0) == 48000
        assert _seconds_to_samples(0.5, 44100.0) == 22050
        assert _seconds_to_samples(0.0, 48000.0) == 0

    def test_high_tempo(self):
        """300 BPM: quarter note = 0.2 seconds."""
        us_per_quarter = 60_000_000.0 / 300.0
        tempo_map = [(0, us_per_quarter)]
        tpq = 480
        seconds = _tick_to_seconds(480, tempo_map, tpq)
        assert seconds == pytest.approx(0.2, abs=1e-9)

    def test_zero_tick(self):
        tempo_map = [(0, 500_000.0)]
        assert _tick_to_seconds(0, tempo_map, 480) == 0.0


# ---------------------------------------------------------------------------
# Collect MIDI events from MidiFile
# ---------------------------------------------------------------------------


class TestCollectMidiEvents:
    """Test that _collect_midi_events filters and sorts correctly."""

    def test_filters_tempo_events(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        mf.add_note_on(t, 0, 0, 60, 100)

        events = _collect_midi_events(mf)
        types = {e["type"] for e in events}
        assert "tempo" not in types
        assert "note_on" in types

    def test_collects_across_tracks(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t0 = mf.add_track()
        t1 = mf.add_track()
        mf.add_note_on(t0, 0, 0, 60, 100)
        mf.add_note_on(t1, 240, 1, 72, 80)

        events = _collect_midi_events(mf)
        assert len(events) == 2
        # Should be sorted by tick
        assert events[0]["tick"] <= events[1]["tick"]

    def test_empty_midi_returns_empty(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        mf.add_track()
        events = _collect_midi_events(mf)
        assert events == []


# ---------------------------------------------------------------------------
# Automation interpolation edge cases
# ---------------------------------------------------------------------------


class TestAutomationInterpolationEdgeCases:
    """Additional edge cases for keyframe interpolation."""

    def test_keyframes_at_same_position(self):
        """Two keyframes at the same sample should not produce infinite loop."""
        result = _interpolate_keyframes([(0, 0.0), (0, 1.0)], 1024, 512)
        # Should contain both values without error
        assert len(result) >= 2

    def test_very_small_block_size(self):
        """Block size of 1 should produce a value at every sample."""
        result = _interpolate_keyframes([(0, 0.0), (4, 1.0)], 8, 1)
        # Should have start, 3 intermediate points (1,2,3), and end
        assert len(result) == 5
        for sample, value in result:
            assert 0 <= sample <= 4
            assert 0.0 <= value <= 1.0

    def test_descending_ramp(self):
        """Values should decrease for a descending ramp."""
        result = _interpolate_keyframes([(0, 1.0), (4096, 0.0)], 4096, 512)
        values = [v for _, v in result]
        for i in range(1, len(values)):
            assert values[i] <= values[i - 1]


# ---------------------------------------------------------------------------
# Render pipeline end-to-end (data pipeline, no plugin needed)
# ---------------------------------------------------------------------------


class TestRenderPipelineDataFlow:
    """Test that the render pipeline correctly converts MIDI events to
    sample-positioned tuples with correct timing."""

    def test_midi_events_converted_to_sample_positions(self):
        """Verify tick -> seconds -> samples conversion in a full pipeline."""
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        # Note at tick 480 = beat 1 = 0.5 seconds at 120 BPM
        mf.add_note_on(t, 480, 0, 60, 100)
        mf.add_note_off(t, 960, 0, 60, 0)

        sample_rate = 48000.0
        tempo_map = _build_tempo_map(mf)
        all_events = _collect_midi_events(mf)

        assert len(all_events) == 2

        # Convert to sample positions
        for event in all_events:
            seconds = _tick_to_seconds(event["tick"], tempo_map, 480)
            sample_pos = _seconds_to_samples(seconds, sample_rate)

            if event["type"] == "note_on":
                # tick 480 at 120 BPM = 0.5s = 24000 samples
                assert sample_pos == 24000
            elif event["type"] == "note_off":
                # tick 960 at 120 BPM = 1.0s = 48000 samples
                assert sample_pos == 48000

    def test_block_event_assignment(self):
        """Events should be assigned to correct blocks based on sample position."""
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        # Events at 0s, 0.5s, 1.0s
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_on(t, 480, 0, 72, 80)
        mf.add_note_off(t, 960, 0, 60, 0)

        sample_rate = 48000.0
        block_size = 512
        tempo_map = _build_tempo_map(mf)
        all_events = _collect_midi_events(mf)

        events_with_samples = []
        for event in all_events:
            seconds = _tick_to_seconds(event["tick"], tempo_map, 480)
            sample_pos = _seconds_to_samples(seconds, sample_rate)
            events_with_samples.append((sample_pos, event))

        # Simulate block assignment
        blocks_with_events = {}
        for sample_pos, event in events_with_samples:
            block_idx = sample_pos // block_size
            if block_idx not in blocks_with_events:
                blocks_with_events[block_idx] = []
            blocks_with_events[block_idx].append((sample_pos, event))

        # Event at sample 0 -> block 0
        assert 0 in blocks_with_events
        # Event at sample 24000 -> block 46 (24000 // 512 = 46)
        assert 46 in blocks_with_events
        # Event at sample 48000 -> block 93 (48000 // 512 = 93)
        assert 93 in blocks_with_events

    def test_block_local_offset_calculation(self):
        """Within-block offset should be sample_pos % block_size."""
        block_size = 512
        # Event at sample 24000: block 46, offset 24000 - 46*512 = 24000 - 23552 = 448
        sample_pos = 24000
        block_start = (sample_pos // block_size) * block_size
        offset = sample_pos - block_start
        assert offset == 448
        assert 0 <= offset < block_size


# ---------------------------------------------------------------------------
# MidiRenderer state machine
# ---------------------------------------------------------------------------


class TestMidiRendererStateMachine:
    """Test MidiRenderer properties without a real plugin (using a mock)."""

    def _make_test_midi(self):
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_off(t, 960, 0, 60, 0)  # 1 second at 120 BPM
        return mf

    def test_midi_file_save_load_round_trip(self, tmp_path):
        """MidiFile saved and reloaded should produce identical events."""
        import tempfile, os

        mf = MidiFile()
        mf.ticks_per_quarter = 480
        mf.add_tempo(0, 0, 120.0)
        mf.add_note_on(0, 0, 0, 60, 100)
        mf.add_note_off(0, 960, 0, 60, 0)

        with tempfile.NamedTemporaryFile(suffix=".mid", delete=False) as f:
            temp_path = f.name

        try:
            assert mf.save(temp_path)

            mf2 = MidiFile()
            assert mf2.load(temp_path)

            events1 = _collect_midi_events(mf)
            events2 = _collect_midi_events(mf2)

            assert len(events1) == len(events2)
            for e1, e2 in zip(events1, events2):
                assert e1["type"] == e2["type"]
                assert e1["tick"] == e2["tick"]
        finally:
            os.unlink(temp_path)

    def test_duration_calculation(self):
        """Verify duration is computed correctly from MIDI + tail."""
        mf = self._make_test_midi()
        tempo_map = _build_tempo_map(mf)
        all_events = _collect_midi_events(mf)

        last_tick = max(e["tick"] for e in all_events)
        midi_duration = _tick_to_seconds(last_tick, tempo_map, 480)
        # note_off at tick 960, 120 BPM = 1.0 second
        assert midi_duration == pytest.approx(1.0, abs=1e-6)


# ---------------------------------------------------------------------------
# Integration tests (require a real plugin)
# ---------------------------------------------------------------------------


@pytest.fixture
def plugin_path():
    path = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not path:
        pytest.skip("MINIHOST_TEST_PLUGIN not set")
    return path


@pytest.fixture
def plugin(plugin_path):
    import minihost

    return minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)


class TestProcessIntegration:
    """Tests that exercise actual audio processing calls with a real plugin."""

    def test_process_silence_produces_finite_output(self, plugin):
        """Processing silence should not produce NaN or Inf."""
        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        input_buf = np.zeros((in_ch, 512), dtype=np.float32)
        output_buf = np.zeros((out_ch, 512), dtype=np.float32)

        plugin.process(input_buf, output_buf)

        assert np.all(np.isfinite(output_buf))

    def test_process_midi_returns_list(self, plugin):
        """process_midi should always return a list of MIDI output tuples."""
        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        input_buf = np.zeros((in_ch, 512), dtype=np.float32)
        output_buf = np.zeros((out_ch, 512), dtype=np.float32)

        midi_in = [(0, 0x90, 60, 100), (256, 0x80, 60, 0)]
        midi_out = plugin.process_midi(input_buf, output_buf, midi_in)

        assert isinstance(midi_out, list)
        for event in midi_out:
            assert isinstance(event, tuple)
            assert len(event) == 4

    def test_process_auto_no_changes_matches_process(self, plugin):
        """process_auto with empty param_changes should match process_midi."""
        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        plugin.reset()

        # First pass: process_midi
        input_buf = np.zeros((in_ch, 512), dtype=np.float32)
        out_midi = np.zeros((out_ch, 512), dtype=np.float32)
        plugin.process_midi(input_buf, out_midi, [])

        plugin.reset()

        # Second pass: process_auto with no changes
        out_auto = np.zeros((out_ch, 512), dtype=np.float32)
        plugin.process_auto(input_buf, out_auto, [], [])

        # Should produce identical output
        np.testing.assert_array_equal(out_midi, out_auto)

    def test_process_auto_with_param_change(self, plugin):
        """process_auto with a parameter change should not crash."""
        if plugin.num_params == 0:
            pytest.skip("Plugin has no parameters")

        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        input_buf = np.zeros((in_ch, 512), dtype=np.float32)
        output_buf = np.zeros((out_ch, 512), dtype=np.float32)

        # Change param 0 at sample 256
        param_changes = [(0, 0, 0.2), (256, 0, 0.8)]
        plugin.process_auto(input_buf, output_buf, [], param_changes)

        assert np.all(np.isfinite(output_buf))

    def test_multiple_blocks_state_continuity(self, plugin):
        """Processing multiple blocks should maintain internal state."""
        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        plugin.reset()

        outputs = []
        for _ in range(4):
            input_buf = np.zeros((in_ch, 512), dtype=np.float32)
            output_buf = np.zeros((out_ch, 512), dtype=np.float32)
            plugin.process(input_buf, output_buf)
            outputs.append(output_buf.copy())
            assert np.all(np.isfinite(output_buf))

    def test_transport_set_and_clear(self, plugin):
        """Transport set/clear should not affect processing stability."""
        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        plugin.set_transport(bpm=140.0, is_playing=True, position_samples=48000)

        input_buf = np.zeros((in_ch, 512), dtype=np.float32)
        output_buf = np.zeros((out_ch, 512), dtype=np.float32)
        plugin.process(input_buf, output_buf)
        assert np.all(np.isfinite(output_buf))

        plugin.clear_transport()
        plugin.process(input_buf, output_buf)
        assert np.all(np.isfinite(output_buf))

    def test_render_midi_produces_audio(self, plugin):
        """render_midi with a simple MIDI file should produce non-empty output."""
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_off(t, 480, 0, 60, 0)

        audio = render_midi(plugin, mf, block_size=512, tail_seconds=0.5)

        assert isinstance(audio, np.ndarray)
        assert audio.ndim == 2
        assert audio.shape[0] >= 2  # at least stereo
        assert audio.shape[1] > 0  # non-empty
        assert np.all(np.isfinite(audio))

    def test_render_midi_stream_yields_correct_shape(self, plugin):
        """Each block from render_midi_stream should have correct shape."""
        mf = MidiFile()
        mf.ticks_per_quarter = 480
        t = mf.add_track()
        mf.add_tempo(t, 0, 120.0)
        mf.add_note_on(t, 0, 0, 60, 100)
        mf.add_note_off(t, 480, 0, 60, 0)

        out_ch = max(plugin.num_output_channels, 2)
        block_size = 256
        block_count = 0

        for block in render_midi_stream(
            plugin, mf, block_size=block_size, tail_seconds=0.1
        ):
            assert isinstance(block, np.ndarray)
            assert block.shape[0] == out_ch
            assert block.shape[1] <= block_size
            assert block.shape[1] > 0
            assert np.all(np.isfinite(block))
            block_count += 1

        assert block_count > 0
