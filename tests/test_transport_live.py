"""The live device's host playhead.

Until this existed, `grep -n transport minihost_audio.c` returned nothing: the
realtime device never called `mh_set_transport`, so a tempo-synced delay, an
arpeggiator or an LFO running under `minihost play` saw no host tempo and a
playhead pinned at sample 0. Offline renders had a playhead (see
test_transport_advance.py); realtime did not.

The audio thread owns the transport and is its only writer. Setters post to a
lock-free command ring it drains at the top of each block, so a change never
tears and never blocks the caller -- and takes effect one block later, which
these tests allow for rather than pretend away.
"""

from __future__ import annotations

import os
import time

import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)

SR = 48000
BLOCK = 256
# Long enough for several audio callbacks, so a command posted before it has
# certainly been drained.
SETTLE = 0.15


def _plugin():
    return minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512)


def _device(plugin):
    return minihost.AudioDevice(plugin, sample_rate=SR, buffer_frames=BLOCK)


# -- default off --------------------------------------------------------------


@skip_if_no_plugin
def test_transport_is_off_by_default():
    """A device that does not ask for a playhead must behave as before."""
    with _device(_plugin()) as audio:
        assert audio.transport_enabled is False
        assert audio.transport is None


@skip_if_no_plugin
def test_enabling_publishes_a_playhead():
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.start()
        time.sleep(SETTLE)
        audio.stop()

        assert audio.transport_enabled is True
        assert audio.transport is not None


@skip_if_no_plugin
def test_the_defaults_are_musically_sane():
    """Enabling must not first hand the plugin a tempo of zero."""
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.start()
        time.sleep(SETTLE)
        audio.stop()

    t = audio.transport
    assert t["bpm"] == pytest.approx(120.0)
    assert t["time_sig_numerator"] == 4
    assert t["time_sig_denominator"] == 4
    assert t["is_playing"] is False


# -- the playhead moves -------------------------------------------------------


@skip_if_no_plugin
def test_the_playhead_advances_only_while_playing():
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.start()
        time.sleep(SETTLE)

        stopped_a = audio.transport["position_samples"]
        time.sleep(0.2)
        stopped_b = audio.transport["position_samples"]

        audio.transport_play()
        time.sleep(0.3)
        playing = audio.transport["position_samples"]

        audio.transport_stop()
        time.sleep(SETTLE)
        held_a = audio.transport["position_samples"]
        time.sleep(0.2)
        held_b = audio.transport["position_samples"]
        audio.stop()

    assert stopped_a == stopped_b == 0, "advanced while stopped"
    assert playing > 0, "did not advance while playing"
    assert held_a == held_b, "advanced after stop"


@skip_if_no_plugin
def test_it_advances_at_roughly_the_sample_rate():
    """The playhead counts rendered samples, so it tracks the audio clock."""
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.transport_play()
        audio.start()
        time.sleep(SETTLE)

        first = audio.transport["position_samples"]
        time.sleep(0.5)
        second = audio.transport["position_samples"]
        audio.stop()

    advanced = second - first
    # Generous: wall-clock sleeps and device buffering both add slack. What
    # is being checked is the order of magnitude -- that it counts samples
    # and not blocks or seconds.
    assert 0.5 * SR * 0.5 < advanced < 0.5 * SR * 1.6, advanced


@skip_if_no_plugin
def test_position_beats_follows_position_and_tempo():
    """Derived, never commanded: two sources of truth for one instant is how
    a playhead ends up disagreeing with itself."""
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.transport_set_bpm(120.0)
        audio.transport_set_position(SR * 2)  # two seconds in
        audio.start()
        time.sleep(SETTLE)
        audio.stop()

    t = audio.transport
    # 2 s at 120 bpm is 4 beats.
    assert t["position_beats"] == pytest.approx(4.0, abs=0.05)


@skip_if_no_plugin
def test_setting_the_position_moves_the_playhead():
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.start()
        audio.transport_set_position(12345)
        time.sleep(SETTLE)
        audio.stop()

    assert audio.transport["position_samples"] == 12345


# -- tempo, time signature, flags ---------------------------------------------


@skip_if_no_plugin
def test_tempo_and_time_signature_are_applied():
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.transport_set_bpm(93.5)
        audio.transport_set_time_sig(7, 8)
        audio.start()
        time.sleep(SETTLE)
        audio.stop()

    t = audio.transport
    assert t["bpm"] == pytest.approx(93.5)
    assert t["time_sig_numerator"] == 7
    assert t["time_sig_denominator"] == 8


@skip_if_no_plugin
def test_the_recording_flag_round_trips():
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.transport_set_recording(True)
        audio.start()
        time.sleep(SETTLE)
        audio.stop()

    assert audio.transport["is_recording"] is True


