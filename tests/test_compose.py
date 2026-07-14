"""Tests for the callable composition layer (minihost.compose).

The pure-python transforms and stochastic combinators are exercised
without a plugin (numpy only). Tests that need a native processor are
gated behind ``skip_if_no_plugin`` and use the default test plugin.
"""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost
from minihost import (
    AddGaussianNoise,
    Compose,
    Fade,
    Gain,
    Maybe,
    Normalize,
    OneOf,
    RandomParam,
    SomeOf,
    Trim,
)

PLUGIN = os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)

SR = 48000


def _identity(audio, sample_rate):
    return audio


def _const(channels=2, frames=100, value=0.5):
    return (np.ones((channels, frames), dtype=np.float32) * value)


# =====================================================================
# Phase 1 -- Compose core: callable protocol, sr, tails, lifetime
# =====================================================================


def test_call_ndarray_in_ndarray_out():
    fx = Compose([Gain(-6.0)])
    out = fx(_const(2, 100), sample_rate=SR)
    assert isinstance(out, np.ndarray)
    assert out.shape == (2, 100)


def test_call_1d_roundtrips_to_1d():
    fx = Compose([_identity])
    out = fx(np.ones(64, dtype=np.float32), sample_rate=SR)
    assert isinstance(out, np.ndarray)
    assert out.ndim == 1 and out.shape == (64,)


def test_call_audiobuffer_in_audiobuffer_out():
    fx = Compose([_identity])
    buf = minihost.AudioBuffer.from_numpy(_const(2, 100))
    out = fx(buf, sample_rate=SR)
    assert isinstance(out, minihost.AudioBuffer)
    assert out.channels == 2 and out.frames == 100


def test_call_does_not_mutate_input_audiobuffer():
    fx = Compose([Gain(-120.0)])
    buf = minihost.AudioBuffer.from_numpy(_const(1, 10, 1.0))
    fx(buf, sample_rate=SR)
    assert float(buf.magnitude()) == pytest.approx(1.0)


def test_missing_sample_rate_raises_without_processor():
    fx = Compose([Gain(-6.0)])
    with pytest.raises(ValueError, match="sample_rate is required"):
        fx(_const(2, 10))


def test_none_input_raises():
    fx = Compose([_identity])
    with pytest.raises(ValueError, match="requires input"):
        fx(None, sample_rate=SR)


def test_unknown_transform_type_raises():
    fx = Compose([12345])  # not callable / not a processor
    with pytest.raises(TypeError, match="not a Plugin"):
        fx(_const(1, 10), sample_rate=SR)


def test_tail_seconds_numeric_pads_length():
    fx = Compose([_identity], tail_seconds=0.5)
    out = fx(np.ones((1, 100), dtype=np.float32), sample_rate=1000)
    assert out.shape == (1, 100 + 500)


def test_tail_seconds_negative_raises():
    fx = Compose([_identity], tail_seconds=-1.0)
    with pytest.raises(ValueError, match="tail_seconds must be >= 0"):
        fx(_const(1, 10), sample_rate=SR)


def test_tail_seconds_bad_string_raises():
    fx = Compose([_identity], tail_seconds="loud")
    with pytest.raises(ValueError, match="must be 'auto'"):
        fx(_const(1, 10), sample_rate=SR)


def test_tail_auto_trims_padding_back_to_content():
    fx = Compose([_identity], tail_seconds="auto", max_tail_seconds=0.05)
    out = fx(np.ones((1, 100), dtype=np.float32), sample_rate=1000)
    # 50 frames of pad added then trimmed away; content keeps 100.
    assert out.shape == (1, 100)


def test_tail_auto_keeps_ringing_content():
    # A decaying signal inside the pad region is preserved above threshold.
    x = np.ones((1, 100), dtype=np.float32)

    def leak(audio, sr):
        # Write a spike at frame 120 (inside the padded tail region).
        data = audio.as_ndarray().copy()
        if data.shape[1] > 120:
            data[0, 120] = 1.0
        return minihost.AudioBuffer.from_numpy(data)

    fx = Compose([leak], tail_seconds="auto", max_tail_seconds=0.2)
    out = fx(x, sample_rate=1000)
    assert out.shape[1] >= 121


def test_len_and_repr():
    fx = Compose([Gain(0.0), Normalize(-1.0)])
    assert len(fx) == 2
    assert "Compose" in repr(fx)


# =====================================================================
# Phase 2 -- pure-python transforms
# =====================================================================


