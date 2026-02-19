"""Tests for MIDI monitor formatting in cli.py."""

import pytest

from minihost.cli import _format_midi_msg, _note_name


class TestNoteName:
    """Tests for _note_name helper."""

    def test_middle_c(self):
        assert _note_name(60) == "C4"

    def test_a4(self):
        assert _note_name(69) == "A4"

    def test_lowest(self):
        assert _note_name(0) == "C-1"

    def test_sharp(self):
        assert _note_name(61) == "C#4"

    def test_highest(self):
        assert _note_name(127) == "G9"


class TestFormatMidiMsg:
    """Tests for _format_midi_msg with known byte sequences."""

    def test_note_on(self):
        # Note On, channel 1, note 60 (C4), velocity 100
        msg = _format_midi_msg(0.0, bytes([0x90, 60, 100]))
        assert "Note On" in msg
        assert "ch=1" in msg
        assert "C4" in msg
        assert "(60)" in msg
        assert "vel=100" in msg

    def test_note_on_vel_zero_is_note_off(self):
        # Note On with velocity 0 should display as Note Off
        msg = _format_midi_msg(0.0, bytes([0x90, 60, 0]))
        assert "Note Off" in msg
        assert "vel=0" in msg

    def test_note_off(self):
        # Note Off, channel 1, note 60 (C4), velocity 0
        msg = _format_midi_msg(0.152, bytes([0x80, 60, 0]))
        assert "Note Off" in msg
        assert "ch=1" in msg
        assert "C4" in msg
        assert "0.152" in msg

    def test_note_on_channel_10(self):
        # Note On, channel 10 (0x99), note 36, velocity 127
        msg = _format_midi_msg(0.0, bytes([0x99, 36, 127]))
        assert "Note On" in msg
        assert "ch=10" in msg
        assert "vel=127" in msg

    def test_cc(self):
        # CC, channel 1, controller 1 (mod wheel), value 64
        msg = _format_midi_msg(0.300, bytes([0xB0, 1, 64]))
        assert "CC" in msg
        assert "ch=1" in msg
        assert "cc=1" in msg
        assert "val=64" in msg

    def test_pitch_bend(self):
        # Pitch Bend, channel 1, value 8192 (center)
        # 8192 = 0x2000 -> LSB=0x00, MSB=0x40
        msg = _format_midi_msg(0.301, bytes([0xE0, 0x00, 0x40]))
        assert "Pitch Bend" in msg
        assert "ch=1" in msg
        assert "val=8192" in msg

    def test_pitch_bend_min(self):
        # Pitch Bend minimum = 0
        msg = _format_midi_msg(0.0, bytes([0xE0, 0x00, 0x00]))
        assert "Pitch Bend" in msg
        assert "val=0" in msg

    def test_program_change(self):
        # Program Change, channel 1, program 5
        msg = _format_midi_msg(0.500, bytes([0xC0, 5]))
        assert "Program" in msg
        assert "ch=1" in msg
        assert "prog=5" in msg

    def test_channel_pressure(self):
        # Channel Pressure (aftertouch), channel 1, value 80
        msg = _format_midi_msg(0.0, bytes([0xD0, 80]))
        assert "Ch Pressure" in msg
        assert "ch=1" in msg
        assert "val=80" in msg

    def test_poly_aftertouch(self):
        # Polyphonic Aftertouch, channel 1, note 60, pressure 50
        msg = _format_midi_msg(0.0, bytes([0xA0, 60, 50]))
        assert "Poly AT" in msg
        assert "ch=1" in msg
        assert "C4" in msg
        assert "val=50" in msg

    def test_sysex(self):
        # SysEx message
        msg = _format_midi_msg(0.0, bytes([0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7]))
        assert "SysEx" in msg
        assert "F0" in msg
        assert "F7" in msg

    def test_system_realtime(self):
        # System real-time: timing clock (0xF8)
        msg = _format_midi_msg(0.0, bytes([0xF8]))
        assert "System" in msg

    def test_empty_data(self):
        msg = _format_midi_msg(0.0, b"")
        assert "empty" in msg

    def test_timestamp_formatting(self):
        msg = _format_midi_msg(12.345, bytes([0x90, 60, 100]))
        assert "12.345" in msg

    def test_unknown_message(self):
        # Single byte that isn't a valid status on its own
        msg = _format_midi_msg(0.0, bytes([0x90]))
        # Should still produce output without crashing
        assert len(msg) > 0
