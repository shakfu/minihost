"""MIDI file rendering utilities for minihost.

This module provides functions to render MIDI files through audio plugins,
returning :class:`AudioBuffer` by default and ``numpy.ndarray`` on request
via the ``as_=`` keyword. Streaming and file-output variants are also
provided.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any, Iterator, Optional, Union, cast

if TYPE_CHECKING:
    import numpy as np

from minihost._core import AudioBuffer, Plugin, PluginChain, MidiFile

# Type alias for plugin or chain
PluginOrChain = Union[Plugin, PluginChain]


def _coerce_block(block: AudioBuffer, as_: type) -> Any:
    """Convert a renderer block to the requested container type."""
    if as_ is AudioBuffer:
        return block
    # numpy or anything that wants the np.ndarray side: convert via numpy().
    # The fast path: AudioBuffer.numpy() returns a zero-copy view.
    return block.numpy()


def _build_tempo_map(midi_file: MidiFile) -> list[tuple[int, float]]:
    """Build a tempo map from MIDI file events.

    Returns list of (tick, microseconds_per_quarter) sorted by tick.
    """
    tempo_map = []

    for track_idx in range(midi_file.num_tracks):
        events = midi_file.get_events(track_idx)
        for event in events:
            if event.get("type") == "tempo":
                bpm = event["bpm"]
                us_per_quarter = 60_000_000.0 / bpm
                tempo_map.append((event["tick"], us_per_quarter))

    # Sort by tick, default tempo if none specified
    if not tempo_map:
        tempo_map.append((0, 500_000.0))  # 120 BPM default

    tempo_map.sort(key=lambda x: x[0])

    # Ensure we have a tempo at tick 0
    if tempo_map[0][0] != 0:
        tempo_map.insert(0, (0, 500_000.0))

    return tempo_map


def _tick_to_seconds(tick: int, tempo_map: list[tuple[int, float]], tpq: int) -> float:
    """Convert MIDI tick to seconds using tempo map.

    Args:
        tick: MIDI tick value
        tempo_map: List of (tick, microseconds_per_quarter)
        tpq: Ticks per quarter note

    Returns:
        Time in seconds
    """
    seconds = 0.0
    prev_tick = 0
    prev_tempo = tempo_map[0][1]

    for map_tick, tempo in tempo_map:
        if map_tick >= tick:
            break
        # Add time for segment from prev_tick to map_tick
        delta_ticks = map_tick - prev_tick
        seconds += (delta_ticks / tpq) * (prev_tempo / 1_000_000.0)
        prev_tick = map_tick
        prev_tempo = tempo

    # Add remaining time from last tempo change to target tick
    delta_ticks = tick - prev_tick
    seconds += (delta_ticks / tpq) * (prev_tempo / 1_000_000.0)

    return seconds


def _seconds_to_samples(seconds: float, sample_rate: float) -> int:
    """Convert seconds to sample position."""
    return int(seconds * sample_rate)


def _collect_midi_events(midi_file: MidiFile) -> list[dict]:
    """Collect all MIDI events from all tracks, sorted by tick."""
    all_events = []

    for track_idx in range(midi_file.num_tracks):
        events = midi_file.get_events(track_idx)
        for event in events:
            # Only include playable events (not meta events like tempo)
            event_type = event.get("type")
            if event_type in (
                "note_on",
                "note_off",
                "control_change",
                "program_change",
                "pitch_bend",
            ):
                all_events.append(event)

    # Sort by tick
    all_events.sort(key=lambda x: x["tick"])
    return all_events


def _event_to_midi_tuple(
    event: dict, sample_offset: int
) -> tuple[int, int, int, int] | None:
    """Convert event dict to (sample_offset, status, data1, data2) tuple."""
    event_type = event["type"]
    channel = event.get("channel", 0)

    if event_type == "note_on":
        status = 0x90 | channel
        data1 = event["pitch"]
        data2 = event["velocity"]
    elif event_type == "note_off":
        status = 0x80 | channel
        data1 = event["pitch"]
        data2 = event.get("velocity", 0)
    elif event_type == "control_change":
        status = 0xB0 | channel
        data1 = event["controller"]
        data2 = event["value"]
    elif event_type == "program_change":
        status = 0xC0 | channel
        data1 = event["program"]
        data2 = 0
    elif event_type == "pitch_bend":
        status = 0xE0 | channel
        value = event["value"]
        data1 = value & 0x7F
        data2 = (value >> 7) & 0x7F
    else:
        return None

    return (sample_offset, status, data1, data2)


def _is_auto_tail(tail_seconds: object) -> bool:
    """Check if tail_seconds requests auto-detection."""
    return tail_seconds == "auto"


# Default threshold for auto-tail detection: -80 dB in linear amplitude
_AUTO_TAIL_THRESHOLD = 1e-4
# Number of consecutive silent blocks before stopping
_AUTO_TAIL_SILENT_BLOCKS = 4
# Maximum auto-tail duration in seconds (safety cap)
_AUTO_TAIL_MAX_SECONDS = 30.0


def render_midi_stream(
    plugin: PluginOrChain,
    midi_file: Union[MidiFile, str],
    block_size: int = 512,
    tail_seconds: Optional[Union[float, str]] = None,
    tail_threshold: float = _AUTO_TAIL_THRESHOLD,
    max_tail_seconds: float = _AUTO_TAIL_MAX_SECONDS,
    as_: type = AudioBuffer,
) -> "Iterator[AudioBuffer | np.ndarray]":
    """Render MIDI file through plugin or chain as a generator of audio blocks.

    Args:
        plugin: Plugin or PluginChain instance to render through
        midi_file: MidiFile object or path to MIDI file
        block_size: Audio block size in samples
        tail_seconds: Extra time to render after MIDI ends for reverb/delay tails.
                     None = use plugin/chain tail_seconds, 0 = no tail,
                     "auto" = detect tail by monitoring output level
        tail_threshold: Peak amplitude threshold for auto-tail detection (linear).
                       Default is 1e-4 (~-80 dB). Only used when tail_seconds="auto".
        max_tail_seconds: Maximum tail duration for auto mode (safety cap).
                         Default is 30.0 seconds.
        as_: Container type for each yielded block. ``AudioBuffer``
            (default) or ``numpy.ndarray``.

    Yields:
        Audio blocks of shape ``(channels, n)`` where ``n <= block_size``.
        Type matches the ``as_`` argument.

    Example:
        >>> plugin = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> for block in minihost.render_midi_stream(plugin, "song.mid"):
        ...     # Process or write each block
        ...     pass

        >>> # numpy blocks if you prefer
        >>> import numpy as np
        >>> for block in minihost.render_midi_stream(plugin, "song.mid",
        ...                                          as_=np.ndarray):
        ...     ...
    """
    renderer = MidiRenderer(
        plugin,
        midi_file,
        block_size=block_size,
        tail_seconds=tail_seconds,
        tail_threshold=tail_threshold,
        max_tail_seconds=max_tail_seconds,
    )
    while not renderer.is_finished:
        block = renderer.render_block()
        if block is not None:
            yield _coerce_block(block, as_)


def render_midi(
    plugin: PluginOrChain,
    midi_file: Union[MidiFile, str],
    block_size: int = 512,
    tail_seconds: Optional[Union[float, str]] = None,
    dtype: Optional[type] = None,
    tail_threshold: float = _AUTO_TAIL_THRESHOLD,
    max_tail_seconds: float = _AUTO_TAIL_MAX_SECONDS,
    as_: type = AudioBuffer,
) -> "AudioBuffer | np.ndarray":
    """Render MIDI file through plugin or chain to a single buffer.

    Args:
        plugin: Plugin or PluginChain instance to render through
        midi_file: MidiFile object or path to MIDI file
        block_size: Audio block size in samples
        tail_seconds: Extra time to render after MIDI ends for reverb/delay tails.
                     "auto" = detect tail by monitoring output level.
        dtype: When ``as_=numpy.ndarray``, the dtype of the returned array
            (``np.float32`` or ``np.float64``). Ignored for the
            ``AudioBuffer`` path (always float32).
        tail_threshold: Peak amplitude threshold for auto-tail detection (linear).
        max_tail_seconds: Maximum tail duration for auto mode (safety cap).
        as_: Container type for the returned audio. ``AudioBuffer``
            (default) or ``numpy.ndarray``.

    Returns:
        An ``AudioBuffer`` or ``numpy.ndarray`` of shape
        ``(channels, total_samples)``. Type matches the ``as_`` argument.

    Example:
        >>> plugin = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> audio = minihost.render_midi(plugin, "song.mid")
        >>> print(f"Rendered {audio.frames / 48000:.2f} seconds")
    """
    import numpy as np

    if dtype is None:
        dtype = np.float32

    renderer = MidiRenderer(
        plugin,
        midi_file,
        block_size=block_size,
        tail_seconds=tail_seconds,
        tail_threshold=tail_threshold,
        max_tail_seconds=max_tail_seconds,
    )
    return renderer.render_all(dtype=dtype, as_=as_)


def render_midi_to_file(
    plugin: PluginOrChain,
    midi_file: Union[MidiFile, str],
    output_path: str,
    block_size: int = 512,
    tail_seconds: Optional[Union[float, str]] = None,
    bit_depth: int = 24,
    tail_threshold: float = _AUTO_TAIL_THRESHOLD,
    max_tail_seconds: float = _AUTO_TAIL_MAX_SECONDS,
) -> int:
    """Render MIDI file through plugin or chain and write to audio file.

    Args:
        plugin: Plugin or PluginChain instance to render through
        midi_file: MidiFile object or path to MIDI file
        output_path: Output WAV file path
        block_size: Audio block size in samples
        tail_seconds: Extra time to render after MIDI ends.
                     "auto" = detect tail by monitoring output level.
        bit_depth: Output bit depth (16, 24, or 32 for float)
        tail_threshold: Peak amplitude threshold for auto-tail detection (linear).
        max_tail_seconds: Maximum tail duration for auto mode (safety cap).

    Returns:
        Number of samples written

    Example:
        >>> plugin = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> samples = minihost.render_midi_to_file(plugin, "song.mid", "output.wav")
        >>> print(f"Wrote {samples} samples")
    """
    import numpy as np

    from minihost.audio_io import write_audio

    if bit_depth not in (16, 24, 32):
        raise ValueError("bit_depth must be 16, 24, or 32")

    # Use MidiRenderer directly so we can pre-allocate the output buffer
    # against its known total_samples upper bound. This avoids the previous
    # ~3x peak memory (block list + np.concatenate + write_audio's internal
    # interleave) by writing each rendered block directly into a single
    # contiguous output array. Auto-tail detection may finish early, in
    # which case we trim before writing.
    renderer = MidiRenderer(
        plugin,
        midi_file,
        block_size=block_size,
        tail_seconds=tail_seconds,
        tail_threshold=tail_threshold,
        max_tail_seconds=max_tail_seconds,
    )
    sample_rate = int(plugin.sample_rate)
    out_channels = renderer.channels
    total = renderer.total_samples

    if total == 0:
        empty = np.zeros((out_channels, 0), dtype=np.float32)
        write_audio(output_path, empty, sample_rate, bit_depth=bit_depth)
        return 0

    # Allocate the output as an AudioBuffer; obtain a numpy view onto its
    # storage for slice-assignment of each rendered block. AudioBuffer
    # blocks coming back from render_block() are converted via np.asarray
    # (zero-copy via __array__).
    audio = AudioBuffer(out_channels, total)
    audio_view = audio.numpy()
    written = 0
    while not renderer.is_finished:
        block = renderer.render_block()
        if block is None:
            continue
        n = block.frames
        # Defensive: never write past the pre-allocated extent. Should not
        # happen given the renderer's own bounds, but truncate if it does.
        if written + n > total:
            n = total - written
            if n <= 0:
                break
            # Slice always returns an AudioBuffer (only scalar/scalar
            # indexing returns a float); cast for mypy.
            block = cast(AudioBuffer, block[:, :n])
        audio_view[:, written:written + n] = np.asarray(block)
        written += n

    if written < total:
        audio = cast(AudioBuffer, audio[:, :written])  # trim
    write_audio(output_path, audio, sample_rate, bit_depth=bit_depth)
    return written


class MidiRenderer:
    """Stateful MIDI renderer for advanced use cases.

    Provides fine-grained control over the rendering process,
    useful for real-time visualization, progress tracking, or
    custom output handling.

    Example:
        >>> renderer = minihost.MidiRenderer(plugin, "song.mid")
        >>> print(f"Duration: {renderer.duration_seconds:.2f}s")
        >>>
        >>> while not renderer.is_finished:
        ...     block = renderer.render_block()
        ...     progress = renderer.progress
        ...     print(f"Progress: {progress:.1%}")
    """

    def __init__(
        self,
        plugin: PluginOrChain,
        midi_file: Union[MidiFile, str],
        block_size: int = 512,
        tail_seconds: Optional[Union[float, str]] = None,
        tail_threshold: float = _AUTO_TAIL_THRESHOLD,
        max_tail_seconds: float = _AUTO_TAIL_MAX_SECONDS,
    ):
        """Initialize renderer.

        Args:
            plugin: Plugin or PluginChain instance to render through
            midi_file: MidiFile object or path to MIDI file
            block_size: Audio block size in samples
            tail_seconds: Extra time after MIDI ends for tails.
                         "auto" = detect tail by monitoring output level.
            tail_threshold: Peak amplitude threshold for auto-tail detection.
            max_tail_seconds: Maximum tail duration for auto mode.
        """
        import numpy as np

        self._np = np  # Store for later use

        self.plugin = plugin
        self.block_size = block_size
        self._auto_tail = _is_auto_tail(tail_seconds)
        self._tail_threshold = tail_threshold
        self._consecutive_silent = 0

        # Load MIDI file if path provided
        if isinstance(midi_file, str):
            self._midi_file = MidiFile()
            if not self._midi_file.load(midi_file):
                raise RuntimeError(f"Failed to load MIDI file: {midi_file}")
        else:
            self._midi_file = midi_file

        # Get parameters
        self.sample_rate = plugin.sample_rate
        self._tpq = self._midi_file.ticks_per_quarter
        self._in_channels = max(plugin.num_input_channels, 2)
        self._out_channels = max(plugin.num_output_channels, 2)

        # Determine tail length
        if self._auto_tail:
            effective_tail = max_tail_seconds
        elif tail_seconds is None:
            effective_tail = plugin.tail_seconds
            if effective_tail <= 0 or effective_tail > 30:
                effective_tail = 2.0
        else:
            effective_tail = float(tail_seconds)
        self._tail_seconds = effective_tail

        # Build tempo map and collect events
        self._tempo_map = _build_tempo_map(self._midi_file)
        all_events = _collect_midi_events(self._midi_file)

        # Find total duration
        if all_events:
            last_tick = max(e["tick"] for e in all_events)
            self._midi_duration = _tick_to_seconds(
                last_tick, self._tempo_map, self._tpq
            )
        else:
            self._midi_duration = 0.0

        self._midi_end_samples = _seconds_to_samples(
            self._midi_duration, self.sample_rate
        )
        self._total_duration = self._midi_duration + self._tail_seconds
        self._total_samples = _seconds_to_samples(
            self._total_duration, self.sample_rate
        )

        # Latency compensation: a plugin reporting N samples of latency emits
        # the response to a MIDI event at input sample T at output sample T+N.
        # To time-align the rendered audio with MIDI tempo positions, we
        # render N extra input samples past the user-visible end and skip
        # the first N output samples (which contain pre-roll silence and
        # nothing else, since no MIDI event has registered yet).
        #
        # The user-facing properties (duration_seconds, total_samples,
        # progress) continue to report the user-visible duration. Internal
        # bookkeeping uses _render_samples for the loop bound.
        try:
            self._latency = int(plugin.latency_samples)
        except Exception:
            self._latency = 0
        if self._latency < 0:
            self._latency = 0
        self._render_samples = self._total_samples + self._latency
        self._skip_remaining = self._latency

        # Convert events to sample positions
        self._events_with_samples = []
        for event in all_events:
            tick = event["tick"]
            seconds = _tick_to_seconds(tick, self._tempo_map, self._tpq)
            sample_pos = _seconds_to_samples(seconds, self.sample_rate)
            self._events_with_samples.append((sample_pos, event))

        # Create buffers
        self._input_buffer = self._np.zeros(
            (self._in_channels, block_size), dtype=self._np.float32
        )
        self._output_buffer = self._np.zeros(
            (self._out_channels, block_size), dtype=self._np.float32
        )

        # State
        self._current_sample = 0
        self._event_idx = 0
        self._auto_tail_finished = False

    @property
    def duration_seconds(self) -> float:
        """Total duration including tail, in seconds."""
        return self._total_duration

    @property
    def midi_duration_seconds(self) -> float:
        """Duration of MIDI content (excluding tail), in seconds."""
        return self._midi_duration

    @property
    def total_samples(self) -> int:
        """Total number of samples to render."""
        return self._total_samples

    @property
    def current_sample(self) -> int:
        """Current sample position."""
        return self._current_sample

    @property
    def current_time(self) -> float:
        """Current time in seconds."""
        return self._current_sample / self.sample_rate

    @property
    def progress(self) -> float:
        """Rendering progress as fraction (0.0 to 1.0)."""
        if self._render_samples == 0:
            return 1.0
        return min(1.0, self._current_sample / self._render_samples)

    @property
    def is_finished(self) -> bool:
        """True if rendering is complete."""
        if self._auto_tail_finished:
            return True
        return self._current_sample >= self._render_samples

    @property
    def channels(self) -> int:
        """Number of output channels."""
        return self._out_channels

    @property
    def latency_samples(self) -> int:
        """Plugin latency in samples that the renderer compensates for.

        The renderer renders ``latency_samples`` extra input samples past
        the user-visible end and discards the first ``latency_samples`` of
        output, so that audio is time-aligned with MIDI tempo positions.
        Read this if you are wrapping the renderer and need to know how
        many samples of pre-roll the plugin reported.
        """
        return self._latency

    def reset(self):
        """Reset renderer to beginning."""
        self._current_sample = 0
        self._event_idx = 0
        self._consecutive_silent = 0
        self._auto_tail_finished = False
        self._skip_remaining = self._latency
        self.plugin.reset()

    def render_block(self) -> Optional[AudioBuffer]:
        """Render next block of audio.

        Returns:
            :class:`AudioBuffer` of shape ``(channels, n)`` where
            ``n <= block_size``, or ``None`` if finished. May also return
            ``None`` for early blocks consumed entirely by latency-
            compensation skip. Call ``.numpy()`` on the returned buffer
            if you need a numpy view.
        """
        if self.is_finished:
            return None

        # Determine block size against the *internal* render bound (which
        # includes the latency tail). User-visible output is bounded by
        # _total_samples, but we render _render_samples to capture the
        # plugin's pre-roll latency.
        remaining = self._render_samples - self._current_sample
        this_block_size = min(self.block_size, remaining)

        # Collect MIDI events for this block
        block_events = []
        while self._event_idx < len(self._events_with_samples):
            sample_pos, event = self._events_with_samples[self._event_idx]
            if sample_pos >= self._current_sample + this_block_size:
                break

            offset = sample_pos - self._current_sample
            offset = max(0, min(offset, this_block_size - 1))

            midi_tuple = _event_to_midi_tuple(event, offset)
            if midi_tuple:
                block_events.append(midi_tuple)

            self._event_idx += 1

        # Clear input and process
        self._input_buffer.fill(0)

        if this_block_size < self.block_size:
            in_slice = self._input_buffer[:, :this_block_size].copy()
            out_slice = self._np.zeros(
                (self._out_channels, this_block_size), dtype=self._np.float32
            )
            self.plugin.process_midi(in_slice, out_slice, block_events)
            result = out_slice
        else:
            self.plugin.process_midi(
                self._input_buffer, self._output_buffer, block_events
            )
            result = self._output_buffer.copy()

        self._current_sample += this_block_size

        # Latency compensation: discard the first _latency samples of output.
        # If this whole block falls inside the skip region, return None and
        # let the caller loop again. Otherwise trim the leading skip portion.
        if self._skip_remaining > 0:
            n = result.shape[1]
            if n <= self._skip_remaining:
                self._skip_remaining -= n
                return None
            result = result[:, self._skip_remaining:]
            self._skip_remaining = 0

        # Auto-tail detection runs against the post-skip (user-visible)
        # output. The MIDI-end check is in input-sample space, but we
        # subtract the latency offset so "after MIDI" matches the
        # user-visible timeline.
        if self._auto_tail and self._current_sample > self._midi_end_samples + self._latency:
            peak = float(self._np.max(self._np.abs(result)))
            if peak < self._tail_threshold:
                self._consecutive_silent += 1
                if self._consecutive_silent >= _AUTO_TAIL_SILENT_BLOCKS:
                    self._auto_tail_finished = True
            else:
                self._consecutive_silent = 0

        # Wrap the (possibly sliced) numpy result in an AudioBuffer at the
        # public-API boundary. AudioBuffer.from_numpy copies; for the
        # full-block case this replaces the explicit .copy() above.
        return AudioBuffer.from_numpy(self._np.ascontiguousarray(result))

    def render_all(
        self,
        dtype: Optional[type] = None,
        as_: type = AudioBuffer,
    ) -> "AudioBuffer | np.ndarray":
        """Render all remaining audio.

        Args:
            dtype: When ``as_=np.ndarray``, the dtype of the returned array
                (``np.float32`` or ``np.float64``). Ignored for the
                ``AudioBuffer`` path (always float32).
            as_: Container type for the returned audio. ``AudioBuffer``
                (default) or ``numpy.ndarray``.

        Returns:
            An ``AudioBuffer`` or ``numpy.ndarray`` of shape
            ``(channels, remaining_samples)``.
        """
        if dtype is None:
            dtype = self._np.float32

        # Pre-allocate against the renderer's known upper bound, then write
        # blocks directly into slices. Same memory-efficient pattern as
        # render_midi_to_file. Avoids list+concatenate's transient peak.
        remaining = max(0, self._render_samples - self._current_sample)
        out = AudioBuffer(self._out_channels, remaining)
        out_view = out.numpy()
        written = 0
        while not self.is_finished:
            block = self.render_block()
            if block is None:
                continue
            n = block.frames
            if written + n > remaining:
                n = remaining - written
                if n <= 0:
                    break
                block = cast(AudioBuffer, block[:, :n])
            out_view[:, written:written + n] = self._np.asarray(block)
            written += n

        if written < remaining:
            out = cast(AudioBuffer, out[:, :written])  # trim

        if as_ is AudioBuffer:
            return out
        if as_ is self._np.ndarray:
            arr = self._np.asarray(out)
            if dtype != self._np.float32:
                arr = arr.astype(dtype)
            return arr
        raise TypeError(f"as_ must be AudioBuffer or numpy.ndarray, got {as_!r}")
