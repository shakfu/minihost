"""MIDI file rendering utilities for minihost.

This module provides functions to render MIDI files through audio plugins,
supporting output to numpy arrays, audio files, or streaming generators.
"""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING, Iterator, Optional, Union

if TYPE_CHECKING:
    import numpy as np
    from numpy.typing import NDArray

from minihost._core import Plugin, MidiFile


def _build_tempo_map(midi_file: MidiFile) -> list[tuple[int, float]]:
    """Build a tempo map from MIDI file events.

    Returns list of (tick, microseconds_per_quarter) sorted by tick.
    """
    tempo_map = []

    for track_idx in range(midi_file.num_tracks):
        events = midi_file.get_events(track_idx)
        for event in events:
            if event.get('type') == 'tempo':
                bpm = event['bpm']
                us_per_quarter = 60_000_000.0 / bpm
                tempo_map.append((event['tick'], us_per_quarter))

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
            event_type = event.get('type')
            if event_type in ('note_on', 'note_off', 'control_change',
                             'program_change', 'pitch_bend'):
                all_events.append(event)

    # Sort by tick
    all_events.sort(key=lambda x: x['tick'])
    return all_events


def _event_to_midi_tuple(event: dict, sample_offset: int) -> tuple[int, int, int, int]:
    """Convert event dict to (sample_offset, status, data1, data2) tuple."""
    event_type = event['type']
    channel = event.get('channel', 0)

    if event_type == 'note_on':
        status = 0x90 | channel
        data1 = event['pitch']
        data2 = event['velocity']
    elif event_type == 'note_off':
        status = 0x80 | channel
        data1 = event['pitch']
        data2 = event.get('velocity', 0)
    elif event_type == 'control_change':
        status = 0xB0 | channel
        data1 = event['controller']
        data2 = event['value']
    elif event_type == 'program_change':
        status = 0xC0 | channel
        data1 = event['program']
        data2 = 0
    elif event_type == 'pitch_bend':
        status = 0xE0 | channel
        value = event['value']
        data1 = value & 0x7F
        data2 = (value >> 7) & 0x7F
    else:
        return None

    return (sample_offset, status, data1, data2)


def render_midi_stream(
    plugin: Plugin,
    midi_file: Union[MidiFile, str],
    block_size: int = 512,
    tail_seconds: Optional[float] = None,
) -> "Iterator[np.ndarray]":
    """Render MIDI file through plugin as a generator of audio blocks.

    Args:
        plugin: Plugin instance to render through (should be a synth/instrument)
        midi_file: MidiFile object or path to MIDI file
        block_size: Audio block size in samples
        tail_seconds: Extra time to render after MIDI ends for reverb/delay tails.
                     None = use plugin.tail_seconds, 0 = no tail

    Yields:
        numpy arrays of shape (channels, block_size) containing rendered audio

    Example:
        >>> plugin = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> for block in minihost.render_midi_stream(plugin, "song.mid"):
        ...     # Process or write each block
        ...     pass
    """
    import numpy as np

    # Load MIDI file if path provided
    if isinstance(midi_file, str):
        mf = MidiFile()
        if not mf.load(midi_file):
            raise RuntimeError(f"Failed to load MIDI file: {midi_file}")
        midi_file = mf

    # Get parameters
    sample_rate = plugin.sample_rate
    tpq = midi_file.ticks_per_quarter
    in_channels = max(plugin.num_input_channels, 2)
    out_channels = max(plugin.num_output_channels, 2)

    # Determine tail length
    if tail_seconds is None:
        tail_seconds = plugin.tail_seconds
        # Reasonable default if plugin reports 0 or very large
        if tail_seconds <= 0 or tail_seconds > 30:
            tail_seconds = 2.0

    # Build tempo map and collect events
    tempo_map = _build_tempo_map(midi_file)
    all_events = _collect_midi_events(midi_file)

    # Find total duration from last event
    if all_events:
        last_tick = max(e['tick'] for e in all_events)
        midi_duration = _tick_to_seconds(last_tick, tempo_map, tpq)
    else:
        midi_duration = 0.0

    total_duration = midi_duration + tail_seconds
    total_samples = _seconds_to_samples(total_duration, sample_rate)

    # Convert all events to sample positions
    events_with_samples = []
    for event in all_events:
        tick = event['tick']
        seconds = _tick_to_seconds(tick, tempo_map, tpq)
        sample_pos = _seconds_to_samples(seconds, sample_rate)
        events_with_samples.append((sample_pos, event))

    # Create buffers
    input_buffer = np.zeros((in_channels, block_size), dtype=np.float32)
    output_buffer = np.zeros((out_channels, block_size), dtype=np.float32)

    # Render loop
    current_sample = 0
    event_idx = 0

    while current_sample < total_samples:
        # Determine block size for this iteration
        remaining = total_samples - current_sample
        this_block_size = min(block_size, remaining)

        # Collect MIDI events for this block
        block_events = []
        while event_idx < len(events_with_samples):
            sample_pos, event = events_with_samples[event_idx]
            if sample_pos >= current_sample + this_block_size:
                break

            # Calculate offset within block
            offset = sample_pos - current_sample
            offset = max(0, min(offset, this_block_size - 1))

            midi_tuple = _event_to_midi_tuple(event, offset)
            if midi_tuple:
                block_events.append(midi_tuple)

            event_idx += 1

        # Clear input buffer (synths don't need input)
        input_buffer.fill(0)

        # Process block
        if this_block_size < block_size:
            # Last partial block
            in_slice = input_buffer[:, :this_block_size].copy()
            out_slice = np.zeros((out_channels, this_block_size), dtype=np.float32)
            plugin.process_midi(in_slice, out_slice, block_events)
            yield out_slice
        else:
            plugin.process_midi(input_buffer, output_buffer, block_events)
            yield output_buffer.copy()

        current_sample += this_block_size


