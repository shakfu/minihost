"""Generating a touch surface from a plugin's parameters.

Most of this needs no py2tosc: generation is pure JSON emission, so the
envelope, the stamped schema, the branch each parameter kind selects, the CC
assignment and the agreement between the layout and the map file are all
checkable without the compiler.

Every generated description also goes through `tests/check_json.py`, a
stdlib-only checker vendored from py2tosc for exactly this purpose. It catches
the class of fault a golden file cannot: a key nothing reads, silently
ignored, so a typo drops a subtree and the output still looks like a file that
read correctly.

The tests that genuinely need the compiler -- that the description builds,
that it resolves, that every address maps back to a real parameter -- are
behind an importorskip.
"""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path
from unittest.mock import MagicMock

import pytest

import minihost
from minihost import touch

sys.path.insert(0, str(Path(__file__).parent))
import check_json  # noqa: E402

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)


def _fake_plugin(infos: list[dict]) -> MagicMock:
    plugin = MagicMock()
    plugin.num_params = len(infos)
    plugin.get_param_info = MagicMock(side_effect=lambda i: infos[i])
    return plugin


def _info(name, **kw) -> dict:
    base = {
        "name": name,
        "label": "",
        "num_steps": 0,
        "is_automatable": True,
        "is_boolean": False,
        "default_value": 0.0,
    }
    base.update(kw)
    return base


def _assert_clean(layout: dict) -> None:
    """No finding from the vendored checker."""
    findings = check_json.check(layout)
    assert findings == [], findings


# -- the parameter table ------------------------------------------------------


def test_widget_kind_comes_from_plugin_metadata():
    """The metadata py2tosc's flat surface path discards is the whole reason
    to generate here: a bypass is a button, not a fader."""
    plugin = _fake_plugin(
        [
            _info("Cutoff"),
            _info("Bypass", is_boolean=True),
            _info("Waveform", num_steps=16),
        ]
    )
    params = touch.collect_parameters(plugin)

    assert [p.kind for p in params] == ["continuous", "toggle", "stepped"]
    assert params[2].steps == 16


def test_a_two_step_parameter_is_stepped_but_a_one_step_one_is_not():
    plugin = _fake_plugin([_info("A", num_steps=1), _info("B", num_steps=2)])
    params = touch.collect_parameters(plugin)
    assert [p.kind for p in params] == ["continuous", "stepped"]


def test_non_automatable_parameters_are_skipped_by_default():
    plugin = _fake_plugin([_info("Gain"), _info("Meter", is_automatable=False)])
    assert len(touch.collect_parameters(plugin)) == 1
    assert len(touch.collect_parameters(plugin, automatable_only=False)) == 2


def test_duplicate_names_are_numbered():
    """Real plugins repeat names, and two controls sharing one OSC address
    makes the second unreachable."""
    plugin = _fake_plugin([_info("Bypass"), _info("Bypass"), _info("Bypass")])
    slugs = [p.slug for p in touch.collect_parameters(plugin)]
    assert slugs == ["bypass", "bypass2", "bypass3"]


def test_cc_numbers_run_out_at_128():
    plugin = _fake_plugin([_info(f"P{i}") for i in range(200)])
    params = touch.collect_parameters(plugin)

    assert params[127].cc == 127
    assert params[128].cc is None
    assert sum(1 for p in params if p.cc is not None) == 128


def test_a_subset_can_be_selected():
    plugin = _fake_plugin([_info(f"P{i}") for i in range(10)])
    params = touch.collect_parameters(plugin, indices=[0, 3, 7])
    assert [p.index for p in params] == [0, 3, 7]


def test_slugify_agrees_with_the_mapper():
    """Generation and the runtime mapper must spell an address identically,
    or the control is silently dead. touch.slugify is deliberately a copy so
    generation imports nothing; this is what keeps the copy honest."""
    for name in ["Cutoff", "Filter Cutoff", "Dry/Wet", "!!!", "", "3 Band EQ", "Mix%"]:
        assert touch.slugify(name) == minihost.slug(name), name


# -- the layout ---------------------------------------------------------------


def _layout(n=3, **kw):
    plugin = _fake_plugin(
        [_info("Cutoff"), _info("Bypass", is_boolean=True), _info("Wave", num_steps=8)][
            :n
        ]
    )
    return touch.build_layout(touch.collect_parameters(plugin), **kw)


