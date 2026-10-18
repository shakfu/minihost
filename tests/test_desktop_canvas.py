"""Canvas edits, driven headlessly through `minihost_desktop --canvas-script`.

The script applies each op through the code the mouse, context menu and
Properties dialog use (`CanvasComponent::applyScriptOp`), then saves. Plugin
nodes carry probe data, so no plugin is opened.
"""

from __future__ import annotations

import json
import subprocess

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

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


def _script(tmp_path, proj, ops, ok=True):
    script = tmp_path / "script.json"
    script.write_text(
        json.dumps({"project": proj.name, "ops": ops, "save": "saved.json"})
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
