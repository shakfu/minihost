"""Inputs that used to crash the interpreter.

Each ran from Python with no misuse beyond an unusual value:

  * ``MidiFile().duration_seconds`` read ``back()`` of an empty track; so did
    loading a 14-byte type-1 file declaring no tracks. ``add_*`` with a track
    index out of range wrote out of bounds.
  * Array shapes were cast to ``int`` unchecked: ``(1, 2**31)`` crashed,
    ``(1, 2**32 + 10)`` silently became ``(1, 10)``.
  * A ``MidiIn`` callback that raised terminated the process from the
    libremidi thread; an ``OscServer`` one silently ended its receive thread.
  * One UDP datagram of 2000 nested OSC bundles overflowed the socket
    thread's stack.

Crash-prone cases run in their own interpreter so a regression fails one
test rather than the whole run.
"""

from __future__ import annotations

import os
import socket
import struct
import subprocess
import sys
import textwrap
import time

import pytest

import minihost
from cli_helpers import find_test_plugin

np = pytest.importorskip("numpy")

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")


def run_isolated(body: str) -> str:
    proc = subprocess.run(
        [sys.executable, "-c", "import minihost\n" + textwrap.dedent(body)],
        capture_output=True,
        text=True,
        timeout=120,
    )
    assert proc.returncode == 0, (
        f"exit {proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )
    return proc.stdout


# -------------------------------------------------------------------- #
# MidiFile                                                              #
# -------------------------------------------------------------------- #


def test_empty_midifile_duration_is_zero():
    out = run_isolated("""
        m = minihost.MidiFile()
        print(m.duration_seconds)
        m.add_track()
        print(m.duration_seconds)
    """)
    assert out.split() == ["0.0", "0.0"]


def test_midifile_declaring_no_tracks(tmp_path):
    path = tmp_path / "zero_tracks.mid"
    path.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 1, 0, 480))
    out = run_isolated(f"""
        m = minihost.MidiFile()
        m.load({str(path)!r})
        print(m.duration_seconds)
    """)
    assert out.strip() in ("0.0", "-1.0")


@pytest.mark.parametrize("track", [-1, 1, 5])
def test_midifile_add_rejects_track_out_of_range(track):
    out = run_isolated(f"""
        m = minihost.MidiFile()
        for add in (lambda: m.add_note_on({track}, 0, 0, 60, 100),
                    lambda: m.add_tempo({track}, 0, 120.0),
                    lambda: m.add_control_change({track}, 0, 0, 7, 64)):
            try:
                add(); print("accepted")
            except IndexError:
                print("IndexError")
    """)
    assert out.split() == ["IndexError"] * 3


# -------------------------------------------------------------------- #
# Array dimensions above INT_MAX                                        #
# -------------------------------------------------------------------- #


# Zero-filled float32 arrays mapped from a sparse temp file. On the Linux CI
# runner neither np.zeros nor an anonymous MAP_NORESERVE mapping of 32 GiB
# succeeds (ENOMEM); a shared file mapping is not charged to the commit limit.
_HUGE_ZEROS = """
import mmap
import tempfile
import numpy as np
def huge_zeros(shape):
    n = 4 * int(np.prod(shape))
    f = tempfile.TemporaryFile()
    f.truncate(n)
    return np.frombuffer(mmap.mmap(f.fileno(), n), np.float32).reshape(shape)
"""


@pytest.mark.parametrize("frames", [2**31, 2**32 + 10])
def test_from_numpy_rejects_dimension_above_int_max(frames):
    out = run_isolated(
        _HUGE_ZEROS
        + textwrap.dedent(f"""
        try:
            b = minihost.AudioBuffer.from_numpy(huge_zeros((1, {frames})))
            print("accepted", b.shape)
        except ValueError as e:
            print("ValueError")
    """)
    )
    assert out.strip() == "ValueError"


@pytest.mark.skipif(not FX or not os.path.exists(FX), reason="MinihostTestFx not built")
def test_process_rejects_frames_above_int_max():
    # (int) 2**32+10 is 10: channel 1's pointer landed on channel 0's data.
    out = run_isolated(
        _HUGE_ZEROS
        + textwrap.dedent(f"""
        p = minihost.Plugin({FX!r}, sample_rate=48000.0, max_block_size=512)
        x = huge_zeros((2, 2**32 + 10))
        try:
            p.process(x, huge_zeros(x.shape)); print("accepted")
        except ValueError:
            print("ValueError")
    """)
    )
    assert out.strip() == "ValueError"


def test_resample_beyond_buffer_size_is_refused_up_front():
    # About 2.25e9 output frames: used to compute for ~40 s, then overflow.
    t0 = time.monotonic()
    with pytest.raises(RuntimeError, match="maximum output length"):
        minihost.audio_io.resample(np.zeros((1, 3), np.float32), 1, 750_000_000)
    assert time.monotonic() - t0 < 5.0