@pytest.mark.parametrize(
    "call,args",
    [
        ("transport_set_bpm", (0.0,)),
        ("transport_set_bpm", (-1.0,)),
        ("transport_set_time_sig", (0, 4)),
        ("transport_set_time_sig", (4, 0)),
        ("transport_set_position", (-1,)),
    ],
)
@skip_if_no_plugin
def test_invalid_transport_settings_raise(call, args):
    with _device(_plugin()) as audio:
        with pytest.raises(ValueError):
            getattr(audio, call)(*args)


# -- looping ------------------------------------------------------------------


@skip_if_no_plugin
def test_the_playhead_wraps_at_the_loop_end():
    loop_start, loop_end = 0, SR // 4  # a quarter second

    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.transport_set_loop(True, loop_start, loop_end)
        audio.transport_play()
        audio.start()
        time.sleep(1.0)  # several loop lengths
        positions = [audio.transport["position_samples"] for _ in range(5)]
        audio.stop()

    assert all(loop_start <= p < loop_end + BLOCK for p in positions), positions


@skip_if_no_plugin
def test_a_loop_shorter_than_a_block_still_wraps_into_range():
    """A subtract would leave the position past the end; the wrap is modulo."""
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.transport_set_loop(True, 1000, 1000 + 32)  # 32 samples
        audio.transport_play()
        audio.start()
        time.sleep(0.3)
        positions = [audio.transport["position_samples"] for _ in range(5)]
        audio.stop()

    assert all(1000 <= p < 1032 for p in positions), positions


@skip_if_no_plugin
def test_an_invalid_loop_is_refused():
    with _device(_plugin()) as audio:
        with pytest.raises(ValueError):
            audio.transport_set_loop(True, 100, 100)  # empty
        with pytest.raises(ValueError):
            audio.transport_set_loop(True, 200, 100)  # inverted
        with pytest.raises(ValueError):
            audio.transport_set_loop(True, -1, 100)
        # Disabling ignores the points, so anything is acceptable.
        audio.transport_set_loop(False, 0, 0)


# -- chains -------------------------------------------------------------------


@skip_if_no_plugin
def test_a_chain_device_also_gets_a_playhead():
    """Every plugin in the chain is handed the transport, resolved once at
    open rather than through the locking accessor on each block."""
    a = _plugin()
    chain = minihost.PluginChain([a])

    with minihost.AudioDevice(chain, sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_transport_enabled(True)
        audio.transport_play()
        audio.start()
        time.sleep(0.3)
        position = audio.transport["position_samples"]
        audio.stop()

    assert position > 0


# -- OSC control --------------------------------------------------------------


@skip_if_no_plugin
def test_osc_drives_the_transport():
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.connect_osc(0)
        audio.start()

        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send("/mh/transport/bpm", 150.0)
            client.send("/mh/transport/play")
            time.sleep(0.3)
            playing = audio.transport
            client.send("/mh/transport/stop")
            time.sleep(SETTLE)
            stopped = audio.transport

        audio.stop()

    assert playing["bpm"] == pytest.approx(150.0)
    assert playing["is_playing"] is True
    assert playing["position_samples"] > 0
    assert stopped["is_playing"] is False


@skip_if_no_plugin
def test_a_button_release_does_not_undo_its_press():
    """A surface button sends 1.0 on press and 0.0 on release. Acting on the
    release would make every press a press-and-undo."""
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.connect_osc(0)
        audio.start()

        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send("/mh/transport/play", 1.0)
            client.send("/mh/transport/play", 0.0)  # release
            time.sleep(0.3)
            state = audio.transport

        audio.stop()

    assert state["is_playing"] is True, "the button release stopped the transport"


@skip_if_no_plugin
def test_osc_position_is_in_beats():
    """The wire carries beats, which is what a surface displays; the playhead
    counts samples, which is what a plugin needs."""
    with _device(_plugin()) as audio:
        audio.set_transport_enabled(True)
        audio.transport_set_bpm(120.0)
        audio.connect_osc(0)
        audio.start()

        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send("/mh/transport/position", 4.0)  # 4 beats = 2 s at 120
            time.sleep(SETTLE)
            state = audio.transport

        audio.stop()

    assert state["position_samples"] == pytest.approx(2 * SR, rel=0.02)


@skip_if_no_plugin
def test_transport_addresses_do_not_collide_with_parameters():
    """/mh/transport/* must not be parsed as a parameter index."""
    plugin = _plugin()
    if plugin.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with _device(plugin) as audio:
        audio.set_transport_enabled(True)
        audio.connect_osc(0)
        audio.start()
        plugin.set_param(0, 0.5)
        before = [plugin.get_param(i) for i in range(min(8, plugin.num_params))]

        with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
            client.send("/mh/transport/bpm", 100.0)
            time.sleep(0.2)

        audio.stop()

    after = [plugin.get_param(i) for i in range(min(8, plugin.num_params))]
    assert after == pytest.approx(before)
