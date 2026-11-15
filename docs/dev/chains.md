# Chains: saved, reusable subgraphs

A chain is a named subgraph of processing nodes. It is saved to a library
and inserted into projects. Both renderers (`src/minihost/project.py` and
the desktop app) read it.

Decisions, with the alternative each one rejected:

- **A project embeds a copy.** Inserting a chain copies its definition into
  the project. Editing the library chain later does not change the
  project. A link (`"ref": "name"`) was rejected: a render would change
  when the library changed, and a project would fail to load on a machine
  without the library file.
- **Subgraph, not a list.** A chain holds nodes and edges, so parallel
  routing (a `mix` node, a dry path) fits. `PluginChain` (serial only) is
  not used to run it.
- **Expanded at load.** Each inner node becomes an ordinary graph node with
  the id `<chain id>/<inner id>`. Bypass, automation, latency
  compensation, meters and editors work on the inner nodes unchanged.
  Running the chain as one opaque graph node would have needed a second
  implementation of each.
- **Ports are nodes.** A chain connects to the outside through
  `audio_inlet`, `audio_outlet`, `midi_inlet` and `midi_outlet` nodes:
  pass-throughs, like the inlets and outlets of a Max subpatcher. Four
  kinds were chosen over two kinds with a `direction` field, so the file
  uses the canvas's names and each kind loads as one node type. Project I/O nodes (`input`, `midi_output`,
  ...) hold files or devices and stay out of chains. Port lists on the chain
  node were rejected: a port node is drawn, wired and renamed like any other
  node, and several of them give a chain several inputs (a sidechain) with
  no new syntax.
- **Encapsulated.** An outside edge names the chain and a port index, never
  a node inside it. Full-id edges (`vox/comp`) were rejected: they tie the
  project's edges to the chain's contents, which Update from Library
  replaces. On the canvas a chain is one box; its nodes are edited by
  opening it.

Planned: macro parameters on the chain box, nested chains.

## Library file

One file per chain, `<library>/<name>.mhchain`, JSON:

```json
{
  "minihost_chain_version": 1,
  "name": "Vocal",
  "nodes": [
    {"id": "in", "kind": "audio_inlet", "channels": 2},
    {"id": "comp", "kind": "plugin", "path": "/Library/Audio/Plug-Ins/VST3/Comp.vst3",
     "state_b64": "..."},
    {"id": "verb", "kind": "plugin", "path": "/Library/Audio/Plug-Ins/VST3/Verb.vst3"},
    {"id": "out", "kind": "audio_outlet", "channels": 2}
  ],
  "edges": [{"src": "in", "dst": "comp"}, {"src": "comp", "dst": "verb"},
            {"src": "verb", "dst": "out"}],
  "layout": {"in": {"x": 0, "y": 0}, "comp": {"x": 200, "y": 0}}
}
```

- `nodes`, `edges`: as in a project, with ids local to the chain. Ids
  must not contain `/`. A node may be any kind except the project I/O
  kinds (`input`, `output`, `device_input`, `device_output`,
  `midi_input`, `midi_output`) and `chain`.
- `audio_inlet` and `audio_outlet` (`channels`, default 2) load as a
  one-input bus; `midi_inlet` and `midi_outlet` as a one-input MIDI
  merge. They are valid only inside a chain. An inner edge may not end at
  an inlet or start at an outlet. An inlet takes one cable; a second
  replaces the first. To sum sources, feed a `mix` node outside the
  chain. A port with no cable in passes silence, or no MIDI, as an
  unconnected Max inlet does. It is not reported as a problem.
- `layout` (optional): positions relative to the chain's origin.
- Relative paths resolve against the file's directory. The desktop app
  writes absolute paths.

The library directory is `chains/` beside the desktop settings file:
`$MINIHOST_DESKTOP_SETTINGS_DIR/chains` when that variable is set,
otherwise `~/Library/minihost/chains` (macOS), `~/.config/minihost/chains`
(Linux), `%APPDATA%\minihost\chains` (Windows).
`minihost.chain_library` reads and writes it from Python.

## In a project

A `chain` node holds the same fields. `source` names the library chain it
came from; it is informational only.

```json
{"id": "vox", "kind": "chain", "source": "Vocal", "nodes": [...], "edges": [...]}
```

Edges outside the chain name the chain by its own id:

- an edge to `vox` goes to its `dst_port`-th inlet of the edge's kind
  (audio or MIDI), counted in file order; `dst_port` defaults to 0;
- an edge from `vox` leaves from its `src_port`-th outlet of the edge's
  kind; `src_port` defaults to 0.

The desktop app writes ports in pin order, top to bottom on the canvas,
so file order is the order of the box's pins.

```json
{"src": "vox", "src_port": 1, "dst": "out2", "dst_port": 0}
```

An edge that names a node inside a chain (`vox/comp`) is an error.
Automation lanes and `layout` keys use full ids (`vox/comp`); the `layout`
entry `vox` places the chain's box.

The desktop app keeps inner nodes as ordinary nodes in memory, and edges
to a chain as edges to its port nodes. A node belongs to chain `vox` when
its id starts with `vox/`. On save, those nodes and the edges between
them are written inside the `chain` node, and edges to its ports are
written as chain id and index.

The canvas draws a chain as one box. Its pins are its port nodes: inlets
on the left, outlets on the right, ordered by their port nodes' height
inside the chain, audio and MIDI together (ties keep file order). Max orders
inlets left to right; a box's pins are stacked vertically, so height is
the matching rule. Hovering a pin shows its port's id and kind.
Double-click opens the chain: the canvas then shows its nodes, with the
ports as nodes, and nodes added there join the chain. Properties show
ids without the chain's prefix.

Save Selection as Chain adds the ports: one inlet per outside node that
feeds the selection, one outlet per selected node that feeds the outside.
An audio port takes its source's channel count. Ungroup removes the ports
and joins the edges on either side.

A port left outside a chain (by a rename or a hand edit) still opens in
the desktop app, marked as a problem, but the project does not render.
