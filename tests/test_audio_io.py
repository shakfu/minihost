"""Tests for minihost.audio_io module."""

import time

import numpy as np
import pytest

from minihost.audio_io import get_audio_info, read_audio, resample, write_audio


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
        # Rounded, not dithered: at most half a 16-bit LSB.
        np.testing.assert_allclose(result, data, atol=0.5 / 32768 + 1e-7)

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

    def test_flac_16bit_round_trip(self, tmp_path):
        data, sr = self._make_test_signal()
        path = tmp_path / "test.flac"

        write_audio(path, data, sr, bit_depth=16)
        result, result_sr = read_audio(path)

        assert result_sr == sr
        assert result.shape == data.shape
        # Rounded, not dithered: at most half a 16-bit LSB.
        np.testing.assert_allclose(result, data, atol=0.5 / 32768 + 1e-7)

    def test_flac_24bit_round_trip(self, tmp_path):
        data, sr = self._make_test_signal()
        path = tmp_path / "test.flac"

        write_audio(path, data, sr, bit_depth=24)
        result, result_sr = read_audio(path)

        assert result_sr == sr
        assert result.shape == data.shape
        # 24-bit has ~1/8388608 quantization error
        np.testing.assert_allclose(result, data, atol=1.0 / 8388608 + 1e-6)


class TestFlacSilence:
    """Exact zeros in the first FLAC block.

    Vendored tflac read the subframe bit depth before setting it, so the first
    block passed 0 bits to its wasted-bits scan. On clang builds the portable
    scan decremented that to UINT_MAX: about 1.35 s per zero sample, so four
    zeros took 5.5 s and a render starting with 4096 silent stereo frames
    would have taken hours.
    """

    @pytest.mark.parametrize("bit_depth", [16, 24])
    @pytest.mark.parametrize(
        "data",
        [
            np.zeros((1, 4), dtype=np.float32),
            np.zeros((2, 48000), dtype=np.float32),
            np.concatenate(
                [np.zeros((2, 4096)), np.full((2, 4096), 0.25)], axis=1
            ).astype(np.float32),
        ],
        ids=["four-zeros", "stereo-second", "leading-silence"],
    )
    def test_silence_encodes_quickly_and_exactly(self, tmp_path, bit_depth, data):
        path = tmp_path / "silence.flac"
        t0 = time.monotonic()
        write_audio(str(path), data, 48000, bit_depth=bit_depth)
        assert time.monotonic() - t0 < 2.0
        out = np.asarray(read_audio(str(path), as_=np.ndarray)[0])
        silent = data == 0.0
        assert np.all(out[silent] == 0.0)
        # Rounded: at most half an LSB at either depth.
        atol = 0.5 / 2 ** (bit_depth - 1) + 1e-7
        np.testing.assert_allclose(out, data, atol=atol)


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

    def test_flac_stereo(self, tmp_path):
        # Use sine waves to stay within [-1, 1]
        t = np.linspace(0, 1, 1000, dtype=np.float32)
        data = np.stack(
            [0.5 * np.sin(2 * np.pi * 440 * t), 0.5 * np.sin(2 * np.pi * 880 * t)]
        )
        path = tmp_path / "stereo.flac"

        write_audio(path, data, 48000, bit_depth=24)
        result, sr = read_audio(path)

        assert sr == 48000
        assert result.shape == (2, 1000)
        np.testing.assert_allclose(result, data, atol=1.0 / 8388608 + 1e-6)

    def test_flac_mono(self, tmp_path):
        t = np.linspace(0, 1, 1000, dtype=np.float32)
        data = (0.5 * np.sin(2 * np.pi * 440 * t)).reshape(1, -1)
        path = tmp_path / "mono.flac"

        write_audio(path, data, 44100, bit_depth=24)
        result, sr = read_audio(path)

        assert sr == 44100
        assert result.shape == (1, 1000)
        np.testing.assert_allclose(result, data, atol=1.0 / 8388608 + 1e-6)


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
            write_audio(tmp_path / "test.mp3", data, 48000)

    def test_flac_32bit_raises(self, tmp_path):
        data = np.zeros((2, 100), dtype=np.float32)
        with pytest.raises(ValueError, match="32-bit"):
            write_audio(tmp_path / "test.flac", data, 48000, bit_depth=32)

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


