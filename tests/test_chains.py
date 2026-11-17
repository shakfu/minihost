"""Saved chains (docs/dev/chains.md): the library, expansion, and parity.

A chain in a project must render exactly as the same nodes written flat,
in the Python renderer and the desktop app. Its ports are `audio_inlet`,
`audio_outlet`, `midi_inlet` and `midi_outlet` nodes, which load as a
one-input bus or MIDI merge. Outside
edges reach a chain only through its ports, by chain id and index.
"""

from __future__ import annotations

import json
import subprocess

import numpy as np
import pytest

import minihost
from minihost import audio_io, chain_library

from cli_helpers import find_test_plugin
from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

SR = 48000
FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")


def _port(pid, direction, channels=2):
    kind = "audio_inlet" if direction == "in" else "audio_outlet"
    return {"id": pid, "kind": kind, "channels": channels}


# Parallel: the input fans out to a wet gain and the dry port of a mix.
PARALLEL = {
    "name": "Parallel",
    "nodes": [
        _port("in", "in"),
        {"id": "wet", "kind": "gain", "channels": 2, "gain": 0.25},
        {
            "id": "sum",
            "kind": "mix",
            "channels": 2,
            "num_inputs": 2,
            "gains": [1.0, 0.5],
        },
        _port("out", "out"),
    ],
    "edges": [
        {"src": "in", "dst": "sum", "dst_port": 0},
        {"src": "in", "dst": "wet"},
        {"src": "wet", "dst": "sum", "dst_port": 1},
        {"src": "sum", "dst": "out"},
    ],
}


def _input(tmp_path, frames=4096):
    wav = tmp_path / "in.wav"
    rng = np.random.default_rng(7)
    audio_io.write_audio(
        str(wav),
        (rng.standard_normal((2, frames)) * 0.1).astype(np.float32),
        SR,
        bit_depth=24,
    )
    return wav


def _project(tmp_path, name, nodes, edges):
    path = tmp_path / f"{name}.json"
    path.write_text(
        json.dumps(
            {
                "minihost_project_version": 1,
                "sample_rate": SR,
                "block_size": 256,
                "nodes": [
                    {
                        "id": "in",
                        "kind": "input",
                        "channels": 2,
                        "source": str(_input(tmp_path)),
                    },
                    *nodes,
                    {
                        "id": "out",
                        "kind": "output",
                        "channels": 2,
                        "sink": str(tmp_path / f"{name}.wav"),
                        "bit_depth": 32,
                    },
                ],
                "edges": edges,
            }
        )
    )
    return path


def _chained(tmp_path, name="chained", chain=PARALLEL):
    return _project(
        tmp_path,
        name,
        [chain_library.node(chain, "par")],
        [{"src": "in", "dst": "par"}, {"src": "par", "dst": "out"}],
    )


def _flat(tmp_path):
    # The same graph with buses where the ports were.
    nodes = [
        {**n, "id": "par/" + n["id"], "kind": "bus"}
        if n["kind"].startswith("audio_")
        else {**n, "id": "par/" + n["id"]}
        for n in PARALLEL["nodes"]
    ]
    edges = [
        {**e, "src": "par/" + e["src"], "dst": "par/" + e["dst"]}
        for e in PARALLEL["edges"]
    ]
    edges += [{"src": "in", "dst": "par/in"}, {"src": "par/out", "dst": "out"}]
    # Flat ids with '/' are plain ids when no chain has that name.
    return _project(tmp_path, "flat", nodes, edges)


def _read(path):
    audio, _ = audio_io.read_audio(str(path), as_=np.ndarray)
    return audio


def _render_desktop(proj, *extra):
    return subprocess.run(
        [str(DESKTOP_BIN), f"--render-project={proj}", *extra],
        capture_output=True,
        text=True,
        timeout=60,
    )


# -- expansion -------------------------------------------------------- #


def test_expand_routes_chain_edges_through_ports():
    nodes, edges = chain_library.expand(
        [{"id": "in", "kind": "input"}, chain_library.node(PARALLEL, "par")],
        [{"src": "in", "dst": "par"}, {"src": "par", "dst": "out"}],
    )
    assert [n["id"] for n in nodes] == ["in", "par/in", "par/wet", "par/sum", "par/out"]
    assert edges[-2:] == [
        {"src": "in", "dst": "par/in", "dst_port": 0},
        {"src": "par/out", "dst": "out"},
    ]


def test_dst_port_picks_the_inlet_in_file_order():
    chain = {
        "nodes": [
            _port("main", "in"),
            _port("key", "in", channels=1),
            {"id": "m", "kind": "midi_inlet"},
        ]
    }
    _, edges = chain_library.expand(
        [chain_library.node(chain, "c")],
        [
            {"src": "a", "dst": "c"},
            {"src": "b", "dst": "c", "dst_port": 1},
            {"src": "k", "dst": "c", "kind": "midi"},
        ],
    )
    assert edges == [
        {"src": "a", "dst": "c/main", "dst_port": 0},
        {"src": "b", "dst": "c/key", "dst_port": 0},
        {"src": "k", "dst": "c/m", "kind": "midi"},
    ]


