"""14-bit MIDI CC pairs.

A plain CC carries 7 bits: 128 steps across a parameter's whole range, which
is audibly stepped on a filter cutoff. The MIDI spec pairs controller ``n``
(0-31), carrying the high 7 bits, with controller ``n + 32`` carrying the low
7, for 16384 steps.

The dispatch rule is the interesting part, and it deviates from what
docs/dev/osc_and_touch.md originally planned. The plan said to reset the
cached LSB to 0 on each MSB. That produces a sawtooth: a controller sending
the same (MSB, LSB) pair repeatedly would emit two *different* values per
pair, oscillating by up to 1/128 of the range at message rate. Keeping the
last LSB instead means an unchanged pair emits an unchanged value, and the
worst case is a stale fine position for the microseconds until the LSB
arrives -- an error of at most one coarse step, never a periodic wobble.
`test_a_repeated_pair_emits_a_stable_value` is the one that decides this.
"""

from __future__ import annotations

import json
import tempfile
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from minihost import MidiMapper


def _make_plugin(params: dict[str, int] | None = None) -> MagicMock:
    if params is None:
        params = {"volume": 0, "pan": 1, "cutoff": 2, "resonance": 3}

    plugin = MagicMock()

    def find_param(name: str) -> int:
        idx = params.get(name.lower())
        if idx is None:
            raise RuntimeError(f"Parameter not found: '{name}'")
        return idx

    plugin.find_param = MagicMock(side_effect=find_param)
    plugin.num_params = len(params)
    return plugin


def _values(plugin) -> list[float]:
    return [c.args[1] for c in plugin.set_param.call_args_list]


def _cc(channel: int, cc: int, value: int) -> bytes:
    return bytes([0xB0 | channel, cc, value])


# -- resolution ---------------------------------------------------------------


def test_a_full_pair_resolves_to_14_bits():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 7, 127))  # MSB
    mapper(_cc(0, 39, 127))  # LSB

    assert _values(plugin)[-1] == pytest.approx(1.0)


def test_the_smallest_step_is_1_in_16383():
    """The whole point: a step a 7-bit CC cannot express."""
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 7, 64))
    mapper(_cc(0, 39, 0))
    first = _values(plugin)[-1]
    mapper(_cc(0, 39, 1))
    second = _values(plugin)[-1]

    assert second - first == pytest.approx(1.0 / 16383.0)
    # A 7-bit CC's smallest step is 128x coarser.
    assert second - first < (1.0 / 127.0) / 100


def test_zero_and_full_scale_are_exact():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 7, 0))
    mapper(_cc(0, 39, 0))
    assert _values(plugin)[-1] == pytest.approx(0.0)

    mapper(_cc(0, 7, 127))
    mapper(_cc(0, 39, 127))
    assert _values(plugin)[-1] == pytest.approx(1.0)


# -- the dispatch rule --------------------------------------------------------


def test_a_repeated_pair_emits_a_stable_value():
    """The test that chose keep-LSB over reset-to-zero.

    A controller re-sending an unchanged (MSB, LSB) pair must produce an
    unchanged parameter value. Resetting the LSB on each MSB would emit
    msb<<7 and then (msb<<7)|lsb forever, a periodic wobble of up to 1/128
    of the range at message rate.
    """
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    for _ in range(4):
        mapper(_cc(0, 7, 64))
        mapper(_cc(0, 39, 100))

    settled = _values(plugin)[1:]  # skip the very first coarse-only emission
    assert len(set(settled)) == 1, f"value oscillates: {sorted(set(settled))}"


def test_an_msb_alone_moves_the_parameter_coarsely():
    """A controller that sends only MSBs still works, at 7-bit resolution."""
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 7, 0))
    mapper(_cc(0, 7, 64))
    mapper(_cc(0, 7, 127))

    values = _values(plugin)
    assert values[0] < values[1] < values[2]
    assert values[2] == pytest.approx((127 << 7) / 16383.0)


