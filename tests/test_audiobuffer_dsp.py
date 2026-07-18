"""Tests for the extended JUCE-backed DSP ops on AudioBuffer.

Each test verifies the AudioBuffer operation against a direct numpy
reference implementation, then also exercises the bounds-checking
error paths.
"""

from __future__ import annotations

import numpy as np
import pytest

from minihost import AudioBuffer


# -- apply_gain_ramp ---------------------------------------------------


def test_apply_gain_ramp_matches_numpy_reference():
    # Initialize all samples to 1.0; ramping from 0 to 2 should produce a
    # clean linear ramp from 0 to (effectively) 2.0 at the end.
    buf = AudioBuffer(2, 100)
    buf[:, :] = 1.0
    buf.apply_gain_ramp(0, 100, 0.0, 2.0)

    ref = np.ones((2, 100), dtype=np.float32)
    # JUCE's applyGainRamp produces gains[i] = startGain + i*(endGain-startGain)/numSamples
    ramp = np.linspace(0.0, 2.0, 100, endpoint=False, dtype=np.float32)
    ref *= ramp[None, :]  # broadcast across both channels

    arr = np.asarray(buf)
    assert np.allclose(arr, ref, atol=1e-6)


def test_apply_gain_ramp_partial_range():
    # Only ramp samples [25, 75); samples outside should be unchanged.
    buf = AudioBuffer(1, 100)
    buf[:, :] = 1.0
    buf.apply_gain_ramp(25, 50, 0.0, 1.0)

    arr = np.asarray(buf)
    assert (arr[0, :25] == 1.0).all()
    assert (arr[0, 75:] == 1.0).all()
    # Inside the ramp: linear from 0 to (just below) 1.0
    ramp = np.linspace(0.0, 1.0, 50, endpoint=False, dtype=np.float32)
    assert np.allclose(arr[0, 25:75], ramp, atol=1e-6)


def test_apply_gain_ramp_out_of_bounds_raises():
    buf = AudioBuffer(2, 100)
    with pytest.raises(ValueError, match="out of bounds"):
        buf.apply_gain_ramp(50, 60, 0.0, 1.0)
    with pytest.raises(ValueError, match="out of bounds"):
        buf.apply_gain_ramp(-1, 10, 0.0, 1.0)


# -- apply_gain_per_channel -------------------------------------------


def test_apply_gain_per_channel():
    buf = AudioBuffer(3, 64)
    buf[:, :] = 1.0
    buf.apply_gain_per_channel([0.5, 1.0, 2.0])
    arr = np.asarray(buf)
    assert np.allclose(arr[0], 0.5)
    assert np.allclose(arr[1], 1.0)
    assert np.allclose(arr[2], 2.0)


def test_apply_gain_per_channel_wrong_length_raises():
    buf = AudioBuffer(3, 64)
    with pytest.raises(ValueError, match="does not match channel count"):
        buf.apply_gain_per_channel([1.0, 2.0])  # 2 != 3
    with pytest.raises(ValueError, match="does not match channel count"):
        buf.apply_gain_per_channel([1.0, 2.0, 3.0, 4.0])  # 4 != 3


# -- add_from ---------------------------------------------------------


def test_add_from_matches_numpy_reference():
    dst = AudioBuffer(2, 100)
    dst[:, :] = 0.1
    src = AudioBuffer(2, 100)
    src[:, :] = 0.5

    dst.add_from(
        dest_channel=0,
        dest_start=10,
        source=src,
        source_channel=1,
        source_start=20,
        count=30,
        gain=2.0,
    )

    ref = np.full((2, 100), 0.1, dtype=np.float32)
    src_ref = np.full((2, 100), 0.5, dtype=np.float32)
    ref[0, 10:40] += 2.0 * src_ref[1, 20:50]

    arr = np.asarray(dst)
    assert np.allclose(arr, ref, atol=1e-6)


def test_add_from_default_gain_is_one():
    dst = AudioBuffer(1, 50)
    dst[:, :] = 0.0
    src = AudioBuffer(1, 50)
    src[:, :] = 0.3
    dst.add_from(0, 0, src, 0, 0, 50)
    arr = np.asarray(dst)
    assert np.allclose(arr, 0.3)


def test_add_from_bounds_checking():
    dst = AudioBuffer(2, 100)
    src = AudioBuffer(2, 50)

    with pytest.raises(ValueError, match="dest_channel out of range"):
        dst.add_from(5, 0, src, 0, 0, 10)
    with pytest.raises(ValueError, match="source_channel out of range"):
        dst.add_from(0, 0, src, 5, 0, 10)
    with pytest.raises(ValueError, match="dest range out of bounds"):
        dst.add_from(0, 95, src, 0, 0, 10)  # 95 + 10 > 100
    with pytest.raises(ValueError, match="source range out of bounds"):
        dst.add_from(0, 0, src, 0, 45, 10)  # 45 + 10 > 50