def test_src_port_picks_the_outlet_in_file_order():
    chain = {
        "nodes": [
            _port("i", "in"),
            _port("wet", "out"),
            _port("dry", "out"),
            {"id": "mo", "kind": "midi_outlet"},
        ],
    }
    _, edges = chain_library.expand(
        [chain_library.node(chain, "c")],
        [
            {"src": "c", "dst": "a"},
            {"src": "c", "src_port": 1, "dst": "b", "dst_port": 2},
            {"src": "c", "dst": "m", "kind": "midi"},
        ],
    )
    assert edges == [
        {"src": "c/wet", "dst": "a"},
        {"src": "c/dry", "dst": "b", "dst_port": 2},
        {"src": "c/mo", "dst": "m", "kind": "midi"},
    ]
    with pytest.raises(ValueError, match="has no audio outlet 3"):
        chain_library.expand(
            [chain_library.node(chain, "c")], [{"src": "c", "src_port": 3, "dst": "a"}]
        )


@pytest.mark.parametrize(
    "edge", [{"src": "x", "dst": "c/g"}, {"src": "c/o", "dst": "x"}]
)
def test_edges_cannot_name_a_node_inside_a_chain(edge):
    chain = {"nodes": [{"id": "g", "kind": "gain"}, _port("o", "out")]}
    with pytest.raises(ValueError, match="connect to chain c, not to a node inside it"):
        chain_library.expand([chain_library.node(chain, "c")], [edge])


def test_chain_renders_as_its_nodes_written_flat(tmp_path):
    minihost.render_project(_chained(tmp_path))
    minihost.render_project(_flat(tmp_path))
    a, b = _read(tmp_path / "chained.wav"), _read(tmp_path / "flat.wav")
    assert np.abs(a).max() > 0.01
    np.testing.assert_array_equal(a, b)
    # dry + 0.5 * 0.25 * wet
    np.testing.assert_allclose(a, _read(tmp_path / "in.wav") * 1.125, atol=1e-6)


@skip_if_no_desktop
def test_a_port_with_no_cable_in_passes_silence(tmp_path):
    # `key`, `notes` and `spare` have nothing feeding them.
    chain = {
        "nodes": [
            _port("main", "in"),
            _port("key", "in"),
            {"id": "notes", "kind": "midi_inlet"},
            {"id": "sum", "kind": "mix", "num_inputs": 2, "channels": 2},
            _port("o", "out"),
            _port("spare", "out"),
        ],
        "edges": [
            {"src": "main", "dst": "sum", "dst_port": 0},
            {"src": "key", "dst": "sum", "dst_port": 1},
            {"src": "sum", "dst": "o"},
        ],
    }
    proj = _project(
        tmp_path,
        "unfed",
        [chain_library.node(chain, "c")],
        [{"src": "in", "dst": "c"}, {"src": "c", "dst": "out"}],
    )
    minihost.render_project(proj)
    expected = _read(tmp_path / "in.wav")
    np.testing.assert_allclose(_read(tmp_path / "unfed.wav"), expected, atol=1e-6)
    res = _render_desktop(proj)
    assert res.returncode == 0, res.stderr
    np.testing.assert_allclose(_read(tmp_path / "unfed.wav"), expected, atol=1e-6)


def test_automation_addresses_inner_nodes(tmp_path):
    if FX is None:
        pytest.skip("MinihostTestFx not built")
    chain = {
        "name": "One",
        "nodes": [
            _port("i", "in"),
            {"id": "fx", "kind": "plugin", "path": FX},
            _port("o", "out"),
        ],
        "edges": [{"src": "i", "dst": "fx"}, {"src": "fx", "dst": "o"}],
    }
    proj = _project(
        tmp_path,
        "auto",
        [chain_library.node(chain, "c")],
        [{"src": "in", "dst": "c"}, {"src": "c", "dst": "out"}],
    )
    doc = json.loads(proj.read_text())
    doc["automation"] = [{"node": "c/fx", "param": 0, "points": [[0, 0.5]]}]
    proj.write_text(json.dumps(doc))
    loaded = minihost.load_project(proj)
    assert [p.id for p in loaded.plugins] == ["c/fx"]
    assert len(loaded.automation) == 1


@pytest.mark.parametrize(
    "chain, message",
    [
        ({"nodes": [{"id": "x", "kind": "input"}]}, "cannot be in a chain"),
        ({"nodes": [{"id": "x", "kind": "midi_output"}]}, "cannot be in a chain"),
        ({"nodes": [{"id": "a/b", "kind": "gain"}]}, "no '/'"),
        ({"nodes": []}, "non-empty"),
        (
            {
                "nodes": [{"id": "g", "kind": "gain"}],
                "edges": [{"src": "g", "dst": "h"}],
            },
            "must join two chain nodes",
        ),
        (
            {
                "nodes": [{"id": "g", "kind": "gain"}, _port("i", "in")],
                "edges": [{"src": "g", "dst": "i"}],
            },
            "edge into inlet 'i'",
        ),
        (
            {
                "nodes": [{"id": "g", "kind": "gain"}, _port("o", "out")],
                "edges": [{"src": "o", "dst": "g"}],
            },
            "edge from outlet 'o'",
        ),
    ],
)
def test_invalid_chains_are_rejected(tmp_path, chain, message):
    proj = _project(tmp_path, "bad", [{"id": "c", "kind": "chain", **chain}], [])
    with pytest.raises(minihost.ProjectError, match=message):
        minihost.load_project(proj)