def test_the_envelope_declares_the_dialect_and_stamps_the_schema():
    """ui_json is read and never written, so the producer stamps. A
    description with no schema key means 'whatever the reader is'."""
    layout = _layout()
    assert layout["format"] == "py2tosc.ui"
    assert layout["schema"] == 3


def test_the_layout_passes_the_vendored_checker():
    _assert_clean(_layout())


def test_a_row_selects_a_branch_per_widget_kind():
    layout = _layout()
    each = layout["root"]["stack"][0]["pager"][0]["tiles"][0]
    kinds = [row["kind"] for row in each["each"]]
    assert kinds == ["continuous", "toggle", "stepped"]

    when = each["of"]["when"]
    assert "fader" in when["continuous"]
    assert "button" in when["toggle"]
    assert "radio" in when["stepped"]


def test_parameter_order_is_preserved():
    """The reason for one `each` over a branch table rather than one `each`
    per widget kind: grouping by widget would regroup the surface and throw
    away an ordering the plugin author chose."""
    plugin = _fake_plugin(
        [
            _info("First"),
            _info("Toggle", is_boolean=True),
            _info("Third"),
            _info("Stepped", num_steps=4),
            _info("Fifth"),
        ]
    )
    layout = touch.build_layout(touch.collect_parameters(plugin))
    rows = layout["root"]["stack"][0]["pager"][0]["tiles"][0]["each"]
    assert [r["name"] for r in rows] == [
        "first",
        "toggle",
        "third",
        "stepped",
        "fifth",
    ]


def test_parameters_past_the_cc_limit_select_a_no_cc_branch():
    """`each` cannot conditionally include a message, so the row picks a
    branch that has no midi_cc binding at all."""
    plugin = _fake_plugin([_info(f"P{i}") for i in range(130)])
    layout = touch.build_layout(touch.collect_parameters(plugin))

    rows = [
        row
        for page in layout["root"]["stack"][0]["pager"]
        for row in page["tiles"][0]["each"]
    ]
    # The widget kind is one question and the controller number another, so
    # both rows take the same arm and differ only in what they say about CC.
    assert rows[127]["kind"] == rows[128]["kind"] == "continuous"
    assert rows[127]["hasCc"] is True and rows[128]["hasCc"] is False
    assert "cc" in rows[127] and "cc" not in rows[128]

    when = layout["root"]["stack"][0]["pager"][0]["tiles"][0]["of"]["when"]
    assert set(when) == {"continuous", "toggle", "stepped"}
    choice = when["continuous"]["messages"][-1]
    assert choice["case"] == "$hasCc"
    assert choice["when"]["true"] == [{"midi_cc": "$cc"}]
    assert choice["when"]["false"] == []


def test_pages_are_filled_to_the_grid():
    plugin = _fake_plugin([_info(f"P{i}") for i in range(30)])
    layout = touch.build_layout(touch.collect_parameters(plugin), columns=4, rows=3)
    pages = layout["root"]["stack"][0]["pager"]
    assert len(pages) == 3  # 12 + 12 + 6
    assert len(pages[0]["tiles"][0]["each"]) == 12
    assert len(pages[2]["tiles"][0]["each"]) == 6


def test_midi_only_and_osc_only():
    midi_only = _layout(midi=True, osc=False)
    when = midi_only["root"]["stack"][0]["pager"][0]["tiles"][0]["of"]["when"]
    assert not any("osc" in m for m in when["continuous"]["messages"])
    _assert_clean(midi_only)

    osc_only = _layout(midi=False, osc=True)
    when = osc_only["root"]["stack"][0]["pager"][0]["tiles"][0]["of"]["when"]
    assert not any("midi_cc" in m for m in when["continuous"]["messages"])
    _assert_clean(osc_only)


def test_neither_binding_is_refused():
    plugin = _fake_plugin([_info("A")])
    with pytest.raises(ValueError, match="neither MIDI nor OSC"):
        touch.build_layout(touch.collect_parameters(plugin), midi=False, osc=False)


def test_an_empty_table_is_refused():
    with pytest.raises(ValueError, match="at least one parameter"):
        touch.build_layout([])


def test_the_prefix_reaches_the_addresses():
    layout = _layout(prefix="/synth/p/")
    rows = layout["root"]["stack"][0]["pager"][0]["tiles"][0]["each"]
    assert rows[0]["address"] == "/synth/p/cutoff"


# -- layout and map agree -----------------------------------------------------


