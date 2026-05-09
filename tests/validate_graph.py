"""
Graph executor validation harness.

Three checks against ground-truth references built from the existing
Plugin / PluginChain bindings. Run before trusting the graph scheduler:

    MINIHOST_TEST_PLUGIN=/path/to/some.vst3 \
        uv run python tests/validate_graph.py

Exits non-zero on any mismatch. No pytest dependency so it can also run
as a standalone smoke test from CI.

The graph executor itself (`minihost.graph`) does not exist yet; this
file defines a minimal in-file reference scheduler so the harness can
land before the real module. When the real module is written, swap the
import and delete `_RefGraph`.
"""
from __future__ import annotations

import os
import sys
from dataclasses import dataclass, field

import numpy as np

import minihost

SR = 48000
BLOCK = 512
TOTAL_FRAMES = BLOCK * 8           # 8 blocks; covers cross-block automation
CHANNELS = 2
RTOL = 0.0
ATOL = 0.0                          # demand bit-identity; relax only with reason


# --------------------------------------------------------------------------- #
# Reference scheduler (placeholder for minihost.graph)
# --------------------------------------------------------------------------- #

@dataclass
class _Node:
    id: str
    kind: str                       # "plugin" | "mix" | "gain"
    plugin: object | None = None
    gain: float = 1.0
    in_ports: int = 1
    out_ports: int = 1


@dataclass
class _Edge:
    src: tuple[str, int]
    dst: tuple[str, int]


@dataclass
class _RefGraph:
    sample_rate: int = SR
    block_size: int = BLOCK
    channels: int = CHANNELS
    nodes: dict[str, _Node] = field(default_factory=dict)
    edges: list[_Edge] = field(default_factory=list)
    automation: dict[str, list[tuple[int, int, float]]] = field(default_factory=dict)

    def add(self, node: _Node) -> str:
        self.nodes[node.id] = node
        return node.id

    def connect(self, s_id: str, s_port: int, d_id: str, d_port: int) -> None:
        self.edges.append(_Edge((s_id, s_port), (d_id, d_port)))

    def topo(self) -> list[str]:
        indeg = {n: 0 for n in self.nodes}
        for e in self.edges:
            indeg[e.dst[0]] += 1
        order, q = [], [n for n, d in indeg.items() if d == 0]
        while q:
            n = q.pop(0)
            order.append(n)
            for e in self.edges:
                if e.src[0] == n:
                    indeg[e.dst[0]] -= 1
                    if indeg[e.dst[0]] == 0:
                        q.append(e.dst[0])
        if len(order) != len(self.nodes):
            raise ValueError("cycle in graph")
        return order

    def render(self, source: np.ndarray) -> np.ndarray:
        """Render the whole graph against an input buffer fed to node 'in'."""
        assert source.shape == (self.channels, TOTAL_FRAMES)
        out = np.zeros_like(source)
        order = self.topo()
        # Per-(node,port) buffer for the full duration. Trades memory for
        # simplicity; fine for a validation harness.
        bufs: dict[tuple[str, int], np.ndarray] = {}
        bufs[("in", 0)] = source.copy()

        for t0 in range(0, TOTAL_FRAMES, self.block_size):
            t1 = t0 + self.block_size
            for nid in order:
                node = self.nodes[nid]
                if nid == "in" or nid == "out":
                    continue
                in_buf = self._gather_input(node, bufs, t0, t1)
                out_buf = np.zeros((self.channels, self.block_size), dtype=np.float32)
                if node.kind == "plugin":
                    autos = [
                        (off - t0, p, v)
                        for (off, p, v) in self.automation.get(nid, [])
                        if t0 <= off < t1
                    ]
                    node.plugin.process_auto(in_buf, out_buf, [], autos)
                elif node.kind == "mix":
                    out_buf[:] = in_buf  # _gather_input already summed
                elif node.kind == "gain":
                    out_buf[:] = in_buf * node.gain
                else:
                    raise ValueError(node.kind)
                for p in range(node.out_ports):
                    key = (nid, p)
                    if key not in bufs:
                        bufs[key] = np.zeros((self.channels, TOTAL_FRAMES), dtype=np.float32)
                    bufs[key][:, t0:t1] = out_buf

        # 'out' node: copy whatever feeds its port 0
        for e in self.edges:
            if e.dst == ("out", 0):
                out[:] = bufs[e.src]
        return out

    def _gather_input(self, node: _Node, bufs, t0: int, t1: int) -> np.ndarray:
        """Sum all edges feeding node.in_ports[0] for plugin/gain; for mix,
        sum across all input ports (each port may itself have one edge)."""
        acc = np.zeros((self.channels, self.block_size), dtype=np.float32)
        wanted_ports = range(node.in_ports) if node.kind == "mix" else [0]
        for port in wanted_ports:
            for e in self.edges:
                if e.dst == (node.id, port):
                    acc += bufs[e.src][:, t0:t1]
        return acc


# --------------------------------------------------------------------------- #
# Helpers
# --------------------------------------------------------------------------- #

def _signal() -> np.ndarray:
    rng = np.random.default_rng(0)
    return rng.standard_normal((CHANNELS, TOTAL_FRAMES)).astype(np.float32) * 0.25


