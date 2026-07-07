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
  * Optional MIDI events, sidechain input, parameter automation, BPM
    transport, and output-channel override -- collectively absorbing
    the workload that used to live in ``minihost process``'s bespoke
    block loop.

These functions accept ``AudioBuffer``, numpy ndarray, or any 2D float32
c-contiguous buffer-protocol producer as input. numpy is not required
for the AudioBuffer-only path; it is lazy-imported when needed (e.g.
when the caller passes a numpy array).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING, Any, Callable, Iterator, Sequence, Union, cast

from minihost._core import AudioBuffer, MidiFile, Plugin, PluginChain
from minihost.audio_io import read_audio, resample, write_audio

if TYPE_CHECKING:
    import numpy as np
    PluginOrChain = Union[Plugin, PluginChain]

ProgressCallback = Callable[[int, int], None]

MidiEvent = tuple[int, int, int, int]
ParamChangePlugin = tuple[int, int, float]            # (sample, param_idx, value)
ParamChangeChain = tuple[int, int, int, float]        # (sample, plugin_idx, param_idx, value)
MidiInput = Union[str, Path, MidiFile, Sequence[MidiEvent]]


def _normalize_peak(buf: AudioBuffer, target_dbfs: float) -> None:
    """Peak-normalize ``buf`` in place to ``target_dbfs`` dBFS.

    ``target_dbfs`` is the desired output peak in dBFS (0 = full scale).
    Silent buffers are left untouched.
    """
    peak = buf.magnitude()
    if peak <= 0.0:
        return
    target_linear = 10.0 ** (target_dbfs / 20.0)
    buf.apply_gain(target_linear / peak)


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
    buf = AudioBuffer.from_numpy(audio)
    if buf.channels < in_ch_required:
        raise ValueError(
            f"Source audio has {buf.channels} channel(s) but the chain "
            f"requires at least {in_ch_required}."
        )
    return buf


def _load_midi_events(
    midi: MidiInput, sample_rate: float,
) -> tuple[list[MidiEvent], int]:
    """Resolve ``midi`` (file path, MidiFile, or list of events) into a
    sorted ``(events_by_sample, max_sample)`` pair.

    Event tuples are ``(sample_offset, status, data1, data2)``.
    """
    # Already-resolved event list -- pass through after sort.
    if isinstance(midi, (list, tuple)) and (not midi or not isinstance(midi[0], (MidiFile, str, Path))):
        events = list(midi)
        for ev in events:
            if not (isinstance(ev, tuple) and len(ev) == 4):
                raise ValueError(
                    "MIDI events must be 4-tuples of "
                    "(sample_offset, status, data1, data2)."
                )
        events.sort(key=lambda e: e[0])
        return events, (events[-1][0] if events else 0)

    # Lazy import to avoid pulling MIDI parsing helpers into the
    # AudioBuffer-only path.
    from minihost.render import (
        _build_tempo_map,
        _collect_midi_events,
        _event_to_midi_tuple,
        _seconds_to_samples,
        _tick_to_seconds,
    )

    if isinstance(midi, (str, Path)):
        mf = MidiFile()
        if not mf.load(str(midi)):
            raise RuntimeError(f"Failed to load MIDI file: {midi}")
    elif isinstance(midi, MidiFile):
        mf = midi
    else:
        raise TypeError(
            f"midi must be a file path, MidiFile, or list of event tuples, "
            f"got {type(midi).__name__}."
        )

    tpq = mf.ticks_per_quarter
    tempo_map = _build_tempo_map(mf)
    raw = _collect_midi_events(mf)

    out: list[MidiEvent] = []
    max_sample = 0
    for event in raw:
        seconds = _tick_to_seconds(event["tick"], tempo_map, tpq)
        sample_pos = _seconds_to_samples(seconds, sample_rate)
        tup = _event_to_midi_tuple(event, sample_pos)
        if tup:
            out.append(tup)
            if sample_pos > max_sample:
                max_sample = sample_pos
    out.sort(key=lambda e: e[0])
    return out, max_sample


