"""High-level offline audio processing through plugin chains.

Functions in this module collapse the typical block-iteration boilerplate
required to run audio through a plugin or chain. For the file-to-file
case use :func:`process_audio_to_file`; for in-memory data use
:func:`process_audio`.

Both functions handle:
  * Block-by-block iteration sized to the plugin's max block size.
  * Tail rendering -- the input is zero-padded past the source duration
    by ``tail_seconds`` so reverb / delay tails are captured.
  * Latency compensation -- when ``compensate_latency=True`` (the
    default) the renderer feeds an extra ``latency_samples`` of input
    and discards the matching number of output samples, so the returned
    audio is time-aligned with the source.
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Union

import numpy as np

from minihost._core import AudioBuffer, Plugin, PluginChain
from minihost.audio_io import read_audio, resample, write_audio

if TYPE_CHECKING:
    PluginOrChain = Union[Plugin, PluginChain]


def _max_block_size(target: "PluginOrChain") -> int:
    """Resolve the largest block size that target.process can accept."""
    # Plugin exposes max_block_size only at construction time; PluginChain
    # has no public max_block_size and falls back to the chain's internal
    # 8192 cap (see minihost_chain.cpp). Use the conservative 512 default
    # if neither is queryable.
    return 512


def _resolve_block_size(target: "PluginOrChain", block_size: int | None) -> int:
    if block_size is not None:
        return int(block_size)
    return _max_block_size(target)


def process_audio(
    plugin_or_chain: "PluginOrChain",
    audio: "AudioBuffer | np.ndarray",
    tail_seconds: float = 0.0,
    block_size: int | None = None,
    compensate_latency: bool = True,
) -> AudioBuffer:
    """Process audio through a plugin or chain.

    Args:
        plugin_or_chain: A :class:`minihost.Plugin` or
            :class:`minihost.PluginChain`.
        audio: Source audio of shape ``(channels, frames)``. Accepts an
            :class:`AudioBuffer`, a numpy ndarray, or any 2D float32
            c-contiguous buffer-protocol producer.
        tail_seconds: Extra silent input rendered after the source so
            reverb / delay tails are captured. Defaults to 0; pass a
            positive value when the chain has a tail.
        block_size: Audio block size used for the internal process loop.
            Defaults to a conservative value compatible with the plugin's
            max block size.
        compensate_latency: When True (default), render
            ``plugin.latency_samples`` extra frames at the end and discard
            the matching number of frames from the start so the returned
            audio is time-aligned with the source.

    Returns:
        A new :class:`AudioBuffer` of shape
        ``(plugin.num_output_channels, source_frames + tail_frames)``.
    """
    block = _resolve_block_size(plugin_or_chain, block_size)
    sample_rate = float(plugin_or_chain.sample_rate)
    in_ch_required = plugin_or_chain.num_input_channels
    out_ch = plugin_or_chain.num_output_channels

    # Coerce input to a numpy view so we can slice rows / frames freely.
    # AudioBuffer satisfies np.asarray via DLPack / __array__.
    src = np.ascontiguousarray(audio, dtype=np.float32)
    if src.ndim == 1:
        src = src.reshape(1, -1)
    if src.shape[0] < in_ch_required:
        raise ValueError(
            f"Source audio has {src.shape[0]} channel(s) but the chain "
            f"requires at least {in_ch_required}."
        )
    src_frames = src.shape[1]
    tail_frames = max(0, int(tail_seconds * sample_rate))

    latency = int(plugin_or_chain.latency_samples) if compensate_latency else 0
    if latency < 0:
        latency = 0
    out_frames = src_frames + tail_frames               # user-visible
    render_frames = out_frames + latency                # internal

    output = AudioBuffer(out_ch, out_frames)
    out_view = output.numpy()
    in_block = AudioBuffer(in_ch_required, block).numpy()
    out_block = AudioBuffer(out_ch, block).numpy()

    skip_remaining = latency
    written = 0  # samples placed into output (post-skip)

    for start in range(0, render_frames, block):
        n = min(block, render_frames - start)

        # Stage input: source if available, else silence.
        in_block[:] = 0
        if start < src_frames:
            copy = min(n, src_frames - start)
            in_block[:in_ch_required, :copy] = src[:in_ch_required, start:start + copy]

        # Process exactly `n` frames. If n < block, slice both buffers.
        if n == block:
            plugin_or_chain.process(in_block, out_block)
            produced = out_block
        else:
            sub_in = np.ascontiguousarray(in_block[:, :n])
            sub_out = np.zeros((out_ch, n), dtype=np.float32)
            plugin_or_chain.process(sub_in, sub_out)
            produced = sub_out

        # Latency-compensation skip.
        if skip_remaining >= n:
            skip_remaining -= n
            continue
        if skip_remaining > 0:
            produced = produced[:, skip_remaining:]
            skip_remaining = 0

        # Append to output (clamped).
        emit = produced.shape[1]
        if written + emit > out_frames:
            emit = out_frames - written
            if emit <= 0:
                break
            produced = produced[:, :emit]
        out_view[:, written:written + emit] = produced
        written += emit

    return output


def process_audio_to_file(
    plugin_or_chain: "PluginOrChain",
    input_path: str | Path,
    output_path: str | Path,
    tail_seconds: float = 0.0,
    block_size: int | None = None,
    bit_depth: int = 24,
    resample_to_plugin_rate: bool = True,
    duplicate_to_stereo: bool = True,
    compensate_latency: bool = True,
) -> int:
    """Process an audio file through a plugin or chain and write the result.

    Args:
        plugin_or_chain: Plugin or PluginChain.
        input_path: Source audio file (any format readable by
            :func:`read_audio`).
        output_path: Destination file (.wav or .flac).
        tail_seconds: See :func:`process_audio`.
        block_size: See :func:`process_audio`.
        bit_depth: See :func:`write_audio`.
        resample_to_plugin_rate: If True (default), the source is
            resampled to the plugin's configured sample rate when they
            differ.
        duplicate_to_stereo: If True (default), a mono source is
            channel-duplicated to stereo when the chain expects more
            input channels than the source provides.
        compensate_latency: See :func:`process_audio`.

    Returns:
        Number of frames written.
    """
    audio, in_sr = read_audio(input_path)
    plugin_sr = int(plugin_or_chain.sample_rate)

    if in_sr != plugin_sr:
        if not resample_to_plugin_rate:
            raise ValueError(
                f"Input is {in_sr} Hz but plugin is {plugin_sr} Hz. "
                f"Pass resample_to_plugin_rate=True to convert automatically."
            )
        audio = resample(audio, in_sr, plugin_sr)

    in_ch_required = plugin_or_chain.num_input_channels
    src_arr = np.asarray(audio, dtype=np.float32)
    if src_arr.shape[0] < in_ch_required:
        if not duplicate_to_stereo:
            raise ValueError(
                f"Input has {src_arr.shape[0]} channel(s) but plugin needs "
                f"{in_ch_required}. Pass duplicate_to_stereo=True to "
                f"channel-duplicate automatically."
            )
        # Duplicate the last channel to fill the missing channels.
        last = src_arr[-1:, :]
        extras = np.repeat(last, in_ch_required - src_arr.shape[0], axis=0)
        src_arr = np.concatenate([src_arr, extras], axis=0)

    output = process_audio(
        plugin_or_chain,
        src_arr,
        tail_seconds=tail_seconds,
        block_size=block_size,
        compensate_latency=compensate_latency,
    )
    write_audio(output_path, output, plugin_sr, bit_depth=bit_depth)
    return output.frames