def _new_plugin(path: str) -> "minihost.Plugin":
    return minihost.Plugin(path, sample_rate=SR, max_block_size=BLOCK)


def _assert_equal(name: str, a: np.ndarray, b: np.ndarray) -> None:
    if a.shape != b.shape:
        raise AssertionError(f"{name}: shape {a.shape} vs {b.shape}")
    if not np.allclose(a, b, rtol=RTOL, atol=ATOL):
        diff = np.max(np.abs(a - b))
        raise AssertionError(f"{name}: max abs diff = {diff:g}")
    print(f"  ok  {name}")


# --------------------------------------------------------------------------- #
# Checks
# --------------------------------------------------------------------------- #

def check_linear_chain(plugin_path: str) -> None:
    """Graph[in -> p1 -> p2 -> out] == PluginChain([p1, p2])."""
    src = _signal()

    # Reference: PluginChain
    ref_p1 = _new_plugin(plugin_path)
    ref_p2 = _new_plugin(plugin_path)
    chain = minihost.PluginChain([ref_p1, ref_p2])
    ref_out = np.zeros_like(src)
    for t0 in range(0, TOTAL_FRAMES, BLOCK):
        chain.process_auto(src[:, t0:t0+BLOCK],
                           ref_out[:, t0:t0+BLOCK], [], [])

    # Candidate: graph executor
    g = _RefGraph()
    g.add(_Node("in",  "plugin"))   # placeholder source; not actually invoked
    g.add(_Node("p1",  "plugin", plugin=_new_plugin(plugin_path)))
    g.add(_Node("p2",  "plugin", plugin=_new_plugin(plugin_path)))
    g.add(_Node("out", "plugin"))
    g.connect("in", 0, "p1", 0)
    g.connect("p1", 0, "p2", 0)
    g.connect("p2", 0, "out", 0)
    cand_out = g.render(src)

    _assert_equal("linear_chain", ref_out, cand_out)


def check_fanout_mix(plugin_path: str) -> None:
    """Graph[in -> p; p -> mix.0; p -> gain(1.0) -> mix.1; mix -> out]
       == 2 * (single-plugin render)."""
    src = _signal()

    # Reference: render once through plugin, double it
    ref_p = _new_plugin(plugin_path)
    wet = np.zeros_like(src)
    for t0 in range(0, TOTAL_FRAMES, BLOCK):
        ref_p.process_auto(src[:, t0:t0+BLOCK], wet[:, t0:t0+BLOCK], [], [])
    ref_out = wet + wet

    # Candidate: fan-out into mix
    g = _RefGraph()
    g.add(_Node("in",   "plugin"))
    g.add(_Node("p",    "plugin", plugin=_new_plugin(plugin_path)))
    g.add(_Node("unity", "gain", gain=1.0))
    g.add(_Node("mix",  "mix", in_ports=2))
    g.add(_Node("out",  "plugin"))
    g.connect("in",    0, "p",     0)
    g.connect("p",     0, "mix",   0)   # dry-side of fan-out
    g.connect("p",     0, "unity", 0)
    g.connect("unity", 0, "mix",   1)
    g.connect("mix",   0, "out",   0)
    cand_out = g.render(src)

    _assert_equal("fanout_mix", ref_out, cand_out)


def check_automation_slicing(plugin_path: str) -> None:
    """Single-node automation crossing a block boundary should match
    direct Plugin.process_auto with the same absolute-time schedule."""
    src = _signal()

    # Pick any param; if none, skip.
    probe = _new_plugin(plugin_path)
    if probe.num_params == 0:
        print("  skip automation_slicing: plugin has no params")
        return
    p_idx = 0

    # Schedule two changes: one mid-block-2, one mid-block-5
    schedule = [
        (BLOCK * 2 + 17, p_idx, 0.25),
        (BLOCK * 5 + 200, p_idx, 0.75),
    ]

    # Reference: Plugin.process_auto block-by-block, slicing the schedule
    ref_p = _new_plugin(plugin_path)
    ref_out = np.zeros_like(src)
    for t0 in range(0, TOTAL_FRAMES, BLOCK):
        t1 = t0 + BLOCK
        autos = [(off - t0, p, v) for (off, p, v) in schedule if t0 <= off < t1]
        ref_p.process_auto(src[:, t0:t1], ref_out[:, t0:t1], [], autos)

    # Candidate: graph w/ same automation expressed in absolute time
    g = _RefGraph()
    g.add(_Node("in",  "plugin"))
    g.add(_Node("p",   "plugin", plugin=_new_plugin(plugin_path)))
    g.add(_Node("out", "plugin"))
    g.connect("in", 0, "p",   0)
    g.connect("p",  0, "out", 0)
    g.automation["p"] = schedule
    cand_out = g.render(src)

    _assert_equal("automation_slicing", ref_out, cand_out)


# --------------------------------------------------------------------------- #
# Entry
# --------------------------------------------------------------------------- #

def main() -> int:
    path = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not path:
        print("MINIHOST_TEST_PLUGIN not set; nothing to validate.")
        return 0
    print(f"validating graph executor against {path}")
    check_linear_chain(path)
    check_fanout_mix(path)
    check_automation_slicing(path)
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
