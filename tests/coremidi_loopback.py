"""macOS CoreMIDI sender, so MIDI-input tests can drive real traffic.

minihost binds MIDI *input* (MidiIn) but not MIDI output, so there is no
in-library way to feed a port and assert that the callback fires. This helper
sends via CoreMIDI through ctypes -- no extra dependency -- which is enough to
exercise mh_midi_in_open's callback path end to end.

The loopback is self-contained: a test opens a minihost *virtual* input port,
waits for it to show up as a CoreMIDI destination (`wait_for_destination`), and
sends to it. No IAC bus and no second application are required -- only a working
CoreMIDI back-end.

`assert_contains_in_order` matches expected messages as a subsequence, so a test
still passes if something else on the machine happens to write to the port.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import struct
import sys
import time

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
        self.cm.MIDIClientDispose.argtypes = [ref]

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
        self.close()

    def close(self) -> None:
        """Dispose the CoreMIDI client. Idempotent."""
        if getattr(self, "client", None):
            self._api.cm.MIDIClientDispose(self.client)
            self.client = self._api.ref()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

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


def wait_for_destination(name: str, timeout: float = 2.0) -> int | None:
    """Poll for a CoreMIDI destination called `name`; return its endpoint ref.

    A virtual port published by `MidiIn.open_virtual` does not appear in the
    system's destination list instantaneously, so tests that create one and
    then send to it need to wait for it rather than assume.
    """
    if not available():
        return None
    try:
        sender = Sender()
    except Exception:
        return None
    try:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for endpoint, dest_name in sender.destinations():
                if dest_name == name:
                    return endpoint
            time.sleep(0.05)
        return None
    finally:
        sender.close()


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