def test_edge_to_a_chain_without_an_inlet_is_rejected(tmp_path):
    chain = {"nodes": [{"id": "g", "kind": "gain"}, _port("out", "out")]}
    proj = _project(
        tmp_path,
        "noin",
        [{"id": "c", "kind": "chain", **chain}],
        [{"src": "in", "dst": "c"}],
    )
    with pytest.raises(minihost.ProjectError, match="has no audio inlet 0"):
        minihost.load_project(proj)


def test_a_port_outside_a_chain_is_rejected(tmp_path):
    proj = _project(tmp_path, "stray", [_port("p", "in")], [])
    with pytest.raises(minihost.ProjectError, match="outside a chain"):
        minihost.load_project(proj)


# -- library ---------------------------------------------------------- #


def test_library_round_trip(tmp_path, monkeypatch):
    monkeypatch.setenv("MINIHOST_DESKTOP_SETTINGS_DIR", str(tmp_path))
    assert chain_library.library_dir() == tmp_path / "chains"
    path = chain_library.write("Parallel", PARALLEL)
    assert path == tmp_path / "chains" / "Parallel.mhchain"
    assert chain_library.names() == ["Parallel"]
    got = chain_library.read("Parallel")
    assert got["minihost_chain_version"] == 1
    assert {k: got[k] for k in PARALLEL} == PARALLEL
    with pytest.raises(ValueError, match="invalid chain name"):
        chain_library.write("a/b", PARALLEL)


def test_read_resolves_relative_plugin_paths(tmp_path):
    (tmp_path / "c.mhchain").write_text(
        json.dumps(
            {
                "minihost_chain_version": 1,
                "nodes": [{"id": "fx", "kind": "plugin", "path": "plugs/Fx.vst3"}],
            }
        )
    )
    got = chain_library.read(tmp_path / "c.mhchain")
    assert got["nodes"][0]["path"] == str((tmp_path / "plugs/Fx.vst3").resolve())


def test_project_keeps_its_copy_when_the_library_changes(tmp_path, monkeypatch):
    monkeypatch.setenv("MINIHOST_DESKTOP_SETTINGS_DIR", str(tmp_path))
    chain_library.write("Parallel", PARALLEL)
    proj = _chained(tmp_path, chain=chain_library.read("Parallel"))
    minihost.render_project(proj)
    before = _read(tmp_path / "chained.wav")
    changed = json.loads(json.dumps(PARALLEL))
    changed["nodes"][1]["gain"] = 1.0
    chain_library.write("Parallel", changed)
    minihost.render_project(proj)
    np.testing.assert_array_equal(_read(tmp_path / "chained.wav"), before)


# -- desktop ---------------------------------------------------------- #


@skip_if_no_desktop
def test_desktop_render_matches_python(tmp_path):
    proj = _chained(tmp_path)
    minihost.render_project(proj)
    py = _read(tmp_path / "chained.wav")
    res = _render_desktop(proj)
    assert res.returncode == 0, res.stderr
    np.testing.assert_array_equal(_read(tmp_path / "chained.wav"), py)


@skip_if_no_desktop
@pytest.mark.parametrize(
    "nodes, message",
    [
        (
            [{"id": "c", "kind": "chain", "nodes": [{"id": "x", "kind": "output"}]}],
            "cannot be in a chain",
        ),
        (
            [
                {
                    "id": "c",
                    "kind": "chain",
                    "nodes": [{"id": "p", "kind": "audio_port"}],
                }
            ],
            "unknown node kind",
        ),
        ([_port("p", "in")], "outside a chain"),
        (
            [chain_library.node(PARALLEL, "par")],
            "connect to chain par, not to a node inside it",
        ),
    ],
)
def test_desktop_rejects_what_python_rejects(tmp_path, nodes, message):
    edges = [{"src": "in", "dst": "par/wet"}] if "par" in str(nodes) else []
    proj = _project(tmp_path, "bad", nodes, edges)
    with pytest.raises(minihost.ProjectError, match=message):
        minihost.load_project(proj)
    res = _render_desktop(proj)
    assert res.returncode != 0
    assert message in res.stderr


