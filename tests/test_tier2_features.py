"""Tests for the Tier 2 feature batch:

- MIDI_OUT_CAPACITY module constant
- AudioBuffer.channel_view zero-copy channel-range slicing
- minihost.morph preset morphing utilities
- honest channel counts (a synth reports 0 input channels)

The constant / channel_view / morph tests need no real plugin. The honest
channel-count assertions are gated behind MINIHOST_TEST_PLUGIN and expect a
synthesizer (0 audio inputs) such as the default Dexed.
"""

import os

import pytest

import minihost
from minihost import morph
from minihost._core import AudioBuffer


# ---------------------------------------------------------------------------
# MIDI_OUT_CAPACITY constant
# ---------------------------------------------------------------------------


def test_midi_out_capacity_constant_exported():
    assert isinstance(minihost.MIDI_OUT_CAPACITY, int)
    assert minihost.MIDI_OUT_CAPACITY == 256
    # Re-exported from the extension module too.
    assert minihost._core.MIDI_OUT_CAPACITY == minihost.MIDI_OUT_CAPACITY


# ---------------------------------------------------------------------------
# channel_view zero-copy channel-range slicing
# ---------------------------------------------------------------------------


def _fill(buf):
    a = buf.as_ndarray()
    for ch in range(buf.channels):
        for fr in range(buf.frames):
            a[ch, fr] = ch * 100 + fr


def test_channel_view_shape_and_values():
    buf = AudioBuffer(4, 8)
    _fill(buf)
    view = buf.channel_view(1, 2)
    assert view.channels == 2
    assert view.frames == 8
    # view channel 0 is parent channel 1, view channel 1 is parent channel 2.
    assert view[0, 0] == 100.0
    assert view[1, 5] == 205.0


def test_channel_view_is_zero_copy_bidirectional():
    buf = AudioBuffer(4, 8)
    _fill(buf)
    view = buf.channel_view(1, 2)
    # Write through the view -> visible in the parent.
    view[0, 3] = -7.0
    assert buf[1, 3] == -7.0
    # Write through the parent -> visible in the view.
    buf[2, 6] = 42.0
    assert view[1, 6] == 42.0
    # Same underlying memory pointer for the aliased range.
    assert (
        view.as_ndarray().__array_interface__["data"][0]
        == buf.channel_view(1, 2).as_ndarray().__array_interface__["data"][0]
    )


def test_channel_view_full_range():
    buf = AudioBuffer(3, 4)
    _fill(buf)
    view = buf.channel_view(0, 3)
    assert view.channels == 3 and view.frames == 4
    view[2, 1] = 99.0
    assert buf[2, 1] == 99.0


def test_channel_view_of_view():
    buf = AudioBuffer(5, 4)
    _fill(buf)
    mid = buf.channel_view(1, 3)  # parent channels 1,2,3
    inner = mid.channel_view(1, 1)  # parent channel 2
    assert inner.channels == 1
    inner[0, 0] = 55.0
    assert buf[2, 0] == 55.0  # transitive aliasing back to the root


@pytest.mark.parametrize("start,count", [(-1, 1), (0, 0), (3, 2), (4, 1), (0, 5)])
def test_channel_view_out_of_bounds_raises(start, count):
    buf = AudioBuffer(4, 8)
    with pytest.raises(ValueError):
        buf.channel_view(start, count)


def test_channel_view_keeps_parent_alive():
    # Drop the only Python reference to the parent; the view must keep the
    # backing memory alive (nanobind keep_alive), not read freed memory.
    view = AudioBuffer(4, 8).channel_view(0, 2)
    import gc

    gc.collect()
    view[0, 0] = 3.0
    assert view[0, 0] == 3.0


# ---------------------------------------------------------------------------
# preset morphing
# ---------------------------------------------------------------------------


class _StubPlugin:
    """Minimal Plugin-like object exposing the param API morph needs."""

    def __init__(self, values):
        self._v = list(values)

    @property
    def num_params(self):
        return len(self._v)

    def get_param(self, i):
        return self._v[i]

    def set_param(self, i, v):
        self._v[i] = v


def test_capture_returns_all_params():
    p = _StubPlugin([0.1, 0.2, 0.3])
    assert morph.capture(p) == [0.1, 0.2, 0.3]


def test_apply_sets_and_clamps():
    p = _StubPlugin([0.0, 0.0])
    morph.apply(p, [1.5, -0.5])
    assert p._v == [1.0, 0.0]  # clamped into [0, 1]


def test_apply_length_mismatch_raises():
    p = _StubPlugin([0.0, 0.0])
    with pytest.raises(ValueError):
        morph.apply(p, [0.1])


def test_lerp_scalar_endpoints_and_midpoint():
    a = [0.0, 1.0, 0.2]
    b = [1.0, 0.0, 0.8]
    assert morph.lerp(a, b, 0.0) == a
    assert morph.lerp(a, b, 1.0) == b
    assert morph.lerp(a, b, 0.5) == [0.5, 0.5, 0.5]


def test_lerp_per_parameter_blend():
    a = [0.0, 0.0, 0.0]
    b = [1.0, 1.0, 1.0]
    assert morph.lerp(a, b, [0.0, 0.5, 1.0]) == [0.0, 0.5, 1.0]


def test_lerp_clamps_extrapolation():
    assert morph.lerp([0.5], [1.0], 5.0) == [1.0]
    assert morph.lerp([0.5], [1.0], -5.0) == [0.0]


def test_lerp_length_mismatch_raises():
    with pytest.raises(ValueError):
        morph.lerp([0.0, 0.0], [0.0], 0.5)
    with pytest.raises(ValueError):
        morph.lerp([0.0, 0.0], [1.0, 1.0], [0.5])  # per-param length mismatch


def test_morph_applies_and_returns_snapshot():
    p = _StubPlugin([0.0, 0.0])
    result = morph.morph(p, [0.0, 0.0], [1.0, 1.0], 0.25)
    assert result == [0.25, 0.25]
    assert p._v == [0.25, 0.25]


def test_public_aliases_exposed():
    assert minihost.capture_params is morph.capture
    assert minihost.lerp_params is morph.lerp
    assert minihost.morph_params is morph.morph
    assert minihost.apply_params is morph.apply


# ---------------------------------------------------------------------------
# honest channel counts (synth reports 0 audio inputs)
# ---------------------------------------------------------------------------

_PLUGIN = os.environ.get("MINIHOST_TEST_PLUGIN")


@pytest.mark.skipif(not _PLUGIN, reason="MINIHOST_TEST_PLUGIN not set")
def test_synth_reports_zero_inputs_and_still_renders():
    p = minihost.Plugin(_PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        if p.num_input_channels != 0:
            pytest.skip("configured plugin is not a 0-input synth")
        # Honest reporting: a synth has no audio inputs but real outputs.
        assert p.num_output_channels > 0
        # Synth-mode render still works despite 0 reported inputs.
        out = minihost.process_audio(
            p,
            None,
            midi=[(0, 0x90, 60, 100), (24000, 0x80, 60, 0)],
            tail_seconds=0.0,
            compensate_latency=False,
        )
        assert out.channels == p.num_output_channels
    finally:
        p.close()