def test_gain_halves_amplitude_at_minus6db():
    out = Gain(-6.0)(minihost.AudioBuffer.from_numpy(_const(1, 10, 1.0)), SR)
    assert float(out.magnitude()) == pytest.approx(0.5011872, rel=1e-4)


def test_gain_returns_new_buffer():
    src = minihost.AudioBuffer.from_numpy(_const(1, 10, 1.0))
    Gain(-6.0)(src, SR)
    assert float(src.magnitude()) == pytest.approx(1.0)  # unchanged


def test_normalize_brings_peak_to_target():
    out = Normalize(-6.0)(
        minihost.AudioBuffer.from_numpy(_const(1, 10, 0.1)), SR
    )
    assert float(out.magnitude()) == pytest.approx(10.0 ** (-6.0 / 20.0), rel=1e-4)


def test_normalize_passes_silence_through():
    out = Normalize(-1.0)(
        minihost.AudioBuffer.from_numpy(np.zeros((1, 10), dtype=np.float32)), SR
    )
    assert float(out.magnitude()) == 0.0


def test_trim_window():
    src = minihost.AudioBuffer.from_numpy(np.ones((1, 1000), dtype=np.float32))
    out = Trim(start=0.1, duration=0.2)(src, 1000)  # frames 100..300
    assert out.frames == 200


def test_trim_full_passthrough():
    src = minihost.AudioBuffer.from_numpy(np.ones((1, 500), dtype=np.float32))
    out = Trim()(src, 1000)
    assert out.frames == 500


def test_fade_zeroes_edges():
    src = minihost.AudioBuffer.from_numpy(np.ones((1, 1000), dtype=np.float32))
    out = Fade(fade_in=0.1, fade_out=0.1)(src, 1000).as_ndarray()
    assert out[0, 0] == pytest.approx(0.0, abs=1e-6)
    assert out[0, -1] == pytest.approx(0.0, abs=1e-3)
    assert out[0, 500] == pytest.approx(1.0)  # middle untouched


def test_transforms_compose_in_order():
    # Gain(+6) then Normalize(-6) -> Normalize wins regardless of gain.
    fx = Compose([Gain(6.0), Normalize(-6.0)])
    out = fx(_const(1, 10, 0.01), sample_rate=SR)
    assert float(np.max(np.abs(out))) == pytest.approx(
        10.0 ** (-6.0 / 20.0), rel=1e-4
    )


# =====================================================================
# Phase 3 -- stochastic combinators
# =====================================================================


def test_maybe_p1_always_applies():
    fx = Compose([Maybe(Gain(-120.0), p=1.0)])
    out = fx(_const(1, 10, 1.0), sample_rate=SR)
    assert float(np.max(np.abs(out))) < 1e-4


def test_maybe_p0_never_applies():
    fx = Compose([Maybe(Gain(-120.0), p=0.0)])
    out = fx(_const(1, 10, 1.0), sample_rate=SR)
    assert float(np.max(np.abs(out))) == pytest.approx(1.0)


def test_maybe_rejects_bad_probability():
    with pytest.raises(ValueError, match=r"p must be in"):
        Maybe(Gain(0.0), p=1.5)


def test_oneof_is_deterministic_with_seed():
    def build():
        return Compose(
            [OneOf([Gain(-120.0), Gain(0.0)])], seed=0
        )(_const(1, 10, 1.0), sample_rate=SR)

    assert np.array_equal(build(), build())


def test_oneof_weight_forces_choice():
    # All weight on the loud-cut branch.
    fx = Compose(
        [OneOf([Gain(-120.0), Gain(0.0)], weights=[1.0, 0.0])], seed=1
    )
    out = fx(_const(1, 10, 1.0), sample_rate=SR)
    assert float(np.max(np.abs(out))) < 1e-4


def test_oneof_empty_raises():
    with pytest.raises(ValueError, match="at least one"):
        OneOf([])


def test_someof_fixed_count_applies_k_in_order():
    # Two of three gains; result is deterministic by seed. Verify count by
    # comparing to applying a known subset would be brittle, so instead
    # assert the output is a pure power-of-attenuation combination.
    fx = Compose(
        [SomeOf(2, [Gain(-6.0), Gain(-6.0), Gain(-6.0)])], seed=0
    )
    out = fx(_const(1, 10, 1.0), sample_rate=SR)
    # Exactly two -6 dB cuts -> -12 dB total.
    assert float(np.max(np.abs(out))) == pytest.approx(
        10.0 ** (-12.0 / 20.0), rel=1e-4
    )