@skip_if_no_desktop
def test_desktop_save_keeps_the_chain_folded(tmp_path):
    proj = _chained(tmp_path)
    doc = json.loads(proj.read_text())
    doc["layout"] = {"par/wet": {"x": 10, "y": 20}}
    proj.write_text(json.dumps(doc))
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--save-roundtrip={proj}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert res.returncode == 0, res.stderr
    saved = json.loads(proj.with_suffix(".resaved.json").read_text())
    chain = next(n for n in saved["nodes"] if n["kind"] == "chain")
    assert chain["id"] == "par" and chain["source"] == "Parallel"
    assert sorted(n["id"] for n in chain["nodes"]) == ["in", "out", "sum", "wet"]
    ports = [n for n in chain["nodes"] if n["kind"].startswith("audio_")]
    assert sorted((p["id"], p["kind"]) for p in ports) == [
        ("in", "audio_inlet"),
        ("out", "audio_outlet"),
    ]
    assert len(chain["edges"]) == 4
    assert not any(n["id"].startswith("par/") for n in saved["nodes"])
    # Outside edges name the chain, not its ports.
    assert sorted(saved["edges"], key=lambda e: e["src"]) == [
        {"src": "in", "dst": "par", "dst_port": 0},
        {"src": "par", "dst": "out", "dst_port": 0},
    ]
    assert saved["layout"]["par/wet"] == {"x": 10.0, "y": 20.0}
    # The saved file renders as the original.
    minihost.render_project(proj)
    a = _read(tmp_path / "chained.wav")
    minihost.render_project(proj.with_suffix(".resaved.json"))
    np.testing.assert_array_equal(_read(tmp_path / "chained.wav"), a)


# -- desktop canvas --------------------------------------------------- #


def _canvas(tmp_path, monkeypatch, nodes=None, edges=None):
    from test_desktop_canvas import _project as canvas_project

    monkeypatch.setenv("MINIHOST_DESKTOP_SETTINGS_DIR", str(tmp_path / "settings"))
    if nodes is None:
        nodes = [
            {"id": "src", "kind": "gain", "channels": 1},
            {"id": "g1", "kind": "gain", "channels": 1, "gain": 0.5},
            {"id": "g2", "kind": "gain", "channels": 1, "gain": 0.25},
            {"id": "dst", "kind": "gain", "channels": 1},
        ]
        edges = [
            {"src": "src", "dst": "g1"},
            {"src": "g1", "dst": "g2"},
            {"src": "g2", "dst": "dst"},
        ]
    return canvas_project(tmp_path, nodes, edges or [])


def _run(tmp_path, proj, ops, ok=True):
    from test_desktop_canvas import _script

    return _script(tmp_path, proj, ops, ok=ok)


def _chain_nodes(doc):
    return {n["id"]: n for n in doc["nodes"] if n["kind"] == "chain"}


def _edge_set(doc):
    return sorted((e["src"], e["dst"]) for e in doc["edges"])


GROUP = [
    {"op": "select", "ids": ["g1", "g2"]},
    {"op": "chain_save", "name": "Two Gains"},
]
GROUPED_EDGES = [("src", "two_gains"), ("two_gains", "dst")]
FLAT_EDGES = [("g1", "g2"), ("g2", "dst"), ("src", "g1")]