def test_the_map_file_and_the_layout_name_the_same_controllers():
    """The whole reason to generate both here rather than pipe two tools:
    they are rendered from the same rows and cannot drift."""
    plugin = _fake_plugin([_info(f"P{i}") for i in range(20)])
    params = touch.collect_parameters(plugin)

    layout = touch.build_layout(params)
    mapping = touch.build_map(params)

    rows = [
        row
        for page in layout["root"]["stack"][0]["pager"]
        for row in page["tiles"][0]["each"]
    ]
    layout_ccs = [row["cc"] for row in rows if "cc" in row]
    map_ccs = [m["cc"] for m in mapping["mappings"]]
    assert layout_ccs == map_ccs

    assert [m["param"] for m in mapping["mappings"]] == [p.name for p in params]


def test_the_map_omits_parameters_with_no_controller_left():
    plugin = _fake_plugin([_info(f"P{i}") for i in range(140)])
    params = touch.collect_parameters(plugin)
    assert len(touch.build_map(params)["mappings"]) == 128


def test_the_generated_map_loads_back_into_a_mapper(tmp_path):
    """Round trip: what the generator wrote, --map-file can read."""
    from minihost.cli import _load_map_file

    plugin = _fake_plugin([_info("Cutoff"), _info("Resonance")])
    params = touch.collect_parameters(plugin)

    mapper_plugin = MagicMock()
    mapper_plugin.find_param = MagicMock(
        side_effect=lambda n: {"cutoff": 0, "resonance": 1}[n.lower()]
    )
    mapper = minihost.MidiMapper(mapper_plugin)

    path = tmp_path / "minihost_touch_map.json"
    path.write_text(json.dumps(touch.build_map(params)))
    assert _load_map_file(str(path), mapper) == 2

    assert mapper.cc_mappings == {(0, 0): "Cutoff", (0, 1): "Resonance"}


# -- write_files --------------------------------------------------------------


def test_write_files_produces_both(tmp_path):
    plugin = _fake_plugin([_info("Cutoff")])
    params = touch.collect_parameters(plugin)
    layout_path, map_path = touch.write_files(params, str(tmp_path / "surface"))

    assert Path(layout_path).name == "surface.ui.json"
    assert Path(map_path).name == "surface.map.json"
    _assert_clean(json.loads(Path(layout_path).read_text()))


# -- with the compiler --------------------------------------------------------


def test_a_generated_layout_compiles_and_resolves():
    """Needs the real compiler: resolution catches a `sizes` that does not
    divide or a row too narrow for its children, which the standalone checker
    cannot see."""
    py2tosc = pytest.importorskip("py2tosc")

    plugin = _fake_plugin(
        [
            _info("Cutoff"),
            _info("Bypass", is_boolean=True),
            _info("Wave", num_steps=8),
            *[_info(f"P{i}") for i in range(20)],
        ]
    )
    layout = touch.build_layout(touch.collect_parameters(plugin))
    doc = py2tosc.ui_json.build(layout)
    assert doc.root is not None


def test_required_schema_matches_what_is_stamped():
    """Understating the stamp is the mistake nothing catches by building: the
    reader that would catch it is new enough to build the file anyway."""
    py2tosc = pytest.importorskip("py2tosc")

    layout = _layout()
    assert py2tosc.ui_json.required_schema(layout) == layout["schema"]


def test_every_address_in_the_compiled_document_is_a_real_parameter():
    py2tosc = pytest.importorskip("py2tosc")

    plugin = _fake_plugin(
        [_info("Cutoff"), _info("Bypass", is_boolean=True), _info("Wave", num_steps=8)]
    )
    params = touch.collect_parameters(plugin)
    doc = py2tosc.ui_json.build(touch.build_layout(params))

    expected = {f"/mh/param/{p.slug}" for p in params}
    found = set()

    def walk(control):
        for message in control.messages or []:
            if type(message).__name__ == "OscMessage":
                found.add(
                    "".join(
                        getattr(part, "value", "") or ""
                        for part in (message.path or [])
                    )
                )
        for child in control.children or []:
            walk(child)

    walk(doc.root)
    assert found == expected


@skip_if_no_plugin
def test_a_real_plugin_end_to_end(tmp_path):
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    params = touch.collect_parameters(plugin)
    assert params, "the plugin exposed no automatable parameters"

    layout = touch.build_layout(params[:24], plugin_name="test")
    _assert_clean(layout)

    py2tosc = pytest.importorskip("py2tosc")
    doc = py2tosc.ui_json.build(layout)
    out = tmp_path / "real.tosc"
    doc.save(out)
    assert out.stat().st_size > 0