def _slice_block_events(
    events: Sequence[tuple], idx: int, start: int, end: int,
    *, drop_leading: int = 0,
) -> tuple[list[tuple], int]:
    """Collect ``events`` whose absolute sample position lies in ``[start, end)``.

    Returns ``(block_events, next_idx)`` where each ``block_events`` entry
    has its first element rewritten to a block-relative offset clamped to
    ``[0, end - start - 1]``. ``drop_leading`` skips that many leading
    fields when computing offsets (unused here -- kept for symmetry).
    """
    block: list[tuple] = []
    bsize = end - start
    while idx < len(events):
        sample_pos = events[idx][0]
        if sample_pos >= end:
            break
        offset = max(0, min(sample_pos - start, bsize - 1))
        rest = events[idx][1:]
        block.append((offset,) + rest)
        idx += 1
    return block, idx


@dataclass
class _RenderContext:
    """Internal state shared between ``process_audio`` and
    ``process_audio_stream``. Produced by :func:`_prepare_render`.
    """
    plugin_or_chain: "PluginOrChain"
    block: int
    in_ch_required: int
    out_ch: int
    src: AudioBuffer | None
    src_frames: int
    sc_buf: AudioBuffer | None
    midi_events: list  # list[MidiEvent]
    has_midi: bool
    auto_list: list  # list[ParamChangePlugin | ParamChangeChain]
    has_auto: bool
    has_sidechain: bool
    out_frames: int
    render_frames: int
    latency: int


def _prepare_render(
    plugin_or_chain: "PluginOrChain",
    audio: Any | None,
    *,
    tail_seconds: float,
    block_size: int | None,
    compensate_latency: bool,
    midi: MidiInput | None,
    sidechain: Any | None,
    param_changes: Sequence | None,
    bpm: float | None,
) -> _RenderContext:
    """Validate inputs, resolve sources, compute geometry, set transport.

    The returned context is consumed by :func:`_iter_blocks` to drive
    the per-block process loop. Side effect: sets the plugin's
    transport when ``bpm`` is given.
    """
    block = _resolve_block_size(block_size)
    sample_rate = float(plugin_or_chain.sample_rate)
    in_ch_required = plugin_or_chain.num_input_channels
    out_ch = plugin_or_chain.num_output_channels
    is_chain = isinstance(plugin_or_chain, PluginChain)

    if sidechain is not None and is_chain:
        raise ValueError(
            "sidechain is not supported for PluginChain (no chain-level "
            "process_sidechain). Use a single Plugin instead."
        )
    if bpm is not None and is_chain:
        raise ValueError(
            "bpm transport is only supported for Plugin, not PluginChain "
            "(set transport on individual plugins instead)."
        )

    midi_events: list = []
    midi_max_sample = 0
    if midi is not None:
        midi_events, midi_max_sample = _load_midi_events(midi, sample_rate)

    if audio is not None:
        src: AudioBuffer | None = _to_audiobuffer(audio, in_ch_required)
        src_frames = src.frames
    else:
        if midi is None and tail_seconds <= 0.0:
            raise ValueError(
                "process_audio requires either audio input, MIDI events, "
                "or a positive tail_seconds for synth-mode rendering."
            )
        src = None
        src_frames = 0

    sc_buf: AudioBuffer | None = None
    if sidechain is not None:
        sc_buf = _to_audiobuffer(sidechain, in_ch_required)

    tail_frames = max(0, int(tail_seconds * sample_rate))
    base_frames = max(src_frames, midi_max_sample + 1 if midi_events else 0)
    out_frames = base_frames + tail_frames

    latency = int(plugin_or_chain.latency_samples) if compensate_latency else 0
    if latency < 0:
        latency = 0
    render_frames = out_frames + latency

    if bpm is not None:
        cast(Plugin, plugin_or_chain).set_transport(
            bpm=float(bpm), is_playing=True,
        )

    return _RenderContext(
        plugin_or_chain=plugin_or_chain,
        block=block,
        in_ch_required=in_ch_required,
        out_ch=out_ch,
        src=src,
        src_frames=src_frames,
        sc_buf=sc_buf,
        midi_events=midi_events,
        has_midi=bool(midi_events),
        auto_list=list(param_changes) if param_changes else [],
        has_auto=bool(param_changes),
        has_sidechain=sc_buf is not None,
        out_frames=out_frames,
        render_frames=render_frames,
        latency=latency,
    )


