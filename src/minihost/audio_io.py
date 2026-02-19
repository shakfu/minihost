"""Audio file I/O utilities for minihost.

Uses miniaudio (via C bindings) for reading and writing audio files.
Supports reading WAV, FLAC, MP3, and Vorbis. Writing is WAV-only
(16-bit, 24-bit, or 32-bit float).
"""

from __future__ import annotations

from pathlib import Path

import numpy as np

from minihost._core import audio_get_file_info as _get_info
from minihost._core import audio_read as _read
from minihost._core import audio_write as _write

# Extensions supported for reading
_READ_EXTENSIONS = {".wav", ".flac", ".mp3", ".ogg"}

# Extensions supported for writing (WAV only)
_WRITE_EXTENSIONS = {".wav"}

_VALID_BIT_DEPTHS = {16, 24, 32}


def read_audio(path: str | Path) -> tuple[np.ndarray, int]:
    """Read an audio file and return (data, sample_rate).

    Args:
        path: Path to audio file (WAV, FLAC, MP3, Vorbis).

    Returns:
        Tuple of (data, sample_rate) where data is a float32 ndarray
        of shape (channels, samples).

    Raises:
        FileNotFoundError: If the file does not exist.
        RuntimeError: If the file cannot be read.
    """
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Audio file not found: {path}")

    try:
        data, sample_rate = _read(str(path))
    except RuntimeError as e:
        raise RuntimeError(f"Failed to read audio file: {path}: {e}") from e

    return np.asarray(data, dtype=np.float32), int(sample_rate)


def write_audio(
    path: str | Path,
    data: np.ndarray,
    sample_rate: int,
    bit_depth: int = 24,
) -> None:
    """Write audio data to a WAV file.

    Args:
        path: Output file path. Only .wav is supported.
        data: Audio data of shape (channels, samples).
        sample_rate: Sample rate in Hz.
        bit_depth: Bit depth (16, 24, or 32). Default 24.
            16 and 24 write integer PCM; 32 writes IEEE float.

    Raises:
        ValueError: If bit_depth is invalid or extension is unsupported.
    """
    path = Path(path)
    ext = path.suffix.lower()

    if ext not in _WRITE_EXTENSIONS:
        raise ValueError(
            f"Unsupported audio format for writing: '{ext}'. "
            f"Only WAV (.wav) is supported."
        )

    if bit_depth not in _VALID_BIT_DEPTHS:
        raise ValueError(f"bit_depth must be 16, 24, or 32, got {bit_depth}")

    # Ensure float32 and contiguous
    write_data = np.ascontiguousarray(data, dtype=np.float32)

    # Ensure 2D (channels, samples)
    if write_data.ndim == 1:
        write_data = write_data.reshape(1, -1)

    _write(str(path), write_data, int(sample_rate), bit_depth)


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
