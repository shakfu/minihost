"""Canvas edits, driven headlessly through `minihost_desktop --canvas-script`.

The script applies each op through the code the mouse, context menu and
Properties dialog use (`CanvasComponent::applyScriptOp`), then saves. Plugin
nodes carry probe data, so no plugin is opened.
"""

from __future__ import annotations

import json
import struct
import subprocess
import wave
from pathlib import Path

import pytest
from cli_helpers import find_test_plugin
from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

REPO = Path(__file__).resolve().parents[1]
FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")

pytestmark = skip_if_no_desktop

PROBE = {
    "in_channels": 2,
    "out_channels": 2,
    "accepts_midi": True,
    "produces_midi": True,
}


def _project(tmp_path, nodes, edges=()):
    # The parser requires an output node; "spk" is left out of _ids.
    nodes = [*nodes, {"id": "spk", "kind": "device_output", "channels": 2}]
    proj = tmp_path / "p.json"
    proj.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": 48000,
                "block_size": 512,
                "nodes": nodes,
                "edges": list(edges),
            }
        )
    )
    return proj


def _script(tmp_path, proj, ops, ok=True, untitled=False):
    script = tmp_path / "script.json"
    script.write_text(
        json.dumps(
            {
                "project": str(proj),
                "ops": ops,
                "save": "saved.json",
                "untitled": untitled,
            }
        )
    )
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--canvas-script={script}"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if ok:
        assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"
    results = json.loads(res.stdout)["results"]
    saved = json.loads((tmp_path / "saved.json").read_text())
    return results, saved


def _edges(doc):
    return sorted(
        (e["src"], e["dst"], e.get("dst_port", 0), e.get("kind", "audio"))
        for e in doc["edges"]
    )


def _ids(doc):
    return [n["id"] for n in doc["nodes"] if n["id"] != "spk"]


def _synth_project(tmp_path):
    return _project(
        tmp_path,
        [
            {"id": "in", "kind": "input", "channels": 2, "source": "in.wav"},
            {"id": "mi", "kind": "midi_input"},
            {"id": "fx", "kind": "plugin", "path": "fx.vst3", "probe": PROBE},
            {"id": "out", "kind": "output", "channels": 2, "sink": "out.wav"},
        ],
        [
            {"src": "mi", "dst": "fx", "kind": "midi"},
            {"src": "fx", "dst": "out"},
        ],
    )


def test_audio_edge_keeps_the_midi_edge_on_the_same_plugin(tmp_path):
    # Both edges reach the plugin at dst_port 0; the audio edge used to
    # replace the MIDI one.
    proj = _synth_project(tmp_path)
    results, saved = _script(
        tmp_path, proj, [{"op": "connect", "src": "in", "dst": "fx", "port": 0}]
    )
    assert results[0]["warnings"] == []
    assert _edges(saved) == [
        ("fx", "out", 0, "audio"),
        ("in", "fx", 0, "audio"),
        ("mi", "fx", 0, "midi"),
    ]


def test_undo_restores_the_document_before_a_connect(tmp_path):
    proj = _synth_project(tmp_path)
    before = _edges(json.loads(proj.read_text()))
    _, saved = _script(
        tmp_path,
        proj,
        [{"op": "connect", "src": "in", "dst": "fx", "port": 0}, {"op": "undo"}],
    )
    assert _edges(saved) == before


def test_rename_to_an_id_in_use_is_refused(tmp_path):
    # Two nodes sharing an id merged on the canvas, and the saved file
    # failed to reopen with "duplicate node id".
    proj = _project(
        tmp_path,
        [
            {"id": "a", "kind": "gain", "channels": 2, "gain": 1.0},
            {"id": "b", "kind": "gain", "channels": 2, "gain": 1.0},
        ],
        [{"src": "a", "dst": "b"}],
    )
    results, saved = _script(
        tmp_path, proj, [{"op": "properties", "id": "a", "fields": {"id": "b"}}]
    )
    assert results[0]["warnings"] == [
        'Id already in use: Another node is already called "b". '
        "Node ids must be unique."
    ]
    assert sorted(_ids(saved)) == ["a", "b"]
    assert _edges(saved) == [("a", "b", 0, "audio")]