def _iter_blocks(
    ctx: _RenderContext,
    progress_callback: ProgressCallback | None = None,
    *,
    copy: bool = True,
) -> Iterator[AudioBuffer]:
    """Per-block process loop yielding user-visible output AudioBuffer
    slices (post-latency-compensation, post-trim).

    Concatenating every yielded block reproduces what
    ``process_audio`` would return into a pre-allocated buffer.

    ``copy`` (default True): yield independent buffers. Required for
    streaming consumers that might read past the next iteration --
    without it they'd see the reused internal ``out_block`` get
    overwritten. Pass ``copy=False`` from in-memory consumers that
    copy out of the yielded block immediately (like
    :func:`process_audio` writing into a pre-allocated output).
    """
    if ctx.out_frames <= 0:
        if progress_callback is not None:
            progress_callback(0, 0)
        return

    block = ctx.block
    in_ch_required = ctx.in_ch_required
    # A synth reports 0 input channels, but the internal I/O buffers still
    # need at least one channel to exist (and AudioBuffer materializes a
    # minimum of one anyway). Size and slice the input/sidechain buffers by
    # this effective width so a 0-input plugin round-trips cleanly; the plugin
    # simply ignores the (silent) input channel.
    work_in = max(in_ch_required, 1)
    out_ch = ctx.out_ch
    src = ctx.src
    src_frames = ctx.src_frames
    sc_buf = ctx.sc_buf
    midi_events = ctx.midi_events
    auto_list = ctx.auto_list
    has_midi = ctx.has_midi
    has_auto = ctx.has_auto
    has_sidechain = ctx.has_sidechain
    out_frames = ctx.out_frames
    render_frames = ctx.render_frames
    plugin_or_chain = ctx.plugin_or_chain

    in_block = AudioBuffer(work_in, block)
    out_block = AudioBuffer(out_ch, block)
    sc_block = AudioBuffer(work_in, block) if sc_buf is not None else None

    midi_idx = 0
    auto_idx = 0
    skip_remaining = ctx.latency
    emitted = 0

    for start in range(0, render_frames, block):
        n = min(block, render_frames - start)

        in_block.clear()
        if src is not None and start < src_frames:
            copy = min(n, src_frames - start)
            in_block[:work_in, :copy] = src[:work_in, start:start + copy]

        if sc_block is not None and sc_buf is not None:
            sc_block.clear()
            if start < sc_buf.frames:
                copy = min(n, sc_buf.frames - start)
                sc_block[:work_in, :copy] = sc_buf[:work_in, start:start + copy]

        block_midi, midi_idx = (
            _slice_block_events(midi_events, midi_idx, start, start + n)
            if has_midi else ([], midi_idx)
        )
        block_auto, auto_idx = (
            _slice_block_events(auto_list, auto_idx, start, start + n)
            if has_auto else ([], auto_idx)
        )

        if n == block:
            pin, pout = in_block, out_block
            psc = sc_block
        else:
            pin = AudioBuffer(work_in, n)
            pin[:work_in, :n] = in_block[:work_in, :n]
            pout = AudioBuffer(out_ch, n)
            if sc_block is not None:
                psc = AudioBuffer(work_in, n)
                psc[:work_in, :n] = sc_block[:work_in, :n]
            else:
                psc = None

        if has_sidechain:
            for ev in block_auto:
                if len(ev) == 3:
                    _, pidx, value = ev
                    cast(Plugin, plugin_or_chain).set_param(pidx, float(value))
                else:
                    raise ValueError(
                        "Sidechain processing requires per-plugin "
                        "param_changes; got chain-shaped 4-tuples."
                    )
            cast(Plugin, plugin_or_chain).process_sidechain(pin, pout, psc)
        elif has_auto:
            plugin_or_chain.process_auto(pin, pout, block_midi, block_auto)
        elif has_midi:
            plugin_or_chain.process_midi(pin, pout, block_midi)
        else:
            plugin_or_chain.process(pin, pout)

        produced: AudioBuffer = pout

        if skip_remaining >= n:
            skip_remaining -= n
            continue
        if skip_remaining > 0:
            produced = cast(AudioBuffer, produced[:, skip_remaining:])
            skip_remaining = 0

        emit = produced.frames
        if emitted + emit > out_frames:
            emit = out_frames - emitted
            if emit <= 0:
                break
            produced = cast(AudioBuffer, produced[:, :emit])

        yield produced.copy() if copy else produced
        emitted += emit
        if progress_callback is not None:
            progress_callback(min(emitted, out_frames), out_frames)

        if emitted >= out_frames:
            break

    if progress_callback is not None:
        progress_callback(out_frames, out_frames)


