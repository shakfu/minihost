"""Plugin discovery and short-MIDI-message construction (H9 / H10 / H11).

Coverage here is deliberately modest, because all three fixes are hard to
observe from macOS with the plugins available. What each test actually pins is
called out in its docstring so the limits are not mistaken for confidence.

  * **H9** -- `mh_scan_directory` searched only for *directories* matching
    `*.vst3`. That is right on macOS, where a VST3 is a bundle, but on Windows
    and Linux a VST3 is very often a single shared library file, and every such
    plugin was invisible to scanning. Not directly testable here: a bare file
    named `.vst3` is not loadable on macOS, so it is discovered and then fails
    to probe either way. These tests pin that the new file-searching path is
    exercised without error.

  * **H10** -- every `MH_MidiEvent` was turned into a three-byte
    `juce::MidiMessage`, but Program Change (0xC0) and Channel Pressure (0xD0)
    are two-byte messages and System Real-Time (0xF8-0xFF) are one-byte. In a
    debug build that trips JUCE's own length assertion; in release it puts a
    malformed message into the MidiBuffer. Measured against a MIDI-effect
    plugin, the *observable* round-trip is unchanged in a release build -- JUCE
    tolerates the over-long message -- so these tests pin that each length
    class is accepted and round-trips correctly, not that behaviour changed.

  * **H11** -- `send_midi` pushed onto the same single-producer ring buffer the
    libremidi input thread owns, making two producers on an SPSC structure.
    It now has its own ring. Reproducing the original corruption needs a live
    MIDI input port feeding events concurrently, which CI does not have.
"""

from __future__ import annotations

import os

import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
# A MIDI effect that echoes what it is given -- lets us round-trip each message
# length class. Skipped if absent.
MIDI_ECHO = os.environ.get(
    "MINIHOST_TEST_MIDI_PLUGIN", "/Library/Audio/Plug-Ins/VST3/Chord Prism 2.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)
skip_if_no_echo = pytest.mark.skipif(
    not os.path.exists(MIDI_ECHO),
    reason=f"MIDI-producing plugin not found at {MIDI_ECHO}",
)


# --- H9: discovery must consider files, not only bundle directories --- #


def test_scan_directory_of_files_does_not_error(tmp_path):
    """A directory holding a plain `.vst3` *file* must scan cleanly.

    Before the fix such a file was never even looked at. It still cannot load on
    macOS (a VST3 must be a bundle here), so the count stays 0 either way -- what
    this pins is that the file-searching path runs and reports success rather
    than the -1 that signals a scan error.
    """
    (tmp_path / "NotReallyAPlugin.vst3").write_bytes(b"\x00" * 64)
    assert minihost.scan_directory(str(tmp_path)) == []


def test_scan_directory_rejects_a_missing_path():
    with pytest.raises(RuntimeError):
        minihost.scan_directory("/definitely/not/a/directory")


def test_scan_directory_of_empty_dir(tmp_path):
    assert minihost.scan_directory(str(tmp_path)) == []


# --- H10: each short-message length class is accepted ----------------- #


@skip_if_no_plugin
@pytest.mark.parametrize(
    "label,event",
    [
        ("note on (3-byte)", (0, 0x90, 60, 100)),
        ("note off (3-byte)", (0, 0x80, 60, 0)),
        ("control change (3-byte)", (0, 0xB0, 7, 64)),
        ("pitch bend (3-byte)", (0, 0xE0, 0, 64)),
        ("program change (2-byte)", (0, 0xC0, 5, 0)),
        ("channel pressure (2-byte)", (0, 0xD0, 64, 0)),
        ("clock (1-byte)", (0, 0xF8, 0, 0)),
        ("start (1-byte)", (0, 0xFA, 0, 0)),
        ("stop (1-byte)", (0, 0xFC, 0, 0)),
    ],
)
def test_every_short_message_length_is_accepted(label, event):
    """Each length class must process without error and leave finite output.

    The two- and one-byte classes are the ones that were being constructed as
    three-byte messages.
    """
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        buf_in = minihost.AudioBuffer(max(plugin.num_input_channels, 1), 512)
        buf_out = minihost.AudioBuffer(max(plugin.num_output_channels, 1), 512)
        plugin.process_midi(buf_in, buf_out, [event])
        assert all(
            abs(buf_out[c, i]) < 1e6
            for c in range(buf_out.channels)
            for i in range(0, 512, 32)
        ), f"{label} produced non-finite output"
    finally:
        plugin.close()


@skip_if_no_echo
@pytest.mark.parametrize(
    "status,data1,data2",
    [(0xC0, 5, 0), (0xD0, 64, 0), (0xE0, 0, 64)],
)
def test_two_and_three_byte_messages_round_trip(status, data1, data2):
    """Through a plugin that echoes MIDI, the message must come back intact.

    Note this passes both before and after the fix -- JUCE tolerates the
    over-long message in a release build. It guards against a future change
    that mangles or drops these classes outright.
    """
    plugin = minihost.Plugin(MIDI_ECHO, sample_rate=48000, max_block_size=512)
    try:
        buf_in = minihost.AudioBuffer(max(plugin.num_input_channels, 1), 512)
        buf_out = minihost.AudioBuffer(max(plugin.num_output_channels, 1), 512)
        out = plugin.process_midi(buf_in, buf_out, [(0, status, data1, data2)])
        echoed = [e for e in out if e[1] == status]
        assert echoed, f"status {status:#04x} was not echoed back: {out}"
        assert echoed[0][2] == data1
    finally:
        plugin.close()


# --- H11: send_midi no longer shares the input thread's ring ---------- #


@skip_if_no_plugin
def test_send_midi_accepts_events_on_its_own_ring():
    """Programmatic sends must keep working now they use a separate ring.

    The race they used to create -- a second producer on the libremidi input
    thread's SPSC ring -- needs a live MIDI port to reproduce, so this pins the
    functional path rather than the corruption.
    """
    import time

    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        try:
            device = minihost.AudioDevice(plugin)
        except RuntimeError as e:
            pytest.skip(f"no usable audio device: {e}")
        try:
            device.start()
            # send_midi returns None and raises if the ring is full, so a clean
            # return is the assertion.
            for i in range(64):
                device.send_midi(0x90, 60 + (i % 12), 100)
                device.send_midi(0x80, 60 + (i % 12), 0)
            time.sleep(0.1)
            device.stop()
        finally:
            del device
    finally:
        plugin.close()