# -- add_from_with_ramp -----------------------------------------------


def test_add_from_with_ramp_matches_numpy_reference():
    dst = AudioBuffer(2, 100)
    dst[:, :] = 0.0
    src = AudioBuffer(1, 50)
    src[:, :] = 1.0  # source is constant 1.0

    dst.add_from_with_ramp(
        dest_channel=1,
        dest_start=20,
        source=src,
        source_channel=0,
        source_start=0,
        count=50,
        gain_start=0.0,
        gain_end=1.0,
    )

    ref = np.zeros((2, 100), dtype=np.float32)
    # Ramp produces gain[i] = startGain + i*(endGain-startGain)/numSamples
    # For src all-ones, the result is the ramp itself.
    ramp = np.linspace(0.0, 1.0, 50, endpoint=False, dtype=np.float32)
    ref[1, 20:70] += ramp

    arr = np.asarray(dst)
    assert np.allclose(arr, ref, atol=1e-6)


def test_add_from_with_ramp_bounds():
    dst = AudioBuffer(1, 100)
    src = AudioBuffer(1, 100)
    with pytest.raises(ValueError, match="dest range out of bounds"):
        dst.add_from_with_ramp(0, 95, src, 0, 0, 20, 0.0, 1.0)


# -- get_rms_level ----------------------------------------------------


def test_get_rms_level_matches_numpy():
    rng = np.random.default_rng(seed=42)
    arr = rng.standard_normal((2, 1024)).astype(np.float32)
    buf = AudioBuffer.from_numpy(arr)

    for ch in (0, 1):
        ref_rms = float(np.sqrt(np.mean(arr[ch] ** 2)))
        assert buf.get_rms_level(ch) == pytest.approx(ref_rms, rel=1e-4)


def test_get_rms_level_partial_range():
    arr = np.zeros((1, 1000), dtype=np.float32)
    arr[0, 100:200] = 0.5  # constant 0.5 over 100 samples
    buf = AudioBuffer.from_numpy(arr)

    # RMS of a constant 0.5 region is 0.5; the whole-channel RMS is much smaller.
    assert buf.get_rms_level(0, 100, 100) == pytest.approx(0.5, abs=1e-6)
    full = float(np.sqrt(np.mean(arr[0] ** 2)))
    assert buf.get_rms_level(0) == pytest.approx(full, rel=1e-4)


def test_get_rms_level_default_count_is_full_remaining():
    arr = np.full((1, 100), 0.4, dtype=np.float32)
    buf = AudioBuffer.from_numpy(arr)
    # No count -> covers from start to end.
    assert buf.get_rms_level(0, 50) == pytest.approx(0.4, abs=1e-6)


def test_get_rms_level_bounds():
    buf = AudioBuffer(2, 100)
    with pytest.raises(ValueError, match="channel out of range"):
        buf.get_rms_level(5)
    with pytest.raises(ValueError, match="out of bounds"):
        buf.get_rms_level(0, 95, 10)


# -- reverse / reverse_channel ----------------------------------------


def test_reverse_all_channels():
    arr = np.arange(20, dtype=np.float32).reshape(2, 10)
    buf = AudioBuffer.from_numpy(arr)
    buf.reverse()
    out = np.asarray(buf)
    assert np.array_equal(out, arr[:, ::-1])


def test_reverse_partial_range():
    arr = np.arange(20, dtype=np.float32).reshape(2, 10)
    buf = AudioBuffer.from_numpy(arr)
    buf.reverse(2, 5)  # reverse samples [2, 7) on all channels
    out = np.asarray(buf)
    expected = arr.copy()
    expected[:, 2:7] = expected[:, 2:7][:, ::-1]
    assert np.array_equal(out, expected)


def test_reverse_channel_single():
    arr = np.arange(20, dtype=np.float32).reshape(2, 10)
    buf = AudioBuffer.from_numpy(arr)
    buf.reverse_channel(0)  # reverse channel 0 only
    out = np.asarray(buf)
    expected = arr.copy()
    expected[0] = expected[0, ::-1]
    assert np.array_equal(out, expected)


def test_reverse_channel_partial_range():
    arr = np.arange(20, dtype=np.float32).reshape(2, 10)
    buf = AudioBuffer.from_numpy(arr)
    buf.reverse_channel(1, 3, 4)
    out = np.asarray(buf)
    expected = arr.copy()
    expected[1, 3:7] = expected[1, 3:7][::-1]
    assert np.array_equal(out, expected)


def test_reverse_bounds():
    buf = AudioBuffer(2, 100)
    with pytest.raises(ValueError, match="out of bounds"):
        buf.reverse(95, 10)
    with pytest.raises(ValueError, match="channel out of range"):
        buf.reverse_channel(5)
    with pytest.raises(ValueError, match="out of bounds"):
        buf.reverse_channel(0, 95, 10)
