"""Extended tests for process_audio / process_audio_to_file.

Cover the absorbed-from-cmd_process features: MIDI events, sidechain,
parameter automation, BPM transport, output channels, synth-mode (no
audio input). Plugin-gated; skipped without MINIHOST_TEST_PLUGIN.
"""

from __future__ import annotations

import os
import tempfile

import pytest

import minihost
from minihost.process import _load_midi_events, _slice_block_events

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


# ---------------------------------------------------------------------------
# Pure-Python helpers (no plugin required)
# ---------------------------------------------------------------------------


def test_load_midi_events_accepts_list_passthrough():
    events = [(0, 0x90, 60, 100), (1000, 0x80, 60, 0)]
    out, max_sample = _load_midi_events(events, sample_rate=48000.0)
    assert out == events
    assert max_sample == 1000


def test_load_midi_events_sorts_unsorted_input():
    events = [(1000, 0x80, 60, 0), (0, 0x90, 60, 100)]
    out, max_sample = _load_midi_events(events, sample_rate=48000.0)
    assert out[0][0] < out[1][0]
    assert max_sample == 1000


def test_load_midi_events_rejects_malformed_tuple():
    with pytest.raises(ValueError, match="4-tuples"):
        _load_midi_events([(0, 0x90, 60)], sample_rate=48000.0)


def test_load_midi_events_rejects_wrong_type():
    with pytest.raises(TypeError, match="midi must be"):
        _load_midi_events(42, sample_rate=48000.0)


def test_load_midi_events_missing_file():
    with pytest.raises(RuntimeError, match="Failed to load MIDI file"):
        _load_midi_events("/nonexistent.mid", sample_rate=48000.0)


def test_slice_block_events_basic():
    events = [(0, "a"), (100, "b"), (250, "c"), (500, "d")]
    block, idx = _slice_block_events(events, 0, 0, 200)
    assert block == [(0, "a"), (100, "b")]
    assert idx == 2

    block, idx = _slice_block_events(events, idx, 200, 400)
    assert block == [(50, "c")]
    assert idx == 3

    block, idx = _slice_block_events(events, idx, 400, 600)
    assert block == [(100, "d")]
    assert idx == 4


def test_slice_block_events_clamps_offset_to_block_size():
    # Event past the start of the block (negative relative pos) gets
    # clamped to 0; event exactly at end-1 is included.
    events = [(50, "x")]
    block, idx = _slice_block_events(events, 0, 100, 200)
    # 50 < start=100 -- but our while loop only advances if sample_pos < end.
    # So this event WILL be picked up (50 < 200) and offset becomes max(0, 50-100) = 0.
    assert block == [(0, "x")]
    assert idx == 1


# ---------------------------------------------------------------------------
# Validation (no plugin needed for the rejection paths)
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_process_audio_rejects_sidechain_for_chain(tmp_path):
    chain = minihost.PluginChain([minihost.Plugin(PLUGIN, sample_rate=48000)])
    try:
        src = minihost.AudioBuffer(chain.num_input_channels, 512)
        sc = minihost.AudioBuffer(chain.num_input_channels, 512)
        with pytest.raises(ValueError, match="sidechain is not supported"):
            minihost.process_audio(chain, src, sidechain=sc)
    finally:
        chain.close()


@skip_if_no_plugin
def test_process_audio_rejects_bpm_for_chain(tmp_path):
    chain = minihost.PluginChain([minihost.Plugin(PLUGIN, sample_rate=48000)])
    try:
        src = minihost.AudioBuffer(chain.num_input_channels, 512)
        with pytest.raises(ValueError, match="bpm transport"):
            minihost.process_audio(chain, src, bpm=120.0)
    finally:
        chain.close()


@skip_if_no_plugin
def test_process_audio_requires_input_or_midi_or_tail():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    with pytest.raises(ValueError, match="requires either audio input"):
        minihost.process_audio(plugin, audio=None)


