# Graph Executor — Design Sketch

Status: design proposal, not implemented. Lives outside the C ABI; intended as a pure-Python module on top of the existing bindings (`Plugin`, `PluginChain`, `audio_io`).

> Note: this doc predates the `AudioBuffer` migration. The pseudocode below uses `np.ndarray` for edge buffers; the v1 implementation should use `minihost.AudioBuffer` (planar float32, JUCE-backed) to keep the buffer pool numpy-optional and consumable by `Plugin.process_auto` directly via DLPack.

## Goal

Run a directed graph of plugin nodes offline (and eventually realtime) with sample-accurate parameter automation, so that a future graph UI has a stable execution model to target.

Non-goals for v1: GUI, realtime audio device callback, plugin sidechain inputs, latency compensation, feedback loops.

## Why Python first

`PluginChain` already covers the linear case in C++. The interesting work in a graph is scheduling, not per-node DSP — every node still bottoms out in `Plugin.process_auto`, which is C++. Prototyping the scheduler in Python keeps the C ABI minimal until the requirements stabilize. If profiling later shows the scheduler is the bottleneck (unlikely at typical block sizes), port to C++ as `MH_Graph`.

## Model

### Nodes

```
Node
  id: str
  kind: "plugin" | "input" | "output" | "gain" | "mix"
  plugin: Plugin | None        # for kind="plugin"
  inputs: list[Port]           # declared channel count per port
  outputs: list[Port]
  params: dict[str, float]     # static; automation handled separately
```

Built-in non-plugin nodes are kept tiny and pure-Python: `input` (graph source, reads from a file or buffer), `output` (graph sink), `gain`, `mix` (sum N inputs). Anything more lives in plugins.

### Edges

```
Edge
  src: (node_id, port_idx)
  dst: (node_id, port_idx)
  channels: int                # must match port declarations
```

One edge per (dst_node, dst_port). Fan-out from a source is allowed; fan-in requires an explicit `mix` node. This keeps semantics unambiguous and removes a class of UI bugs.

### Automation

Reuse the existing `MH_ParamChange` model. Per-node automation is a sorted list of `(sample_offset, param_index, value)` tuples in graph time, sliced per block before dispatch to `process_auto`.

## Execution

### Compilation

`Graph.compile()` produces an immutable `CompiledGraph`:

1. Validate: no cycles (Kahn's algorithm), all edges have matching channel counts, every plugin node has been `prepare`d.
2. Topological sort -> linear `schedule: list[NodeId]`.
3. Allocate buffer pool: one `np.ndarray(shape=(channels, block_size), dtype=f32)` per edge. Reuse via liveness analysis (an edge's buffer is free after its last consumer runs in the schedule) — this matters once graphs get wide; v1 can skip and allocate per-edge.
4. Resolve each node's input/output buffer views into the pool.

Recompilation is required on any topology change. Parameter and automation changes do not recompile.

### Per-block step

```python
def process_block(cg: CompiledGraph, block_size: int, t0: int) -> None:
    for node_id in cg.schedule:
        node = cg.nodes[node_id]
        in_bufs  = cg.inputs_for(node_id)    # list[np.ndarray]
        out_bufs = cg.outputs_for(node_id)
        autos    = cg.slice_automation(node_id, t0, block_size)

        if node.kind == "plugin":
            node.plugin.process_auto(
                stack(in_bufs), stack(out_bufs),
                midi_in=[], automation=autos,
                transport=cg.transport_at(t0),
            )
        else:
            BUILTINS[node.kind](in_bufs, out_bufs, node.params)
```

Offline render is just a loop over `process_block` advancing `t0` by `block_size`.

### Block size

Fixed per compile (default 512). The graph's block size is independent of any plugin's internal block size — `process_auto` already handles re-blocking internally.

## API sketch

```python
from minihost.graph import Graph, Node

g = Graph(sample_rate=48000, block_size=512)
src  = g.add(Node.input("in", channels=2, source="kick.wav"))
comp = g.add(Node.plugin("comp", Plugin("OTT.vst3")))
rev  = g.add(Node.plugin("rev",  Plugin("Valhalla.vst3")))
mix  = g.add(Node.mix("mix", inputs=2, channels=2))
out  = g.add(Node.output("out", channels=2, sink="bounce.wav"))

g.connect(src, 0, comp, 0)
g.connect(comp, 0, rev, 0)
g.connect(comp, 0, mix, 0)   # fan-out: dry
g.connect(rev,  0, mix, 1)   # wet
g.connect(mix,  0, out, 0)

g.automate("comp", param="Threshold", points=[(0.0, -20.0), (5.0, -8.0)])

cg = g.compile()
cg.render(duration_seconds=10.0)
```

## Open questions

- **Latency compensation (PDC).** Plugins report latency via `mh_get_latency`; correct PDC requires per-path delay insertion at fan-in points. Defer to v2; document that mixed wet/dry paths will phase until then.
- **Feedback loops.** Require a one-block delay node on the back-edge to break the cycle. v2.
- **Sidechain inputs.** Plugins with multiple input buses need port metadata from JUCE we don't currently expose. Track as a C ABI gap, not a graph problem.
- **MIDI routing.** Same model as audio edges, separate buffer type. Straightforward but adds a second scheduling lane; defer to v1.5.
- **Realtime mode.** Same scheduler driven from an `AudioDevice` callback. The Python GIL is the obvious risk; if it bites, the scheduler is the right thing to port to C++.

## Validation plan

Before committing to the design, prove out:

1. Linear chain through the graph executor matches `PluginChain.process_auto` sample-for-sample on a fixed input.
2. Fan-out + `mix` produces bit-identical output to manually summing two `PluginChain` renders.
3. Automation slicing across block boundaries matches single-node `process_auto` behavior.

If those three hold, the scheduler is correct enough to build a UI against.