class TestResample:
    """Test sample rate conversion."""

    def test_upsample_44100_to_48000(self):
        """Upsample 1 second of sine from 44.1k to 48k."""
        sr_in, sr_out = 44100, 48000
        t = np.arange(sr_in, dtype=np.float32) / sr_in
        data = (0.5 * np.sin(2 * np.pi * 440 * t)).reshape(1, -1).astype(np.float32)

        out = resample(data, sr_in, sr_out)
        assert out.shape[0] == 1
        assert out.shape[1] == sr_out
        assert out.dtype == np.float32
        # Peak should be preserved approximately
        assert abs(np.max(np.abs(out)) - 0.5) < 0.05

    def test_downsample_48000_to_44100(self):
        sr_in, sr_out = 48000, 44100
        t = np.arange(sr_in, dtype=np.float32) / sr_in
        data = (0.5 * np.sin(2 * np.pi * 440 * t)).reshape(1, -1).astype(np.float32)

        out = resample(data, sr_in, sr_out)
        assert out.shape[0] == 1
        assert out.shape[1] == sr_out

    def test_same_rate_is_identity(self):
        data = np.random.randn(2, 1000).astype(np.float32) * 0.5
        out = resample(data, 48000, 48000)
        assert out.shape == data.shape
        np.testing.assert_array_equal(out, data)

    def test_stereo(self):
        sr_in, sr_out = 44100, 48000
        data = np.random.randn(2, sr_in).astype(np.float32) * 0.3
        out = resample(data, sr_in, sr_out)
        assert out.shape[0] == 2
        assert out.shape[1] == sr_out

    def test_preserves_silence(self):
        data = np.zeros((1, 44100), dtype=np.float32)
        out = resample(data, 44100, 48000)
        assert np.max(np.abs(out)) == 0.0

    def test_large_ratio(self):
        """Resample from 8000 to 48000 (6x upsample)."""
        data = np.random.randn(1, 8000).astype(np.float32) * 0.3
        out = resample(data, 8000, 48000)
        assert out.shape[0] == 1
        assert out.shape[1] == 48000

    def test_round_trip_preserves_length(self):
        """44100 -> 48000 -> 44100 should produce approximately the same frame count."""
        data = np.random.randn(1, 44100).astype(np.float32) * 0.3
        up = resample(data, 44100, 48000)
        down = resample(up, 48000, 44100)
        # Allow small rounding differences
        assert abs(down.shape[1] - 44100) <= 2


class TestResampleBoundaries:
    """Extreme dimensions and sample-rate ratios.

    Frame counts, channel counts and sample rates all reach the C layer as
    unsigned ints, and the output length is the product of a frame count and
    an unbounded rate ratio. These cases pin the sizes that must be refused
    before any allocation or narrowing happens.
    """

    def test_zero_sample_rate_in_is_rejected(self):
        data = np.zeros((1, 100), dtype=np.float32)
        with pytest.raises(RuntimeError, match="Sample rates must be > 0"):
            resample(data, 0, 48000)

    def test_zero_sample_rate_out_is_rejected(self):
        data = np.zeros((1, 100), dtype=np.float32)
        with pytest.raises(RuntimeError, match="Sample rates must be > 0"):
            resample(data, 48000, 0)

    @pytest.mark.parametrize("sample_rate_out", [2**31, 2**32 - 1, 4_000_000_000])
    def test_extreme_upsample_ratio_is_refused(self, sample_rate_out):
        # 1000 frames at a ratio of 2**31 or more is over 2**41 output frames,
        # which cannot be represented in MH_AudioData.frames (unsigned int).
        # The estimate is range-checked in double, because converting an
        # out-of-range double to an unsigned integer type is undefined rather
        # than saturating.
        data = np.zeros((1, 1000), dtype=np.float32)
        with pytest.raises(RuntimeError, match="exceeds the maximum output length"):
            resample(data, 1, sample_rate_out)

    def test_sample_rate_above_unsigned_int_is_a_type_error(self):
        data = np.zeros((1, 100), dtype=np.float32)
        with pytest.raises(TypeError):
            resample(data, 1, 2**64)

    def test_extreme_downsample_ratio_yields_at_least_one_frame(self):
        # The mirror image: a ratio small enough to round the output to zero
        # frames still has to produce a valid buffer, not a zero-size one.
        data = np.zeros((1, 1000), dtype=np.float32)
        out = resample(data, 4_000_000_000, 1)
        assert out.shape[0] == 1
        assert out.shape[1] >= 1

    def test_single_frame_input(self):
        data = np.zeros((2, 1), dtype=np.float32)
        out = resample(data, 44100, 48000)
        assert out.shape[0] == 2

    def test_many_channels(self):
        # Output bytes are frames * channels * 4; a wide file exercises the
        # product rather than either factor.
        data = np.zeros((64, 1000), dtype=np.float32)
        out = resample(data, 44100, 48000)
        assert out.shape[0] == 64
        assert out.shape[1] == pytest.approx(1088, abs=2)