# -------------------------------------------------------------------- #
# Callbacks that raise                                                  #
# -------------------------------------------------------------------- #


def _osc_message(address: bytes) -> bytes:
    def pad(b):
        return b + b"\0" * (4 - len(b) % 4)

    return pad(address) + pad(b",f") + struct.pack(">f", 0.5)


def _send(port: int, packet: bytes) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 17)
    sock.sendto(packet, ("127.0.0.1", port))
    sock.close()


def _wait_for(pred, timeout=3.0):
    t0 = time.monotonic()
    while not pred() and time.monotonic() - t0 < timeout:
        time.sleep(0.02)
    return pred()


# The exception is reported as unraisable, which pytest surfaces as a warning.
@pytest.mark.filterwarnings("ignore::pytest.PytestUnraisableExceptionWarning")
def test_osc_server_survives_a_raising_callback():
    got = []

    def cb(address, args):
        got.append(address)
        if address == "/bad":
            raise ValueError("boom")

    server = minihost.OscServer.open(0, cb)
    try:
        for a in (b"/ok1", b"/bad", b"/ok2"):
            _send(server.port, _osc_message(a))
            time.sleep(0.1)
        assert _wait_for(lambda: "/ok2" in got), got
    finally:
        server.close()


@pytest.mark.skipif(sys.platform != "darwin", reason="needs a CoreMIDI loopback")
def test_midiin_survives_a_raising_callback():
    tests_dir = os.path.dirname(__file__)
    out = run_isolated(f"""
        import sys, time
        sys.path.insert(0, {tests_dir!r})
        import coremidi_loopback
        if not coremidi_loopback.available():
            print("skip"); raise SystemExit
        got = []
        def cb(data):
            got.append(bytes(data))
            raise ValueError("boom")
        m = minihost.MidiIn.open_virtual("mh-test-raise", cb)
        ep = coremidi_loopback.wait_for_destination("mh-test-raise")
        with coremidi_loopback.Sender() as s:
            for n in range(3):
                s.send(ep, bytes([0x90, 60 + n, 100])); time.sleep(0.05)
            time.sleep(0.3)
        m.close()
        print(len(got))
    """)
    if out.strip() == "skip":
        pytest.skip("CoreMIDI loopback unavailable")
    # Every message reached the callback: the first exception neither
    # terminated the process nor stopped delivery.
    assert out.strip() == "3"


# -------------------------------------------------------------------- #
# Nested OSC bundles                                                    #
# -------------------------------------------------------------------- #


def _nested_bundle(depth: int, leaf: bytes) -> bytes:
    hdr = b"#bundle\0" + b"\0" * 8
    body = hdr + struct.pack(">i", len(leaf)) + leaf
    for _ in range(depth - 1):
        body = hdr + struct.pack(">i", len(body)) + body
    return body


def test_deeply_nested_osc_bundle_is_dropped_not_fatal():
    out = run_isolated(f"""
        import socket, struct, time
        exec({_SEND_HELPERS!r})
        got = []
        server = minihost.OscServer.open(0, lambda a, x: got.append(a))
        send(server.port, nested(2000, msg(b"/deep")))
        time.sleep(0.3)
        send(server.port, msg(b"/after"))
        t0 = time.time()
        while "/after" not in got and time.time() - t0 < 3: time.sleep(0.02)
        server.close()
        print(got)
    """)
    assert out.strip() == "['/after']"


def test_osc_bundle_depth_limit():
    got = []
    server = minihost.OscServer.open(0, lambda a, x: got.append(a))
    try:
        _send(server.port, _nested_bundle(16, _osc_message(b"/sixteen")))
        _send(server.port, _nested_bundle(17, _osc_message(b"/seventeen")))
        _send(server.port, _osc_message(b"/done"))
        assert _wait_for(lambda: "/done" in got), got
        assert "/sixteen" in got
        assert "/seventeen" not in got
    finally:
        server.close()


_SEND_HELPERS = """
def pad(b): return b + b"\\0" * (4 - len(b) % 4)
def msg(a): return pad(a) + pad(b",f") + struct.pack(">f", 0.5)
def nested(depth, leaf):
    hdr = b"#bundle\\0" + b"\\0" * 8
    body = hdr + struct.pack(">i", len(leaf)) + leaf
    for _ in range(depth - 1):
        body = hdr + struct.pack(">i", len(body)) + body
    return body
def send(port, packet):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 17)
    s.sendto(packet, ("127.0.0.1", port)); s.close()
"""


# -------------------------------------------------------------------- #
# OscServer.close() while a callback runs                               #
# -------------------------------------------------------------------- #


