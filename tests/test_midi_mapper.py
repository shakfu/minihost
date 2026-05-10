"""Tests for MidiMapper -- dispatch of MIDI events from a control surface
into plugin parameter writes / user callbacks.

Uses a mock plugin so the tests don't need a real MIDI controller or a
real plugin. The mock records find_param/set_param calls; tests check
that the right calls happen in response to synthetic MIDI bytes.
"""

from __future__ import annotations

import threading
from unittest.mock import MagicMock

import pytest

from minihost import MidiMapper


# -- helpers ----------------------------------------------------------


def _make_plugin(params: dict[str, int] | None = None) -> MagicMock:
    """Mock plugin matching the MidiMapper subset of the Plugin API."""
    if params is None:
        params = {"volume": 0, "pan": 1, "cutoff": 2, "resonance": 3}

    plugin = MagicMock()

    def find_param(name: str) -> int:
        idx = params.get(name.lower())
        if idx is None:
            raise RuntimeError(f"Parameter not found: '{name}'")
        return idx

    plugin.find_param = MagicMock(side_effect=find_param)
    plugin.set_param = MagicMock()
    return plugin


def _cc(channel: int, cc: int, value: int) -> bytes:
    return bytes([0xB0 | channel, cc, value])


def _note_on(channel: int, note: int, velocity: int) -> bytes:
    return bytes([0x90 | channel, note, velocity])


def _note_off(channel: int, note: int, velocity: int = 0) -> bytes:
    return bytes([0x80 | channel, note, velocity])


# -- map_cc -----------------------------------------------------------


def test_map_cc_dispatches_to_set_param():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc(channel=0, cc=7, param="Volume")

    mapper(_cc(channel=0, cc=7, value=64))

    plugin.set_param.assert_called_once_with(0, pytest.approx(64 / 127.0))


def test_map_cc_value_range_translation():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    # Pan ranges over [-1, 1].
    mapper.map_cc(channel=0, cc=10, param="Pan", value_range=(-1.0, 1.0))

    mapper(_cc(0, 10, 0))      # min
    mapper(_cc(0, 10, 127))    # max
    mapper(_cc(0, 10, 64))     # ~middle

    args = [c.args for c in plugin.set_param.call_args_list]
    assert args[0] == (1, -1.0)
    assert args[1] == (1, 1.0)
    assert args[2][0] == 1
    assert args[2][1] == pytest.approx(-1.0 + 2 * 64 / 127)


def test_map_cc_curves():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc(channel=0, cc=1, param="Cutoff", curve="exp")
    mapper.map_cc(channel=0, cc=2, param="Resonance", curve="log")

    mapper(_cc(0, 1, 64))   # exp curve, mid input
    mapper(_cc(0, 2, 64))   # log curve, mid input

    # exp: n^2; midpoint ~0.504 -> ~0.254
    # log: 1 - (1-n)^2; midpoint ~0.504 -> ~0.754
    exp_call, log_call = plugin.set_param.call_args_list
    assert exp_call.args[0] == 2  # cutoff idx
    assert exp_call.args[1] == pytest.approx((64 / 127) ** 2, abs=1e-6)
    assert log_call.args[0] == 3  # resonance idx
    assert log_call.args[1] == pytest.approx(1 - (1 - 64 / 127) ** 2, abs=1e-6)


def test_map_cc_channel_filter():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc(channel=0, cc=7, param="Volume")

    # Same CC on a different channel should not trigger.
    mapper(_cc(channel=3, cc=7, value=100))
    plugin.set_param.assert_not_called()

    # Right channel does trigger.
    mapper(_cc(channel=0, cc=7, value=100))
    plugin.set_param.assert_called_once()


def test_map_cc_unknown_param_raises_immediately():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    with pytest.raises(RuntimeError, match="Parameter not found"):
        mapper.map_cc(channel=0, cc=7, param="NotARealParam")


def test_map_cc_invalid_channel_raises():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    with pytest.raises(ValueError, match="channel must be 0-15"):
        mapper.map_cc(channel=16, cc=7, param="Volume")
    with pytest.raises(ValueError, match="channel must be 0-15"):
        mapper.map_cc(channel=-1, cc=7, param="Volume")


def test_map_cc_invalid_cc_raises():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    with pytest.raises(ValueError, match="cc must be 0-127"):
        mapper.map_cc(channel=0, cc=128, param="Volume")


def test_map_cc_invalid_curve_raises():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    with pytest.raises(ValueError, match="curve must be one of"):
        mapper.map_cc(channel=0, cc=7, param="Volume", curve="weird")