def test_rename_rewrites_edges(tmp_path):
    proj = _project(
        tmp_path,
        [
            {"id": "a", "kind": "gain", "channels": 2, "gain": 1.0},
            {"id": "b", "kind": "gain", "channels": 2, "gain": 1.0},
        ],
        [{"src": "a", "dst": "b"}],
    )
    results, saved = _script(
        tmp_path, proj, [{"op": "properties", "id": "a", "fields": {"id": "c"}}]
    )
    assert results[0]["warnings"] == []
    assert sorted(_ids(saved)) == ["b", "c"]
    assert _edges(saved) == [("c", "b", 0, "audio")]


def test_generated_ids_avoid_every_kind(tmp_path):
    # The channel-split helper checked only other pick_channel nodes, so a
    # gain node called L1 got a second L1.
    proj = _project(
        tmp_path, [{"id": "L1", "kind": "gain", "channels": 2, "gain": 1.0}]
    )
    _, saved = _script(tmp_path, proj, [{"op": "add", "kind": "channel_split"}])
    ids = _ids(saved)
    assert len(ids) == 3
    assert len(set(ids)) == 3

    # The saved file loads again: the parser rejects duplicate ids.
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--save-roundtrip={tmp_path / 'saved.json'}"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert res.returncode == 0, res.stderr


def test_malformed_op_fails_the_script(tmp_path):
    proj = _project(tmp_path, [{"id": "a", "kind": "gain", "channels": 2, "gain": 1.0}])
    results, _ = _script(tmp_path, proj, [{"op": "delete", "id": "nope"}], ok=False)
    assert results[0] == {"ok": False, "error": "no node nope", "warnings": []}


def _input_project(tmp_path):
    src = tmp_path / "in.wav"
    src.write_bytes(b"")  # parsing does not read it
    return _project(
        tmp_path, [{"id": "in", "kind": "input", "channels": 2, "source": str(src)}]
    )


def test_relative_path_in_properties_resolves_against_the_project(tmp_path):
    proj = _input_project(tmp_path)
    op = {"op": "properties", "id": "in", "fields": {"source": "audio/drums.wav"}}
    results, saved = _script(tmp_path, proj, [op])
    assert results[0]["warnings"] == []
    (node,) = [n for n in saved["nodes"] if n["id"] == "in"]
    # Absolute in the document, relative again on save.
    assert node["source"] == "audio/drums.wav"


def test_relative_path_in_properties_of_an_untitled_project_warns(tmp_path):
    proj = _input_project(tmp_path)
    op = {"op": "properties", "id": "in", "fields": {"source": "audio/drums.wav"}}
    results, saved = _script(tmp_path, proj, [op], untitled=True)
    assert any("Path is relative" in w for w in results[0]["warnings"])
    (node,) = [n for n in saved["nodes"] if n["id"] == "in"]
    assert node["source"] == "in.wav"  # unchanged


def _overlapping(bounds):
    def hit(a, b):
        return (
            a[0] < b[0] + b[2]
            and b[0] < a[0] + a[2]
            and a[1] < b[1] + b[3]
            and b[1] < a[1] + a[3]
        )

    ids = sorted(bounds)
    return [
        (a, b)
        for i, a in enumerate(ids)
        for b in ids[i + 1 :]
        if hit(bounds[a], bounds[b])
    ]


@pytest.mark.parametrize(
    "example",
    sorted((REPO / "examples" / "projects").glob("*.json")),
    ids=lambda p: p.stem,
)
def test_saved_example_layouts_do_not_overlap(tmp_path, example):
    # Node size is bounded by the 220 x 110 grid the examples are saved on.
    results, _ = _script(tmp_path, example, [{"op": "bounds"}])
    assert _overlapping(results[0]["bounds"]) == []


