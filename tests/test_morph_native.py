"""Tests for the native (C-backed) parameter morphing bindings.

These exercise ``Plugin.morph_capture`` / ``morph_apply`` / ``morph``, which
wrap the libminihost ``mh_morph_*`` C API. They require a real plugin (unlike
the pure-Python ``minihost.morph`` tests, which use mock plugins). The two
surfaces are independent: the C-backed methods run natively in one call; the
``minihost.morph`` module stays pure-Python and duck-typed.
"""

from __future__ import annotations

import math
import os

import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)

SR = 48000


@pytest.fixture
def plugin():
    p = minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512)
    yield p
    p.close()


@skip_if_no_plugin
def test_capture_returns_one_value_per_param(plugin):
    snap = plugin.morph_capture()
    assert isinstance(snap, list)
    assert len(snap) == plugin.num_params


@skip_if_no_plugin
def test_capture_matches_get_param_loop(plugin):
    native = plugin.morph_capture()
    manual = [plugin.get_param(i) for i in range(plugin.num_params)]
    assert native == manual


@skip_if_no_plugin
def test_apply_roundtrips(plugin):
    snap = plugin.morph_capture()
    # Nudge a couple of params, then restore the snapshot.
    if plugin.num_params >= 1:
        plugin.set_param(0, 0.0 if snap[0] > 0.5 else 1.0)
    plugin.morph_apply(snap)
    restored = plugin.morph_capture()
    # Values that were unclamped come back within float tolerance.
    assert all(math.isclose(a, b, abs_tol=1e-4) for a, b in zip(snap, restored))


@skip_if_no_plugin
def test_morph_interpolates_and_applies(plugin):
    if plugin.num_programs < 2:
        pytest.skip("plugin has < 2 factory programs")
    plugin.program = 0
    a = plugin.morph_capture()
    plugin.program = 1
    b = plugin.morph_capture()

    t = 0.25
    result = plugin.morph(a, b, t)
    assert len(result) == len(a)
    # Every returned value is the clamped linear blend.
    for x, y, r in zip(a, b, result):
        expected = min(1.0, max(0.0, x + (y - x) * t))
        assert math.isclose(r, expected, rel_tol=1e-5, abs_tol=1e-6)


@skip_if_no_plugin
def test_morph_t0_is_a_t1_is_b(plugin):
    n = plugin.num_params
    a = [0.2] * n
    b = [0.8] * n
    assert plugin.morph(a, b, 0.0) == pytest.approx(a)
    assert plugin.morph(a, b, 1.0) == pytest.approx(b)


@skip_if_no_plugin
def test_morph_clamps_extrapolation(plugin):
    n = plugin.num_params
    a = [0.4] * n
    b = [0.6] * n
    # t=5 would extrapolate to 1.4 -> clamped to 1.0.
    assert plugin.morph(a, b, 5.0) == pytest.approx([1.0] * n)
    # t=-5 -> -0.6 -> clamped to 0.0.
    assert plugin.morph(a, b, -5.0) == pytest.approx([0.0] * n)


@skip_if_no_plugin
def test_apply_length_mismatch_raises(plugin):
    with pytest.raises(ValueError):
        plugin.morph_apply([0.1])


@skip_if_no_plugin
def test_morph_length_mismatch_raises(plugin):
    with pytest.raises(ValueError):
        plugin.morph([0.1], [0.2], 0.5)


@skip_if_no_plugin
def test_plugin_morph_matches_the_morph_module(plugin):
    """Plugin.morph and minihost.morph.lerp agree on the math.

    Both now route to the same C implementation -- morph.lerp used to
    write the interpolation out again in Python, so this compared two
    implementations; it now pins that the two entry points stay in step.
    """
    from minihost import morph as pymorph

    a = plugin.morph_capture()
    # Build a distinct B by clamping everything toward its opposite.
    b = [1.0 - x for x in a]
    t = 0.3
    native = plugin.morph(a, b, t)  # lerp + apply, returns snapshot
    module = pymorph.lerp(a, b, t)
    assert native == pytest.approx(module)


def test_lerp_results_are_float32_exact():
    """Snapshots come back at parameter precision, not double precision.

    Parameters are float32 on both sides of the C API -- get_param hands
    back a float, set_param takes one -- so the interpolation is done in
    float32 and the result is exactly what the plugin will receive. It is
    not the double-precision value a hand-written Python expression would
    produce; the two differ around 1e-8, well under float32 resolution.
    """
    import struct

    from minihost import morph as pymorph

    def as_float32(x: float) -> float:
        return struct.unpack("f", struct.pack("f", x))[0]

    for a, b, t in (([0.1], [0.2], 0.3), ([0.7], [0.9], 1.0 / 3.0)):
        got = pymorph.lerp(a, b, t)[0]
        assert got == as_float32(got), "result is not representable in float32"
        assert got == pytest.approx(a[0] + (b[0] - a[0]) * t, abs=1e-6)


def test_lerp_accepts_empty_snapshots():
    """A plugin with no parameters morphs to an empty snapshot, not an error."""
    from minihost import morph as pymorph

    assert pymorph.lerp([], [], 0.5) == []
    assert pymorph.lerp([], [], []) == []
