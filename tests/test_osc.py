"""OSC transport: a UDP port in, a UDP sender out.

Built on JUCE's juce_osc rather than a vendored OSC library -- it is already
in the tree, needs only juce_events, and carries the same licence as every
other JUCE module minihost links, so it adds no dependency. The comparison
against embedding liblo is in docs/dev/osc_and_touch.md.

These tests are loopback only: a server on an OS-chosen port and a client
aimed at it, so nothing here needs a network, a fixed port, or a second
machine.
"""

from __future__ import annotations

import os
import threading

import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)


class Collector:
    """Gathers messages and lets a test wait for a specific count.

    Waiting on an event rather than sleeping a fixed interval: loopback UDP is
    quick but not synchronous, and a sleep long enough to be reliable on a
    loaded machine makes the whole file slow.
    """

    def __init__(self, expected: int = 1):
        self.messages: list[tuple[str, list[float]]] = []
        self._expected = expected
        self._done = threading.Event()

    def __call__(self, address: str, args: list[float]) -> None:
        self.messages.append((address, args))
        if len(self.messages) >= self._expected:
            self._done.set()

    def wait(self, timeout: float = 5.0) -> bool:
        return self._done.wait(timeout)


# -- binding ------------------------------------------------------------------


def test_port_zero_reports_the_port_it_was_given():
    """Opening on 0 must say which port the OS chose, or a test cannot
    connect to it and neither can anything else."""
    with minihost.OscServer.open(0, lambda a, v: None) as server:
        assert server.port > 0


def test_two_servers_cannot_share_a_port():
    with minihost.OscServer.open(0, lambda a, v: None) as first:
        with pytest.raises(RuntimeError, match="Failed to open OSC server"):
            minihost.OscServer.open(first.port, lambda a, v: None)


def test_an_out_of_range_port_is_refused():
    with pytest.raises(RuntimeError):
        minihost.OscServer.open(70000, lambda a, v: None)
    with pytest.raises(RuntimeError):
        minihost.OscClient("127.0.0.1", 0)


# -- round trip ---------------------------------------------------------------


def test_a_float_arrives_intact():
    collector = Collector()
    with minihost.OscServer.open(0, collector) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            client.send("/mh/param/cutoff", 0.75)
            assert collector.wait(), "no OSC message arrived"

    address, args = collector.messages[0]
    assert address == "/mh/param/cutoff"
    assert args == pytest.approx([0.75])


def test_an_int_is_converted_to_float():
    """int32 is a normal thing for a surface to send for a toggle."""
    collector = Collector()
    with minihost.OscServer.open(0, collector) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            client.send("/mh/transport/loop", 1)
            assert collector.wait()

    assert collector.messages[0] == ("/mh/transport/loop", [1.0])


def test_a_bool_is_sent_as_an_int():
    """bool is an int in Python, and a caller writing True means 1."""
    collector = Collector()
    with minihost.OscServer.open(0, collector) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            client.send("/mh/transport/loop", True)
            assert collector.wait()

    assert collector.messages[0] == ("/mh/transport/loop", [1.0])


def test_a_message_with_no_arguments_is_a_trigger():
    collector = Collector()
    with minihost.OscServer.open(0, collector) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            client.send("/mh/transport/play")
            assert collector.wait()

    assert collector.messages[0] == ("/mh/transport/play", [])


def test_several_floats_arrive_in_order():
    collector = Collector()
    with minihost.OscServer.open(0, collector) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            client.send("/mh/xy", [0.1, 0.9])
            assert collector.wait()

    _, args = collector.messages[0]
    assert args == pytest.approx([0.1, 0.9], abs=1e-6)


def test_a_string_argument_holds_its_position_as_zero():
    """Documented behaviour, and the reason it is not simply dropped.

    Skipping a non-numeric argument would shift every later index, so a
    surface sending (label, value) would deliver its value at index 0 to one
    receiver and index 1 to another. Reporting 0.0 keeps positions stable.
    """
    collector = Collector()
    with minihost.OscServer.open(0, collector) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            client.send("/mh/name", "hello")
            assert collector.wait()

    assert collector.messages[0] == ("/mh/name", [0.0])


def test_messages_arrive_in_send_order():
    collector = Collector(expected=3)
    with minihost.OscServer.open(0, collector) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            for i in range(3):
                client.send("/mh/step", float(i))
            assert collector.wait()

    assert [args[0] for _, args in collector.messages] == pytest.approx([0.0, 1.0, 2.0])


# -- address validation -------------------------------------------------------


@pytest.mark.parametrize("address", ["no-leading-slash", "", "/bad address"])
def test_an_invalid_address_raises_rather_than_escaping_as_a_juce_exception(address):
    """juce_osc signals a bad address by throwing OSCFormatError. That must
    not cross back into C, and must reach Python as a normal error."""
    with minihost.OscServer.open(0, lambda a, v: None) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            with pytest.raises(RuntimeError, match="Failed to send OSC"):
                client.send(address, 1.0)


def test_an_unsupported_value_type_is_a_type_error():
    with minihost.OscServer.open(0, lambda a, v: None) as server:
        with minihost.OscClient("127.0.0.1", server.port) as client:
            with pytest.raises(TypeError):
                client.send("/mh/x", {"not": "sendable"})


