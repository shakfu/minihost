"""Stable OSC addressing for chain slots.

`/mh/<slot>/param/<index>` addresses a slot by its position in the chain. That
is only stable while the chain is built the same way -- and a generated layout
outlives the script that builds it. Save a surface for
`[synth, reverb, limiter]`, edit the script to put the limiter second, and
every address silently points at a different plugin with nothing to say so.

Chains cannot be reordered at runtime today, so this is a hazard of the
persisted layout outliving the code, not of live editing. It is still a
hazard: the layout file is the durable artefact.

`set_slot_name` fixes it by letting the caller attach a name to the plugin
rather than to a position. `test_a_named_slot_survives_reordering` is the test
that makes that a guarantee rather than a claim: it builds a chain, addresses a
plugin by name, rebuilds the chain in the opposite order, and requires the same
address to reach the same plugin.
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


def _plugin():
    return minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=BLOCK * 2)


def _send(audio, address, value, settle=0.25):
    with minihost.OscClient("127.0.0.1", audio.osc_port) as client:
        client.send(address, value)
        time.sleep(settle)


# -- naming -------------------------------------------------------------------


@skip_if_no_plugin
def test_a_slot_starts_unnamed():
    with minihost.AudioDevice(_plugin(), sample_rate=SR, buffer_frames=BLOCK) as audio:
        assert audio.slot_name(0) is None


@skip_if_no_plugin
def test_a_name_round_trips_and_clears():
    with minihost.AudioDevice(_plugin(), sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(1, "reverb")
        assert audio.slot_name(1) == "reverb"
        audio.set_slot_name(1, None)
        assert audio.slot_name(1) is None


@pytest.mark.parametrize(
    "name",
    [
        "2reverb",  # would be ambiguous with the numeric form
        "re verb",  # OSC forbids spaces
        "re/verb",  # a second path segment
        "re-verb",
        "",
        "x" * 64,  # too long to store
    ],
)
@skip_if_no_plugin
def test_an_unusable_name_is_refused(name):
    with minihost.AudioDevice(_plugin(), sample_rate=SR, buffer_frames=BLOCK) as audio:
        with pytest.raises(ValueError):
            audio.set_slot_name(0, name)


@skip_if_no_plugin
def test_two_slots_cannot_share_a_name():
    """One of them would be silently unreachable."""
    with minihost.AudioDevice(_plugin(), sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(0, "verb")
        with pytest.raises(ValueError):
            audio.set_slot_name(1, "verb")
        # Renaming the same slot is not a collision with itself.
        audio.set_slot_name(0, "verb")


@skip_if_no_plugin
def test_naming_is_refused_once_osc_is_connected():
    """The table is read by the socket thread and never written while that
    thread exists -- which is what makes it lock-free rather than usually
    fine. The constraint is enforced, not merely documented."""
    with minihost.AudioDevice(_plugin(), sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.connect_osc(0)
        with pytest.raises(ValueError):
            audio.set_slot_name(0, "verb")

        audio.disconnect_osc()
        audio.set_slot_name(0, "verb")  # allowed again


@skip_if_no_plugin
def test_an_out_of_range_slot_is_refused():
    with minihost.AudioDevice(_plugin(), sample_rate=SR, buffer_frames=BLOCK) as audio:
        with pytest.raises(ValueError):
            audio.set_slot_name(-1, "verb")
        with pytest.raises(ValueError):
            audio.set_slot_name(64, "verb")


# -- addressing ---------------------------------------------------------------


@skip_if_no_plugin
def test_a_named_slot_is_addressable():
    a, b = _plugin(), _plugin()
    chain = minihost.PluginChain([a, b])
    if a.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(chain, sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(1, "second")
        audio.connect_osc(0)
        audio.start()
        _send(audio, "/mh/second/param/0", 0.7)
        audio.stop()

    assert b.get_param(0) == pytest.approx(0.7, abs=1e-3)
    assert a.get_param(0) != pytest.approx(0.7, abs=1e-3)


@skip_if_no_plugin
def test_the_numeric_form_still_works():
    """Named addressing is additive; nothing that worked before may break."""
    a, b = _plugin(), _plugin()
    chain = minihost.PluginChain([a, b])
    if a.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(chain, sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(1, "second")
        audio.connect_osc(0)
        audio.start()
        _send(audio, "/mh/1/param/0", 0.4)
        audio.stop()

    assert b.get_param(0) == pytest.approx(0.4, abs=1e-3)


@skip_if_no_plugin
def test_a_named_slot_survives_reordering():
    """The guarantee, stated as a test.

    Build [a, b] and name b "target". Address it by name. Then build [b, a] --
    the same edit a user makes to their own script -- name b "target" at its
    new position, and require the identical address to reach the identical
    plugin. The positional address would have swapped.
    """
    a, b = _plugin(), _plugin()
    if a.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    # First arrangement: b is slot 1.
    chain = minihost.PluginChain([a, b])
    with minihost.AudioDevice(chain, sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(1, "target")
        audio.connect_osc(0)
        audio.start()
        _send(audio, "/mh/target/param/0", 0.8)
        audio.stop()

    assert b.get_param(0) == pytest.approx(0.8, abs=1e-3)

    # Second arrangement: the same plugins, opposite order, so b is slot 0.
    a2, b2 = _plugin(), _plugin()
    chain2 = minihost.PluginChain([b2, a2])
    with minihost.AudioDevice(chain2, sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(0, "target")
        audio.connect_osc(0)
        audio.start()
        _send(audio, "/mh/target/param/0", 0.8)
        audio.stop()

    # The same address reached the plugin the name was attached to, not the
    # one that happens to sit where it used to.
    assert b2.get_param(0) == pytest.approx(0.8, abs=1e-3)
    assert a2.get_param(0) != pytest.approx(0.8, abs=1e-3)


@skip_if_no_plugin
def test_an_unknown_name_is_ignored_not_misrouted():
    """The failure worth guarding is a silent misroute, not a crash."""
    a, b = _plugin(), _plugin()
    chain = minihost.PluginChain([a, b])
    if a.num_params == 0:
        pytest.skip("plugin exposes no parameters")

    with minihost.AudioDevice(chain, sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(1, "second")
        audio.connect_osc(0)
        audio.start()
        a.set_param(0, 0.5)
        b.set_param(0, 0.5)
        _send(audio, "/mh/nosuchslot/param/0", 0.9)
        audio.stop()

    assert a.get_param(0) == pytest.approx(0.5, abs=1e-3)
    assert b.get_param(0) == pytest.approx(0.5, abs=1e-3)


@skip_if_no_plugin
def test_a_name_does_not_shadow_the_transport_addresses():
    with minihost.AudioDevice(_plugin(), sample_rate=SR, buffer_frames=BLOCK) as audio:
        audio.set_slot_name(0, "transport")
        audio.set_transport_enabled(True)
        audio.connect_osc(0)
        audio.start()
        _send(audio, "/mh/transport/bpm", 133.0)
        state = audio.transport
        audio.stop()

    assert state["bpm"] == pytest.approx(133.0)
