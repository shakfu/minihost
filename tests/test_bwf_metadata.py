"""Tests for Broadcast Wave (BWF / bext) metadata in WAV output.

Verifies that write_audio(..., bwf=...) embeds a valid `bext` chunk, that the
audio still decodes correctly afterwards, and that the metadata fields
round-trip by parsing the raw RIFF chunks.
"""

import struct

import numpy as np
import pytest

from minihost import write_audio, read_audio


def _read_chunks(path):
    """Return {chunk_id: bytes} for a RIFF/WAVE file, and validate the RIFF
    size field matches the actual file length."""
    raw = open(path, "rb").read()
    assert raw[:4] == b"RIFF"
    riff_size = struct.unpack_from("<I", raw, 4)[0]
    assert riff_size == len(raw) - 8, "RIFF size field must equal file size - 8"
    assert raw[8:12] == b"WAVE"
    chunks = {}
    pos = 12
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        csize = struct.unpack_from("<I", raw, pos + 4)[0]
        body = raw[pos + 8:pos + 8 + csize]
        chunks[cid] = body
        pos += 8 + csize + (csize & 1)  # chunks are word-aligned
    return chunks


def _parse_bext(body):
    assert len(body) >= 602
    def field(off, size):
        return body[off:off + size].split(b"\x00", 1)[0].decode("ascii")
    tref_lo = struct.unpack_from("<I", body, 338)[0]
    tref_hi = struct.unpack_from("<I", body, 342)[0]
    return {
        "description": field(0, 256),
        "originator": field(256, 32),
        "originator_reference": field(288, 32),
        "origination_date": field(320, 10),
        "origination_time": field(330, 8),
        "time_reference": tref_lo | (tref_hi << 32),
        "version": struct.unpack_from("<H", body, 346)[0],
    }


def test_bext_chunk_written_and_fields_roundtrip(tmp_path):
    path = tmp_path / "bwf.wav"
    audio = (np.random.default_rng(0).standard_normal((2, 480)) * 0.1).astype(
        np.float32
    )
    meta = {
        "description": "minihost test render",
        "originator": "minihost",
        "originator_reference": "REF-12345",
        "origination_date": "2026-07-07",
        "origination_time": "12:34:56",
        "time_reference": 48000 * 3600,  # one hour of samples
    }
    write_audio(path, audio, 48000, bit_depth=24, bwf=meta)

    chunks = _read_chunks(path)
    assert b"bext" in chunks
    assert b"data" in chunks and b"fmt " in chunks
    parsed = _parse_bext(chunks[b"bext"])
    for k, v in meta.items():
        assert parsed[k] == v, f"{k}: {parsed[k]!r} != {v!r}"
    assert parsed["version"] == 1


def test_bext_audio_still_decodes(tmp_path):
    path = tmp_path / "bwf_decode.wav"
    audio = (np.random.default_rng(1).standard_normal((2, 512)) * 0.1).astype(
        np.float32
    )
    write_audio(path, audio, 44100, bit_depth=16, bwf={"description": "hi"})
    # miniaudio must still decode the file with the trailing bext chunk.
    data, sr = read_audio(path)
    assert sr == 44100
    assert data.shape == (2, 512)


def test_partial_metadata_leaves_other_fields_empty(tmp_path):
    path = tmp_path / "partial.wav"
    audio = np.zeros((1, 64), dtype=np.float32)
    write_audio(path, audio, 48000, bwf={"originator": "only-orig"})
    parsed = _parse_bext(_read_chunks(path)[b"bext"])
    assert parsed["originator"] == "only-orig"
    assert parsed["description"] == ""
    assert parsed["time_reference"] == 0


def test_long_fields_are_truncated(tmp_path):
    path = tmp_path / "trunc.wav"
    audio = np.zeros((1, 32), dtype=np.float32)
    write_audio(path, audio, 48000, bwf={"originator": "X" * 100})
    parsed = _parse_bext(_read_chunks(path)[b"bext"])
    assert parsed["originator"] == "X" * 32  # Originator field is 32 bytes


def test_no_bext_chunk_when_metadata_absent(tmp_path):
    path = tmp_path / "plain.wav"
    audio = np.zeros((1, 32), dtype=np.float32)
    write_audio(path, audio, 48000)
    assert b"bext" not in _read_chunks(path)


def test_bwf_on_flac_raises(tmp_path):
    path = tmp_path / "nope.flac"
    audio = np.zeros((1, 32), dtype=np.float32)
    with pytest.raises(ValueError):
        write_audio(path, audio, 48000, bit_depth=24, bwf={"description": "x"})
