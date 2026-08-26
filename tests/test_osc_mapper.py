"""OscMapper: OSC addresses to plugin parameter writes.

Uses a mock plugin, like tests/test_midi_mapper.py, so nothing here needs a
real plugin or a real surface.

The point of the class relative to MidiMapper is resolution: a 7-bit CC gives
128 steps, an OSC float32 is not quantized at all. The point of the shared
core underneath both is that the curve and range logic exists once, keyed on a
normalized float rather than on CC numbers or OSC addresses -- so a third
transport is an adapter and not a rewrite.
"""

from __future__ import annotations

import threading
from unittest.mock import MagicMock

import pytest

import minihost
from minihost import OscMapper, slug


def _make_plugin(params: dict[str, int] | None = None) -> MagicMock:
    """Mock plugin matching the OscMapper subset of the Plugin API."""
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

    ordered = sorted(params.items(), key=lambda kv: kv[1])

    def get_param_info(index: int) -> dict:
        name, _ = ordered[index]
        return {"name": name, "is_automatable": True}

    plugin.get_param_info = MagicMock(side_effect=get_param_info)
    return plugin


def _writes(plugin) -> list[tuple[int, float]]:
    return [(c.args[0], c.args[1]) for c in plugin.set_param.call_args_list]


# -- basic dispatch -----------------------------------------------------------


def test_a_mapped_address_writes_its_parameter():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/cutoff", "cutoff")

    mapper("/mh/param/cutoff", [0.25])

    assert _writes(plugin) == [(2, 0.25)]


def test_the_float_is_not_quantized():
    """The whole reason for preferring OSC over 7-bit CC.

    A CC would round this to the nearest 1/127; nothing here may.
    """
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/cutoff", "cutoff")

    for value in (0.001, 0.5004, 0.99999):
        mapper("/mh/param/cutoff", [value])

    assert [v for _, v in _writes(plugin)] == pytest.approx(
        [0.001, 0.5004, 0.99999], abs=1e-9
    )


def test_an_unmapped_address_writes_nothing():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/cutoff", "cutoff")

    mapper("/mh/param/nothing", [0.5])

    assert _writes(plugin) == []


def test_a_message_with_no_arguments_writes_nothing():
    """A bare trigger is not a parameter write; it belongs to on_unmapped."""
    plugin = _make_plugin()
    seen = []
    mapper = OscMapper(plugin, on_unmapped=lambda a, v: seen.append((a, v)))
    mapper.map_address("/mh/param/cutoff", "cutoff")

    mapper("/mh/param/cutoff", [])

    assert _writes(plugin) == []
    assert seen == [("/mh/param/cutoff", [])]


def test_values_outside_the_unit_range_are_clamped():
    """A surface can send out of range; the plugin must not see it."""
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/cutoff", "cutoff")

    mapper("/mh/param/cutoff", [-0.5])
    mapper("/mh/param/cutoff", [1.5])

    assert [v for _, v in _writes(plugin)] == pytest.approx([0.0, 1.0])


# -- ranges and curves --------------------------------------------------------


def test_value_range_is_applied():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/pan", "pan", value_range=(-1.0, 1.0))

    mapper("/mh/param/pan", [0.0])
    mapper("/mh/param/pan", [0.5])
    mapper("/mh/param/pan", [1.0])

    assert [v for _, v in _writes(plugin)] == pytest.approx([-1.0, 0.0, 1.0])


@pytest.mark.parametrize(
    "curve,unit,expected",
    [
        ("linear", 0.5, 0.5),
        ("exp", 0.5, 0.25),
        ("log", 0.5, 0.75),
    ],
)
def test_curves_match_the_midi_mapper_definitions(curve, unit, expected):
    """Both mappers must shape a value identically -- one core, one answer."""
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/cutoff", "cutoff", curve=curve)

    mapper("/mh/param/cutoff", [unit])

    assert _writes(plugin)[0][1] == pytest.approx(expected)


def test_the_two_mappers_agree_on_every_curve():
    """The shared core, asserted rather than assumed.

    A 7-bit CC of 127 and an OSC 1.0 are the same normalized input, so every
    curve must produce the same parameter value through either path.
    """
    for curve in ("linear", "exp", "log"):
        midi_plugin = _make_plugin()
        midi = minihost.MidiMapper(midi_plugin)
        midi.map_cc(channel=0, cc=7, param="cutoff", curve=curve)
        midi(bytes([0xB0, 7, 127]))

        osc_plugin = _make_plugin()
        osc = OscMapper(osc_plugin)
        osc.map_address("/x", "cutoff", curve=curve)
        osc("/x", [1.0])

        assert _writes(midi_plugin) == pytest.approx(_writes(osc_plugin))