def test_added_node_does_not_land_on_a_saved_node(tmp_path):
    proj = _project(tmp_path, [{"id": "a", "kind": "gain", "channels": 2, "gain": 1.0}])
    doc = json.loads(proj.read_text())
    # Where auto-layout puts column 0, row 0.
    doc["layout"] = {"a": {"x": 40, "y": 40}, "spk": {"x": 300, "y": 40}}
    proj.write_text(json.dumps(doc))
    results, _ = _script(
        tmp_path, proj, [{"op": "add", "kind": "mix"}, {"op": "bounds"}]
    )
    bounds = results[1]["bounds"]
    assert len(bounds) == 3
    assert _overlapping(bounds) == []


def test_snapshot_writes_a_png_of_the_requested_size(tmp_path):
    proj = _synth_project(tmp_path)
    op = {"op": "snapshot", "path": "shot.png", "width": 640, "height": 360}
    results, _ = _script(tmp_path, proj, [op])
    assert results[0]["ok"]
    png = (tmp_path / "shot.png").read_bytes()
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">II", png[16:24]) == (640, 360)


# ----- editing: placement, clipboard, insert on a cable ----- #


def _gains(tmp_path, *ids, channels=2, edges=()):
    return _project(
        tmp_path,
        [{"id": i, "kind": "gain", "channels": channels, "gain": 1.0} for i in ids],
        edges,
    )


def _new_ids(results, before):
    return sorted(set(results[-1]["bounds"]) - set(before))


def test_add_places_the_node_where_asked(tmp_path):
    proj = _gains(tmp_path, "a")
    ops = [{"op": "add", "kind": "gain", "x": 500, "y": 320}, {"op": "bounds"}]
    results, _ = _script(tmp_path, proj, ops)
    (new,) = _new_ids(results, ["a", "spk"])
    assert results[1]["bounds"][new][:2] == [500, 320]


def test_paste_copies_nodes_and_the_edges_between_them(tmp_path):
    proj = _gains(
        tmp_path,
        "a",
        "b",
        "c",
        edges=[{"src": "a", "dst": "b"}, {"src": "b", "dst": "c"}],
    )
    ops = [
        {"op": "select", "ids": ["a", "b"]},
        {"op": "copy"},
        {"op": "paste", "x": 400, "y": 300},
        {"op": "bounds"},
    ]
    results, saved = _script(tmp_path, proj, ops)
    new = _new_ids(results, ["a", "b", "c", "spk"])
    assert len(new) == 2
    # The a -> b edge comes along under the new ids; b -> c does not.
    copied = [e for e in _edges(saved) if e[0] in new]
    assert len(copied) == 1 and copied[0][1] in new
    assert min(results[3]["bounds"][i][0] for i in new) == 400


def test_duplicate_leaves_the_clipboard_alone(tmp_path):
    proj = _gains(tmp_path, "a", "b")
    ops = [
        {"op": "select", "ids": ["a"]},
        {"op": "copy"},
        {"op": "select", "ids": ["b"]},
        {"op": "duplicate"},
        {"op": "paste"},
    ]
    _, saved = _script(tmp_path, proj, ops)
    ids = _ids(saved)
    assert len(ids) == 4
    assert {"a", "b"} < set(ids)


def test_deleting_a_selection_is_one_undo_step(tmp_path):
    proj = _gains(tmp_path, "a", "b", "c")
    ops = [{"op": "select", "ids": ["a", "c"]}, {"op": "delete_selection"}]
    _, saved = _script(tmp_path, proj, ops)
    assert _ids(saved) == ["b"]
    _, saved = _script(tmp_path, proj, [*ops, {"op": "undo"}])
    assert sorted(_ids(saved)) == ["a", "b", "c"]


def _cable_project(tmp_path, channels=2):
    # a -> b along y = 40; "fx" waits below, unconnected.
    proj = _project(
        tmp_path,
        [
            {"id": "a", "kind": "gain", "channels": 2, "gain": 1.0},
            {"id": "b", "kind": "gain", "channels": 2, "gain": 1.0},
            {"id": "fx", "kind": "gain", "channels": channels, "gain": 1.0},
        ],
        [{"src": "a", "dst": "b"}],
    )
    doc = json.loads(proj.read_text())
    doc["layout"] = {
        "a": {"x": 40, "y": 40},
        "b": {"x": 600, "y": 40},
        "fx": {"x": 300, "y": 300},
    }
    proj.write_text(json.dumps(doc))
    return proj


