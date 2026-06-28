"""AudioBufferD: the float64 sibling of AudioBuffer.

AudioBufferD shares AudioBuffer's whole surface (one C++ template, two
instantiations) but holds double-precision samples. It completes the
numpy-optional story for float64: its DLPack export is float64, so it feeds
Plugin.process_double directly without numpy.
"""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost
from minihost import AudioBuffer, AudioBufferD


# ---------------------------------------------------------------------------
# Basics + dtype identity
# ---------------------------------------------------------------------------

def test_dtype_and_repr():
    b = AudioBufferD(2, 8)
    assert b.dtype == "float64"
    assert AudioBufferD.dtype == "float64"
    assert b.shape == (2, 8)
    assert b.channels == 2 and b.frames == 8
    assert len(b) == 2
    assert repr(b) == "AudioBufferD(channels=2, frames=8)"


def test_float32_buffer_unaffected():
    # The float32 class keeps reporting float32 (no regression from sharing
    # the template).
    assert AudioBuffer.dtype == "float32"
    assert repr(AudioBuffer(1, 1)) == "AudioBuffer(channels=1, frames=1)"


def test_zero_initialized():
    b = AudioBufferD(2, 4)
    assert b.magnitude() == 0.0


# ---------------------------------------------------------------------------
# Indexing preserves double precision
# ---------------------------------------------------------------------------

def test_scalar_get_set_keeps_double_precision():
    b = AudioBufferD(1, 1)
    # A value not representable in float32 must round-trip exactly.
    val = 0.1234567890123456789
    b[0, 0] = val
    assert b[0, 0] == val
    # Sanity: a float32 store would have lost precision (compare in float64
    # to dodge numpy's value-based scalar promotion).
    assert float(np.float32(val)) != val


def test_slice_returns_audiobufferd_copy():
    b = AudioBufferD(2, 8)
    b[0, 3] = 2.5
    sub = b[0:1, 2:5]
    assert type(sub).__name__ == "AudioBufferD"
    assert sub.shape == (1, 3)
    assert sub[0, 1] == 2.5  # original index 3 -> sub index 1
    # It is a copy, not a view.
    sub[0, 1] = 9.0
    assert b[0, 3] == 2.5


def test_setitem_buffer_source_converts_dtype():
    b = AudioBufferD(2, 4)
    src = np.full((2, 2), 1.5, dtype=np.float64)
    b[:, 0:2] = src
    assert b[0, 0] == 1.5 and b[1, 1] == 1.5
    # A float32 source is accepted and up-converted (nanobind's ndarray
    # binding converts mismatched dtypes -- same leniency as AudioBuffer).
    b[:, 2:4] = np.full((2, 2), 2.0, dtype=np.float32)
    assert b[0, 2] == 2.0 and b[1, 3] == 2.0


# ---------------------------------------------------------------------------
# DSP ops (shared surface)
# ---------------------------------------------------------------------------

def test_dsp_ops():
    b = AudioBufferD.from_numpy(np.ones((2, 100), dtype=np.float64))
    b.apply_gain(0.5)
    assert b[0, 0] == 0.5
    assert b.magnitude() == 0.5
    assert b.get_rms_level(0, 0, 100) == pytest.approx(0.5)
    b.clear()
    assert b.magnitude() == 0.0

    # apply_gain_ramp + reverse + add_from operate in place.
    r = AudioBufferD.from_numpy(np.ones((1, 4), dtype=np.float64))
    r.apply_gain_ramp(0, 4, 0.0, 1.0)
    assert r[0, 0] == 0.0
    r2 = AudioBufferD.from_numpy(np.array([[1.0, 2.0, 3.0, 4.0]], dtype=np.float64))
    r2.reverse()
    assert [r2[0, i] for i in range(4)] == [4.0, 3.0, 2.0, 1.0]


# ---------------------------------------------------------------------------
# numpy interop
# ---------------------------------------------------------------------------

def test_as_ndarray_is_float64_zero_copy_view():
    b = AudioBufferD(2, 4)
    v = b.as_ndarray()
    assert str(v.dtype) == "float64"
    assert v.shape == (2, 4)
    # Zero-copy: mutating the view mutates the buffer.
    v[1, 2] = 7.0
    assert b[1, 2] == 7.0


def test_np_asarray_and_from_dlpack_are_float64():
    b = AudioBufferD(1, 3)
    b[0, 0] = 0.5
    a = np.asarray(b)            # consults __array__
    assert str(a.dtype) == "float64" and a[0, 0] == 0.5
    d = np.from_dlpack(b)        # consults __dlpack__ -- the process_double path
    assert str(d.dtype) == "float64"


def test_from_numpy_float64_and_converts_float32():
    # Native float64 path.
    b = AudioBufferD.from_numpy(np.arange(6, dtype=np.float64).reshape(2, 3))
    assert b[1, 2] == 5.0 and b.dtype == "float64"
    # A float32 array is up-converted to a float64 buffer.
    b32 = AudioBufferD.from_numpy(np.array([[1.0, 2.0]], dtype=np.float32))
    assert b32.dtype == "float64" and b32[0, 1] == 2.0


def test_float32_from_numpy_converts_float64():
    # The float32 class mirrors the behavior: a float64 array is down-cast.
    b = AudioBuffer.from_numpy(np.array([[1.5, 2.5]], dtype=np.float64))
    assert b.dtype == "float32" and b[0, 0] == 1.5


# ---------------------------------------------------------------------------
# Integration: feed Plugin.process_double without numpy
# ---------------------------------------------------------------------------

DOUBLE_PLUGIN = os.environ.get("MINIHOST_TEST_DOUBLE_PLUGIN")


@pytest.mark.skipif(
    not (DOUBLE_PLUGIN and os.path.exists(DOUBLE_PLUGIN)),
    reason="no double-precision plugin set via MINIHOST_TEST_DOUBLE_PLUGIN",
)
def test_process_double_accepts_audiobufferd():
    p = minihost.Plugin(DOUBLE_PLUGIN, sample_rate=48000, max_block_size=256)
    if not p.supports_double:
        p.close()
        pytest.skip("configured plugin does not support double precision")
    in_ch = p.num_input_channels or 2
    out_ch = p.num_output_channels or 2
    inp = AudioBufferD.from_numpy(
        (np.random.default_rng(0).standard_normal((in_ch, 256)) * 0.1)
    )
    out = AudioBufferD(out_ch, 256)
    # Passed straight through DLPack -- no numpy arrays involved.
    p.process_double(inp, out)
    p.close()
    assert out.dtype == "float64"