# -- map_note ---------------------------------------------------------


def test_map_note_invokes_callback_with_velocity():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)

    received = []
    mapper.map_note(channel=0, note=36, callback=lambda v: received.append(v))

    mapper(_note_on(0, 36, 100))
    assert received == [100]


def test_map_note_zero_velocity_is_treated_as_note_off():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    received = []
    mapper.map_note(channel=0, note=36, callback=lambda v: received.append(v))

    # Zero-velocity note-on by convention = note-off; callback NOT invoked.
    mapper(_note_on(0, 36, 0))
    assert received == []

    # Real note-off (status 0x8x) also not dispatched to note mapping.
    mapper(_note_off(0, 36))
    assert received == []


def test_map_note_channel_filter():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    received = []
    mapper.map_note(channel=0, note=36, callback=lambda v: received.append(v))

    mapper(_note_on(channel=2, note=36, velocity=80))
    assert received == []   # wrong channel

    mapper(_note_on(channel=0, note=36, velocity=80))
    assert received == [80]


# -- on_unmapped ------------------------------------------------------


def test_unmapped_events_invoke_fallback():
    plugin = _make_plugin()
    seen = []
    mapper = MidiMapper(plugin, on_unmapped=lambda data: seen.append(bytes(data)))

    # No mappings -- everything falls through.
    mapper(_cc(0, 99, 50))
    mapper(_note_on(0, 60, 100))
    mapper(_note_off(0, 60))

    assert len(seen) == 3
    assert seen[0] == _cc(0, 99, 50)
    assert seen[1] == _note_on(0, 60, 100)
    assert seen[2] == _note_off(0, 60)


def test_mapped_events_do_not_reach_fallback():
    plugin = _make_plugin()
    seen = []
    mapper = MidiMapper(plugin, on_unmapped=lambda data: seen.append(bytes(data)))
    mapper.map_cc(channel=0, cc=7, param="Volume")

    mapper(_cc(0, 7, 50))      # mapped, no fallback
    mapper(_cc(0, 99, 50))     # unmapped, fallback
    assert len(seen) == 1
    assert seen[0] == _cc(0, 99, 50)


def test_no_fallback_means_silent_drop():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)  # no on_unmapped
    # Should not raise.
    mapper(_cc(0, 99, 50))
    mapper(b"")              # empty
    mapper(b"\xf0\x7e")      # short / sysex header


# -- introspection ----------------------------------------------------


def test_cc_mappings_reflects_state():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc(0, 7, "Volume")
    mapper.map_cc(0, 10, "Pan")
    assert mapper.cc_mappings == {(0, 7): "Volume", (0, 10): "Pan"}

    mapper.unmap_cc(0, 7)
    assert mapper.cc_mappings == {(0, 10): "Pan"}

    mapper.clear()
    assert mapper.cc_mappings == {}


def test_note_mappings_reflects_state():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_note(0, 36, lambda v: None)
    mapper.map_note(0, 38, lambda v: None)
    assert mapper.note_mappings == {(0, 36), (0, 38)}

    mapper.unmap_note(0, 36)
    assert mapper.note_mappings == {(0, 38)}


# -- thread safety ----------------------------------------------------


def test_concurrent_remap_and_dispatch_does_not_crash():
    """Stress test: hammer __call__ from one thread while another thread
    remaps. With the internal lock, neither thread should crash and the
    plugin should still receive valid set_param calls."""
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc(0, 7, "Volume")

    stop = threading.Event()

    def hammer():
        i = 0
        while not stop.is_set():
            mapper(_cc(0, 7, i % 128))
            i += 1

    def remap():
        toggle = True
        while not stop.is_set():
            if toggle:
                mapper.map_cc(0, 7, "Cutoff")
            else:
                mapper.map_cc(0, 7, "Volume")
            toggle = not toggle

    workers = [
        threading.Thread(target=hammer, daemon=True),
        threading.Thread(target=remap, daemon=True),
    ]
    for w in workers:
        w.start()
    threading.Event().wait(0.2)
    stop.set()
    for w in workers:
        w.join(timeout=2.0)
        assert not w.is_alive()

    # set_param was called many times; all should have been valid.
    assert plugin.set_param.call_count > 0
    # Every call's first arg must be one of the mapped param indices (0 or 2).
    for c in plugin.set_param.call_args_list:
        assert c.args[0] in (0, 2)
        assert 0.0 <= c.args[1] <= 1.0