@skip_if_no_desktop
def test_save_selection_adds_ports_and_writes_the_library(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    results, saved = _run(tmp_path, proj, GROUP)
    assert results[1]["ok"] and results[1]["id"] == "two_gains"
    chain = _chain_nodes(saved)["two_gains"]
    assert chain["source"] == "Two Gains"
    ports = {n["id"]: n for n in chain["nodes"] if n["kind"].startswith("audio_")}
    # Channel counts follow the nodes the ports connect.
    assert ports == {
        "inlet": _port("inlet", "in", channels=1),
        "outlet": _port("outlet", "out", channels=1),
    }
    assert sorted((e["src"], e["dst"]) for e in chain["edges"]) == [
        ("g1", "g2"),
        ("g2", "outlet"),
        ("inlet", "g1"),
    ]
    assert _edge_set(saved) == GROUPED_EDGES
    lay = saved["layout"]
    # The box takes the selection's corner; the ports flank the nodes.
    assert lay["two_gains"] == {
        k: min(lay[f"two_gains/{n}"][k] for n in ("g1", "g2")) for k in ("x", "y")
    }
    assert lay["two_gains/inlet"]["x"] < lay["two_gains/g1"]["x"]
    assert lay["two_gains/outlet"]["x"] > lay["two_gains/g2"]["x"]
    lib = chain_library.read("Two Gains")
    assert len(lib["nodes"]) == 4
    corner = [min(p[k] for p in lib["layout"].values()) for k in ("x", "y")]
    assert corner == [0, 0]


@skip_if_no_desktop
def test_fan_out_and_midi_get_one_port_per_source(tmp_path, monkeypatch):
    proj = _canvas(
        tmp_path,
        monkeypatch,
        [
            {"id": "src", "kind": "gain", "channels": 2},
            {"id": "a", "kind": "gain", "channels": 2},
            {"id": "b", "kind": "gain", "channels": 2},
            {"id": "m", "kind": "mix", "channels": 2, "num_inputs": 2},
            {"id": "seq", "kind": "sequencer"},
            {"id": "t", "kind": "midi_transpose", "semitones": 2},
        ],
        [
            {"src": "src", "dst": "a"},
            {"src": "src", "dst": "b"},
            {"src": "a", "dst": "m", "dst_port": 0},
            {"src": "b", "dst": "m", "dst_port": 1},
            {"src": "seq", "dst": "t", "kind": "midi"},
        ],
    )
    _, saved = _run(
        tmp_path,
        proj,
        [{"op": "select", "ids": ["a", "b", "t"]}, {"op": "chain_save", "name": "Fan"}],
    )
    chain = _chain_nodes(saved)["fan"]
    kinds = [n["kind"] for n in chain["nodes"]]
    # One audio port for src's two edges, one per inside source going out.
    assert kinds.count("audio_inlet") == 1
    assert kinds.count("audio_outlet") == 2
    assert kinds.count("midi_inlet") == 1
    assert {"src": "seq", "dst": "fan", "kind": "midi"} in saved["edges"]
    # Each outlet is addressed by its index.
    to_m = sorted(
        (e.get("src_port", 0), e["dst_port"]) for e in saved["edges"] if e["dst"] == "m"
    )
    assert to_m == [(0, 0), (1, 1)]


@skip_if_no_desktop
def test_group_is_one_undo_step(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _, saved = _run(tmp_path, proj, [*GROUP, {"op": "undo"}])
    assert not _chain_nodes(saved)
    assert _edge_set(saved) == FLAT_EDGES


@skip_if_no_desktop
def test_ungroup_removes_the_ports_and_joins_the_edges(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _, saved = _run(
        tmp_path, proj, [*GROUP, {"op": "chain_ungroup", "chain": "two_gains"}]
    )
    assert not _chain_nodes(saved)
    assert not any(n["kind"].startswith("audio_") for n in saved["nodes"])
    assert _edge_set(saved) == FLAT_EDGES


@skip_if_no_desktop
def test_insert_copies_the_library_chain(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _run(tmp_path, proj, GROUP)
    results, saved = _run(
        tmp_path,
        proj,
        [
            {"op": "chain_insert", "name": "Two Gains", "x": 400, "y": 300},
            {"op": "chain_insert", "name": "Two Gains", "x": 400, "y": 500},
        ],
    )
    assert [r["id"] for r in results] == ["two_gains", "two_gains_2"]
    chains = _chain_nodes(saved)
    assert chains["two_gains_2"]["nodes"] == chains["two_gains"]["nodes"]
    pos = [saved["layout"][f"two_gains_2/{n}"] for n in ("inlet", "g1", "g2", "outlet")]
    assert min(p["x"] for p in pos) == 400 and min(p["y"] for p in pos) == 500

    # The project keeps its copy when the library changes.
    lib = chain_library.read("Two Gains")
    next(n for n in lib["nodes"] if n["id"] == "g1")["gain"] = 0.75
    chain_library.write("Two Gains", lib)
    _, resaved = _run(tmp_path, tmp_path / "saved.json", [])
    assert _chain_nodes(resaved)["two_gains"]["nodes"] == chains["two_gains"]["nodes"]


@skip_if_no_desktop
def test_update_from_library_keeps_boundary_edges(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _run(tmp_path, proj, GROUP)
    lib = chain_library.read("Two Gains")
    next(n for n in lib["nodes"] if n["id"] == "g1")["gain"] = 0.75
    chain_library.write("Two Gains", lib)
    results, saved = _run(
        tmp_path,
        tmp_path / "saved.json",
        [{"op": "chain_update", "chain": "two_gains"}],
    )
    assert results[0]["ok"], results
    inner = {n["id"]: n for n in _chain_nodes(saved)["two_gains"]["nodes"]}
    assert inner["g1"]["gain"] == 0.75
    assert _edge_set(saved) == GROUPED_EDGES


@skip_if_no_desktop
def test_update_drops_edges_to_removed_ports(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _run(tmp_path, proj, GROUP)
    lib = chain_library.read("Two Gains")
    lib["nodes"] = [n for n in lib["nodes"] if n["id"] != "outlet"]
    lib["edges"] = [e for e in lib["edges"] if e["dst"] != "outlet"]
    del lib["layout"]["outlet"]
    chain_library.write("Two Gains", lib)
    _, saved = _run(
        tmp_path,
        tmp_path / "saved.json",
        [{"op": "chain_update", "chain": "two_gains"}],
    )
    assert _edge_set(saved) == [("src", "two_gains")]
    assert "two_gains/outlet" not in saved["layout"]


@skip_if_no_desktop
def test_a_chain_is_one_box_with_a_pin_per_port(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    results, _ = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "chains"},
            {"op": "bounds"},
            {"op": "open_chain", "chain": "two_gains"},
            {"op": "bounds"},
            {"op": "chains"},
            {"op": "close_chain"},
            {"op": "chains"},
        ],
    )
    (info,) = results[2]["chains"]
    assert sorted(info["members"]) == [
        f"two_gains/{n}" for n in ("g1", "g2", "inlet", "outlet")
    ]
    assert results[2]["library"] == ["Two Gains"]
    assert info["inlets"] == ["two_gains/inlet"] and info["outlets"] == [
        "two_gains/outlet"
    ]
    # The top shows the box, not its nodes; the open chain shows its nodes.
    assert sorted(results[3]["bounds"]) == ["dst", "spk", "src", "two_gains"]
    assert sorted(results[5]["bounds"]) == sorted(info["members"])
    assert results[6]["open"] == "two_gains" and results[8]["open"] == ""

    x, y, w, h = info["box"]
    _, saved = _run(
        tmp_path,
        tmp_path / "saved.json",
        [
            {"op": "fit"},
            {"op": "click", "x": x + w / 2, "y": y + h / 2},
            {"op": "delete_selection"},
        ],
    )
    assert not _chain_nodes(saved)
    assert sorted(n["id"] for n in saved["nodes"]) == ["dst", "spk", "src"]


@skip_if_no_desktop
def test_nodes_added_in_an_open_chain_join_it(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    results, saved = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "open_chain", "chain": "two_gains"},
            {"op": "add", "kind": "gain"},
            {"op": "add", "kind": "audio_inlet"},
            {"op": "add", "kind": "midi_output"},
            {"op": "close_chain"},
            {"op": "chains"},
            # A second source into the new inlet: a sidechain.
            {"op": "add", "kind": "gain"},
        ],
        ok=False,
    )
    inner = {n["id"]: n for n in _chain_nodes(saved)["two_gains"]["nodes"]}
    assert len(inner) == 6 and "inlet_1" in inner
    assert inner["inlet_1"]["kind"] == "audio_inlet"
    assert not any(n["kind"] == "midi_output" for n in saved["nodes"])
    assert "cannot be inside a chain" in " ".join(results[5]["warnings"])
    (info,) = results[7]["chains"]
    assert info["inlets"] == ["two_gains/inlet", "two_gains/inlet_1"]
    side = next(
        n["id"]
        for n in saved["nodes"]
        if n["id"] not in ("src", "dst", "spk") and n["kind"] == "gain"
    )
    _, saved = _run(
        tmp_path,
        tmp_path / "saved.json",
        [{"op": "connect", "src": side, "dst": "two_gains/inlet_1", "port": 0}],
    )
    assert {"src": side, "dst": "two_gains", "dst_port": 1} in saved["edges"]


@skip_if_no_desktop
def test_pins_run_top_to_bottom_and_name_their_port(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _, saved = _run(tmp_path, proj, GROUP)
    y = saved["layout"]["two_gains/inlet"]["y"]
    x = saved["layout"]["two_gains/inlet"]["x"]
    results, saved = _run(
        tmp_path,
        tmp_path / "saved.json",
        [
            {"op": "open_chain", "chain": "two_gains"},
            # Above the existing inlet: it becomes the first pin.
            {"op": "add", "kind": "audio_inlet", "x": x, "y": y - 200},
            # Higher still: pins sort by height alone, MIDI with audio.
            {"op": "add", "kind": "midi_inlet", "x": x, "y": y - 400},
            {"op": "close_chain"},
            {"op": "chains"},
            {"op": "tooltip", "id": "two_gains/inlet_1"},
            {"op": "tooltip", "id": "two_gains/outlet"},
        ],
    )
    (info,) = results[4]["chains"]
    assert info["inlets"] == [
        "two_gains/midi_inlet_1",
        "two_gains/inlet_1",
        "two_gains/inlet",
    ]
    assert results[5]["tooltip"] == "inlet_1 (audio inlet, 2ch)"
    assert results[6]["tooltip"] == "outlet (audio outlet, 1ch)"
    # The file lists ports in pin order, so the index of src's edge moves.
    ports = [
        n["id"]
        for n in _chain_nodes(saved)["two_gains"]["nodes"]
        if n["kind"].endswith("let")
    ]
    assert ports.index("inlet_1") < ports.index("inlet")
    assert {"src": "src", "dst": "two_gains", "dst_port": 1} in saved["edges"]


@skip_if_no_desktop
def test_undo_in_an_open_chain_restores_nodes_where_they_were(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _, saved = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "delete", "id": "dst"},
            {"op": "open_chain", "chain": "two_gains"},
            {"op": "undo"},
        ],
    )
    assert "dst" in {n["id"] for n in saved["nodes"]}
    assert len(_chain_nodes(saved)["two_gains"]["nodes"]) == 4


@skip_if_no_desktop
def test_quick_add_and_drop_insert_chains(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _run(tmp_path, proj, GROUP)
    path = chain_library.library_dir() / "Two Gains.mhchain"
    results, saved = _run(
        tmp_path,
        tmp_path / "saved.json",
        [
            {"op": "quick_add", "query": "chain: two"},
            {"op": "drop", "files": [str(path)], "x": 10, "y": 10},
        ],
    )
    assert all(r["ok"] for r in results), results
    assert sorted(_chain_nodes(saved)) == ["two_gains", "two_gains_2", "two_gains_3"]


@skip_if_no_desktop
def test_project_io_cannot_join_a_chain(tmp_path, monkeypatch):
    proj = _canvas(
        tmp_path,
        monkeypatch,
        [
            {"id": "g", "kind": "gain"},
            {"id": "o", "kind": "output", "channels": 2, "sink": "o.wav"},
        ],
        [],
    )
    results, _ = _run(
        tmp_path,
        proj,
        [
            {"op": "select", "ids": ["g", "o"]},
            {"op": "chain_save", "name": "X"},
            {"op": "chain_update", "chain": "nope"},
        ],
        ok=False,
    )
    assert "cannot be in a chain" in results[1]["error"]
    assert "no library source" in results[2]["error"]
    assert not chain_library.names()


@skip_if_no_desktop
def test_a_port_outside_a_chain_opens_with_a_problem(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch, [_port("p", "in")], [])
    results, _ = _run(tmp_path, proj, [{"op": "problems"}])
    assert "A port works only inside a chain." in results[0]["problems"]["p"]


@skip_if_no_desktop
def test_duplicate_in_an_open_chain_stays_in_the_chain(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _, saved = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "open_chain", "chain": "two_gains"},
            {"op": "select", "ids": ["two_gains/g1"]},
            {"op": "duplicate"},
        ],
    )
    inner = {n["id"] for n in _chain_nodes(saved)["two_gains"]["nodes"]}
    assert inner == {"inlet", "g1", "g2", "outlet", "g_1"}


