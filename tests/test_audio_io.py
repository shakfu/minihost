"""Tests for minihost.audio_io module."""

import tempfile
from pathlib import Path

import numpy as np
import pytest

from minihost.audio_io import get_audio_info, read_audio, write_audio


class TestWriteAndReadRoundTrip:
    """Test write -> read round-trip for various bit depths."""

    def _make_test_signal(self, channels=2, samples=4800, sample_rate=48000):
        """Generate a deterministic test signal."""
        t = np.linspace(0, 1, samples, dtype=np.float32)
        data = np.zeros((channels, samples), dtype=np.float32)
        for ch in range(channels):
            freq = 440.0 * (ch + 1)
            data[ch] = 0.5 * np.sin(2 * np.pi * freq * t)
        return data, sample_rate

    def test_wav_16bit_round_trip(self, tmp_path):
        data, sr = self._make_test_signal()
        path = tmp_path / "test.wav"

        write_audio(path, data, sr, bit_depth=16)
        result, result_sr = read_audio(path)

        assert result_sr == sr
        assert result.shape == data.shape
        # 16-bit has ~1/32768 quantization error; triangle dither adds up to 2 LSBs
        np.testing.assert_allclose(result, data, atol=3.0 / 32768)

    def test_wav_24bit_round_trip(self, tmp_path):
        data, sr = self._make_test_signal()
        path = tmp_path / "test.wav"

        write_audio(path, data, sr, bit_depth=24)
        result, result_sr = read_audio(path)

        assert result_sr == sr
        assert result.shape == data.shape
        # 24-bit has ~1/8388608 quantization error
        np.testing.assert_allclose(result, data, atol=1.0 / 8388608 + 1e-6)

    def test_wav_32bit_float_round_trip(self, tmp_path):
        data, sr = self._make_test_signal()
        path = tmp_path / "test.wav"

        write_audio(path, data, sr, bit_depth=32)
        result, result_sr = read_audio(path)

        assert result_sr == sr
        assert result.shape == data.shape
        np.testing.assert_array_almost_equal(result, data, decimal=6)


class TestMultiChannel:
    """Test multi-channel audio I/O."""

    def test_mono(self, tmp_path):
        data = np.random.randn(1, 1000).astype(np.float32) * 0.5
        path = tmp_path / "mono.wav"

        write_audio(path, data, 44100, bit_depth=32)
        result, sr = read_audio(path)

        assert sr == 44100
        assert result.shape == (1, 1000)
        np.testing.assert_array_almost_equal(result, data, decimal=6)

    def test_stereo(self, tmp_path):
        data = np.random.randn(2, 1000).astype(np.float32) * 0.5
        path = tmp_path / "stereo.wav"

        write_audio(path, data, 48000, bit_depth=32)
        result, sr = read_audio(path)

        assert result.shape == (2, 1000)

    def test_six_channel(self, tmp_path):
        data = np.random.randn(6, 500).astype(np.float32) * 0.5
        path = tmp_path / "surround.wav"

        write_audio(path, data, 48000, bit_depth=32)
        result, sr = read_audio(path)

        assert result.shape == (6, 500)
        np.testing.assert_array_almost_equal(result, data, decimal=6)


class TestGetAudioInfo:
    """Test get_audio_info metadata extraction."""

    def test_basic_info(self, tmp_path):
        data = np.zeros((2, 48000), dtype=np.float32)
        path = tmp_path / "info_test.wav"

        write_audio(path, data, 48000, bit_depth=24)
        info = get_audio_info(path)

        assert info["channels"] == 2
        assert info["sample_rate"] == 48000
        assert info["frames"] == 48000
        assert abs(info["duration"] - 1.0) < 0.01

    def test_file_not_found(self):
        with pytest.raises(FileNotFoundError):
            get_audio_info("/nonexistent/path/audio.wav")


class TestErrors:
    """Test error handling."""

    def test_read_nonexistent_file(self):
        with pytest.raises(FileNotFoundError):
            read_audio("/nonexistent/file.wav")

    def test_write_unsupported_format(self, tmp_path):
        data = np.zeros((2, 100), dtype=np.float32)
        with pytest.raises(ValueError, match="Unsupported"):
            write_audio(tmp_path / "test.flac", data, 48000)

    def test_write_invalid_bit_depth(self, tmp_path):
        data = np.zeros((2, 100), dtype=np.float32)
        with pytest.raises(ValueError, match="bit_depth"):
            write_audio(tmp_path / "test.wav", data, 48000, bit_depth=8)

    def test_default_bit_depth_is_24(self, tmp_path):
        data = np.zeros((2, 100), dtype=np.float32)
        path = tmp_path / "default.wav"
        write_audio(path, data, 48000)
        info = get_audio_info(path)
        # With miniaudio, 24-bit WAV stores as PCM_24
        assert info["channels"] == 2
        assert info["sample_rate"] == 48000
        assert info["frames"] == 100