class TestExactIntegerWrites:
    """16- and 24-bit writes are rounded, not dithered, at the reader's scale.

    miniaudio's conversion used triangle dither from a global RNG (two writes
    of the same data differed; 16-bit-exact input moved by up to 2 LSB) and a
    2**(n-1) - 1 scale that the 2**(n-1) reader undid 1 LSB short.
    """

    @pytest.mark.parametrize("ext", ["wav", "flac"])
    @pytest.mark.parametrize("bit_depth", [16, 24])
    def test_on_grid_values_round_trip_exactly(self, tmp_path, ext, bit_depth):
        full = 2 ** (bit_depth - 1)
        step = 1 if bit_depth == 16 else 64  # every 64th 24-bit code
        codes = np.arange(-full, full, step, dtype=np.float64)
        data = (codes / full).astype(np.float32)[None, :]
        path = tmp_path / f"grid.{ext}"
        write_audio(str(path), data, 48000, bit_depth=bit_depth)
        first = path.read_bytes()
        write_audio(str(path), data, 48000, bit_depth=bit_depth)
        assert path.read_bytes() == first
        out, _ = read_audio(str(path), as_=np.ndarray)
        assert np.array_equal(np.asarray(out), data)

    @pytest.mark.parametrize("ext", ["wav", "flac"])
    @pytest.mark.parametrize("bit_depth", [16, 24])
    def test_nan_writes_zero_and_inf_clamps(self, tmp_path, ext, bit_depth):
        data = np.array([[np.nan, np.inf, -np.inf, 1.0, -1.0]], dtype=np.float32)
        path = tmp_path / f"edge.{ext}"
        write_audio(str(path), data, 48000, bit_depth=bit_depth)
        out = np.asarray(read_audio(str(path), as_=np.ndarray)[0])[0]
        top = 1.0 - 2.0 ** -(bit_depth - 1)
        np.testing.assert_array_equal(out, np.float32([0.0, top, -1.0, top, -1.0]))


class TestWriteValidation:
    @pytest.mark.parametrize(
        "shape, ext, rate, match",
        [
            ((300, 4), "wav", 48000, "WAV supports 1 to 254 channels"),
            ((9, 4), "flac", 48000, "FLAC supports 1 to 8 channels"),
            ((2, 4), "wav", 0, "WAV sample rate must be"),
            ((2, 4), "flac", 700000, "FLAC sample rate must be"),
        ],
    )
    def test_parameters_are_checked_up_front(self, tmp_path, shape, ext, rate, match):
        with pytest.raises(RuntimeError, match=match):
            write_audio(str(tmp_path / f"x.{ext}"), np.zeros(shape, np.float32), rate)

    @pytest.mark.parametrize(
        "bwf, exc, match",
        [
            ({"time_reference": -1}, ValueError, "time_reference"),
            ({"time_reference": 1.5}, ValueError, "time_reference"),
            ({"time_reference": 2**64}, ValueError, "time_reference"),
            ({"description": 5}, TypeError, "description"),
            ({"orginator": "typo"}, ValueError, "unknown bwf key: orginator"),
            (["not", "a", "dict"], TypeError, "must be a dict"),
        ],
    )
    def test_bwf_arguments_are_checked(self, tmp_path, bwf, exc, match):
        with pytest.raises(exc, match=match):
            write_audio(
                str(tmp_path / "x.wav"), np.zeros((1, 4), np.float32), 48000, bwf=bwf
            )


def test_header_declaring_zero_hz_is_rejected(tmp_path):
    path = tmp_path / "sr0.flac"
    write_audio(str(path), np.zeros((2, 64), np.float32), 48000, bit_depth=16)
    raw = bytearray(path.read_bytes())
    # STREAMINFO starts at byte 8; the 20-bit rate occupies bytes 18-20.
    raw[18] = 0
    raw[19] = 0
    raw[20] &= 0x0F
    path.write_bytes(bytes(raw))
    with pytest.raises(RuntimeError, match="at 0 Hz"):
        read_audio(str(path))
    with pytest.raises(RuntimeError, match="at 0 Hz"):
        get_audio_info(str(path))


def test_magnitude_reports_nan_and_normalize_refuses_it():
    # JUCE's scan skipped NaN: NaN at frame 0 reported 0.0, so normalisation
    # silently did nothing.
    from minihost import AudioBuffer
    from minihost.process import _normalize_peak

    for row in ([np.nan, 0.5], [0.25, np.nan]):
        buf = AudioBuffer.from_numpy(np.array([row], np.float32))
        assert np.isnan(buf.magnitude())
        with pytest.raises(ValueError, match="NaN or inf"):
            _normalize_peak(buf, 0.0)


@pytest.mark.parametrize("tail", [-1.0, float("nan"), float("inf")])
def test_tail_seconds_must_be_finite_and_non_negative(tail):
    # render_midi truncated the MIDI by a negative tail, process_audio clamped
    # it to 0, Compose raised: one check now serves all three.
    from minihost.process import _check_tail_seconds

    with pytest.raises(ValueError, match="tail_seconds must be >= 0"):
        _check_tail_seconds(tail)