def test_dropping_a_node_on_a_cable_inserts_it(tmp_path):
    proj = _cable_project(tmp_path)
    move = {"op": "move", "id": "fx", "x": 245, "y": 40}
    _, saved = _script(tmp_path, proj, [move])
    assert _edges(saved) == [("a", "fx", 0, "audio"), ("fx", "b", 0, "audio")]
    # The move and the splice undo together.
    _, saved = _script(tmp_path, proj, [move, {"op": "undo"}])
    assert _edges(saved) == [("a", "b", 0, "audio")]
    assert saved["layout"]["fx"] == {"x": 300, "y": 300}


def test_insert_on_a_cable_with_other_channels_is_refused(tmp_path):
    proj = _cable_project(tmp_path, channels=1)
    results, saved = _script(
        tmp_path, proj, [{"op": "move", "id": "fx", "x": 245, "y": 40}]
    )
    assert any("Cannot insert fx" in w for w in results[0]["warnings"])
    assert _edges(saved) == [("a", "b", 0, "audio")]


def test_moving_a_node_away_from_cables_only_moves_it(tmp_path):
    proj = _cable_project(tmp_path)
    _, saved = _script(tmp_path, proj, [{"op": "move", "id": "fx", "x": 300, "y": 500}])
    assert _edges(saved) == [("a", "b", 0, "audio")]
    assert saved["layout"]["fx"] == {"x": 300, "y": 500}


# ----- adding: file drops, quick add ----- #


def _wav(path, rate=48000, channels=2):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"\0\0" * channels * 64)
    return path


def _mid(path):
    track = b"\x00\xff\x2f\x00"  # end of track
    path.write_bytes(
        b"MThd"
        + struct.pack(">IHHH", 6, 0, 1, 96)
        + b"MTrk"
        + struct.pack(">I", len(track))
        + track
    )
    return path


def test_dropped_files_become_nodes(tmp_path):
    proj = _gains(tmp_path, "a")
    files = [
        str(_wav(tmp_path / "mono.wav", channels=1)),
        str(_mid(tmp_path / "song.mid")),
        str(tmp_path / "notes.txt"),
    ]
    ops = [{"op": "drop", "files": files, "x": 200, "y": 100}, {"op": "bounds"}]
    results, saved = _script(tmp_path, proj, ops)
    by_kind = {n["kind"]: n for n in saved["nodes"]}
    assert by_kind["input"]["channels"] == 1
    assert by_kind["input"]["source"] == "mono.wav"
    assert by_kind["midi_input"]["source"] == "song.mid"
    bounds = results[1]["bounds"]
    assert bounds[by_kind["input"]["id"]][:2] == [200, 100]
    assert bounds[by_kind["midi_input"]["id"]][:2] == [224, 124]


def test_dropped_audio_at_another_rate_is_resampled(tmp_path):
    proj = _gains(tmp_path, "a")
    wav = _wav(tmp_path / "cd.wav", rate=44100)
    _, saved = _script(
        tmp_path, proj, [{"op": "drop", "files": [str(wav)], "x": 0, "y": 0}]
    )
    (node,) = [n for n in saved["nodes"] if n["kind"] == "input"]
    assert node["resample"] is True


def test_quick_add_runs_the_first_match(tmp_path):
    proj = _gains(tmp_path, "a")
    ops = [{"op": "quick_add", "query": "metro", "x": 80, "y": 400}, {"op": "bounds"}]
    results, saved = _script(tmp_path, proj, ops)
    (metro,) = [n for n in saved["nodes"] if n["kind"] == "metronome"]
    assert results[1]["bounds"][metro["id"]][:2] == [80, 400]


def test_quick_add_with_no_match_fails(tmp_path):
    proj = _gains(tmp_path, "a")
    results, _ = _script(
        tmp_path, proj, [{"op": "quick_add", "query": "zzz"}], ok=False
    )
    assert results[0]["error"] == "nothing matches zzz"


# ----- problems and view ----- #


