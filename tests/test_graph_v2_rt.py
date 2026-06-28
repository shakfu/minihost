"""Steady-state behavioural tests for `mh_graph_render_block`.

The C-side render_block is required to be allocation-free after compile
(it uses a per-node buffer pool sized at compile time). We can't trap
mallocs in a portable way from Python, but we can pressure the path by
running many iterations at varying nframes and asserting stability +
correctness. Pre-fix paths that allocated per block would still pass
these tests, but they catch the bugs that allocation refactors tend to
leak (stale state, off-by-one in scratch reuse, dropped frames).

Pairs with `tests/test_rt_allocations.py` (mh_process / mh_chain_*).
"""

from __future__ import annotations

import numpy as np

import minihost


def _build_mix_graph(max_block: int = 512, sr: float = 48000.0):
    g = minihost.PluginGraph(max_block, sr)
    a   = g.add_input(2)
    b   = g.add_input(2)
    mix = g.add_mix(2, 2)
    out = g.add_output(2)
    g.connect(a,   mix, dst_port=0)
    g.connect(b,   mix, dst_port=1)
    g.connect(mix, out)
    g.set_mix_gain(mix, 0, 0.5)
    g.set_mix_gain(mix, 1, 0.5)
    g.compile()
    return g


def test_render_block_repeated_calls_stable():
    """Two-input mix at fixed nframes: 500 iterations must produce the
    same output every time given the same input."""
    F = 256
    g = _build_mix_graph(max_block=F)
    a = np.full((2, F), 1.0, dtype=np.float32)
    b = np.full((2, F), 3.0, dtype=np.float32)
    out = np.zeros((2, F), dtype=np.float32)

    for _ in range(500):
        g.render_block([a, b], [out], F)
        # 0.5 * 1.0 + 0.5 * 3.0 = 2.0 per sample
        assert np.allclose(out, 2.0, atol=1e-6)


def test_render_block_varying_nframes():
    """The graph is compiled for max_block=512 and must handle any
    smaller nframes without re-allocating or dropping samples."""
    g = _build_mix_graph(max_block=512)
    sizes = [1, 17, 64, 200, 512, 33, 5, 256]
    for n in sizes:
        a = np.full((2, n), 0.25, dtype=np.float32)
        b = np.full((2, n), 0.75, dtype=np.float32)
        out = np.zeros((2, n), dtype=np.float32)
        g.render_block([a, b], [out], n)
        # 0.5*0.25 + 0.5*0.75 = 0.5
        assert np.allclose(out[:, :n], 0.5, atol=1e-6)


def test_render_block_inputs_not_mutated():
    """The render path reads inputs into the pool; it must not write
    to caller buffers."""
    F = 128
    g = _build_mix_graph(max_block=F)
    rng = np.random.default_rng(7)
    a = rng.standard_normal((2, F)).astype(np.float32)
    b = rng.standard_normal((2, F)).astype(np.float32)
    a_copy = a.copy()
    b_copy = b.copy()
    out = np.zeros((2, F), dtype=np.float32)

    for _ in range(50):
        g.render_block([a, b], [out], F)
    assert np.array_equal(a, a_copy), "render_block mutated input a"
    assert np.array_equal(b, b_copy), "render_block mutated input b"


def test_render_block_buffer_reuse_safe():
    """A caller that reuses the *same* output array across iterations
    sees the new block's data each time -- no stale contents."""
    F = 64
    g = _build_mix_graph(max_block=F)
    a = np.zeros((2, F), dtype=np.float32)
    b = np.zeros((2, F), dtype=np.float32)
    out = np.zeros((2, F), dtype=np.float32)

    a.fill(2.0); b.fill(4.0)
    g.render_block([a, b], [out], F)
    assert np.allclose(out, 3.0)        # 0.5*2 + 0.5*4

    a.fill(-1.0); b.fill(-3.0)
    g.render_block([a, b], [out], F)
    assert np.allclose(out, -2.0)       # confirms out was overwritten


def test_set_mix_gain_takes_effect_per_block_without_realloc():
    """Gain changes between blocks must not require recompile and must
    not leak state from the previous block."""
    F = 32
    g = _build_mix_graph(max_block=F)
    a = np.full((2, F), 1.0, dtype=np.float32)
    b = np.full((2, F), 1.0, dtype=np.float32)
    out = np.zeros((2, F), dtype=np.float32)

    for k, expected in enumerate([0.5, 1.0, 2.0, 0.0, 4.0]):
        g.set_mix_gain(0 + 2, 0, expected)    # mix is node id 2 (a, b, mix, out)
        g.set_mix_gain(0 + 2, 1, 0.0)
        g.render_block([a, b], [out], F)
        assert np.allclose(out, expected), \
            f"iter {k}: expected {expected}, got {out[0, 0]}"