@skip_if_no_desktop
def test_duplicate_of_a_chain_box_copies_the_chain(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    results, saved = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "duplicate"},
            {"op": "problems"},
            {"op": "copy"},
            {"op": "open_chain", "chain": "two_gains"},
            {"op": "paste"},
        ],
        ok=False,
    )
    chains = _chain_nodes(saved)
    assert sorted(chains) == ["two_gains", "two_gains_2"]
    assert chains["two_gains_2"]["nodes"] == chains["two_gains"]["nodes"]
    # The copy is not wired; an inlet with no cable in is not a problem.
    assert not results[3]["problems"].get("two_gains_2")
    assert "Chains cannot contain chains." in " ".join(results[6]["warnings"])


@skip_if_no_desktop
def test_save_as_chain_dialog_suggests_saves_and_asks_to_replace(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    results, saved = _run(
        tmp_path,
        proj,
        [
            {"op": "select", "ids": ["g1", "g2"]},
            {"op": "save_as_chain"},
            {"op": "save_as_chain", "name": "  Two Gains "},
            {"op": "save_as_chain"},
            {"op": "select", "ids": ["src"]},
            {"op": "save_as_chain", "name": "Two Gains", "replace": False},
            {"op": "chains"},
            {"op": "save_as_chain", "name": "Two Gains", "replace": True},
            {"op": "select", "ids": ["dst"]},
            {"op": "save_as_chain", "name": ""},
        ],
    )
    assert all(r["ok"] for r in results), results
    # A new group has no suggestion; a saved chain suggests its source.
    assert results[1]["shown"] and results[1]["suggested"] == ""
    assert not results[2]["asked_replace"]
    assert results[3]["suggested"] == "Two Gains"
    # A taken name asks first; Cancel saves nothing.
    assert results[5]["asked_replace"]
    assert [c["id"] for c in results[6]["chains"]] == ["two_gains"]
    assert results[7]["asked_replace"]
    assert chain_library.names() == ["Two Gains"]
    assert [n["id"] for n in chain_library.read("Two Gains")["nodes"]] == [
        "src",
        "outlet",
    ]
    assert sorted(_chain_nodes(saved)) == ["two_gains", "two_gains_2"]
    assert any("invalid chain name" in w for w in results[9]["warnings"])


@skip_if_no_desktop
def test_save_as_chain_is_refused_inside_an_open_chain(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    results, saved = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "open_chain", "chain": "two_gains"},
            {"op": "select", "ids": ["two_gains/g1"]},
            {"op": "save_as_chain", "name": "Inner"},
        ],
    )
    assert results[4]["ok"] and not results[4]["shown"]
    assert any("Chains cannot contain chains" in w for w in results[4]["warnings"])
    assert chain_library.names() == ["Two Gains"]
    assert sorted(_chain_nodes(saved)) == ["two_gains"]