def test_problems_flag_unfed_inputs_and_channel_mismatches(tmp_path):
    proj = _project(
        tmp_path,
        [
            {"id": "m", "kind": "mix", "num_inputs": 2, "channels": 2},
            {"id": "mono", "kind": "gain", "channels": 1, "gain": 1.0},
            {"id": "st", "kind": "gain", "channels": 2, "gain": 1.0},
        ],
        [{"src": "st", "dst": "m", "dst_port": 0}],
    )
    ops = [{"op": "connect", "src": "mono", "dst": "st", "port": 0}, {"op": "problems"}]
    results, _ = _script(tmp_path, proj, ops)
    # The canvas refuses a mismatched connect, so the edge comes from a file.
    assert any("Channel mismatch" in w for w in results[0]["warnings"])
    problems = results[1]["problems"]
    assert problems["m"] == ["Input 2 is not connected."]
    assert problems["mono"] == ["Input is not connected."]
    assert problems["spk"] == ["Input is not connected."]


def test_a_mismatched_edge_from_the_file_is_a_problem(tmp_path):
    proj = _project(
        tmp_path,
        [
            {"id": "mono", "kind": "gain", "channels": 1, "gain": 1.0},
            {"id": "st", "kind": "gain", "channels": 2, "gain": 1.0},
        ],
        [{"src": "mono", "dst": "st"}],
    )
    results, _ = _script(tmp_path, proj, [{"op": "problems"}])
    assert "mono sends 1 channels; this input takes 2." in results[0]["problems"]["st"]


@pytest.mark.parametrize(
    "example",
    sorted((REPO / "examples" / "projects").glob("*.json")),
    ids=lambda p: p.stem,
)
def test_example_projects_have_no_problems(tmp_path, example):
    results, _ = _script(tmp_path, example, [{"op": "problems"}])
    assert results[0]["problems"] == {}


def test_fit_zooms_out_to_show_a_wide_graph(tmp_path):
    proj = _gains(tmp_path, "a", "b")
    doc = json.loads(proj.read_text())
    doc["layout"] = {"a": {"x": 0, "y": 0}, "b": {"x": 4000, "y": 0}}
    proj.write_text(json.dumps(doc))
    ops = [
        {"op": "snapshot", "path": "shot.png", "width": 1000, "height": 600},
        {"op": "fit"},
    ]
    results, _ = _script(tmp_path, proj, ops)
    assert 0.2 <= results[1]["zoom"] < 0.3


# ----- properties panel ----- #


def test_panel_lists_the_selected_nodes_fields(tmp_path):
    proj = _gains(tmp_path, "a")
    results, _ = _script(
        tmp_path, proj, [{"op": "select", "ids": ["a"]}, {"op": "panel"}]
    )
    assert results[1]["fields"] == ["id", "channels", "gain"]


def test_panel_edit_applies_as_one_undo_step(tmp_path):
    proj = _gains(tmp_path, "a")
    edit = [
        {"op": "select", "ids": ["a"]},
        {"op": "panel", "field": "gain", "value": "0.25"},
    ]
    _, saved = _script(tmp_path, proj, edit)
    (node,) = [n for n in saved["nodes"] if n["id"] == "a"]
    assert node["gain"] == 0.25
    _, saved = _script(tmp_path, proj, [*edit, {"op": "undo"}])
    (node,) = [n for n in saved["nodes"] if n["id"] == "a"]
    assert node["gain"] == 1.0


def test_panel_rename_keeps_the_node_selected(tmp_path):
    proj = _gains(tmp_path, "a")
    ops = [
        {"op": "select", "ids": ["a"]},
        {"op": "panel", "field": "id", "value": "lead_gain"},
        {"op": "panel"},
    ]
    results, saved = _script(tmp_path, proj, ops)
    assert "lead_gain" in _ids(saved)
    assert results[2]["fields"] == ["id", "channels", "gain"]


def test_panel_with_nothing_selected_has_no_fields(tmp_path):
    proj = _gains(tmp_path, "a")
    results, _ = _script(tmp_path, proj, [{"op": "panel"}])
    assert results[0]["fields"] == []