def process_audio(
    plugin_or_chain: "PluginOrChain",
    audio: Any | None = None,
    tail_seconds: float = 0.0,
    block_size: int | None = None,
    compensate_latency: bool = True,
    normalize: float | None = None,
    progress_callback: ProgressCallback | None = None,
    *,
    midi: MidiInput | None = None,
    sidechain: Any | None = None,
    param_changes: Sequence[ParamChangePlugin | ParamChangeChain] | None = None,
    bpm: float | None = None,
    in_place: bool = False,
) -> AudioBuffer:
    """Process audio through a plugin or chain.

    Args:
        plugin_or_chain: A :class:`minihost.Plugin` or
            :class:`minihost.PluginChain`.
        audio: Source audio of shape ``(channels, frames)``. Accepts an
            :class:`AudioBuffer`, a numpy ndarray, or any 2D float32
            c-contiguous buffer-protocol producer. Pass ``None`` for
            synth-mode renders driven entirely by ``midi`` -- in that
            case the length is derived from the last MIDI event plus
            ``tail_seconds``.
        tail_seconds: Extra silent input rendered after the source so
            reverb / delay tails are captured.
        block_size: Audio block size used for the internal process loop.
        compensate_latency: When True (default), render
            ``plugin.latency_samples`` extra frames at the end and discard
            the matching number of frames from the start.
        normalize: If not None, peak-normalize the output to this target
            in dBFS. Silent output is left untouched.
        progress_callback: Optional ``(processed_frames, total_frames)``
            callback invoked once per block.
        midi: Optional MIDI driver. A file path, ``MidiFile`` object, or
            a pre-resolved list of ``(sample_offset, status, data1,
            data2)`` tuples. Routed through :meth:`Plugin.process_midi`
            (or :meth:`process_auto` when ``param_changes`` is also
            given).
        sidechain: Optional sidechain audio (same accepted types as
            ``audio``). Plugin-only; PluginChain has no sidechain
            process method.
        param_changes: Sample-accurate parameter automation. For a
            :class:`Plugin` use ``(sample, param_idx, value)``; for a
            :class:`PluginChain` use ``(sample, plugin_idx, param_idx,
            value)``. Routed through ``process_auto``.
        bpm: Set transport tempo (Plugin only) once before rendering.
        in_place: When True, write output into the input buffer instead
            of allocating a new one. Saves one buffer's worth of memory
            for the common stereo-in / stereo-out case. **Mutates the
            caller's ``audio`` argument.** Requires:

            * ``audio`` is an :class:`AudioBuffer` (not a numpy array or
              other buffer-protocol producer -- those go through a
              copying coercion path).
            * ``audio.channels == plugin.num_output_channels``.
            * ``tail_seconds == 0`` (a tail would need extra frames the
              input doesn't have).

            The block loop is already structured to snapshot each input
            block into a scratch buffer before processing, so writing
            output into the input's storage at a (latency-) lagged
            position is safe.

    Returns:
        A new :class:`AudioBuffer` of shape
        ``(plugin.num_output_channels, total_frames)``. When
        ``in_place=True``, the returned buffer is the same object as
        ``audio``.
    """
    ctx = _prepare_render(
        plugin_or_chain, audio,
        tail_seconds=tail_seconds, block_size=block_size,
        compensate_latency=compensate_latency,
        midi=midi, sidechain=sidechain,
        param_changes=param_changes, bpm=bpm,
    )

    if in_place:
        if not isinstance(audio, AudioBuffer):
            raise TypeError(
                "in_place=True requires audio to be an AudioBuffer "
                "(numpy / buffer-protocol inputs are copied through a "
                "coercion path and cannot alias the output)."
            )
        if audio.channels != ctx.out_ch:
            raise ValueError(
                f"in_place=True requires matching channel counts; "
                f"input has {audio.channels} channel(s), plugin "
                f"produces {ctx.out_ch}."
            )
        if ctx.out_frames != ctx.src_frames:
            raise ValueError(
                "in_place=True is incompatible with tail_seconds > 0 "
                "(output would need more frames than the input has)."
            )
        output = audio
    else:
        output = AudioBuffer(ctx.out_ch, ctx.out_frames)
    written = 0
    # copy=False: process_audio memcpys each block straight into the
    # pre-allocated output, so the reused internal buffer is safe.
    for block in _iter_blocks(ctx, progress_callback=progress_callback, copy=False):
        n = block.frames
        output[:, written:written + n] = block
        written += n

    if normalize is not None:
        _normalize_peak(output, float(normalize))

    return output


