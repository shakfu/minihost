"""Feedback: parameter values back out to a surface.

Without it a generated surface is write-only -- load a preset and every fader
lies, showing where the finger left it rather than where the parameter is.

`poll_once` is public precisely so these tests need no timing: they drive the
poll directly instead of sleeping and hoping.
"""

from __future__ import annotations

import os
import threading
import time
from unittest.mock import MagicMock

import pytest

import minihost
from minihost import OscFeedback, OscMapper

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)


class FakeClient:
    def __init__(self):
        self.sent: list[tuple[str, float]] = []
        self.fail = False

    def send(self, address, value=None):
        if self.fail:
            raise RuntimeError("closed")
        self.sent.append((address, value))


def _make_plugin(values: dict[int, float]) -> MagicMock:
    plugin = MagicMock()
    plugin.get_param = MagicMock(side_effect=lambda i: values[i])
    return plugin


# -- what gets sent -----------------------------------------------------------


def test_the_first_poll_sends_every_watched_parameter():
    values = {0: 0.25, 1: 0.75}
    client = FakeClient()
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0, "/b": 1})

    assert fb.poll_once() == 2
    assert sorted(client.sent) == [("/a", 0.25), ("/b", 0.75)]


def test_an_unchanged_parameter_is_not_resent():
    """A surface that is already right does not need a packet."""
    values = {0: 0.25}
    client = FakeClient()
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0})

    fb.poll_once()
    assert fb.poll_once() == 0
    assert len(client.sent) == 1


def test_a_change_is_sent():
    values = {0: 0.25}
    client = FakeClient()
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0})

    fb.poll_once()
    values[0] = 0.8
    assert fb.poll_once() == 1
    assert client.sent[-1] == ("/a", 0.8)


def test_a_change_below_epsilon_is_not_worth_a_packet():
    values = {0: 0.5}
    client = FakeClient()
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0}, epsilon=0.01)

    fb.poll_once()
    values[0] = 0.5001
    assert fb.poll_once() == 0


def test_the_default_epsilon_passes_a_14_bit_step():
    """A filter must not be so coarse that real movement is swallowed."""
    values = {0: 0.5}
    client = FakeClient()
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0})

    fb.poll_once()
    values[0] = 0.5 + 1.0 / 16383.0
    assert fb.poll_once() == 1


def test_sent_count_accumulates():
    values = {0: 0.1}
    client = FakeClient()
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0})
    fb.poll_once()
    values[0] = 0.2
    fb.poll_once()
    assert fb.sent_count == 2


# -- echo suppression ---------------------------------------------------------


def test_a_parameter_the_mapper_just_wrote_is_not_echoed():
    """The hazard is a loop with a human in it: the surface sends 0.5, the
    poller sends 0.5 back a frame later, and during a drag that fights the
    finger."""
    values = {0: 0.5}
    client = FakeClient()
    mapper = MagicMock()
    mapper.wrote_recently = MagicMock(return_value=True)

    fb = OscFeedback(_make_plugin(values), client, {"/a": 0}, mapper=mapper)

    assert fb.poll_once() == 0
    assert client.sent == []


def test_the_value_is_sent_once_the_suppression_window_closes():
    """Suppression must delay, not drop: the surface has to converge."""
    values = {0: 0.5}
    client = FakeClient()
    mapper = MagicMock()
    suppressed = {"yes": True}
    mapper.wrote_recently = MagicMock(side_effect=lambda i, w: suppressed["yes"])

    fb = OscFeedback(_make_plugin(values), client, {"/a": 0}, mapper=mapper)
    fb.poll_once()
    assert client.sent == []

    suppressed["yes"] = False
    assert fb.poll_once() == 1
    assert client.sent == [("/a", 0.5)]


def test_suppression_is_per_parameter():
    values = {0: 0.1, 1: 0.2}
    client = FakeClient()
    mapper = MagicMock()
    mapper.wrote_recently = MagicMock(side_effect=lambda i, w: i == 0)

    fb = OscFeedback(_make_plugin(values), client, {"/a": 0, "/b": 1}, mapper=mapper)

    assert fb.poll_once() == 1
    assert client.sent == [("/b", 0.2)]


