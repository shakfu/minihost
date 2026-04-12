"""MIDI file rendering utilities for minihost.

This module provides functions to render MIDI files through audio plugins,
supporting output to numpy arrays, audio files, or streaming generators.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Iterator, Optional, Union

if TYPE_CHECKING:
    import numpy as np

from minihost._core import Plugin, PluginChain, MidiFile

# Type alias for plugin or chain
PluginOrChain = Union[Plugin, PluginChain]


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
) -> "Iterator[np.ndarray]":
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

    Yields:
        numpy arrays of shape (channels, block_size) containing rendered audio

    Example:
        >>> plugin = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> for block in minihost.render_midi_stream(plugin, "song.mid"):
        ...     # Process or write each block
        ...     pass

        >>> # Auto-detect reverb tail
        >>> for block in minihost.render_midi_stream(plugin, "song.mid", tail_seconds="auto"):
        ...     pass

        >>> # With a plugin chain
        >>> synth = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> reverb = minihost.Plugin("reverb.vst3", sample_rate=48000)
        >>> chain = minihost.PluginChain([synth, reverb])
        >>> audio = minihost.render_midi(chain, "song.mid")
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
            yield block


def render_midi(
    plugin: PluginOrChain,
    midi_file: Union[MidiFile, str],
    block_size: int = 512,
    tail_seconds: Optional[Union[float, str]] = None,
    dtype: Optional[type] = None,
    tail_threshold: float = _AUTO_TAIL_THRESHOLD,
    max_tail_seconds: float = _AUTO_TAIL_MAX_SECONDS,
) -> "np.ndarray":
    """Render MIDI file through plugin or chain to a numpy array.

    Args:
        plugin: Plugin or PluginChain instance to render through
        midi_file: MidiFile object or path to MIDI file
        block_size: Audio block size in samples
        tail_seconds: Extra time to render after MIDI ends for reverb/delay tails.
                     "auto" = detect tail by monitoring output level.
        dtype: Output dtype (np.float32 or np.float64). None = np.float32
        tail_threshold: Peak amplitude threshold for auto-tail detection (linear).
        max_tail_seconds: Maximum tail duration for auto mode (safety cap).

    Returns:
        numpy array of shape (channels, total_samples)

    Example:
        >>> plugin = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> audio = minihost.render_midi(plugin, "song.mid")
        >>> print(f"Rendered {audio.shape[1] / 48000:.2f} seconds")
    """
    import numpy as np

    if dtype is None:
        dtype = np.float32

    blocks = list(
        render_midi_stream(
            plugin,
            midi_file,
            block_size,
            tail_seconds,
            tail_threshold=tail_threshold,
            max_tail_seconds=max_tail_seconds,
        )
    )

    if not blocks:
        out_channels = max(plugin.num_output_channels, 2)
        return np.zeros((out_channels, 0), dtype=dtype)

    # Concatenate all blocks
    result = np.concatenate(blocks, axis=1)

    if dtype != np.float32:
        result = result.astype(dtype)

    return result


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

    sample_rate = int(plugin.sample_rate)

    blocks = list(
        render_midi_stream(
            plugin,
            midi_file,
            block_size,
            tail_seconds,
            tail_threshold=tail_threshold,
            max_tail_seconds=max_tail_seconds,
        )
    )

    if not blocks:
        out_channels = max(plugin.num_output_channels, 2)
        empty = np.zeros((out_channels, 0), dtype=np.float32)
        write_audio(output_path, empty, sample_rate, bit_depth=bit_depth)
        return 0

    audio = np.concatenate(blocks, axis=1)
    total_samples = audio.shape[1]

    write_audio(output_path, audio, sample_rate, bit_depth=bit_depth)

    return total_samples


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
        if self._total_samples == 0:
            return 1.0
        return min(1.0, self._current_sample / self._total_samples)

    @property
    def is_finished(self) -> bool:
        """True if rendering is complete."""
        if self._auto_tail_finished:
            return True
        return self._current_sample >= self._total_samples

    @property
    def channels(self) -> int:
        """Number of output channels."""
        return self._out_channels

    def reset(self):
        """Reset renderer to beginning."""
        self._current_sample = 0
        self._event_idx = 0
        self._consecutive_silent = 0
        self._auto_tail_finished = False
        self.plugin.reset()

    def render_block(self) -> "Optional[np.ndarray]":
        """Render next block of audio.

        Returns:
            numpy array of shape (channels, block_size), or None if finished
        """
        if self.is_finished:
            return None

        # Determine block size
        remaining = self._total_samples - self._current_sample
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

        # Auto-tail detection
        if self._auto_tail and self._current_sample > self._midi_end_samples:
            peak = float(self._np.max(self._np.abs(result)))
            if peak < self._tail_threshold:
                self._consecutive_silent += 1
                if self._consecutive_silent >= _AUTO_TAIL_SILENT_BLOCKS:
                    self._auto_tail_finished = True
            else:
                self._consecutive_silent = 0

        return result

    def render_all(self, dtype: Optional[type] = None) -> "np.ndarray":
        """Render all remaining audio.

        Args:
            dtype: Output dtype (np.float32 or np.float64). None = np.float32

        Returns:
            numpy array of shape (channels, remaining_samples)
        """
        if dtype is None:
            dtype = self._np.float32

        blocks = []
        while not self.is_finished:
            block = self.render_block()
            if block is not None:
                blocks.append(block)

        if not blocks:
            return self._np.zeros((self._out_channels, 0), dtype=dtype)

        result = self._np.concatenate(blocks, axis=1)
        if dtype != self._np.float32:
            result = result.astype(dtype)

        return result