def process_audio_stream(
    plugin_or_chain: "PluginOrChain",
    audio: Any | None = None,
    tail_seconds: float = 0.0,
    block_size: int | None = None,
    compensate_latency: bool = True,
    progress_callback: ProgressCallback | None = None,
    *,
    midi: MidiInput | None = None,
    sidechain: Any | None = None,
    param_changes: Sequence[ParamChangePlugin | ParamChangeChain] | None = None,
    bpm: float | None = None,
    as_: type | None = None,
) -> "Iterator[AudioBuffer | np.ndarray]":
    """Stream blocks of processed audio.

    The audio-in counterpart to :func:`render_midi_stream`. Yields
    user-visible output blocks (post-latency-compensation, post-trim)
    so a consumer that concatenates every yielded block reproduces
    :func:`process_audio`'s return value. Lets callers write to disk
    block-by-block without holding the full output in memory.

    Args:
        plugin_or_chain, audio, tail_seconds, block_size,
        compensate_latency, midi, sidechain, param_changes, bpm:
            Identical to :func:`process_audio`.
        progress_callback: ``(processed, total)`` callback fired after
            each yielded block (and once more after the final block).
        as_: Container type for each yielded block. ``AudioBuffer``
            (default) or ``numpy.ndarray``.

    Yields:
        Audio blocks of shape ``(channels, n)`` where ``n <=
        block_size``. The final block may be shorter.

    Notes:
        Peak normalization (the ``normalize=`` kwarg on
        :func:`process_audio`) is intentionally absent here -- peak
        normalization requires the full output's peak, which a
        streaming consumer doesn't have. Normalize offline if needed.

    Example:
        >>> import minihost
        >>> plugin = minihost.Plugin("reverb.vst3", sample_rate=48000)
        >>> audio, sr = minihost.read_audio("very_long.wav")
        >>> for block in minihost.process_audio_stream(
        ...         plugin, audio, tail_seconds=4.0, block_size=4096):
        ...     # write the block to disk incrementally
        ...     ...
    """
    from minihost.render import _coerce_block

    ctx = _prepare_render(
        plugin_or_chain, audio,
        tail_seconds=tail_seconds, block_size=block_size,
        compensate_latency=compensate_latency,
        midi=midi, sidechain=sidechain,
        param_changes=param_changes, bpm=bpm,
    )

    for block in _iter_blocks(ctx, progress_callback=progress_callback):
        yield _coerce_block(block, as_)


def _read_optional_audio(
    source: Any, plugin_sr: int, allow_resample: bool,
    label: str,
) -> AudioBuffer | None:
    """Resolve ``source`` (file path or in-memory buffer) to an AudioBuffer
    at ``plugin_sr``. Returns None when ``source`` is None.
    """
    if source is None:
        return None
    if isinstance(source, (str, Path)):
        audio, in_sr = read_audio(source)
        if in_sr != plugin_sr:
            if not allow_resample:
                raise ValueError(
                    f"{label} is {in_sr} Hz but plugin is {plugin_sr} Hz. "
                    f"Pass resample_to_plugin_rate=True to convert automatically."
                )
            audio = resample(audio, in_sr, plugin_sr)
        return audio if isinstance(audio, AudioBuffer) else AudioBuffer.from_numpy(audio)
    if isinstance(source, AudioBuffer):
        return source
    return AudioBuffer.from_numpy(source)