# -- validation ---------------------------------------------------------------


@pytest.mark.parametrize("address", ["no-slash", "", "/has space", "/bad#char"])
def test_an_invalid_address_is_refused_at_map_time(address):
    """The failure this prevents is silent: a control bound to an address the
    host will not accept simply never arrives, logging nothing either end."""
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    with pytest.raises(ValueError, match="not a valid OSC address"):
        mapper.map_address(address, "cutoff")


def test_an_unknown_parameter_raises_at_map_time_not_on_the_first_message():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    with pytest.raises(RuntimeError, match="Parameter not found"):
        mapper.map_address("/mh/param/nope", "nope")


def test_an_invalid_curve_raises():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    with pytest.raises(ValueError, match="curve must be one of"):
        mapper.map_address("/x", "cutoff", curve="sigmoid")


def test_a_malformed_range_raises():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    with pytest.raises(ValueError, match="value_range"):
        mapper.map_address("/x", "cutoff", value_range=(0.0,))


# -- wildcards ----------------------------------------------------------------


def test_a_wildcard_address_writes_every_match():
    """A pattern legitimately addresses many parameters -- resetting a page,
    say -- so every match is written, not just the first."""
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/cutoff", "cutoff")
    mapper.map_address("/mh/param/resonance", "resonance")
    mapper.map_address("/other/volume", "volume")

    mapper("/mh/param/*", [0.5])

    written = sorted(idx for idx, _ in _writes(plugin))
    assert written == [2, 3]  # cutoff and resonance, not volume


def test_wildcard_matching_is_delegated_to_juce():
    """Both ends of a connection must agree on the OSC pattern rules, which
    they do by construction if only one implementation exists."""
    assert minihost.osc_address_matches("/mh/param/*", "/mh/param/cutoff")
    assert minihost.osc_address_matches("/mh/param/cut?ff", "/mh/param/cutoff")
    assert minihost.osc_address_matches("/mh/param/{cutoff,res}", "/mh/param/res")
    assert not minihost.osc_address_matches("/mh/param/*", "/other/cutoff")


def test_an_exact_hit_does_not_scan_the_table():
    """The common case is one dict lookup. Asserted through behaviour: an
    address that is also a literal match must write exactly once, not once
    for the lookup and again for a pattern pass."""
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/mh/param/cutoff", "cutoff")

    mapper("/mh/param/cutoff", [0.5])

    assert len(_writes(plugin)) == 1


def test_a_wildcard_matching_nothing_falls_through_to_on_unmapped():
    plugin = _make_plugin()
    seen = []
    mapper = OscMapper(plugin, on_unmapped=lambda a, v: seen.append(a))
    mapper.map_address("/mh/param/cutoff", "cutoff")

    mapper("/nothing/*", [0.5])

    assert _writes(plugin) == []
    assert seen == ["/nothing/*"]


# -- bind_all -----------------------------------------------------------------


def test_bind_all_binds_every_parameter_by_name_and_index():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)

    count = mapper.bind_all()

    assert count == 4
    addresses = mapper.addresses
    assert "/mh/param/cutoff" in addresses
    assert "/mh/param/2" in addresses  # cutoff's index
    mapper("/mh/param/cutoff", [0.5])
    mapper("/mh/param/2", [0.25])
    assert _writes(plugin) == [(2, 0.5), (2, 0.25)]


def test_bind_all_can_omit_the_numeric_form():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.bind_all(numeric=False)
    assert "/mh/param/cutoff" in mapper.addresses
    assert "/mh/param/2" not in mapper.addresses


def test_bind_all_honours_the_prefix():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.bind_all(prefix="/synth/p/")
    assert "/synth/p/cutoff" in mapper.addresses


def test_bind_all_skips_non_automatable_parameters():
    plugin = _make_plugin()

    def info(index: int) -> dict:
        names = ["volume", "pan", "cutoff", "resonance"]
        return {"name": names[index], "is_automatable": index != 1}

    plugin.get_param_info = MagicMock(side_effect=info)

    mapper = OscMapper(plugin)
    assert mapper.bind_all() == 3
    assert "/mh/param/pan" not in mapper.addresses


