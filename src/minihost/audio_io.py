"""Audio file I/O utilities for minihost.

Uses miniaudio (via C bindings) for reading and writing audio files.
Supports reading WAV, FLAC, MP3, and Vorbis. Writing supports WAV
(16/24/32-bit) and FLAC (16/24-bit).

The default container type is ``minihost.AudioBuffer`` (stdlib-only,
backed by ``juce::AudioBuffer<float>``). Pass ``as_=numpy.ndarray`` to
``read_audio`` if you'd rather work with numpy directly.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any

import numpy as np

from minihost._core import AudioBuffer
from minihost._core import audio_get_file_info as _get_info
from minihost._core import audio_read as _read
from minihost._core import audio_resample as _resample
from minihost._core import audio_write as _write

if TYPE_CHECKING:
    from typing import Union

    AudioData = Union[AudioBuffer, np.ndarray]

# Extensions supported for reading
_READ_EXTENSIONS = {".wav", ".flac", ".mp3", ".ogg"}

# Extensions supported for writing
_WRITE_EXTENSIONS = {".wav", ".flac"}

_VALID_BIT_DEPTHS = {16, 24, 32}


def read_audio(
    path: str | Path,
    as_: type = AudioBuffer,
) -> tuple[Any, int]:
    """Read an audio file and return (data, sample_rate).

    Args:
        path: Path to audio file (WAV, FLAC, MP3, Vorbis).
        as_: Container type for the returned audio data. ``AudioBuffer``
            (default, stdlib-only) or ``numpy.ndarray``. The data is
            float32 with shape ``(channels, samples)`` either way.

    Returns:
        Tuple of (data, sample_rate). ``data`` has shape
        ``(channels, samples)`` and is the type requested via ``as_``.

    Raises:
        FileNotFoundError: If the file does not exist.
        RuntimeError: If the file cannot be read.
        TypeError: If ``as_`` is not a recognized container type.
    """
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Audio file not found: {path}")

    try:
        # The C binding returns a numpy ndarray (zero-copy view onto the
        # decoded buffer). When the caller wants AudioBuffer we copy once
        # into a fresh AudioBuffer of matching shape.
        data, sample_rate = _read(str(path))
    except RuntimeError as e:
        raise RuntimeError(f"Failed to read audio file: {path}: {e}") from e

    if as_ is AudioBuffer:
        return AudioBuffer.from_numpy(np.ascontiguousarray(data, dtype=np.float32)), int(sample_rate)
    if as_ is np.ndarray:
        return np.asarray(data, dtype=np.float32), int(sample_rate)
    raise TypeError(
        f"as_ must be AudioBuffer or numpy.ndarray, got {as_!r}"
    )


def write_audio(
    path: str | Path,
    data: "AudioData",
    sample_rate: int,
    bit_depth: int = 24,
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

    # Coerce to a 2D float32 c-contiguous numpy array. AudioBuffer satisfies
    # this via DLPack / __array__; numpy ndarrays go through ascontiguousarray.
    write_data = np.ascontiguousarray(data, dtype=np.float32)

    # Ensure 2D (channels, samples)
    if write_data.ndim == 1:
        write_data = write_data.reshape(1, -1)

    _write(str(path), write_data, int(sample_rate), bit_depth)


def resample(
    data: "AudioData",
    sample_rate_in: int,
    sample_rate_out: int,
) -> "AudioData":
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
        ``numpy.ndarray`` in -> ``numpy.ndarray`` out).
    """
    is_audiobuffer = isinstance(data, AudioBuffer)

    arr = np.ascontiguousarray(data, dtype=np.float32)
    if arr.ndim == 1:
        arr = arr.reshape(1, -1)
    out = np.asarray(
        _resample(arr, int(sample_rate_in), int(sample_rate_out)),
        dtype=np.float32,
    )

    if is_audiobuffer:
        return AudioBuffer.from_numpy(out)
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
