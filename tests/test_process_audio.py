"""Tests for the high-level process_audio / process_audio_to_file helpers."""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost

PLUGIN = os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
INPUT_WAV = "tests/_wav/piano.wav"

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)
skip_if_no_input = pytest.mark.skipif(
    not os.path.exists(INPUT_WAV),
    reason=f"test wav not found at {INPUT_WAV}",
)


@skip_if_no_plugin
def test_process_audio_returns_audiobuffer_of_expected_shape():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)  # 0.1s
    out = minihost.process_audio(plugin, src, tail_seconds=0.0,
                                  compensate_latency=False)
    assert isinstance(out, minihost.AudioBuffer)
    assert out.channels == plugin.num_output_channels
    assert out.frames == src.frames


@skip_if_no_plugin
def test_process_audio_with_tail_extends_output():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    out_no_tail = minihost.process_audio(plugin, src, tail_seconds=0.0,
                                          compensate_latency=False)
    out_tail = minihost.process_audio(plugin, src, tail_seconds=0.5,
                                       compensate_latency=False)
    assert out_tail.frames - out_no_tail.frames == int(0.5 * 48000)


@skip_if_no_plugin
def test_process_audio_accepts_numpy_input():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src = np.zeros((plugin.num_input_channels, 4800), dtype=np.float32)
    out = minihost.process_audio(plugin, src, compensate_latency=False)
    assert isinstance(out, minihost.AudioBuffer)
    assert out.frames == 4800


@skip_if_no_plugin
def test_process_audio_rejects_too_few_channels():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_input_channels < 2:
        pytest.skip("plugin has 1 input channel; cannot test undersize")
    src = np.zeros((plugin.num_input_channels - 1, 4800), dtype=np.float32)
    with pytest.raises(ValueError, match="channel"):
        minihost.process_audio(plugin, src)


@skip_if_no_plugin
@skip_if_no_input
def test_process_audio_to_file_roundtrip(tmp_path):
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    out_path = tmp_path / "out.wav"
    n = minihost.process_audio_to_file(
        plugin, INPUT_WAV, str(out_path),
        tail_seconds=0.5,
        compensate_latency=False,
    )
    assert n > 0
    assert out_path.exists()
    # Verify we can read it back.
    data, sr = minihost.read_audio(str(out_path))
    assert sr == 48000
    assert data.frames == n


@skip_if_no_input
def test_process_audio_to_file_resamples_when_rates_differ(tmp_path):
    # Input is 22050 Hz mono; plugin is 48000 Hz. The helper must
    # resample to match the plugin rate.
    if not os.path.exists(PLUGIN):
        pytest.skip("plugin not available")
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    out_path = tmp_path / "out.wav"
    minihost.process_audio_to_file(
        plugin, INPUT_WAV, str(out_path),
        tail_seconds=0.0,
        compensate_latency=False,
    )
    info = minihost.get_audio_info(str(out_path))
    assert info["sample_rate"] == 48000


@skip_if_no_plugin
@skip_if_no_input
def test_process_audio_to_file_duplicates_mono_to_required_channels(tmp_path):
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    out_path = tmp_path / "out.wav"
    # piano.wav is mono; plugin may need >1 input channel.
    minihost.process_audio_to_file(
        plugin, INPUT_WAV, str(out_path),
        tail_seconds=0.0,
        compensate_latency=False,
    )
    info = minihost.get_audio_info(str(out_path))
    assert info["channels"] == plugin.num_output_channels
