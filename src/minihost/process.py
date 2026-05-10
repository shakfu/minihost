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

These functions accept ``AudioBuffer``, numpy ndarray, or any 2D float32
c-contiguous buffer-protocol producer as input. numpy is not required
for the AudioBuffer-only path; it is lazy-imported when needed (e.g.
when the caller passes a numpy array).
"""

from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Any, Union, cast

from minihost._core import AudioBuffer, Plugin, PluginChain
from minihost.audio_io import read_audio, resample, write_audio

if TYPE_CHECKING:
    import numpy as np
    PluginOrChain = Union[Plugin, PluginChain]


def _resolve_block_size(block_size: int | None) -> int:
    if block_size is not None:
        return int(block_size)
    # Conservative default. Plugin.max_block_size isn't queryable
    # post-construction; PluginChain has no public max_block_size.
    return 512


def _to_audiobuffer(audio: Any, in_ch_required: int) -> AudioBuffer:
    """Coerce an input to an AudioBuffer (one copy if not already one)."""
    if isinstance(audio, AudioBuffer):
        if audio.channels < in_ch_required:
            raise ValueError(
                f"Source audio has {audio.channels} channel(s) but the "
                f"chain requires at least {in_ch_required}."
            )
        return audio
    # Anything else (numpy ndarray, etc.) -- delegate to AudioBuffer's
    # buffer-protocol-based constructor. This requires numpy only if the
    # input itself is numpy; AudioBuffer.from_numpy works for any 2D
    # float32 c-contig source.
    buf = AudioBuffer.from_numpy(audio)
    if buf.channels < in_ch_required:
        raise ValueError(
            f"Source audio has {buf.channels} channel(s) but the chain "
            f"requires at least {in_ch_required}."
        )
    return buf


def process_audio(
    plugin_or_chain: "PluginOrChain",
    audio: Any,
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
    block = _resolve_block_size(block_size)
    sample_rate = float(plugin_or_chain.sample_rate)
    in_ch_required = plugin_or_chain.num_input_channels
    out_ch = plugin_or_chain.num_output_channels

    src = _to_audiobuffer(audio, in_ch_required)
    src_frames = src.frames
    tail_frames = max(0, int(tail_seconds * sample_rate))

    latency = int(plugin_or_chain.latency_samples) if compensate_latency else 0
    if latency < 0:
        latency = 0
    out_frames = src_frames + tail_frames               # user-visible
    render_frames = out_frames + latency                # internal

    output = AudioBuffer(out_ch, out_frames)
    in_block = AudioBuffer(in_ch_required, block)
    out_block = AudioBuffer(out_ch, block)

    skip_remaining = latency
    written = 0  # samples placed into output (post-skip)

    for start in range(0, render_frames, block):
        n = min(block, render_frames - start)

        # Stage input: source if available, else silence.
        in_block.clear()
        if start < src_frames:
            copy = min(n, src_frames - start)
            in_block[:in_ch_required, :copy] = src[:in_ch_required, start:start + copy]

        # Process exactly `n` frames. If n < block, allocate a partial-
        # size buffer pair (avoids the plugin processing trailing zeros).
        if n == block:
            plugin_or_chain.process(in_block, out_block)
            produced: AudioBuffer = out_block
        else:
            sub_in = AudioBuffer(in_ch_required, n)
            sub_in[:in_ch_required, :n] = in_block[:in_ch_required, :n]
            sub_out = AudioBuffer(out_ch, n)
            plugin_or_chain.process(sub_in, sub_out)
            produced = sub_out

        # Latency-compensation skip.
        if skip_remaining >= n:
            skip_remaining -= n
            continue
        if skip_remaining > 0:
            produced = cast(AudioBuffer, produced[:, skip_remaining:])
            skip_remaining = 0

        # Append to output (clamped).
        emit = produced.frames
        if written + emit > out_frames:
            emit = out_frames - written
            if emit <= 0:
                break
            produced = cast(AudioBuffer, produced[:, :emit])
        output[:, written:written + emit] = produced
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
    audio, in_sr = read_audio(input_path)  # AudioBuffer, sr
    plugin_sr = int(plugin_or_chain.sample_rate)

    if in_sr != plugin_sr:
        if not resample_to_plugin_rate:
            raise ValueError(
                f"Input is {in_sr} Hz but plugin is {plugin_sr} Hz. "
                f"Pass resample_to_plugin_rate=True to convert automatically."
            )
        audio = resample(audio, in_sr, plugin_sr)  # AudioBuffer in -> AudioBuffer out

    in_ch_required = plugin_or_chain.num_input_channels
    src: AudioBuffer = audio if isinstance(audio, AudioBuffer) else AudioBuffer.from_numpy(audio)

    if src.channels < in_ch_required:
        if not duplicate_to_stereo:
            raise ValueError(
                f"Input has {src.channels} channel(s) but plugin needs "
                f"{in_ch_required}. Pass duplicate_to_stereo=True to "
                f"channel-duplicate automatically."
            )
        # Build a fresh AudioBuffer with the required channel count and
        # copy the last source channel into each missing slot.
        expanded = AudioBuffer(in_ch_required, src.frames)
        # Copy existing channels.
        expanded[:src.channels, :] = src
        # Duplicate the last channel into the remaining slots.
        last = cast(AudioBuffer, src[src.channels - 1: src.channels, :])
        for ch in range(src.channels, in_ch_required):
            expanded[ch:ch + 1, :] = last
        src = expanded

    output = process_audio(
        plugin_or_chain,
        src,
        tail_seconds=tail_seconds,
        block_size=block_size,
        compensate_latency=compensate_latency,
    )
    write_audio(output_path, output, plugin_sr, bit_depth=bit_depth)
    return output.frames