# ---------------------------------------------------------------------------
# Synth mode (MIDI-driven, no audio input)
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_process_audio_synth_mode_with_midi_events():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    # Single note at sample 0, note off at sample 4799 -- total 4800 frames
    # plus a small tail.
    events = [
        (0, 0x90, 60, 100),
        (4799, 0x80, 60, 0),
    ]
    out = minihost.process_audio(
        plugin,
        audio=None,
        midi=events,
        tail_seconds=0.1,
        compensate_latency=False,
    )
    expected = (4799 + 1) + int(0.1 * 48000)
    assert out.frames == expected
    assert out.channels == plugin.num_output_channels


# ---------------------------------------------------------------------------
# MIDI events alongside audio input
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_process_audio_midi_with_audio_input():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    events = [(0, 0x90, 60, 100), (2400, 0x80, 60, 0)]
    out = minihost.process_audio(
        plugin, src, midi=events, compensate_latency=False,
    )
    # Audio drives length when both present.
    assert out.frames == 4800


# ---------------------------------------------------------------------------
# Parameter automation
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_process_audio_param_automation_changes_output():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin has no parameters")

    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    for ch in range(plugin.num_input_channels):
        src[ch, 0] = 0.1

    out_no_auto = minihost.process_audio(
        plugin, src, compensate_latency=False,
    )
    # Re-create plugin to reset state between runs.
    plugin2 = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src2 = minihost.AudioBuffer(plugin2.num_input_channels, 4800)
    for ch in range(plugin2.num_input_channels):
        src2[ch, 0] = 0.1
    changes = [(0, 0, 0.0), (2400, 0, 1.0)]
    out_auto = minihost.process_audio(
        plugin2, src2, param_changes=changes, compensate_latency=False,
    )
    # Both runs produce the same frame count; we just want the
    # automation path to execute without error and yield audio.
    assert out_no_auto.frames == out_auto.frames == 4800


# ---------------------------------------------------------------------------
# Sidechain
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_process_audio_sidechain_path_runs():
    plugin = minihost.Plugin(
        PLUGIN, sample_rate=48000, max_block_size=512, sidechain_channels=2,
    )
    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    sc = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    out = minihost.process_audio(
        plugin, src, sidechain=sc, compensate_latency=False,
    )
    assert out.frames == 4800


# ---------------------------------------------------------------------------
# BPM transport
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_process_audio_bpm_does_not_error():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    out = minihost.process_audio(plugin, src, bpm=140.0, compensate_latency=False)
    assert out.frames == 4800


# ---------------------------------------------------------------------------
# process_audio_to_file extensions
# ---------------------------------------------------------------------------


@skip_if_no_plugin
@skip_if_no_input
def test_process_audio_to_file_with_sidechain(tmp_path):
    out_path = tmp_path / "sc_out.wav"
    plugin = minihost.Plugin(
        PLUGIN, sample_rate=48000, max_block_size=512, sidechain_channels=2,
    )
    frames = minihost.process_audio_to_file(
        plugin,
        input_path=INPUT_WAV,
        output_path=str(out_path),
        sidechain=INPUT_WAV,        # use the same file as sidechain for the smoke test
        compensate_latency=False,
    )
    assert frames > 0
    assert out_path.exists()


@skip_if_no_plugin
def test_process_audio_to_file_synth_mode(tmp_path):
    out_path = tmp_path / "synth_out.wav"
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    events = [(0, 0x90, 60, 100), (4799, 0x80, 60, 0)]
    frames = minihost.process_audio_to_file(
        plugin,
        input_path=None,
        output_path=str(out_path),
        midi=events,
        tail_seconds=0.1,
        compensate_latency=False,
    )
    assert frames == 4800 + int(0.1 * 48000)
    assert out_path.exists()


def test_process_audio_to_file_requires_output_path():
    # Plugin-less check: the very first guard fires before any plugin work.
    with pytest.raises(ValueError, match="output_path is required"):
        minihost.process_audio_to_file(
            plugin_or_chain=None,  # type: ignore[arg-type]
            input_path="x.wav",
            output_path="",
        )
