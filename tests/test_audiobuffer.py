"""Tests for the AudioBuffer class and its numpy / DLPack interop."""

from __future__ import annotations

import numpy as np
import pytest

import minihost
from minihost import AudioBuffer


def test_construction_zero_initialized():
    buf = AudioBuffer(2, 256)
    assert buf.channels == 2
    assert buf.frames == 256
    assert buf.shape == (2, 256)
    assert len(buf) == 2  # numpy convention: len = shape[0]
    assert buf.magnitude() == 0.0
    arr = np.asarray(buf)
    assert arr.shape == (2, 256)
    assert arr.dtype == np.float32
    assert not np.isnan(arr).any()
    assert (arr == 0).all()


def test_dtype_class_attr():
    assert AudioBuffer.dtype == "float32"


def test_repr_includes_shape():
    r = repr(AudioBuffer(3, 1024))
    assert "channels=3" in r
    assert "frames=1024" in r


def test_scalar_indexing_round_trip():
    buf = AudioBuffer(2, 256)
    buf[0, 100] = 0.5
    buf[1, 200] = -0.25
    buf[0, -1] = 0.75   # negative index
    assert buf[0, 100] == 0.5
    assert buf[1, 200] == -0.25
    assert buf[0, 255] == 0.75
    assert buf[-1, 200] == -0.25  # negative channel


def test_slice_setitem_scalar_broadcast():
    buf = AudioBuffer(2, 256)
    buf[:, 0:10] = 0.1
    arr = np.asarray(buf)
    assert (arr[:, 0:10] == np.float32(0.1)).all()
    assert (arr[:, 10:] == 0).all()


def test_slice_setitem_buffer_source():
    buf = AudioBuffer(2, 256)
    src = np.full((2, 50), 0.3, dtype=np.float32)
    buf[:, 100:150] = src
    arr = np.asarray(buf)
    assert (arr[:, 100:150] == np.float32(0.3)).all()
    assert (arr[:, 0:100] == 0).all()
    assert (arr[:, 150:] == 0).all()


def test_slice_setitem_audiobuffer_source():
    buf = AudioBuffer(2, 256)
    src = AudioBuffer(2, 50)
    src[:, :] = 0.7
    buf[:, 100:150] = src
    arr = np.asarray(buf)
    assert (arr[:, 100:150] == np.float32(0.7)).all()


def test_slice_getitem_returns_copy_not_view():
    buf = AudioBuffer(2, 256)
    buf[:, :] = 0.4
    sub = buf[:, 0:10]
    assert isinstance(sub, AudioBuffer)
    assert sub.shape == (2, 10)
    # Mutate the slice; original must be untouched.
    sub[0, 0] = 99.0
    assert buf[0, 0] == pytest.approx(0.4)
    assert sub[0, 0] == 99.0


def test_single_axis_indexing_rejected():
    buf = AudioBuffer(2, 256)
    with pytest.raises(TypeError, match="2-axis"):
        _ = buf[0]
    with pytest.raises(TypeError, match="2-axis"):
        _ = buf[0:1]


def test_strided_slice_rejected():
    buf = AudioBuffer(2, 256)
    with pytest.raises(TypeError, match="strided"):
        _ = buf[:, ::2]
    with pytest.raises(TypeError, match="strided"):
        buf[:, ::2] = 0.5


def test_ellipsis_rejected():
    buf = AudioBuffer(2, 256)
    with pytest.raises(TypeError, match="Ellipsis"):
        _ = buf[..., 0:10]


def test_fancy_indexing_rejected():
    buf = AudioBuffer(2, 256)
    with pytest.raises(TypeError, match="fancy"):
        _ = buf[[0, 1], 0:10]


def test_setitem_shape_mismatch_raises():
    buf = AudioBuffer(2, 256)
    src = np.zeros((2, 30), dtype=np.float32)
    with pytest.raises(ValueError, match="does not match"):
        buf[:, 100:150] = src  # 50 != 30


def test_index_out_of_range():
    buf = AudioBuffer(2, 256)
    with pytest.raises(IndexError):
        _ = buf[5, 0]
    with pytest.raises(IndexError):
        _ = buf[0, 1000]


def test_clear_full_and_range():
    buf = AudioBuffer(2, 256)
    buf[:, :] = 0.5
    buf.clear(100, 50)
    arr = np.asarray(buf)
    assert (arr[:, :100] == 0.5).all()
    assert (arr[:, 100:150] == 0).all()
    assert (arr[:, 150:] == 0.5).all()
    buf.clear()
    arr = np.asarray(buf)
    assert (arr == 0).all()


def test_apply_gain():
    buf = AudioBuffer(2, 256)
    buf[:, :] = 0.5
    buf.apply_gain(0.5)
    arr = np.asarray(buf)
    assert np.allclose(arr, 0.25)


def test_magnitude():
    buf = AudioBuffer(2, 256)
    buf[0, 100] = 0.5
    buf[1, 50] = -0.75
    assert buf.magnitude() == pytest.approx(0.75)
    assert buf.magnitude(0, 60) == pytest.approx(0.75)  # only ch1's -0.75
    assert buf.magnitude(60, 60) == pytest.approx(0.5)  # only ch0's 0.5


def test_copy_is_independent():
    buf = AudioBuffer(2, 256)
    buf[:, :] = 0.3
    cp = buf.copy()
    cp[0, 0] = 99.0
    assert buf[0, 0] == pytest.approx(0.3)
    assert cp[0, 0] == 99.0


def test_numpy_view_is_zero_copy():
    buf = AudioBuffer(2, 256)
    view = buf.as_ndarray()
    view[0, 50] = 0.42
    assert buf[0, 50] == pytest.approx(0.42)


def test_from_numpy_round_trip():
    arr = np.random.default_rng(42).standard_normal((2, 128)).astype(np.float32)
    buf = AudioBuffer.from_numpy(arr)
    assert buf.shape == arr.shape
    back = np.asarray(buf)
    assert np.array_equal(back, arr)


def test_audiobuffer_passes_directly_to_plugin_process():
    """The headline win: AudioBuffer is consumed by Plugin.process via DLPack
    without an explicit .as_ndarray() / .array conversion."""
    plugin_path = "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
    import os
    if not os.path.exists(plugin_path):
        pytest.skip("test plugin not available")
    plugin = minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)
    inp = AudioBuffer(plugin.num_input_channels, 256)
    out = AudioBuffer(plugin.num_output_channels, 256)
    plugin.process(inp, out)  # must not raise
    # process must not have populated NaN garbage
    assert np.isfinite(np.asarray(out)).all()


def test_audiobuffer_mixed_with_numpy_in_process():
    plugin_path = "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
    import os
    if not os.path.exists(plugin_path):
        pytest.skip("test plugin not available")
    plugin = minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)
    inp = AudioBuffer(plugin.num_input_channels, 256)
    out_np = np.zeros((plugin.num_output_channels, 256), dtype=np.float32)
    plugin.process(inp, out_np)
    assert np.isfinite(out_np).all()
