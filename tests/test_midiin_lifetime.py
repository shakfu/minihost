"""Regression tests: MidiIn must stay at the address it registered with.

Pre-fix, the factories looked like:

    static MidiIn open(int port_index, nb::callable callback) {
        MidiIn m;
        m.callback_ = std::move(callback);
        m.handle_ = mh_midi_in_open(port_index, &MidiIn::midi_callback, &m, ...);
        return m;                       // <- &m is a stack address
    }

`&m` escaped into the C layer as the callback's user_data, but the function
returned by value: nanobind then move-constructed the result into its heap
instance, so the registered pointer referred to destroyed stack memory. The
first MIDI byte received dereferenced it, acquired the GIL, and called through
a garbage nb::callable -- a use-after-free affecting the whole standalone MIDI
input feature (MidiIn, MidiMapper, `minihost midi -m`).

The fix heap-allocates in the factory, registers the final address, and hands
ownership to Python via nb::rv_policy::take_ownership. MidiIn's copy *and*
move operations are now deleted so the address can never change again -- that
deletion is the real guard, and it is enforced at compile time.

The functional tests below need a real MIDI input port, so they skip on
machines (and CI runners) without one. The structural tests always run.

The last test is the one that actually reproduced the bug: it drives real MIDI
through a loopback bus (macOS IAC Driver) and asserts the callback fires with
the right payload. Pre-fix it segfaulted the interpreter (exit 139) before
delivering anything; post-fix all messages arrive intact.
"""

from __future__ import annotations

import gc
import time

import pytest

import minihost

import coremidi_loopback


def _input_ports():
    try:
        return minihost.midi_get_input_ports()
    except Exception:
        return []


def _openable_port():
    """Index of a MIDI input port this process can actually open, or None.

    Being *listed* does not mean being *openable*: enumeration reports every
    endpoint the platform publishes, including virtual ports owned by another
    running application (a DAW, Max/MSP, ...), which can refuse or intermittently
    fail an open depending on what that application is doing. Probing once here
    keeps the tests below measuring minihost rather than the neighbours.
    """
    for port in _input_ports():
        try:
            handle = minihost.MidiIn.open(port["index"], lambda data: None)
        except Exception:
            continue
        handle.close()
        return port["index"]
    return None


_OPENABLE_PORT = _openable_port()

skip_if_no_midi_port = pytest.mark.skipif(
    _OPENABLE_PORT is None,
    reason="no openable MIDI input port available on this machine",
)

skip_if_no_loopback = pytest.mark.skipif(
    coremidi_loopback.find_loopback(_input_ports()) is None,
    reason=(
        "no MIDI loopback available "
        "(macOS only; enable a bus in Audio MIDI Setup > IAC Driver)"
    ),
)


# --- structural / always-runnable ------------------------------------- #


def test_open_with_invalid_port_raises_cleanly():
    """A failed open must raise, not crash, and must not leave a
    half-registered object behind. Pre-fix this also constructed a stack
    MidiIn and handed its address to the C layer before failing.
    """
    with pytest.raises(RuntimeError, match="Failed to open MIDI input"):
        minihost.MidiIn.open(9999, lambda data: None)


def test_open_with_negative_port_raises_cleanly():
    with pytest.raises(RuntimeError, match="Failed to open MIDI input"):
        minihost.MidiIn.open(-1, lambda data: None)


def test_close_is_idempotent():
    """close() on a never-opened / already-closed handle must be safe."""
    with pytest.raises(RuntimeError):
        m = minihost.MidiIn.open(9999, lambda data: None)
        m.close()
        m.close()


# --- functional (needs hardware) -------------------------------------- #


@skip_if_no_midi_port
def test_open_survives_gc_and_close():
    """Open a real port, force a collection cycle (which pre-fix could
    already have reclaimed the stack-registered object's storage), let the
    MIDI thread run, then close. Any callback arriving in this window went
    through the registered pointer.
    """
    received = []
    port = _OPENABLE_PORT

    m = minihost.MidiIn.open(port, lambda data: received.append(data))
    try:
        gc.collect()
        time.sleep(0.2)
        gc.collect()
    finally:
        m.close()


@skip_if_no_midi_port
def test_open_context_manager_round_trip():
    port = _OPENABLE_PORT
    with minihost.MidiIn.open(port, lambda data: None):
        time.sleep(0.1)


@skip_if_no_midi_port
def test_repeated_open_close_cycles():
    """Repeated cycles shake out both the dangling-pointer bug and any
    double-free introduced by the ownership change.
    """
    port = _OPENABLE_PORT
    for _ in range(5):
        m = minihost.MidiIn.open(port, lambda data: None)
        time.sleep(0.05)
        m.close()
        del m
        gc.collect()


@skip_if_no_midi_port
def test_two_simultaneous_handles_are_independent():
    """Two open handles must each dispatch to their own callback. Pre-fix
    both registered stack addresses, so this was doubly undefined.
    """
    port = _OPENABLE_PORT
    a, b = [], []
    m1 = minihost.MidiIn.open(port, lambda d: a.append(d))
    try:
        m2 = minihost.MidiIn.open(port, lambda d: b.append(d))
        try:
            time.sleep(0.1)
        finally:
            m2.close()
    finally:
        m1.close()


# --- the actual reproduction (needs a loopback bus) ------------------- #


def _churn_stack(depth: int = 0) -> int:
    """Overwrite the stack region that MidiIn::open used to return from.

    Pre-fix, the C layer's user_data pointed into that frame, so reusing it
    corrupted the callback target before any message arrived.
    """
    junk = [b"\xaa" * 512 for _ in range(8)]
    if depth < 40:
        _churn_stack(depth + 1)
    return len(junk)


@skip_if_no_loopback
def test_callback_receives_real_midi_after_stack_churn():
    """End-to-end reproduction of the use-after-free.

    Pre-fix this crashed the interpreter with SIGSEGV. Post-fix every sent
    message is delivered with an intact payload.
    """
    pair = coremidi_loopback.find_loopback(_input_ports())
    assert pair is not None
    port_index, endpoint = pair

    received: list[bytes] = []
    midi_in = minihost.MidiIn.open(
        port_index, lambda data: received.append(bytes(data))
    )
    try:
        _churn_stack()
        gc.collect()

        sent = []
        with coremidi_loopback.Sender() as sender:
            for note in (60, 64, 67):
                on = bytes([0x90, note, 100])
                off = bytes([0x80, note, 0])
                sender.send(endpoint, on)
                time.sleep(0.05)
                sender.send(endpoint, off)
                time.sleep(0.05)
                sent += [on, off]
            time.sleep(0.4)
    finally:
        midi_in.close()

    # The loopback bus is shared with the rest of the system, so match as a
    # subsequence rather than asserting an exact count.
    coremidi_loopback.assert_contains_in_order(received, sent)


@skip_if_no_loopback
def test_callback_stops_after_close():
    """close() must unregister before the object dies -- otherwise the
    libremidi thread could still reach a freed callback holder.
    """
    pair = coremidi_loopback.find_loopback(_input_ports())
    assert pair is not None
    port_index, endpoint = pair

    received: list[bytes] = []
    midi_in = minihost.MidiIn.open(
        port_index, lambda data: received.append(bytes(data))
    )
    midi_in.close()
    del midi_in
    gc.collect()

    with coremidi_loopback.Sender() as sender:
        sender.send(endpoint, bytes([0x90, 72, 100]))
        time.sleep(0.3)

    assert received == [], f"callback fired after close(): {received!r}"