def _maybe_duplicate_to_match(
    src: AudioBuffer, required: int, duplicate: bool, label: str,
) -> AudioBuffer:
    if src.channels >= required:
        return src
    if not duplicate:
        raise ValueError(
            f"{label} has {src.channels} channel(s) but plugin needs "
            f"{required}. Pass duplicate_to_stereo=True to "
            f"channel-duplicate automatically."
        )
    expanded = AudioBuffer(required, src.frames)
    expanded[:src.channels, :] = src
    last = cast(AudioBuffer, src[src.channels - 1: src.channels, :])
    for ch in range(src.channels, required):
        expanded[ch:ch + 1, :] = last
    return expanded


def process_audio_to_file(
    plugin_or_chain: "PluginOrChain",
    input_path: str | Path | None = None,
    output_path: str | Path = "",
    tail_seconds: float = 0.0,
    block_size: int | None = None,
    bit_depth: int = 24,
    resample_to_plugin_rate: bool = True,
    duplicate_to_stereo: bool = True,
    compensate_latency: bool = True,
    normalize: float | None = None,
    progress_callback: ProgressCallback | None = None,
    *,
    midi: MidiInput | None = None,
    sidechain: str | Path | AudioBuffer | Any | None = None,
    param_changes: Sequence[ParamChangePlugin | ParamChangeChain] | None = None,
    bpm: float | None = None,
) -> int:
    """Process an audio file through a plugin or chain and write the result.

    The full-featured offline render entry point: accepts main audio,
    sidechain audio, a MIDI file or pre-parsed events, sample-accurate
    parameter automation, BPM transport, peak normalization, and a
    progress callback.

    Args:
        plugin_or_chain: Plugin or PluginChain.
        input_path: Source audio file. ``None`` enables synth-mode
            rendering (``midi`` must then be set, or ``tail_seconds``
            must be positive).
        output_path: Destination file (.wav or .flac).
        tail_seconds: Extra silent input rendered after the source.
        block_size: Audio block size for the internal process loop.
        bit_depth: Output bit depth (16/24/32 for WAV; 16/24 for FLAC).
        resample_to_plugin_rate: Resample mismatched inputs to the
            plugin's sample rate when True (default).
        duplicate_to_stereo: Channel-duplicate mono inputs to match the
            plugin's input channel count when True (default).
        compensate_latency: Drop the first ``plugin.latency_samples``
            output samples so the result is time-aligned with the source.
        normalize: Peak-normalize the output to this dBFS target.
        progress_callback: ``(processed, total)`` callback per block.
        midi: Path to a MIDI file, a ``MidiFile`` object, or a
            pre-resolved event list. Sample positions are recomputed
            against the plugin's sample rate.
        sidechain: Sidechain audio (file path or in-memory buffer).
            Plugin only; PluginChain has no sidechain method.
        param_changes: Sample-accurate parameter automation.
        bpm: Transport tempo (Plugin only).

    Returns:
        Number of frames written.
    """
    if not output_path:
        raise ValueError("output_path is required.")

    plugin_sr = int(plugin_or_chain.sample_rate)
    in_ch_required = plugin_or_chain.num_input_channels

    main_src: AudioBuffer | None = None
    if input_path is not None:
        main_src = _read_optional_audio(
            input_path, plugin_sr, resample_to_plugin_rate, "Input",
        )
        if main_src is not None:
            main_src = _maybe_duplicate_to_match(
                main_src, in_ch_required, duplicate_to_stereo, "Input",
            )

    sc_src: AudioBuffer | None = None
    if sidechain is not None:
        sc_src = _read_optional_audio(
            sidechain, plugin_sr, resample_to_plugin_rate, "Sidechain",
        )
        if sc_src is not None:
            sc_src = _maybe_duplicate_to_match(
                sc_src, in_ch_required, duplicate_to_stereo, "Sidechain",
            )

    output = process_audio(
        plugin_or_chain,
        main_src,
        tail_seconds=tail_seconds,
        block_size=block_size,
        compensate_latency=compensate_latency,
        normalize=normalize,
        progress_callback=progress_callback,
        midi=midi,
        sidechain=sc_src,
        param_changes=param_changes,
        bpm=bpm,
    )
    write_audio(output_path, output, plugin_sr, bit_depth=bit_depth)
    return output.frames
