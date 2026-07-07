"""Audio file I/O utilities for minihost.

Uses miniaudio (via C bindings) for reading and writing audio files.
Supports reading WAV, FLAC, MP3, and Vorbis. Writing supports WAV
(16/24/32-bit) and FLAC (16/24-bit).

The default container type is :class:`minihost.AudioBuffer` (stdlib-only,
backed by ``juce::AudioBuffer<float>``). numpy is supported but optional;
pass ``as_=numpy.ndarray`` to ``read_audio`` (or call ``.as_ndarray()``
on the returned AudioBuffer) when you need a numpy array. ``write_audio``
and ``resample`` accept either container type, plus anything else
implementing the buffer protocol.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

from minihost._core import AudioBuffer
from minihost._core import audio_get_file_info as _get_info
from minihost._core import audio_read as _read
from minihost._core import audio_resample as _resample
from minihost._core import audio_write as _write

if TYPE_CHECKING:
    import numpy as np

# Extensions supported for reading
_READ_EXTENSIONS = {".wav", ".flac", ".mp3", ".ogg"}

# Extensions supported for writing
_WRITE_EXTENSIONS = {".wav", ".flac"}

_VALID_BIT_DEPTHS = {16, 24, 32}


def _require_numpy(feature: str):
    """Lazy-import numpy. Raises ImportError with install instructions if absent."""
    try:
        import numpy as _np
    except ImportError as e:
        raise ImportError(
            f"{feature} requires numpy. Install minihost with the numpy "
            f"extra: 'pip install minihost[numpy]'."
        ) from e
    return _np


def read_audio(
    path: str | Path,
    as_: type | None = None,
) -> tuple[Any, int]:
    """Read an audio file and return (data, sample_rate).

    Args:
        path: Path to audio file (WAV, FLAC, MP3, Vorbis).
        as_: Container type for the returned audio data. ``AudioBuffer``
            (default, no numpy required) or ``numpy.ndarray`` (requires
            numpy installed). The data is float32 with shape
            ``(channels, samples)`` either way.

    Returns:
        Tuple of (data, sample_rate). ``data`` has shape
        ``(channels, samples)`` and is the type requested via ``as_``.

    Raises:
        FileNotFoundError: If the file does not exist.
        RuntimeError: If the file cannot be read.
        ImportError: If ``as_=numpy.ndarray`` is requested but numpy is
            not installed.
        TypeError: If ``as_`` is not a recognized container type.
    """
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Audio file not found: {path}")

    try:
        # The C binding now returns an AudioBuffer directly; no numpy
        # involved on the read path even if as_=numpy.ndarray.
        data, sample_rate = _read(str(path))
    except RuntimeError as e:
        raise RuntimeError(f"Failed to read audio file: {path}: {e}") from e

    if as_ is None or as_ is AudioBuffer:
        return data, int(sample_rate)

    np = _require_numpy("read_audio(..., as_=numpy.ndarray)")
    if as_ is np.ndarray:
        return data.as_ndarray(), int(sample_rate)
    raise TypeError(
        f"as_ must be AudioBuffer or numpy.ndarray, got {as_!r}"
    )


def write_audio(
    path: str | Path,
    data: Any,
    sample_rate: int,
    bit_depth: int = 24,
    bwf: dict | None = None,
) -> None:
    """Write audio data to an audio file (WAV or FLAC).

    Args:
        path: Output file path (.wav or .flac).
        data: Audio data of shape (channels, samples). Accepts an
            ``AudioBuffer``, a numpy ndarray, or any 2D float32 c-contiguous
            buffer (DLPack / buffer-protocol producer).
        sample_rate: Sample rate in Hz.
        bit_depth: Bit depth (16, 24, or 32). Default 24.
            16 and 24 write integer PCM; 32 writes IEEE float (WAV only).
            FLAC supports 16 and 24 only.
        bwf: Optional Broadcast Wave (bext) metadata for WAV output. A dict
            with any of: ``description``, ``originator``,
            ``originator_reference``, ``origination_date`` ('yyyy-mm-dd'),
            ``origination_time`` ('hh:mm:ss'), ``time_reference`` (int samples
            since midnight). Only valid for WAV; passing it with a FLAC path
            raises.

    Raises:
        ValueError: If bit_depth is invalid or extension is unsupported.
    """
    path = Path(path)
    ext = path.suffix.lower()

    if ext not in _WRITE_EXTENSIONS:
        raise ValueError(
            f"Unsupported audio format for writing: '{ext}'. "
            f"Supported: WAV (.wav), FLAC (.flac)."
        )

    if ext == ".flac" and bit_depth == 32:
        raise ValueError(
            "FLAC does not support 32-bit float. Use 16 or 24-bit, "
            "or use WAV for 32-bit float."
        )

    if bit_depth not in _VALID_BIT_DEPTHS:
        raise ValueError(f"bit_depth must be 16, 24, or 32, got {bit_depth}")

    if bwf is not None and ext != ".wav":
        raise ValueError("BWF (bext) metadata is only supported for WAV output.")

    # AudioBuffer satisfies _write directly via DLPack (no numpy needed).
    # For other inputs (numpy ndarray, list-of-lists, etc.) we may need
    # numpy to coerce to a 2D float32 c-contiguous buffer. Detect.
    if isinstance(data, AudioBuffer):
        write_data = data
    else:
        # Lazy: only require numpy if the input isn't already an AudioBuffer.
        np = _require_numpy("write_audio with non-AudioBuffer input")
        write_data = np.ascontiguousarray(data, dtype=np.float32)
        if write_data.ndim == 1:
            write_data = write_data.reshape(1, -1)

    _write(str(path), write_data, int(sample_rate), bit_depth, bwf)


def resample(
    data: Any,
    sample_rate_in: int,
    sample_rate_out: int,
) -> Any:
    """Resample audio data to a different sample rate.

    Args:
        data: Audio data of shape (channels, samples), float32. Accepts
            ``AudioBuffer``, numpy ndarray, or any 2D float32 c-contiguous
            buffer.
        sample_rate_in: Source sample rate in Hz.
        sample_rate_out: Target sample rate in Hz.

    Returns:
        Resampled data of shape (channels, new_samples). The return type
        matches the input type (``AudioBuffer`` in -> ``AudioBuffer`` out;
        ``numpy.ndarray`` in -> ``numpy.ndarray`` out). Other buffer-
        protocol inputs return ``AudioBuffer``.
    """
    is_audiobuffer = isinstance(data, AudioBuffer)

    if is_audiobuffer:
        # AudioBuffer goes straight through DLPack; no numpy needed.
        out = _resample(data, int(sample_rate_in), int(sample_rate_out))
        return out  # AudioBuffer

    # Non-AudioBuffer input: coerce via numpy (lazy import). The C
    # binding returns AudioBuffer regardless; convert back to numpy if
    # the caller passed numpy in.
    np = _require_numpy("resample with non-AudioBuffer input")
    is_numpy = isinstance(data, np.ndarray)

    arr = np.ascontiguousarray(data, dtype=np.float32)
    if arr.ndim == 1:
        arr = arr.reshape(1, -1)
    out = _resample(arr, int(sample_rate_in), int(sample_rate_out))  # AudioBuffer

    if is_numpy:
        return out.as_ndarray()
    return out


def get_audio_info(path: str | Path) -> dict:
    """Get audio file metadata without loading the full data.

    Args:
        path: Path to audio file.

    Returns:
        Dict with keys: channels, sample_rate, frames, duration.

    Raises:
        FileNotFoundError: If the file does not exist.
    """
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Audio file not found: {path}")

    return dict(_get_info(str(path)))
