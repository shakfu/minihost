"""DLPack zero-copy interop for AudioBuffer / AudioBufferD.

Motivation: a 2026-07-07 review flagged ``__dlpack__`` as possibly not
returning a DLPack capsule (which would break ``torch.from_dlpack`` /
``jax``). Empirical testing refuted that -- ``__dlpack__`` returns a
proper ``"dltensor"`` PyCapsule and the export is genuinely zero-copy.
These tests lock that behavior in so a future regression is caught, and
document the one real nuance: numpy imports the buffer read-only (a
writable zero-copy view is available via ``as_ndarray()``).

The numpy tests always run (numpy is the sole runtime dependency); the
torch / jax tests skip cleanly when those frameworks are absent.
"""

import numpy as np
import pytest

from minihost._core import AudioBuffer, AudioBufferD


@pytest.mark.parametrize("cls", [AudioBuffer, AudioBufferD])
def test_dlpack_returns_dltensor_capsule(cls):
    b = cls(2, 4)
    cap = b.__dlpack__()
    # A DLPack capsule is named "dltensor"; consumers (numpy/torch/jax)
    # require this exact object type, not a bare ndarray.
    assert type(cap).__name__ == "PyCapsule"
    assert b.__dlpack_device__() == (1, 0)  # (kDLCPU, device 0)


@pytest.mark.parametrize("cls", [AudioBuffer, AudioBufferD])
def test_dlpack_shape_and_dtype(cls):
    b = cls(3, 5)
    a = np.from_dlpack(b)
    assert a.shape == (3, 5)
    expected = "float32" if cls is AudioBuffer else "float64"
    assert str(a.dtype) == expected


@pytest.mark.parametrize("cls", [AudioBuffer, AudioBufferD])
def test_dlpack_is_zero_copy_read_aliasing(cls):
    """Mutating the buffer must be observable through a previously-created
    DLPack view -- proving the export aliases memory rather than copying."""
    b = cls(2, 4)
    view = np.from_dlpack(b)
    b[0, 0] = 1.25
    b[1, 3] = -9.5
    assert view[0, 0] == 1.25
    assert view[1, 3] == -9.5
    # Same underlying memory as the writable as_ndarray() path.
    assert (
        view.__array_interface__["data"][0]
        == b.as_ndarray().__array_interface__["data"][0]
    )


@pytest.mark.parametrize("cls", [AudioBuffer, AudioBufferD])
def test_dlpack_numpy_import_is_read_only(cls):
    """Documented nuance: numpy 2.x imports DLPack tensors read-only. Callers
    needing a writable zero-copy view should use as_ndarray() instead."""
    b = cls(1, 3)
    a = np.from_dlpack(b)
    assert a.flags.writeable is False
    assert b.as_ndarray().flags.writeable is True


def test_dlpack_torch_zero_copy():
    torch = pytest.importorskip("torch")
    b = AudioBuffer(2, 8)
    t = torch.from_dlpack(b)
    assert tuple(t.shape) == (2, 8)
    assert t.dtype == torch.float32
    # Read-direction aliasing always holds for a true zero-copy import.
    b[0, 0] = 3.5
    assert float(t[0, 0]) == 3.5


def test_dlpack_jax_import():
    jnp = pytest.importorskip("jax.numpy")
    b = AudioBuffer(2, 8)
    b[0, 0] = 2.0
    j = jnp.from_dlpack(b)
    assert tuple(j.shape) == (2, 8)
    assert float(j[0, 0]) == 2.0