def _two_message_bundle(a: bytes, b: bytes) -> bytes:
    els = [_osc_message(a), _osc_message(b)]
    return (
        b"#bundle\0" + b"\0" * 8 + b"".join(struct.pack(">i", len(e)) + e for e in els)
    )


def test_osc_close_from_its_own_callback_returns_and_stops_delivery():
    # The socket thread waited on itself: close() never returned and the
    # server leaked.
    calls = []
    holder = {}

    def cb(address, args):
        calls.append(address)
        t0 = time.monotonic()
        holder["server"].close()
        calls.append(round(time.monotonic() - t0, 1))

    holder["server"] = minihost.OscServer.open(0, cb)
    _send(holder["server"].port, _two_message_bundle(b"/a", b"/b"))
    assert _wait_for(lambda: len(calls) >= 2), calls
    time.sleep(0.3)
    assert calls == ["/a", 0.0]  # returned at once; "/b" never delivered


def test_osc_close_waits_for_a_running_callback():
    started = []

    def cb(address, args):
        started.append(time.monotonic())
        time.sleep(0.3)

    server = minihost.OscServer.open(0, cb)
    _send(server.port, _osc_message(b"/slow"))
    assert _wait_for(lambda: started)
    server.close()
    assert time.monotonic() - started[0] >= 0.3


def test_osc_close_outlasting_juce_disconnect_timeout():
    # JUCE's disconnect() gives up after 10 s; the server was then freed
    # under a callback still running, which crashed when it returned.
    out = run_isolated(f"""
        import socket, struct, time
        exec({_SEND_HELPERS!r})
        started = []
        def cb(a, x):
            started.append(a); time.sleep(11)
        server = minihost.OscServer.open(0, cb)
        send(server.port, msg(b"/slow"))
        while not started: time.sleep(0.02)
        t0 = time.time(); server.close()
        print("closed after %.0f" % (time.time() - t0))
    """)
    assert out.strip() == "closed after 11"


# -------------------------------------------------------------------- #
# Non-numeric and non-finite OSC input                                  #
# -------------------------------------------------------------------- #


def _typed(address: bytes, tag: bytes, payload: bytes) -> bytes:
    def pad(b):
        return b + b"\0" * (4 - len(b) % 4)

    return pad(address) + pad(b"," + tag) + payload


def test_osc_string_argument_is_nan_and_unparsable_types_are_counted():
    # A string used to arrive as 0.0, which a bound fader took as a write to 0;
    # d/T/h packets vanished without a trace.
    got = []
    server = minihost.OscServer.open(0, lambda a, x: got.append((a, x)))
    try:
        _send(server.port, _typed(b"/s", b"s", b"x\0\0\0"))
        _send(server.port, _typed(b"/d", b"d", struct.pack(">d", 0.5)))
        _send(server.port, _typed(b"/T", b"T", b""))
        _send(server.port, _typed(b"/h", b"h", struct.pack(">q", 5)))
        _send(server.port, _osc_message(b"/f"))
        assert _wait_for(lambda: any(a == "/f" for a, _ in got)), got
        strings = [x for a, x in got if a == "/s"]
        assert len(strings) == 1 and strings[0][0] != strings[0][0]  # NaN
        assert _wait_for(lambda: server.format_errors == 3), server.format_errors
    finally:
        server.close()


@pytest.mark.skipif(not FX or not os.path.exists(FX), reason="MinihostTestFx not built")
def test_osc_transport_ignores_non_finite_values():
    from device_helpers import audio_device_available

    if not audio_device_available():
        pytest.skip("no audio playback device")
    plugin = minihost.Plugin(FX, sample_rate=48000.0, max_block_size=512)
    try:
        dev = minihost.AudioDevice(plugin)
    except RuntimeError as e:
        pytest.skip(f"no usable audio device: {e}")
    try:
        dev.set_transport_enabled(True)
        dev.connect_osc(0)
        dev.start()

        def msg(address, value):
            return _typed(address, b"f", struct.pack(">f", value))

        _send(dev.osc_port, msg(b"/mh/transport/play", 1.0))
        _send(dev.osc_port, msg(b"/mh/transport/bpm", 90.0))
        assert _wait_for(lambda: (dev.transport or {}).get("bpm") == 90.0)
        for v in (float("nan"), float("inf")):
            _send(dev.osc_port, msg(b"/mh/transport/bpm", v))
        _send(dev.osc_port, msg(b"/mh/transport/position", float("nan")))
        time.sleep(0.4)
        assert dev.transport["bpm"] == 90.0
        # 3e38 beats overflowed the cast to long long; now clamped.
        _send(dev.osc_port, msg(b"/mh/transport/position", 3e38))
        assert _wait_for(lambda: dev.transport["position_samples"] > 10**18)
        with pytest.raises(ValueError, match="finite"):
            dev.transport_set_bpm(float("nan"))
        dev.stop()
    finally:
        plugin.close()  # the device keeps its own reference