def test_bind_all_numbers_duplicate_names():
    """Real plugins repeat names -- three parameters called Bypass is
    ordinary -- and two sharing one address makes the second unreachable."""
    plugin = _make_plugin({"bypass": 0, "gain": 1})

    def info(index: int) -> dict:
        return {"name": "Bypass", "is_automatable": True}

    plugin.get_param_info = MagicMock(side_effect=info)

    mapper = OscMapper(plugin)
    mapper.bind_all(numeric=False)

    assert set(mapper.addresses) == {"/mh/param/bypass", "/mh/param/bypass2"}


# -- slug ---------------------------------------------------------------------


@pytest.mark.parametrize(
    "name,expected",
    [
        ("Cutoff", "cutoff"),
        ("Filter Cutoff", "filterCutoff"),
        ("Dry/Wet", "dryWet"),
        ("Attack (ms)", "attackMs"),
        ("!!!", "parameter"),
        ("", "parameter"),
        ("3 Band EQ", "3BandEq"),
    ],
)
def test_slug_rules(name, expected):
    assert slug(name) == expected


def test_slug_agrees_with_py2tosc():
    """A generated layout and this mapper must spell an address identically,
    or the control is silently dead. Skipped when py2tosc is absent, which is
    the normal case -- it is an optional extra."""
    py2tosc_surface = pytest.importorskip("py2tosc.surface")

    corpus = [
        "Cutoff",
        "Filter Cutoff",
        "Dry/Wet",
        "Attack (ms)",
        "LFO 1 -> Pitch",
        "!!!",
        "",
        "3 Band EQ",
        "  leading space",
        "Mix%",
        "A_B_C",
    ]
    for name in corpus:
        assert slug(name) == py2tosc_surface.slug(name), name


# -- mapping management -------------------------------------------------------


def test_unmap_and_clear():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/a", "cutoff")
    mapper.map_address("/b", "volume")

    mapper.unmap_address("/a")
    assert set(mapper.addresses) == {"/b"}
    mapper.unmap_address("/missing")  # no-op

    mapper.clear()
    assert mapper.addresses == {}


def test_remapping_an_address_replaces_the_binding():
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.map_address("/x", "cutoff")
    mapper.map_address("/x", "volume")

    mapper("/x", [1.0])

    assert _writes(plugin) == [(0, 1.0)]


def test_concurrent_remap_and_dispatch_does_not_crash():
    """Mutation is safe while messages arrive, as for MidiMapper."""
    plugin = _make_plugin()
    mapper = OscMapper(plugin)
    mapper.bind_all()
    stop = threading.Event()

    def churn():
        while not stop.is_set():
            mapper.map_address("/x", "cutoff")
            mapper.unmap_address("/x")

    t = threading.Thread(target=churn)
    t.start()
    try:
        for _ in range(2000):
            mapper("/mh/param/cutoff", [0.5])
            mapper("/x", [0.5])
    finally:
        stop.set()
        t.join(timeout=5.0)


# -- device routing -----------------------------------------------------------


def test_a_bound_device_receives_the_write_instead_of_the_plugin():
    plugin = _make_plugin()
    device = MagicMock()
    mapper = OscMapper(plugin, device=device, plugin_index=2)
    mapper.map_address("/x", "cutoff")

    mapper("/x", [0.5])

    plugin.set_param.assert_not_called()
    device.send_param_control.assert_called_once_with(2, 0.5, 2)


def test_a_full_device_queue_is_dropped_not_raised():
    """This runs on the OSC socket thread, where an exception would escape
    into a C callback. The value is superseded by the next message anyway."""
    plugin = _make_plugin()
    device = MagicMock()
    device.send_param_control.side_effect = RuntimeError("queue full")
    mapper = OscMapper(plugin, device=device)
    mapper.map_address("/x", "cutoff")

    mapper("/x", [0.5])  # must not raise


def test_bind_device_can_be_set_after_construction():
    plugin = _make_plugin()
    device = MagicMock()
    mapper = OscMapper(plugin)
    mapper.map_address("/x", "cutoff")

    mapper("/x", [0.25])
    assert _writes(plugin) == [(2, 0.25)]

    mapper.bind_device(device)
    mapper("/x", [0.75])
    device.send_param_control.assert_called_once_with(2, 0.75, 0)

    mapper.bind_device(None)
    mapper("/x", [0.5])
    assert _writes(plugin) == [(2, 0.25), (2, 0.5)]