def test_an_lsb_alone_refines_without_a_new_msb():
    """The fine-adjust case: many controllers send LSB only between coarse
    moves, and each must refine the held coarse position."""
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 7, 64))
    for lsb in (10, 20, 30):
        mapper(_cc(0, 39, lsb))

    values = _values(plugin)
    assert values[1] < values[2] < values[3]
    assert values[3] == pytest.approx(((64 << 7) | 30) / 16383.0)


def test_an_lsb_before_any_msb_is_held_not_slammed_to_zero():
    """With no coarse position yet, an LSB alone would read as msb=0 and
    drop the parameter to the bottom of its range. It is held instead."""
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 39, 100))
    assert _values(plugin) == []

    mapper(_cc(0, 7, 64))
    assert _values(plugin) == [pytest.approx(((64 << 7) | 100) / 16383.0)]


def test_the_pair_is_per_channel():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")
    mapper.map_cc14(channel=1, cc=7, param="cutoff")

    mapper(_cc(0, 7, 127))
    mapper(_cc(1, 7, 0))

    indices = [c.args[0] for c in plugin.set_param.call_args_list]
    assert indices == [0, 2]


def test_an_unmapped_channel_is_ignored():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(5, 7, 127))

    assert _values(plugin) == []


# -- ranges and curves reuse the shared core ----------------------------------


def test_value_range_and_curve_apply():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="pan", value_range=(-1.0, 1.0))

    mapper(_cc(0, 7, 127))
    mapper(_cc(0, 39, 127))
    assert _values(plugin)[-1] == pytest.approx(1.0)

    mapper.map_cc14(channel=0, cc=8, param="cutoff", curve="exp")
    mapper(_cc(0, 8, 64))
    mapper(_cc(0, 40, 0))
    unit = (64 << 7) / 16383.0
    assert _values(plugin)[-1] == pytest.approx(unit * unit)


# -- conflict rejection -------------------------------------------------------


def test_a_14_bit_pair_refuses_an_msb_already_mapped_as_a_plain_cc():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc(channel=0, cc=7, param="volume")

    with pytest.raises(ValueError, match="already mapped as a plain CC"):
        mapper.map_cc14(channel=0, cc=7, param="cutoff")


def test_a_14_bit_pair_refuses_an_lsb_already_mapped_as_a_plain_cc():
    """The shadowing case that would otherwise be near-undebuggable: the
    fader moves in coarse steps and nothing says why."""
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc(channel=0, cc=39, param="volume")

    with pytest.raises(ValueError, match="would be the LSB"):
        mapper.map_cc14(channel=0, cc=7, param="cutoff")


def test_a_plain_cc_refuses_the_msb_of_an_existing_pair():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    with pytest.raises(ValueError, match="already the MSB"):
        mapper.map_cc(channel=0, cc=7, param="cutoff")


def test_a_plain_cc_refuses_the_lsb_of_an_existing_pair():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    with pytest.raises(ValueError, match="already the LSB"):
        mapper.map_cc(channel=0, cc=39, param="cutoff")


def test_a_plain_cc_outside_the_pair_is_fine():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")
    mapper.map_cc(channel=0, cc=8, param="cutoff")
    mapper.map_cc(channel=0, cc=40, param="pan")
    mapper.map_cc(channel=1, cc=7, param="resonance")


@pytest.mark.parametrize("cc", [32, 63, 127, -1])
def test_an_msb_outside_0_31_is_refused_with_a_reason(cc):
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    with pytest.raises(ValueError, match="cc must be 0-31"):
        mapper.map_cc14(channel=0, cc=cc, param="volume")


def test_an_invalid_channel_is_refused():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    with pytest.raises(ValueError, match="channel must be 0-15"):
        mapper.map_cc14(channel=16, cc=7, param="volume")


# -- mapping management -------------------------------------------------------


