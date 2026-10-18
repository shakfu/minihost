"""Linux ALSA sequencer sender, so MIDI-input tests can drive real traffic.

The Linux counterpart of `coremidi_loopback`, with the same API: `available`,
`Sender`, `wait_for_destination`. It binds libasound through ctypes, so it
needs no extra dependency -- only the ALSA sequencer (`/dev/snd/seq`). GitHub's
hosted runners lack it, so tests using this helper skip there.

A destination is a `(client, port)` pair. Messages are encoded into
`snd_seq_event_t` by libasound's own `snd_midi_event_encode` and sent direct,
bypassing any queue.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import sys
import time

_OPEN_OUTPUT = 1
_CAP_READ, _CAP_WRITE, _CAP_SUBS_READ, _CAP_SUBS_WRITE = 0x01, 0x02, 0x20, 0x40
_PORT_TYPE = (1 << 1) | (1 << 20)  # MIDI_GENERIC | APPLICATION
_QUEUE_DIRECT = 253
_EVENT_SIZE = 28  # sizeof(snd_seq_event_t)


def _lib():
    path = ctypes.util.find_library("asound")
    if path is None:
        return None
    lib = ctypes.CDLL(path)
    vp, cp, i = ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int
    sigs = {
        "snd_seq_open": ([ctypes.POINTER(vp), cp, i, i], i),
        "snd_seq_close": ([vp], i),
        "snd_seq_set_client_name": ([vp, cp], i),
        "snd_seq_create_simple_port": ([vp, cp, ctypes.c_uint, ctypes.c_uint], i),
        "snd_seq_client_info_malloc": ([ctypes.POINTER(vp)], i),
        "snd_seq_client_info_free": ([vp], None),
        "snd_seq_client_info_set_client": ([vp, i], None),
        "snd_seq_client_info_get_client": ([vp], i),
        "snd_seq_query_next_client": ([vp, vp], i),
        "snd_seq_port_info_malloc": ([ctypes.POINTER(vp)], i),
        "snd_seq_port_info_free": ([vp], None),
        "snd_seq_port_info_set_client": ([vp, i], None),
        "snd_seq_port_info_set_port": ([vp, i], None),
        "snd_seq_port_info_get_port": ([vp], i),
        "snd_seq_port_info_get_name": ([vp], cp),
        "snd_seq_port_info_get_capability": ([vp], ctypes.c_uint),
        "snd_seq_query_next_port": ([vp, vp], i),
        "snd_midi_event_new": ([ctypes.c_size_t, ctypes.POINTER(vp)], i),
        "snd_midi_event_free": ([vp], None),
        "snd_midi_event_reset_encode": ([vp], None),
        "snd_midi_event_encode": ([vp, cp, ctypes.c_long, vp], ctypes.c_long),
        "snd_seq_event_output_direct": ([vp, vp], i),
    }
    for name, (argtypes, restype) in sigs.items():
        fn = getattr(lib, name)
        fn.argtypes, fn.restype = argtypes, restype
    return lib


def available() -> bool:
    """True if the ALSA sequencer can be opened on this machine."""
    if not sys.platform.startswith("linux"):
        return False
    try:
        Sender().close()
    except Exception:
        return False
    return True


class Sender:
    """An ALSA sequencer output client. Use as a context manager."""

    def __init__(self, name: str = "minihost-test-sender"):
        self._lib = _lib()
        if self._lib is None:
            raise RuntimeError("libasound not found")
        lib = self._lib
        self.seq = ctypes.c_void_p()
        if lib.snd_seq_open(ctypes.byref(self.seq), b"default", _OPEN_OUTPUT, 0) < 0:
            self.seq = ctypes.c_void_p()
            raise RuntimeError("snd_seq_open failed")
        self._parser = ctypes.c_void_p()
        if lib.snd_midi_event_new(256, ctypes.byref(self._parser)) < 0:
            self.close()
            raise RuntimeError("snd_midi_event_new failed")
        lib.snd_seq_set_client_name(self.seq, name.encode())
        self.port = lib.snd_seq_create_simple_port(
            self.seq, b"out", _CAP_READ | _CAP_SUBS_READ, _PORT_TYPE
        )
        if self.port < 0:
            self.close()
            raise RuntimeError("snd_seq_create_simple_port failed")

    def __enter__(self) -> "Sender":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def close(self) -> None:
        """Close the sequencer client. Idempotent."""
        if getattr(self, "_parser", None):
            self._lib.snd_midi_event_free(self._parser)
            self._parser = ctypes.c_void_p()
        if getattr(self, "seq", None):
            self._lib.snd_seq_close(self.seq)
            self.seq = ctypes.c_void_p()

    def __del__(self) -> None:
        try:
            self.close()
        except Exception:
            pass

    def destinations(self) -> list[tuple[tuple[int, int], str]]:
        """[((client, port), name)] for every writable sequencer port."""
        lib = self._lib
        cinfo, pinfo = ctypes.c_void_p(), ctypes.c_void_p()
        lib.snd_seq_client_info_malloc(ctypes.byref(cinfo))
        lib.snd_seq_port_info_malloc(ctypes.byref(pinfo))
        want = _CAP_WRITE | _CAP_SUBS_WRITE
        out = []
        try:
            lib.snd_seq_client_info_set_client(cinfo, -1)
            while lib.snd_seq_query_next_client(self.seq, cinfo) >= 0:
                client = lib.snd_seq_client_info_get_client(cinfo)
                lib.snd_seq_port_info_set_client(pinfo, client)
                lib.snd_seq_port_info_set_port(pinfo, -1)
                while lib.snd_seq_query_next_port(self.seq, pinfo) >= 0:
                    if lib.snd_seq_port_info_get_capability(pinfo) & want != want:
                        continue
                    port = lib.snd_seq_port_info_get_port(pinfo)
                    name = lib.snd_seq_port_info_get_name(pinfo) or b""
                    out.append(((client, port), name.decode(errors="replace")))
        finally:
            lib.snd_seq_port_info_free(pinfo)
            lib.snd_seq_client_info_free(cinfo)
        return out

    def send(self, dest: tuple[int, int], message: bytes) -> None:
        """Send one short MIDI message direct to `dest`."""
        lib = self._lib
        ev = (ctypes.c_ubyte * _EVENT_SIZE)()
        lib.snd_midi_event_reset_encode(self._parser)
        n = lib.snd_midi_event_encode(self._parser, bytes(message), len(message), ev)
        if n != len(message):
            raise RuntimeError(f"snd_midi_event_encode rejected {message!r}")
        # Field offsets in snd_seq_event_t: queue @3, source.port @13, dest @14.
        ev[3] = _QUEUE_DIRECT
        ev[13] = self.port
        ev[14], ev[15] = dest
        rc = lib.snd_seq_event_output_direct(self.seq, ev)
        if rc < 0:
            raise RuntimeError(f"snd_seq_event_output_direct failed: {rc}")


def wait_for_destination(name: str, timeout: float = 2.0) -> tuple[int, int] | None:
    """Poll for a writable sequencer port called `name`; return (client, port)."""
    try:
        sender = Sender()
    except Exception:
        return None
    try:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for dest, port_name in sender.destinations():
                if port_name == name:
                    return dest
            time.sleep(0.05)
        return None
    finally:
        sender.close()