def test_example_projects_ship_beside_the_app_as_templates():
    # File > New from Template lists these.
    exe_dir = DESKTOP_BIN.parent
    examples = (
        exe_dir.parent / "Resources" / "examples"
        if exe_dir.name == "MacOS"
        else exe_dir / "examples"
    )
    shipped = {p.name for p in examples.glob("*.json")}
    expected = {p.name for p in (REPO / "examples" / "projects").glob("*.json")}
    assert shipped == expected


# ----- meters ----- #


def _meter_project(tmp_path):
    return _project(tmp_path, [{"id": "m", "kind": "meter", "channels": 2}])


def _meter(peaks, t):
    return {"op": "meter", "id": "m", "peaks": peaks, "t": t}


def test_meter_reads_in_db_and_falls_24_db_per_second(tmp_path):
    proj = _meter_project(tmp_path)
    ops = [
        _meter([0.5, 0.1], 0.0),
        _meter([0.0, 0.0], 0.5),
        {"op": "meter_state", "id": "m"},
    ]
    results, _ = _script(tmp_path, proj, ops)
    level = results[2]["level_db"]
    assert level[0] == pytest.approx(-6.02 - 12.0, abs=0.05)
    assert level[1] == pytest.approx(-20.0 - 12.0, abs=0.05)


def test_meter_holds_the_peak_for_one_and_a_half_seconds(tmp_path):
    proj = _meter_project(tmp_path)
    ops = [
        _meter([0.5, 0.5], 0.0),
        _meter([0.01, 0.01], 1.0),
        {"op": "meter_state", "id": "m"},
        _meter([0.01, 0.01], 2.0),
        {"op": "meter_state", "id": "m"},
    ]
    results, _ = _script(tmp_path, proj, ops)
    assert results[2]["hold_db"][0] == pytest.approx(-6.02, abs=0.05)
    # Released to the falling level once the hold time has passed.
    assert results[4]["hold_db"][0] == pytest.approx(results[4]["level_db"][0])


def test_clip_light_latches_until_the_meter_is_clicked(tmp_path):
    proj = _meter_project(tmp_path)
    ops = [
        _meter([1.0, 0.2], 0.0),
        _meter([0.1, 0.1], 5.0),
        {"op": "meter_state", "id": "m"},
        {"op": "click", "id": "m"},
        {"op": "meter_state", "id": "m"},
    ]
    results, _ = _script(tmp_path, proj, ops)
    assert results[2]["clip"] is True
    assert results[4]["clip"] is False


def test_peak_below_full_scale_does_not_clip(tmp_path):
    proj = _meter_project(tmp_path)
    ops = [_meter([0.999, 0.999], 0.0), {"op": "meter_state", "id": "m"}]
    results, _ = _script(tmp_path, proj, ops)
    assert results[1]["clip"] is False


# ----- plugin probe ----- #


@pytest.mark.skipif(not FX, reason="MinihostTestFx not built")
def test_added_plugin_records_its_latency(tmp_path):
    proj = _gains(tmp_path, "a")
    _, saved = _script(tmp_path, proj, [{"op": "drop", "files": [FX], "x": 0, "y": 0}])
    (node,) = [n for n in saved["nodes"] if n["kind"] == "plugin"]
    assert node["probe"]["latency"] == 0


# ----- automation follows node edits ----- #


def _automated(tmp_path):
    proj = _project(
        tmp_path,
        [{"id": "fx", "kind": "plugin", "path": "fx.vst3", "probe": PROBE}],
    )
    doc = json.loads(proj.read_text())
    doc["automation"] = [{"node": "fx", "param": 0, "points": [[0.0, 0.5]]}]
    proj.write_text(json.dumps(doc))
    return proj


def test_renaming_a_plugin_renames_its_automation(tmp_path):
    op = {"op": "properties", "id": "fx", "fields": {"id": "comp"}}
    _, saved = _script(tmp_path, _automated(tmp_path), [op])
    assert [a["node"] for a in saved["automation"]] == ["comp"]


def test_deleting_a_plugin_drops_its_automation(tmp_path):
    _, saved = _script(tmp_path, _automated(tmp_path), [{"op": "delete", "id": "fx"}])
    assert "automation" not in saved