# -- lifetime -----------------------------------------------------------------


def test_close_is_idempotent():
    server = minihost.OscServer.open(0, lambda a, v: None)
    server.close()
    server.close()

    client = minihost.OscClient("127.0.0.1", 9999)
    client.close()
    client.close()


def test_sending_on_a_closed_client_raises():
    client = minihost.OscClient("127.0.0.1", 9999)
    client.close()
    with pytest.raises(RuntimeError, match="closed"):
        client.send("/mh/x", 1.0)


def test_no_callback_fires_after_close():
    """close() joins the socket thread, so a message sent afterwards cannot
    reach a callback that is about to be torn down."""
    collector = Collector()
    server = minihost.OscServer.open(0, collector)
    port = server.port
    client = minihost.OscClient("127.0.0.1", port)

    client.send("/mh/before", 1.0)
    assert collector.wait()
    server.close()

    client.send("/mh/after", 2.0)
    client.close()

    assert [a for a, _ in collector.messages] == ["/mh/before"]


# -- integration with the parameter ring (Phase 0) ----------------------------


@skip_if_no_plugin
def test_osc_can_drive_a_plugin_parameter():
    """The point of the whole layer: a message on the wire moves a parameter.

    The socket thread hands the value to AudioDevice.send_param_control, which
    is lock-free, so the OSC thread never blocks and the value is applied by
    the audio thread at the next block boundary.
    """
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    applied = threading.Event()

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:

        def on_osc(address: str, args: list[float]) -> None:
            if address == "/mh/param/0" and args:
                audio.send_param_control(0, args[0])
                applied.set()

        with minihost.OscServer.open(0, on_osc) as server:
            with minihost.OscClient("127.0.0.1", server.port) as client:
                audio.start()
                client.send("/mh/param/0", 0.8)
                assert applied.wait(5.0), "the OSC message never arrived"
                import time

                time.sleep(0.2)
                audio.stop()

    assert plugin.get_param(0) == pytest.approx(0.8, abs=1e-3)


# -- native OSC input (no Python on the socket thread) ------------------------


@skip_if_no_plugin
def test_connect_osc_drives_a_parameter_with_no_python_in_the_path():
    """AudioDevice.connect_osc parses the address in C and pushes to the ring.

    Nothing Python-side runs on the socket thread, so no GIL is taken per
    message -- the reason this exists alongside OscServer.
    """
    import time

    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.connect_osc(0)
        assert audio.osc_port > 0
        audio.start()
        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send("/mh/param/0", 0.4)
            time.sleep(0.3)
        audio.stop()
        audio.disconnect_osc()
        assert audio.osc_port == -1

    assert plugin.get_param(0) == pytest.approx(0.4, abs=1e-3)


@skip_if_no_plugin
def test_the_slot_form_addresses_a_chain_slot():
    import time

    a = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if a.num_params == 0:
        pytest.skip("plugin exposes no parameters")
    chain = minihost.PluginChain([a])

    with minihost.AudioDevice(chain, sample_rate=48000, buffer_frames=256) as audio:
        audio.connect_osc(0)
        audio.start()
        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send("/mh/0/param/0", 0.3)
            time.sleep(0.3)
        audio.stop()

    assert a.get_param(0) == pytest.approx(0.3, abs=1e-3)


@skip_if_no_plugin
@pytest.mark.parametrize(
    "address",
    [
        "/mh/param/notanumber",
        "/mh/param/",
        "/mh/param",
        "/other/param/0",
        "/mh/param/0/extra",
        "/mh/x/param/0",
        "/mh/-1/param/0",
    ],
)
def test_unrecognised_addresses_are_ignored_not_misrouted(address):
    """A near-miss address must do nothing at all.

    The failure that matters here is not a crash but a silent misroute -- an
    address that parses as some *other* parameter index and moves the wrong
    control.
    """
    import time

    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.connect_osc(0)
        audio.start()
        plugin.set_param(0, 0.5)
        before = [plugin.get_param(i) for i in range(min(8, plugin.num_params))]
        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send(address, 0.9)
            time.sleep(0.2)
        audio.stop()

    after = [plugin.get_param(i) for i in range(min(8, plugin.num_params))]
    assert after == pytest.approx(before), f"{address!r} moved a parameter"


@skip_if_no_plugin
def test_an_out_of_range_parameter_index_is_harmless():
    """The ring carries the index; mh_process_auto range-checks before it is
    applied. The device must keep running and keep accepting real writes."""
    import time

    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.connect_osc(0)
        audio.start()
        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send("/mh/param/999999", 0.9)
            time.sleep(0.1)
            # The device survived it and still applies a valid write.
            client.send("/mh/param/0", 0.25)
            time.sleep(0.2)
        assert audio.is_playing
        audio.stop()

    assert plugin.get_param(0) == pytest.approx(0.25, abs=1e-3)


@skip_if_no_plugin
def test_connect_osc_twice_rebinds_rather_than_leaking_the_first_port():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.connect_osc(0)
        first = audio.osc_port
        audio.connect_osc(0)
        second = audio.osc_port
        assert first > 0 and second > 0

        # The first port must have been released, so it can be bound again.
        with minihost.OscServer.open(first, lambda a, v: None) as probe:
            assert probe.port == first