def test_someof_range_count():
    fx = Compose([SomeOf((0, 3), [Gain(-6.0)] * 3)], seed=3)
    out = fx(_const(1, 10, 1.0), sample_rate=SR)
    # Result is 1.0 * 10**(-6k/20) for some 0<=k<=3 -> a valid attenuation.
    peak = float(np.max(np.abs(out)))
    valid = {10.0 ** (-6.0 * k / 20.0) for k in range(4)}
    assert any(peak == pytest.approx(v, rel=1e-4) for v in valid)


def test_someof_out_of_range_raises():
    with pytest.raises(ValueError, match="out of bounds"):
        SomeOf(5, [Gain(0.0)])


def test_shuffle_is_deterministic_with_seed():
    def build():
        return Compose(
            [Gain(-6.0), Trim(0.0, 0.005)], shuffle=True, seed=7
        )(np.ones((1, 100), dtype=np.float32), sample_rate=1000)

    a, b = build(), build()
    assert a.shape == b.shape and np.array_equal(a, b)


def test_add_gaussian_noise_amplitude_and_reproducibility():
    def build():
        return Compose([AddGaussianNoise(0.02, 0.02)], seed=42)(
            np.zeros((1, 4000), dtype=np.float32), sample_rate=SR
        )

    a, b = build(), build()
    assert np.array_equal(a, b)
    assert a.std() == pytest.approx(0.02, rel=0.1)


def test_pipeline_rng_advances_across_calls():
    # Same pipeline instance: two successive calls should differ because
    # the RNG state advances (not reseeded per call).
    fx = Compose([AddGaussianNoise(0.02, 0.02)], seed=42)
    a = fx(np.zeros((1, 4000), dtype=np.float32), sample_rate=SR)
    b = fx(np.zeros((1, 4000), dtype=np.float32), sample_rate=SR)
    assert not np.array_equal(a, b)


# =====================================================================
# Native-processor integration (needs a real plugin)
# =====================================================================


@skip_if_no_plugin
def test_compose_infers_sample_rate_from_plugin():
    with minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512) as p:
        fx = Compose([p, Normalize(-3.0)], close_children=False)
        assert fx.sample_rate == SR


@skip_if_no_plugin
def test_compose_processes_through_plugin_length_preserving():
    with minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512) as p:
        fx = Compose([p, Gain(-3.0)], close_children=False)
        buf = minihost.AudioBuffer(max(p.num_input_channels, 2), 4800)
        out = fx(buf)
        assert out.channels == p.num_output_channels
        assert out.frames == 4800


@skip_if_no_plugin
def test_compose_sample_rate_mismatch_raises():
    with minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512) as p:
        fx = Compose([p], close_children=False)
        buf = minihost.AudioBuffer(max(p.num_input_channels, 2), 480)
        with pytest.raises(ValueError, match="sample_rate mismatch"):
            fx(buf, sample_rate=44100)


@skip_if_no_plugin
def test_compose_closes_children_on_exit():
    p = minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512)
    with Compose([p]) as fx:
        buf = minihost.AudioBuffer(max(p.num_input_channels, 2), 480)
        fx(buf)
    # close() is idempotent and the child was closed; a second close on the
    # plugin must not raise.
    p.close()


@skip_if_no_plugin
def test_compose_keeps_shared_child_open():
    p = minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512)
    with Compose([p], close_children=False) as fx:
        buf = minihost.AudioBuffer(max(p.num_input_channels, 2), 480)
        fx(buf)
    # Not closed by Compose -> still usable.
    out = minihost.process_audio(p, buf, compensate_latency=False)
    assert out.frames == 480
    p.close()


@skip_if_no_plugin
def test_random_param_processes_and_is_reproducible():
    def render(seed):
        with minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512) as p:
            fx = Compose(
                [RandomParam(p, 0, 0.0, 1.0)], close_children=False, seed=seed
            )
            buf = minihost.AudioBuffer(max(p.num_input_channels, 2), 480)
            return fx(buf).as_ndarray()

    assert np.array_equal(render(11), render(11))


@skip_if_no_plugin
def test_to_file_roundtrip(tmp_path):
    in_wav = tmp_path / "in.wav"
    out_wav = tmp_path / "out.wav"
    minihost.write_audio(
        str(in_wav), _const(2, SR // 10), SR, bit_depth=24  # 0.1 s
    )
    with minihost.Plugin(PLUGIN, sample_rate=SR, max_block_size=512) as p:
        with Compose([p, Normalize(-1.0)], close_children=False) as fx:
            frames = fx.to_file(str(in_wav), str(out_wav))
    assert out_wav.exists()
    assert frames == SR // 10