def test_cc14_mappings_reflects_state():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")
    assert mapper.cc14_mappings == {(0, 7): "volume"}

    mapper.unmap_cc14(0, 7)
    assert mapper.cc14_mappings == {}
    mapper.unmap_cc14(0, 7)  # no-op


def test_unmapping_frees_the_controllers_for_a_plain_cc():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")
    mapper.unmap_cc14(0, 7)
    mapper.map_cc(channel=0, cc=7, param="cutoff")
    mapper.map_cc(channel=0, cc=39, param="pan")


def test_unmapping_forgets_the_cached_msb():
    """A stale coarse position must not leak into a later remapping."""
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")
    mapper(_cc(0, 7, 127))
    mapper.unmap_cc14(0, 7)

    mapper.map_cc14(channel=0, cc=7, param="cutoff")
    plugin.set_param.reset_mock()
    mapper(_cc(0, 39, 10))  # LSB with no MSB since remapping

    assert _values(plugin) == []


def test_clear_removes_14_bit_mappings_too():
    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    mapper.map_cc14(channel=0, cc=7, param="volume")
    mapper.map_cc(channel=0, cc=8, param="cutoff")

    mapper.clear()

    assert mapper.cc14_mappings == {}
    assert mapper.cc_mappings == {}


def test_a_14_bit_message_does_not_reach_the_unmapped_fallback():
    plugin = _make_plugin()
    seen = []
    mapper = MidiMapper(plugin, on_unmapped=seen.append)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 7, 64))
    mapper(_cc(0, 39, 10))

    assert seen == []


def test_an_lsb_held_for_want_of_an_msb_is_still_not_forwarded():
    """It was recognised as ours, just not actionable yet -- forwarding it
    would send a raw controller message on to the plugin as well."""
    plugin = _make_plugin()
    seen = []
    mapper = MidiMapper(plugin, on_unmapped=seen.append)
    mapper.map_cc14(channel=0, cc=7, param="volume")

    mapper(_cc(0, 39, 10))

    assert _values(plugin) == []
    assert seen == []


# -- map file -----------------------------------------------------------------


def _write_map(entries) -> str:
    fd = tempfile.NamedTemporaryFile("w", suffix=".json", delete=False)
    json.dump({"mappings": entries}, fd)
    fd.close()
    return fd.name


def test_map_file_accepts_cc14():
    from minihost.cli import _load_map_file

    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    path = _write_map([{"channel": 0, "cc14": 1, "param": "volume"}])
    try:
        assert _load_map_file(path, mapper) == 1
    finally:
        Path(path).unlink()

    assert mapper.cc14_mappings == {(0, 1): "volume"}


def test_map_file_rejects_an_entry_claiming_both():
    from minihost.cli import _load_map_file

    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    path = _write_map([{"channel": 0, "cc": 7, "cc14": 1, "param": "volume"}])
    try:
        with pytest.raises(ValueError, match="not both"):
            _load_map_file(path, mapper)
    finally:
        Path(path).unlink()


def test_map_file_rejects_an_entry_with_neither():
    from minihost.cli import _load_map_file

    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    path = _write_map([{"channel": 0, "param": "volume"}])
    try:
        with pytest.raises(ValueError, match="'cc' \\(7-bit\\) or 'cc14'"):
            _load_map_file(path, mapper)
    finally:
        Path(path).unlink()


def test_map_file_mixes_both_kinds():
    from minihost.cli import _load_map_file

    plugin = _make_plugin()
    mapper = MidiMapper(plugin)
    path = _write_map(
        [
            {"channel": 0, "cc": 74, "param": "cutoff", "curve": "exp"},
            {"channel": 0, "cc14": 1, "param": "volume"},
        ]
    )
    try:
        assert _load_map_file(path, mapper) == 2
    finally:
        Path(path).unlink()

    assert mapper.cc_mappings == {(0, 74): "cutoff"}
    assert mapper.cc14_mappings == {(0, 1): "volume"}
