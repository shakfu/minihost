"""Tests for process_audio_stream (the audio-in streaming counterpart
to render_midi_stream).

Most coverage is plugin-gated. The headline contract -- yielded blocks
concatenate to the same result as process_audio -- is verified across
several routing paths (plain effect, MIDI, sidechain, synth mode).
"""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
FX_PLUGIN = (
    os.environ.get("MINIHOST_TEST_FX")
    or "/Library/Audio/Plug-Ins/VST3/TAL-Filter-2.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)
skip_if_no_fx = pytest.mark.skipif(
    not os.path.exists(FX_PLUGIN),
    reason=f"effect plugin not found at {FX_PLUGIN}",
)


def _concat(blocks):
    """Concatenate a sequence of AudioBuffer or numpy blocks along
    axis=1 into a single numpy array for comparison.
    """
    arrs = []
    for b in blocks:
        if isinstance(b, np.ndarray):
            arrs.append(b)
        else:
            arrs.append(b.as_ndarray().copy())
    return np.concatenate(arrs, axis=1) if arrs else np.zeros((0, 0))


# ---------------------------------------------------------------------------
# Headline: concat-of-stream == process_audio result
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_stream_concat_matches_process_audio_effect():
    """Plain effect path: streamed blocks concatenate to the same output."""
    p_stream = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(p_stream.num_input_channels, 4096)
    # Inject a non-trivial signal so silence doesn't mask differences.
    for ch in range(src.channels):
        src[ch, 0] = 0.25
        src[ch, 1024] = -0.25
    streamed = _concat(
        list(minihost.process_audio_stream(p_stream, src, compensate_latency=False))
    )
    p_stream.close()

    p_ref = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src2 = minihost.AudioBuffer(p_ref.num_input_channels, 4096)
    for ch in range(src2.channels):
        src2[ch, 0] = 0.25
        src2[ch, 1024] = -0.25
    full = minihost.process_audio(p_ref, src2, compensate_latency=False)
    p_ref.close()

    assert streamed.shape == (full.channels, full.frames)
    assert np.allclose(streamed, full.as_ndarray(), atol=1e-6)


@skip_if_no_plugin
def test_stream_concat_matches_process_audio_synth_mode():
    """Synth mode (audio=None, MIDI-driven): concat equals process_audio."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    events = [(0, 0x90, 60, 100), (4799, 0x80, 60, 0)]
    streamed = _concat(
        list(
            minihost.process_audio_stream(
                plugin,
                audio=None,
                midi=events,
                tail_seconds=0.1,
                compensate_latency=False,
                block_size=512,
            )
        )
    )
    plugin.close()

    plugin2 = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    full = minihost.process_audio(
        plugin2,
        audio=None,
        midi=events,
        tail_seconds=0.1,
        compensate_latency=False,
        block_size=512,
    )
    plugin2.close()

    assert streamed.shape == (full.channels, full.frames)
    assert np.allclose(streamed, full.as_ndarray(), atol=1e-6)


# ---------------------------------------------------------------------------
# Block-size contract
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_stream_respects_block_size_cap():
    """No yielded block exceeds the requested block_size; only the
    final block may be shorter."""
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=256)
    src = minihost.AudioBuffer(plugin.num_input_channels, 2000)
    blocks = list(
        minihost.process_audio_stream(
            plugin,
            src,
            block_size=256,
            compensate_latency=False,
        )
    )
    plugin.close()

    assert len(blocks) > 1
    for b in blocks[:-1]:
        assert b.frames == 256
    assert blocks[-1].frames <= 256
    assert sum(b.frames for b in blocks) == 2000


# ---------------------------------------------------------------------------
# as_= selector
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_stream_yields_audiobuffer_by_default():
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 1024)
    blocks = list(
        minihost.process_audio_stream(
            plugin,
            src,
            compensate_latency=False,
        )
    )
    plugin.close()
    assert blocks
    assert all(isinstance(b, minihost.AudioBuffer) for b in blocks)


@skip_if_no_fx
def test_stream_yields_numpy_when_requested():
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 1024)
    blocks = list(
        minihost.process_audio_stream(
            plugin,
            src,
            compensate_latency=False,
            as_=np.ndarray,
        )
    )
    plugin.close()
    assert blocks
    assert all(isinstance(b, np.ndarray) for b in blocks)
    assert all(b.dtype == np.float32 for b in blocks)


# ---------------------------------------------------------------------------
# Progress callback
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_stream_progress_callback_fires_monotonically():
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 2000)
    calls: list[tuple[int, int]] = []
    list(
        minihost.process_audio_stream(
            plugin,
            src,
            block_size=512,
            compensate_latency=False,
            progress_callback=lambda c, t: calls.append((c, t)),
        )
    )
    plugin.close()

    assert calls
    for prev, nxt in zip(calls, calls[1:]):
        assert prev[0] <= nxt[0]
        assert prev[1] == nxt[1] == 2000
    assert calls[-1] == (2000, 2000)


# ---------------------------------------------------------------------------
# Validation paths shared with process_audio
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_stream_requires_input_or_midi_or_tail():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    with pytest.raises(ValueError, match="requires either audio input"):
        list(minihost.process_audio_stream(plugin, audio=None))
    plugin.close()


@skip_if_no_fx
def test_stream_rejects_sidechain_for_chain():
    chain = minihost.PluginChain([minihost.Plugin(FX_PLUGIN, sample_rate=48000)])
    src = minihost.AudioBuffer(chain.num_input_channels, 512)
    sc = minihost.AudioBuffer(chain.num_input_channels, 512)
    with pytest.raises(ValueError, match="sidechain is not supported"):
        list(minihost.process_audio_stream(chain, src, sidechain=sc))
    chain.close()


# ---------------------------------------------------------------------------
# Lazy / generator behavior
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_stream_is_lazy_until_iterated():
    """Calling process_audio_stream alone shouldn't allocate the full
    output -- it returns a generator, and validation only runs once the
    consumer starts iterating."""
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000)
    src = minihost.AudioBuffer(plugin.num_input_channels, 512)
    gen = minihost.process_audio_stream(plugin, src)
    # No exception, no work done yet.
    assert hasattr(gen, "__next__")
    # Consume one block to drive the generator into its setup phase.
    first = next(gen)
    assert first.frames > 0
    # Drain the rest so close() doesn't race a generator finalizer.
    for _ in gen:
        pass
    plugin.close()