@skip_if_no_desktop
def test_chain_menu_lists_and_runs_its_items(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    results, _ = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "select", "ids": []},
            {"op": "chain_menu", "chain": "two_gains"},
            {"op": "chain_menu", "chain": "two_gains", "choose": "Open Chain"},
            {"op": "chains"},
            {"op": "close_chain"},
            {
                "op": "chain_menu",
                "chain": "two_gains",
                "choose": "Save to Library",
                "name": "Copy",
            },
            {"op": "chains"},
        ],
    )
    assert all(r["ok"] for r in results), results
    items = {i["text"].split("  ")[0]: i["enabled"] for i in results[3]["items"]}
    assert items == {
        "Open Chain": True,
        "Save to Library...": True,
        "Update from Library (Two Gains)": True,
        "Ungroup": True,
        "Cut": True,
        "Copy": True,
        "Duplicate": True,
        "Delete": True,
    }
    assert results[5]["open"] == "two_gains"
    assert not results[7]["asked_replace"]
    assert results[8]["chains"][0]["source"] == "Copy"
    assert chain_library.names() == ["Copy", "Two Gains"]

    # Update is disabled once the library file is gone.
    (chain_library.library_dir() / "Copy.mhchain").unlink()
    results, saved = _run(
        tmp_path,
        tmp_path / "saved.json",
        [
            {"op": "chain_menu", "chain": "two_gains"},
            {"op": "chain_menu", "chain": "two_gains", "choose": "Update"},
            {"op": "chain_menu", "chain": "two_gains", "choose": "Ungroup"},
        ],
        ok=False,
    )
    update = next(i for i in results[0]["items"] if i["text"].startswith("Update"))
    assert not update["enabled"]
    assert not results[1]["ok"] and "disabled" in results[1]["error"]
    assert results[2]["ok"]
    assert not _chain_nodes(saved)
    assert _edge_set(saved) == FLAT_EDGES


