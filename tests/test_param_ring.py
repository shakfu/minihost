"""Live parameter writes go through the device's ring, not the state mutex.

`MidiMapper` used to reach a parameter by calling `Plugin.set_param`, which
takes the plugin's state mutex and calls `setValueNotifyingHost` -- while the
audio callback was in `mh_process_midi_io`, which by contract takes no lock at
all. Nothing ordered the two, so a control write landed at an undefined point
inside the block and the MIDI thread could block behind an offline caller
holding that mutex.

The device now carries two lock-free parameter rings (one for control input,
one for application code, for the same single-producer reason the MIDI pair
has), drains both at the top of the callback into one coalesced array, and
hands it to the `_auto` process entry point.

Coalescing is the part worth testing hardest: `mh_process_auto` splits the
block at every distinct change offset, so passing it every intermediate value
of a fader drag would turn one block into hundreds of sub-blocks.
"""

from __future__ import annotations

import os

import pytest

import minihost

PLUGIN = os.environ.get("MINIHOST_TEST_PLUGIN")
requires_plugin = pytest.mark.skipif(not PLUGIN, reason="MINIHOST_TEST_PLUGIN not set")


def _plugin():
    return minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)


# -- API shape ---------------------------------------------------------------


def test_send_param_is_exposed():
    assert hasattr(minihost.AudioDevice, "send_param")
    assert hasattr(minihost.AudioDevice, "send_param_control")


def test_mapper_accepts_a_device_binding():
    """The binding is part of the constructor and rebindable afterwards."""
    assert hasattr(minihost.MidiMapper, "bind_device")


# -- coalescing --------------------------------------------------------------


@requires_plugin
def test_repeated_writes_to_one_param_land_as_the_last_value():
    """A fader drag inside one block collapses to its final value.

    The mechanism is not observable from Python directly -- the drain happens
    on the audio thread -- so this asserts the outcome: after the device has
    run, the parameter holds the last value sent, not an intermediate one.
    """
    plugin = _plugin()
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.start()
        for i in range(200):
            audio.send_param(0, i / 199.0)
        # Let the audio thread run several blocks.
        import time

        time.sleep(0.2)
        audio.stop()

    assert plugin.get_param(0) == pytest.approx(1.0, abs=1e-3)


@requires_plugin
def test_a_burst_of_writes_is_coalesced_before_the_processor_sees_it():
    """The point of the drain: N writes to one parameter become far fewer
    applied changes.

    `setValueNotifyingHost` fires the plugin's parameter-value listener once
    per applied change, and `poll_callbacks` drains those to Python -- so
    counting them counts what actually reached the processor. Without
    coalescing this is the number of writes exactly; with it, it is bounded by
    the number of blocks the burst spanned.
    """
    plugin = _plugin()
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    applied = []
    plugin.set_param_value_callback(lambda idx, val: applied.append((idx, val)))

    writes = 200
    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.start()
        for i in range(writes):
            audio.send_param(0, i / (writes - 1.0))
        import time

        time.sleep(0.3)
        audio.stop()

    plugin.poll_callbacks()
    on_param_0 = [v for idx, v in applied if idx == 0]

    # A 256-frame block at 48 kHz is 5.3 ms; 200 Python calls take well under
    # that, so the burst spans a couple of blocks at most. The margin is wide
    # because the exact block count is a scheduling detail -- what is being
    # asserted is the order of magnitude, and that it is not `writes`.
    assert len(on_param_0) < writes // 4, (
        f"{len(on_param_0)} of {writes} writes reached the processor; "
        "the drain is not coalescing"
    )
    assert on_param_0, "no parameter change reached the processor at all"
    assert on_param_0[-1] == pytest.approx(1.0, abs=1e-3)


@requires_plugin
def test_two_params_both_arrive():
    """Coalescing is per parameter, not a single-slot latch."""
    plugin = _plugin()
    if plugin.num_params < 2:
        pytest.skip("plugin exposes fewer than two parameters")

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.start()
        audio.send_param(0, 0.25)
        audio.send_param(1, 0.75)
        import time

        time.sleep(0.2)
        audio.stop()

    assert plugin.get_param(0) == pytest.approx(0.25, abs=1e-3)
    assert plugin.get_param(1) == pytest.approx(0.75, abs=1e-3)


# -- argument validation -----------------------------------------------------


@requires_plugin
def test_negative_indices_are_refused():
    plugin = _plugin()
    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        with pytest.raises(RuntimeError):
            audio.send_param(-1, 0.5)
        with pytest.raises(RuntimeError):
            audio.send_param(0, 0.5, plugin_index=-1)


@requires_plugin
def test_a_write_before_start_is_applied_once_running():
    """The ring is live from open, not from start."""
    plugin = _plugin()
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        audio.send_param(0, 0.6)
        audio.start()
        import time

        time.sleep(0.2)
        audio.stop()

    assert plugin.get_param(0) == pytest.approx(0.6, abs=1e-3)


# -- mapper routing ----------------------------------------------------------


@requires_plugin
def test_bound_mapper_routes_through_the_device():
    """A CC write reaches the parameter via the device queue."""
    plugin = _plugin()
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")
    name = plugin.get_param_info(0)["name"]

    with minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256) as audio:
        mapper = minihost.MidiMapper(plugin, device=audio)
        mapper.map_cc(channel=0, cc=7, param=name)
        audio.start()
        mapper(bytes([0xB0, 7, 127]))
        import time

        time.sleep(0.2)
        audio.stop()

    assert plugin.get_param(0) == pytest.approx(1.0, abs=1e-3)


@requires_plugin
def test_unbound_mapper_still_writes_directly():
    """The offline path is unchanged: no device, no queue, set_param as before."""
    plugin = _plugin()
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")
    name = plugin.get_param_info(0)["name"]

    mapper = minihost.MidiMapper(plugin)
    mapper.map_cc(channel=0, cc=7, param=name)
    mapper(bytes([0xB0, 7, 127]))

    assert plugin.get_param(0) == pytest.approx(1.0, abs=1e-3)