def render_midi(
    plugin: Plugin,
    midi_file: Union[MidiFile, str],
    block_size: int = 512,
    tail_seconds: Optional[float] = None,
    dtype: Optional[type] = None,
) -> "np.ndarray":
    """Render MIDI file through plugin to a numpy array.

    Args:
        plugin: Plugin instance to render through
        midi_file: MidiFile object or path to MIDI file
        block_size: Audio block size in samples
        tail_seconds: Extra time to render after MIDI ends for reverb/delay tails
        dtype: Output dtype (np.float32 or np.float64). None = np.float32

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

    blocks = list(render_midi_stream(plugin, midi_file, block_size, tail_seconds))

    if not blocks:
        out_channels = max(plugin.num_output_channels, 2)
        return np.zeros((out_channels, 0), dtype=dtype)

    # Concatenate all blocks
    result = np.concatenate(blocks, axis=1)

    if dtype != np.float32:
        result = result.astype(dtype)

    return result


def _write_wav_header(f, channels: int, sample_rate: int, bits_per_sample: int, data_size: int):
    """Write WAV file header."""
    byte_rate = sample_rate * channels * bits_per_sample // 8
    block_align = channels * bits_per_sample // 8

    # RIFF header
    f.write(b'RIFF')
    f.write(struct.pack('<I', 36 + data_size))  # File size - 8
    f.write(b'WAVE')

    # fmt chunk
    f.write(b'fmt ')
    f.write(struct.pack('<I', 16))  # Chunk size

    if bits_per_sample == 32:
        audio_format = 3  # IEEE float
    else:
        audio_format = 1  # PCM

    f.write(struct.pack('<HHIIHH',
        audio_format,      # Audio format
        channels,          # Number of channels
        sample_rate,       # Sample rate
        byte_rate,         # Byte rate
        block_align,       # Block align
        bits_per_sample    # Bits per sample
    ))

    # data chunk header
    f.write(b'data')
    f.write(struct.pack('<I', data_size))


def render_midi_to_file(
    plugin: Plugin,
    midi_file: Union[MidiFile, str],
    output_path: str,
    block_size: int = 512,
    tail_seconds: Optional[float] = None,
    bit_depth: int = 24,
) -> int:
    """Render MIDI file through plugin and write to audio file.

    Args:
        plugin: Plugin instance to render through
        midi_file: MidiFile object or path to MIDI file
        output_path: Output file path (currently only .wav supported)
        block_size: Audio block size in samples
        tail_seconds: Extra time to render after MIDI ends
        bit_depth: Output bit depth (16, 24, or 32 for float)

    Returns:
        Number of samples written

    Example:
        >>> plugin = minihost.Plugin("synth.vst3", sample_rate=48000)
        >>> samples = minihost.render_midi_to_file(plugin, "song.mid", "output.wav")
        >>> print(f"Wrote {samples} samples")
    """
    import numpy as np

    if not output_path.lower().endswith('.wav'):
        raise ValueError("Only .wav output is currently supported")

    if bit_depth not in (16, 24, 32):
        raise ValueError("bit_depth must be 16, 24, or 32")

    sample_rate = int(plugin.sample_rate)
    out_channels = max(plugin.num_output_channels, 2)

    # Collect all audio first to know total size
    # (Could also write header placeholder and seek back, but this is simpler)
    blocks = list(render_midi_stream(plugin, midi_file, block_size, tail_seconds))

    if not blocks:
        # Write empty WAV file
        with open(output_path, 'wb') as f:
            _write_wav_header(f, out_channels, sample_rate, bit_depth, 0)
        return 0

    audio = np.concatenate(blocks, axis=1)
    total_samples = audio.shape[1]

    # Calculate data size
    if bit_depth == 32:
        bytes_per_sample = 4
    elif bit_depth == 24:
        bytes_per_sample = 3
    else:
        bytes_per_sample = 2

    data_size = total_samples * out_channels * bytes_per_sample

    with open(output_path, 'wb') as f:
        _write_wav_header(f, out_channels, sample_rate, bit_depth, data_size)

        # Interleave channels and write
        # audio is (channels, samples), need to interleave to (samples * channels)
        interleaved = audio.T.flatten()  # Now (samples * channels,)

        if bit_depth == 32:
            # 32-bit float
            f.write(interleaved.astype(np.float32).tobytes())
        elif bit_depth == 24:
            # 24-bit PCM
            # Scale to 24-bit range and convert
            scaled = np.clip(interleaved * 8388607.0, -8388608, 8388607).astype(np.int32)
            # Write as 3 bytes per sample (little-endian)
            for sample in scaled:
                f.write(struct.pack('<i', sample)[:3])
        else:
            # 16-bit PCM
            scaled = np.clip(interleaved * 32767.0, -32768, 32767).astype(np.int16)
            f.write(scaled.tobytes())

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
        plugin: Plugin,
        midi_file: Union[MidiFile, str],
        block_size: int = 512,
        tail_seconds: Optional[float] = None,
    ):
        """Initialize renderer.

        Args:
            plugin: Plugin instance to render through
            midi_file: MidiFile object or path to MIDI file
            block_size: Audio block size in samples
            tail_seconds: Extra time after MIDI ends for tails
        """
        import numpy as np
        self._np = np  # Store for later use

        self.plugin = plugin
        self.block_size = block_size

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
        if tail_seconds is None:
            tail_seconds = plugin.tail_seconds
            if tail_seconds <= 0 or tail_seconds > 30:
                tail_seconds = 2.0
        self._tail_seconds = tail_seconds

        # Build tempo map and collect events
        self._tempo_map = _build_tempo_map(self._midi_file)
        all_events = _collect_midi_events(self._midi_file)

        # Find total duration
        if all_events:
            last_tick = max(e['tick'] for e in all_events)
            self._midi_duration = _tick_to_seconds(last_tick, self._tempo_map, self._tpq)
        else:
            self._midi_duration = 0.0

        self._total_duration = self._midi_duration + self._tail_seconds
        self._total_samples = _seconds_to_samples(self._total_duration, self.sample_rate)

        # Convert events to sample positions
        self._events_with_samples = []
        for event in all_events:
            tick = event['tick']
            seconds = _tick_to_seconds(tick, self._tempo_map, self._tpq)
            sample_pos = _seconds_to_samples(seconds, self.sample_rate)
            self._events_with_samples.append((sample_pos, event))

        # Create buffers
        self._input_buffer = self._np.zeros((self._in_channels, block_size), dtype=self._np.float32)
        self._output_buffer = self._np.zeros((self._out_channels, block_size), dtype=self._np.float32)

        # State
        self._current_sample = 0
        self._event_idx = 0

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
        return self._current_sample >= self._total_samples

    @property
    def channels(self) -> int:
        """Number of output channels."""
        return self._out_channels

    def reset(self):
        """Reset renderer to beginning."""
        self._current_sample = 0
        self._event_idx = 0
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
            out_slice = self._np.zeros((self._out_channels, this_block_size), dtype=self._np.float32)
            self.plugin.process_midi(in_slice, out_slice, block_events)
            result = out_slice
        else:
            self.plugin.process_midi(self._input_buffer, self._output_buffer, block_events)
            result = self._output_buffer.copy()

        self._current_sample += this_block_size
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