@skip_if_no_desktop
def test_chain_menu_delete_acts_on_the_box(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    _, saved = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            {"op": "select", "ids": ["src"]},
            {"op": "chain_menu", "chain": "two_gains", "choose": "Delete"},
        ],
    )
    # The right-click selects the box, so src survives.
    assert not _chain_nodes(saved)
    assert sorted(n["id"] for n in saved["nodes"]) == ["dst", "spk", "src"]


@skip_if_no_desktop
def test_escape_cmd_up_and_the_bar_close_an_open_chain(tmp_path, monkeypatch):
    proj = _canvas(tmp_path, monkeypatch)
    open_ = {"op": "open_chain", "chain": "two_gains"}
    chains = {"op": "chains"}
    results, _ = _run(
        tmp_path,
        proj,
        [
            *GROUP,
            open_,
            {"op": "select", "ids": ["two_gains/g1"]},
            {"op": "key", "key": "escape"},
            chains,
            {"op": "key", "key": "escape"},
            chains,
            open_,
            {"op": "key", "key": "command + cursor up"},
            chains,
            open_,
            # Sizes the canvas, as a window does, and paints the bar.
            {"op": "snapshot", "path": "open.png"},
            {"op": "click", "bar": True},
            chains,
            {"op": "select", "ids": []},
            {"op": "click", "bar": True},
        ],
        ok=False,
    )
    assert all(r["ok"] for r in results[:-1]), results
    # The first Escape clears the selection; the second closes the chain.
    assert results[5]["open"] == "two_gains"
    assert results[7]["open"] == ""
    assert results[9]["handled"] and results[10]["open"] == ""
    assert results[14]["open"] == ""
    # At the top there is no bar.
    assert not results[16]["ok"] and "no chain is open" in results[16]["error"]


@skip_if_no_desktop
def test_midi_ports_pass_notes_through(tmp_path):
    # sequencer -> chain(midi_inlet -> transpose -> midi_outlet) -> midi_output
    chain = {
        "nodes": [
            {"id": "i", "kind": "midi_inlet"},
            {"id": "t", "kind": "midi_transpose", "semitones": 5},
            {"id": "o", "kind": "midi_outlet"},
        ],
        "edges": [
            {"src": "i", "dst": "t", "kind": "midi"},
            {"src": "t", "dst": "o", "kind": "midi"},
        ],
    }
    seq = {"id": "seq", "kind": "sequencer", "density": 1.0, "seed": 3}
    logs = {}
    for name, middle, edges in [
        (
            "chain",
            [chain_library.node(chain, "c")],
            [
                {"src": "seq", "dst": "c", "kind": "midi"},
                {"src": "c", "dst": "mo", "kind": "midi"},
            ],
        ),
        (
            "plain",
            [{"id": "t", "kind": "midi_transpose", "semitones": 5}],
            [
                {"src": "seq", "dst": "t", "kind": "midi"},
                {"src": "t", "dst": "mo", "kind": "midi"},
            ],
        ),
    ]:
        proj = _project(
            tmp_path,
            name,
            [seq, *middle, {"id": "mo", "kind": "midi_output"}],
            [{"src": "in", "dst": "out"}, *edges],
        )
        doc = json.loads(proj.read_text())
        doc["duration_seconds"] = 0.5
        proj.write_text(json.dumps(doc))
        minihost.load_project(proj)  # Python accepts it too
        res = _render_desktop(proj, "--render-midi-log")
        assert res.returncode == 0, res.stderr
        logs[name] = proj.with_suffix(".render-midi.txt").read_text()
    assert logs["chain"] and logs["chain"] == logs["plain"]
