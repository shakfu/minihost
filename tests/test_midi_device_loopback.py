"""MIDI through a running AudioDevice, end to end on the platform back-end.

`test_midiin_lifetime.py` covers standalone `MidiIn`. These drive the device
path: a loopback sender writes to the device's MIDI input, the
`MinihostTestFx` fixture passes MIDI through, and a `MidiIn` reads the
device's MIDI output. That exercises libremidi input and output inside
`minihost_audio.c` on CoreMIDI or ALSA, whichever this machine has.

The `Midi Through` tests use the ALSA kernel's `snd-seq-dummy` port to reach
the non-virtual open-by-index paths, so they run only on Linux.

Needs a MIDI loopback sender, an audio device and the effect fixture.
"""

from __future__ import annotations

import os
import sys
import time

import pytest

import minihost

import midi_loopback
from cli_helpers import find_test_plugin
from device_helpers import skip_if_no_audio_device

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")

pytestmark = [
    pytest.mark.skipif(
        not midi_loopback.available(),
        reason="no MIDI loopback sender (CoreMIDI or ALSA sequencer)",
    ),
    pytest.mark.skipif(
        not FX or not os.path.exists(FX),
        reason="MinihostTestFx not built; set MINIHOST_TEST_PLUGIN_FX or build "
        "with -DMINIHOST_BUILD_TEST_PLUGIN=ON",
    ),
    skip_if_no_audio_device,
]

THROUGH = "Midi Through Port-0"

NOTES = [bytes([s, n, v]) for n in (60, 62, 64) for s, v in ((0x90, 100), (0x80, 0))]


def _input_index(name: str, timeout: float = 2.0) -> int | None:
    deadline = time.monotonic() + timeout
    while True:
        for port in minihost.midi_get_input_ports():
            if port["name"] == name:
                return port["index"]
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.05)


def _output_index(name: str) -> int | None:
    for port in minihost.midi_get_output_ports():
        if port["name"] == name:
            return port["index"]
    return None


def _wait_for(received: list, count: int, timeout: float = 2.0) -> None:
    deadline = time.monotonic() + timeout
    while len(received) < count and time.monotonic() < deadline:
        time.sleep(0.02)


@pytest.fixture
def device():
    with minihost.Plugin(FX, sample_rate=48000, max_block_size=512) as plugin:
        dev = minihost.AudioDevice(plugin)
        try:
            yield dev
        finally:
            dev.stop()
            del dev


def _listen(name: str, received: list):
    index = _input_index(name)
    if index is None:
        pytest.skip(f"MIDI port {name!r} did not appear as an input")
    return minihost.MidiIn.open(index, lambda data: received.append(bytes(data)))


def test_virtual_ports_round_trip_through_the_plugin(device):
    device.create_virtual_midi_input("mh-loop-dev-in")
    device.create_virtual_midi_output("mh-loop-dev-out")
    assert device.is_midi_input_virtual and device.is_midi_output_virtual
    device.start()

    received: list[bytes] = []
    with _listen("mh-loop-dev-out", received):
        dest = midi_loopback.wait_for_destination("mh-loop-dev-in")
        assert dest is not None, "device's virtual MIDI input never appeared"
        with midi_loopback.Sender() as sender:
            for msg in NOTES:
                sender.send(dest, msg)
                time.sleep(0.02)
            _wait_for(received, len(NOTES))

    midi_loopback.assert_contains_in_order(received, NOTES)


def test_send_midi_reaches_the_device_output(device):
    device.create_virtual_midi_output("mh-loop-send-out")
    device.start()

    received: list[bytes] = []
    with _listen("mh-loop-send-out", received):
        device.send_midi(0x90, 70, 33)
        device.send_midi(0xB0, 7, 99)
        device.send_midi(0xE0, 0, 64)
        _wait_for(received, 3)

    # Unordered: in one block, a VST3 receives controllers as parameter
    # changes, which the plugin sees before same-offset note events.
    expected = [bytes([0x90, 70, 33]), bytes([0xB0, 7, 99]), bytes([0xE0, 0, 64])]
    assert sorted(received) == sorted(expected)


linux_through = pytest.mark.skipif(
    not sys.platform.startswith("linux"),
    reason="Midi Through is an ALSA (snd-seq-dummy) port",
)


@linux_through
def test_midiin_opens_a_non_virtual_port():
    if _input_index(THROUGH, timeout=0) is None:
        pytest.skip("no Midi Through port (snd-seq-dummy not loaded)")
    received: list[bytes] = []
    with _listen(THROUGH, received):
        dest = midi_loopback.wait_for_destination(THROUGH)
        assert dest is not None
        with midi_loopback.Sender() as sender:
            for msg in NOTES:
                sender.send(dest, msg)
            _wait_for(received, len(NOTES))
    midi_loopback.assert_contains_in_order(received, NOTES)


@linux_through
def test_device_connects_output_and_input_by_index(device):
    out_index = _output_index(THROUGH)
    if out_index is None:
        pytest.skip("no Midi Through port (snd-seq-dummy not loaded)")
    device.connect_midi_output(out_index)
    assert device.midi_output_port == out_index
    device.start()

    received: list[bytes] = []
    with _listen(THROUGH, received):
        device.send_midi(0x90, 72, 90)
        _wait_for(received, 1)
    midi_loopback.assert_contains_in_order(received, [bytes([0x90, 72, 90])])

    device.disconnect_midi_output()
    in_index = _input_index(THROUGH)
    device.connect_midi_input(in_index)
    assert device.midi_input_port == in_index
    device.disconnect_midi_input()
    assert device.midi_input_port == -1