@pytest.mark.skipif(not FX or not os.path.exists(FX), reason="MinihostTestFx not built")
def test_plugin_transport_rejects_nan_tempo():
    plugin = minihost.Plugin(FX, sample_rate=48000.0, max_block_size=512)
    try:
        with pytest.raises(ValueError, match="finite"):
            plugin.set_transport(bpm=float("nan"))
    finally:
        plugin.close()


# -------------------------------------------------------------------- #
# MIDI file timing and MidiFile.add_* ranges                            #
# -------------------------------------------------------------------- #


def _vlq(n: int) -> bytes:
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.append(0x80 | (n & 0x7F))
        n >>= 7
    return bytes(reversed(out))


def _smf(division: int, events: bytes) -> bytes:
    track = events + b"\x00\xff\x2f\x00"
    return (
        b"MThd"
        + struct.pack(">IHHH", 6, 0, 1, division)
        + b"MTrk"
        + struct.pack(">I", len(track))
        + track
    )


NOTE = b"\x00\x90\x3c\x64" + _vlq(96) + b"\x80\x3c\x00"


@pytest.mark.parametrize(
    "name, data",
    [
        # Deltas summing past INT_MAX wrapped to negative event times.
        (
            "tick-overflow",
            _smf(96, b"".join(_vlq(0x0FFFFFFF) + b"\x90\x3c\x64" for _ in range(20))),
        ),
        # Division 0 divided by zero when rendering.
        ("division-0", _smf(0, NOTE)),
        # SMPTE (25 fps x 40): midifile stored ticks per second as ticks per
        # quarter, and the renderer applied tempo to it.
        ("smpte", _smf(0xE728, NOTE)),
    ],
)
def test_unsupported_midi_files_are_refused(tmp_path, name, data):
    path = tmp_path / f"{name}.mid"
    path.write_bytes(data)
    assert minihost.MidiFile().load(str(path)) is False


def test_zero_tempo_is_skipped(tmp_path):
    # A 0 us tempo read as bpm inf: note-on and note-off landed together.
    path = tmp_path / "tempo0.mid"
    path.write_bytes(_smf(96, b"\x00\xff\x51\x03\x00\x00\x00" + NOTE))
    mf = minihost.MidiFile()
    assert mf.load(str(path))
    assert mf.duration_seconds == pytest.approx(0.5)
    from minihost.render import midi_file_to_events

    assert [e[0] for e in midi_file_to_events(mf, 48000)] == [0, 24000]


def test_failed_load_leaves_the_file_unchanged(tmp_path):
    good = tmp_path / "good.mid"
    good.write_bytes(_smf(96, NOTE))
    bad = tmp_path / "bad.mid"
    bad.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 1, 65535, 96))
    mf = minihost.MidiFile()
    assert mf.load(str(good))
    before = mf.get_events(0)
    assert mf.load(str(bad)) is False
    assert mf.num_tracks == 1 and mf.get_events(0) == before


def test_pitch_bend_round_trips_its_14_bit_value():
    # The int went straight into midifile's -1..1 amount: 0 was written as
    # centre and 8192 as full up.
    mf = minihost.MidiFile()
    for v in (0, 8192, 16383):
        mf.add_pitch_bend(0, 0, 0, v)
    got = [e["value"] for e in mf.get_events(0) if e["type"] == "pitch_bend"]
    assert got == [0, 8192, 16383]


@pytest.mark.parametrize(
    "call, match",
    [
        (lambda m: m.add_note_on(0, 0, 20, 60, 100), "channel"),
        (lambda m: m.add_note_on(0, 0, 0, 300, 100), "pitch"),
        (lambda m: m.add_note_off(0, 0, 0, 60, -5), "velocity"),
        (lambda m: m.add_note_on(0, -1, 0, 60, 100), "tick"),
        (lambda m: m.add_control_change(0, 0, 0, 128, 0), "controller"),
        (lambda m: m.add_program_change(0, 0, 0, 200), "program"),
        (lambda m: m.add_pitch_bend(0, 0, 0, 16384), "value"),
        (lambda m: m.add_tempo(0, 0, 0.0), "bpm"),
        (lambda m: setattr(m, "ticks_per_quarter", 0), "ticks_per_quarter"),
    ],
)
def test_midifile_add_rejects_out_of_range_values(call, match):
    # midifile masked them: channel 20 became 4, pitch 300 became 44.
    with pytest.raises(ValueError, match=match):
        call(minihost.MidiFile())
