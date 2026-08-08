"""macOS CoreMIDI sender, so MIDI-input tests can drive real traffic.

minihost binds MIDI *input* (MidiIn) but not MIDI output, so there is no
in-library way to feed a port and assert that the callback fires. This helper
sends via CoreMIDI through ctypes -- no extra dependency -- which is enough to
exercise mh_midi_in_open's callback path end to end.

It needs a loopback: a MIDI destination whose name matches one of minihost's
input ports, so that what we send comes back to us. On macOS that is the IAC
Driver (enable a bus in Audio MIDI Setup > Window > MIDI Studio > IAC Driver).
`find_loopback()` returns None when no such pair exists, and callers skip.

Note that a loopback bus is *shared*: other applications on the machine can
send to it too, so assertions must tolerate foreign traffic interleaved with
the messages under test (see `assert_contains_in_order`).
"""

from __future__ import annotations

import ctypes
import ctypes.util
import struct
import sys

_KCF_UTF8 = 0x08000100


def available() -> bool:
    """True if the CoreMIDI ctypes bridge can be used on this machine."""
    if sys.platform != "darwin":
        return False
    return ctypes.util.find_library("CoreMIDI") is not None


class _CoreMidi:
    """Lazily-bound CoreMIDI / CoreFoundation entry points."""

    def __init__(self) -> None:
        self.cm = ctypes.CDLL(ctypes.util.find_library("CoreMIDI"))
        self.cf = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))

        self.cf.CFStringCreateWithCString.restype = ctypes.c_void_p
        self.cf.CFStringCreateWithCString.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_uint32,
        ]
        self.cf.CFStringGetCString.argtypes = [
            ctypes.c_void_p,
            ctypes.c_char_p,
            ctypes.c_long,
            ctypes.c_uint32,
        ]

        ref = ctypes.c_uint32  # MIDIObjectRef
        self.ref = ref
        self.cm.MIDIClientCreate.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.POINTER(ref),
        ]
        self.cm.MIDIOutputPortCreate.argtypes = [
            ref,
            ctypes.c_void_p,
            ctypes.POINTER(ref),
        ]
        self.cm.MIDIGetNumberOfDestinations.restype = ctypes.c_ulong
        self.cm.MIDIGetDestination.restype = ref
        self.cm.MIDIGetDestination.argtypes = [ctypes.c_ulong]
        self.cm.MIDIObjectGetStringProperty.argtypes = [
            ref,
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_void_p),
        ]
        self.cm.MIDISend.argtypes = [ref, ref, ctypes.c_void_p]

    def cfstr(self, s: str):
        return self.cf.CFStringCreateWithCString(None, s.encode(), _KCF_UTF8)


class Sender:
    """A CoreMIDI output client. Use as a context manager."""

    def __init__(self, name: str = "minihost-test-sender"):
        self._api = _CoreMidi()
        api = self._api
        self.client = api.ref()
        if api.cm.MIDIClientCreate(
            api.cfstr(name), None, None, ctypes.byref(self.client)
        ):
            raise RuntimeError("MIDIClientCreate failed")
        self.port = api.ref()
        if api.cm.MIDIOutputPortCreate(
            self.client, api.cfstr("out"), ctypes.byref(self.port)
        ):
            raise RuntimeError("MIDIOutputPortCreate failed")

    def __enter__(self) -> "Sender":
        return self

    def __exit__(self, *exc) -> None:
        return None

    def destinations(self) -> list[tuple[int, str]]:
        """[(endpoint_ref, name)] for every CoreMIDI destination."""
        api = self._api
        prop = ctypes.c_void_p.in_dll(api.cm, "kMIDIPropertyName")
        out = []
        for i in range(api.cm.MIDIGetNumberOfDestinations()):
            endpoint = api.cm.MIDIGetDestination(i)
            cfname = ctypes.c_void_p()
            buf = ctypes.create_string_buffer(256)
            if (
                api.cm.MIDIObjectGetStringProperty(endpoint, prop, ctypes.byref(cfname))
                == 0
            ):
                api.cf.CFStringGetCString(cfname, buf, 256, _KCF_UTF8)
            out.append((int(endpoint), buf.value.decode(errors="replace")))
        return out

    def send(self, endpoint: int, message: bytes) -> None:
        """Send one short MIDI message to `endpoint`.

        Builds a MIDIPacketList by hand. CoreMIDI declares these structs under
        `#pragma pack(push, 4)` (MIDIServices.h), giving the layout:
            UInt32 numPackets @0 | UInt64 timeStamp @4 | UInt16 length @12 | data @14
        which is exactly what struct's '<' (standard size, no alignment) emits.
        """
        packet = struct.pack("<IQH", 1, 0, len(message)) + bytes(message)
        buf = ctypes.create_string_buffer(packet, len(packet))
        rc = self._api.cm.MIDISend(self.port, endpoint, buf)
        if rc != 0:
            raise RuntimeError(f"MIDISend failed: {rc}")


def find_loopback(input_ports: list[dict]) -> tuple[int, int] | None:
    """Pair a minihost input port with the CoreMIDI destination of the same
    name, i.e. a loopback we can send into and receive from.

    Returns (minihost_input_port_index, coremidi_endpoint_ref), or None.
    """
    if not available() or not input_ports:
        return None
    try:
        sender = Sender()
    except Exception:
        return None
    by_name = {name: ref for ref, name in sender.destinations()}
    for port in input_ports:
        endpoint = by_name.get(port["name"])
        if endpoint is not None:
            return port["index"], endpoint
    return None


def assert_contains_in_order(received: list[bytes], expected: list[bytes]) -> None:
    """Assert every message in `expected` appears in `received`, in order.

    A loopback bus is shared with the rest of the system, so unrelated traffic
    can interleave with ours. Subsequence matching keeps the test meaningful
    without making it flaky.
    """
    it = iter(received)
    for want in expected:
        for got in it:
            if got == want:
                break
        else:
            raise AssertionError(
                f"expected MIDI message {want!r} not found in order; "
                f"received {received!r}"
            )