def test_the_real_mapper_reports_its_own_writes():
    """wrote_recently is the mapper's half of the contract."""
    plugin = MagicMock()
    plugin.find_param = MagicMock(return_value=3)
    mapper = OscMapper(plugin)
    mapper.map_address("/x", "cutoff")

    assert not mapper.wrote_recently(3, within=1.0)
    mapper("/x", [0.5])
    assert mapper.wrote_recently(3, within=1.0)
    assert not mapper.wrote_recently(3, within=0.0)
    assert not mapper.wrote_recently(99, within=1.0)


# -- robustness ---------------------------------------------------------------


def test_a_failing_client_does_not_stop_the_poll():
    """A closed client or a rejected address must not take the thread down."""
    values = {0: 0.1, 1: 0.2}
    client = FakeClient()
    client.fail = True
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0, "/b": 1})

    assert fb.poll_once() == 0  # no exception


def test_a_failed_send_is_retried_next_poll():
    """The value must not be recorded as sent when it was not."""
    values = {0: 0.1}
    client = FakeClient()
    client.fail = True
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0})
    fb.poll_once()

    client.fail = False
    assert fb.poll_once() == 1


@pytest.mark.parametrize(
    "kwargs", [{"interval": 0}, {"interval": -1}, {"suppress": -1}, {"epsilon": -1}]
)
def test_invalid_settings_are_refused(kwargs):
    with pytest.raises(ValueError):
        OscFeedback(MagicMock(), FakeClient(), {}, **kwargs)


def test_no_targets_is_not_an_error():
    fb = OscFeedback(MagicMock(), FakeClient(), {})
    assert fb.poll_once() == 0


# -- the thread ---------------------------------------------------------------


def test_start_and_stop_are_idempotent():
    values = {0: 0.5}
    fb = OscFeedback(_make_plugin(values), FakeClient(), {"/a": 0}, interval=0.01)
    fb.stop()  # not running
    fb.start()
    fb.start()  # already running
    assert fb.is_running
    fb.stop()
    fb.stop()
    assert not fb.is_running


def test_the_thread_actually_polls():
    values = {0: 0.5}
    client = FakeClient()
    fb = OscFeedback(_make_plugin(values), client, {"/a": 0}, interval=0.01)
    with fb:
        deadline = time.monotonic() + 5.0
        while not client.sent and time.monotonic() < deadline:
            time.sleep(0.01)
    assert client.sent, "the feedback thread never sent anything"


def test_a_raising_plugin_does_not_kill_the_thread():
    plugin = MagicMock()
    plugin.get_param = MagicMock(side_effect=RuntimeError("boom"))
    fb = OscFeedback(plugin, FakeClient(), {"/a": 0}, interval=0.01)
    with fb:
        time.sleep(0.05)
        assert fb.is_running


# -- address selection --------------------------------------------------------


def test_feedback_addresses_prefers_names_over_indices():
    """bind_all binds both forms; echoing both would double traffic."""
    plugin = MagicMock()
    plugin.num_params = 2
    plugin.find_param = MagicMock(side_effect=lambda n: {"alpha": 0, "beta": 1}[n])
    plugin.get_param_info = MagicMock(
        side_effect=lambda i: {"name": ["alpha", "beta"][i], "is_automatable": True}
    )

    mapper = OscMapper(plugin)
    mapper.bind_all()
    targets = mapper.feedback_addresses()

    assert targets == {"/mh/param/alpha": 0, "/mh/param/beta": 1}


# -- end to end ---------------------------------------------------------------


@skip_if_no_plugin
def test_feedback_reaches_a_real_surface_over_udp():
    """A parameter changed behind the surface's back arrives at the surface."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    received: list[tuple[str, list[float]]] = []
    arrived = threading.Event()

    def on_osc(address, args):
        received.append((address, args))
        arrived.set()

    name = plugin.get_param_info(0)["name"]
    address = f"/mh/param/{minihost.slug(name)}"

    with minihost.OscServer.open(0, on_osc) as surface:
        with minihost.OscClient("127.0.0.1", surface.port) as out:
            mapper = OscMapper(plugin)
            mapper.bind_all()
            fb = OscFeedback(plugin, out, mapper.feedback_addresses(), mapper=mapper)

            plugin.set_param(0, 0.625)
            fb.poll_once()
            assert arrived.wait(5.0), "no feedback arrived"

    matching = [args[0] for addr, args in received if addr == address]
    assert matching, f"{address} not among {[a for a, _ in received]}"
    assert matching[-1] == pytest.approx(0.625, abs=1e-4)
