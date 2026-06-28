# Python API Reference

## AudioBuffer

`minihost.AudioBuffer` is the canonical container for audio data: planar
float32, JUCE-backed, stdlib-only (numpy not required). Exposes the
DLPack and `__array__` protocols, so it is accepted directly by every
process method, by `numpy.asarray`, and by any other 2D float32
c-contiguous buffer-protocol consumer.

### Constructor

```python
AudioBuffer(channels: int, frames: int)
```

Allocates a `(channels, frames)` planar float32 buffer, zero-initialized.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `channels` | `int` | Number of channels |
| `frames` | `int` | Number of frames per channel |
| `shape` | `tuple[int, int]` | `(channels, frames)` -- matches numpy's `.shape` on 2D arrays |
| `dtype` | `str` (class attr) | Always `"float32"` |

### Indexing

Numpy-style 2-axis indexing with deliberate limits:

| Form | Returns | Notes |
|------|---------|-------|
| `buf[ch, frame]` | `float` | Both indices scalar |
| `buf[ch_slice, frame_slice]` | `AudioBuffer` | New buffer, copy not view |
| `buf[ch_slice, frame_slice] = scalar` | `None` | Scalar broadcast assignment |
| `buf[ch_slice, frame_slice] = buf2` | `None` | Source must match exact shape |

Negative indices supported. Strided slices (`step != 1`), fancy
indexing, boolean indexing, Ellipsis, and single-axis `buf[ch]` raise
`TypeError` directing to `.as_ndarray()`.

### DSP Operations

| Method | Description |
|--------|-------------|
| `clear(start=0, count=None)` | Zero a range (or whole buffer if no args) |
| `apply_gain(gain)` | Multiply every sample by `gain` in place |
| `magnitude(start=0, count=None)` | Peak absolute sample value across all channels in range |
| `copy()` | Deep copy of the buffer |

### numpy interop

| Method | Description |
|--------|-------------|
| `as_ndarray()` | Return a numpy.ndarray view (zero-copy). Requires numpy installed; raises `ImportError` otherwise. |
| `AudioBuffer.from_numpy(arr)` | Construct an AudioBuffer by copying a 2D float32 c-contiguous array (numpy ndarray, another AudioBuffer, etc.). |

`numpy.asarray(buf)` and `np.asarray(buf)` work via `__array__` (zero-copy).

## Plugin

Load and process audio through VST3, AudioUnit, or LV2 plugins.

### Constructor

```python
Plugin(
    path: str,
    sample_rate: float = 48000.0,
    max_block_size: int = 512,
    in_channels: int = 2,
    out_channels: int = 2,
    sidechain_channels: int = 0,
)
```

### Properties

| Property | Type | Writable | Description |
|----------|------|----------|-------------|
| `path` | `str` | No | Plugin file path passed to the constructor |
| `num_params` | `int` | No | Number of parameters |
| `num_input_channels` | `int` | No | Number of input channels |
| `num_output_channels` | `int` | No | Number of output channels |
| `latency_samples` | `int` | No | Processing latency in samples |
| `tail_seconds` | `float` | No | Reverb/delay tail length in seconds |
| `sidechain_channels` | `int` | No | Configured sidechain channel count |
| `num_input_buses` | `int` | No | Number of input buses |
| `num_output_buses` | `int` | No | Number of output buses |
| `num_programs` | `int` | No | Number of factory presets |
| `accepts_midi` | `bool` | No | Whether plugin accepts MIDI input |
| `produces_midi` | `bool` | No | Whether plugin produces MIDI output |
| `is_midi_effect` | `bool` | No | Whether plugin is a MIDI effect |
| `supports_mpe` | `bool` | No | Whether plugin supports MPE |
| `supports_double` | `bool` | No | Whether plugin supports 64-bit processing |
| `sample_rate` | `float` | Yes | Current sample rate (preserves parameter state on change) |
| `program` | `int` | Yes | Current factory preset index |
| `bypass` | `bool` | Yes | Bypass state |
| `non_realtime` | `bool` | Yes | Non-realtime mode (higher-quality offline algorithms) |
| `processing_precision` | `int` | Yes | `MH_PRECISION_SINGLE` or `MH_PRECISION_DOUBLE` |

### Audio Processing

All audio inputs accept `AudioBuffer`, `numpy.ndarray`, or any 2D
float32 c-contiguous buffer-protocol producer.

| Method | Description |
|--------|-------------|
| `process(input, output)` | Process audio. Buffers shape: `(channels, frames)`, dtype: float32 |
| `process_midi(input, output, midi_in)` | Process with MIDI. Returns list of output MIDI events (max 256 per call) |
| `process_auto(input, output, midi_in, param_changes)` | Process with sample-accurate automation and MIDI. Returns output MIDI (max 256) |
| `process_sidechain(main_in, main_out, sidechain_in)` | Process with sidechain input |
| `process_double(input, output)` | Process with 64-bit double precision. Buffers dtype: `float64` (currently numpy-only) |

MIDI events are tuples of `(sample_offset, status, data1, data2)`.
Parameter changes are tuples of `(sample_offset, param_index, value)`.

### Parameters

| Method | Description |
|--------|-------------|
| `get_param(index)` | Get normalized value (0.0--1.0) |
| `set_param(index, value)` | Set normalized value (0.0--1.0) |
| `find_param(name)` | Find parameter index by name (case-insensitive). Raises `RuntimeError` if not found |
| `get_param_by_name(name)` | Get normalized value by parameter name (case-insensitive) |
| `set_param_by_name(name, value)` | Set normalized value by parameter name (case-insensitive) |
| `get_param_info(index)` | Get metadata dict (`name`, `label`, `default`, `num_steps`, `id`, `category`) |
| `param_to_text(index, value)` | Convert normalized value to display string (e.g. `"2500 Hz"`) |
| `param_from_text(index, text)` | Convert display string to normalized value |
| `begin_param_gesture(index)` | Signal start of parameter change gesture |
| `end_param_gesture(index)` | Signal end of parameter change gesture |

### State Management

| Method | Description |
|--------|-------------|
| `get_state()` | Save full plugin state as `bytes` |
| `set_state(data)` | Restore full plugin state from `bytes` |
| `get_program_state()` | Save current program state as `bytes` |
| `set_program_state(data)` | Restore current program state from `bytes` |
| `get_program_name(index)` | Get factory preset name by index |

### Transport and Playback

| Method | Description |
|--------|-------------|
| `set_transport(bpm, time_sig_num=4, time_sig_denom=4, position_samples=0, position_beats=0.0, is_playing=True, is_recording=False, is_looping=False, loop_start=0, loop_end=0)` | Set transport info |
| `clear_transport()` | Clear transport info |
| `reset()` | Reset internal state (clears delay lines, filter states) |

### Bus Layout

| Method | Description |
|--------|-------------|
| `get_bus_info(is_input, bus_index)` | Get bus info dict (`name`, `channels`, `is_main`, `is_enabled`) |
| `check_buses_layout(input_channels, output_channels)` | Check if a bus layout is supported |

### Change Notifications

Callback events are queued internally and never dispatched on the audio thread.
Call `poll_callbacks()` from your main/UI thread to drain the queue and invoke registered callbacks.

| Method | Description |
|--------|-------------|
| `set_change_callback(callback)` | Register callback `(change_flags: int) -> None` for processor-level changes |
| `set_param_value_callback(callback)` | Register callback `(index: int, value: float) -> None` for plugin-initiated value changes |
| `set_param_gesture_callback(callback)` | Register callback `(index: int, is_begin: bool) -> None` for gesture begin/end |
| `poll_callbacks()` | Drain pending events and dispatch to registered callbacks. Returns number of events dispatched |

Change flag constants: `MH_CHANGE_LATENCY`, `MH_CHANGE_PARAM_INFO`, `MH_CHANGE_PROGRAM`, `MH_CHANGE_NON_PARAM_STATE`.

### Miscellaneous

| Method | Description |
|--------|-------------|
| `set_track_properties(name=None, colour=None)` | Set track name and/or color metadata |

---

## PluginChain

Chain multiple plugins for sequential processing.

### Constructor

```python
PluginChain(plugins: list[Plugin])
```

All plugins must share the same sample rate.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `num_plugins` | `int` | Number of plugins in chain |
| `latency_samples` | `int` | Total chain latency (sum of all plugins) |
| `num_input_channels` | `int` | Input channels (from first plugin) |
| `num_output_channels` | `int` | Output channels (from last plugin) |
| `sample_rate` | `float` | Shared sample rate |
| `tail_seconds` | `float` | Maximum tail length (max of all plugins) |

### Methods

| Method | Description |
|--------|-------------|
| `process(input, output)` | Process audio through chain |
| `process_midi(input, output, midi_events)` | Process with MIDI (MIDI goes to first plugin). Returns output MIDI events (max 256) |
| `process_auto(input, output, midi_in, param_changes)` | Process with sample-accurate automation and MIDI. Returns output MIDI (max 256) |
| `get_plugin(index)` | Get plugin by index |
| `reset()` | Reset all plugins |
| `set_non_realtime(enabled)` | Set non-realtime mode for all plugins |

MIDI events are tuples of `(sample_offset, status, data1, data2)`.
Chain parameter changes are tuples of `(sample_offset, plugin_index, param_index, value)` -- the extra `plugin_index` field (0-based) targets a specific plugin in the chain.

---

## PluginBus

Run N `PluginChain` branches in parallel against the same input and sum their
outputs with a per-branch gain (a mix bus). Use it for parallel compression,
dry-bus + reverb-send, multi-band processing, and -- via `process_midi` --
layering one MIDI part across several instruments.

### Constructor

```python
PluginBus(num_in_channels, num_out_channels, max_block_size=8192, sample_rate=48000.0)
```

Every branch added later must accept exactly `num_in_channels` inputs, produce
exactly `num_out_channels` outputs, and run at `sample_rate`; `add_branch`
rejects mismatches with a descriptive error.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `num_branches` | `int` | Number of branches |
| `num_input_channels` | `int` | Configured input channels |
| `num_output_channels` | `int` | Configured output channels |
| `sample_rate` | `float` | Configured sample rate |
| `max_block_size` | `int` | Maximum block size |
| `latency_samples` | `int` | Maximum latency across branches (parallel branches do not accumulate latency) |
| `tail_seconds` | `float` | Maximum tail across branches |

### Methods

| Method | Description |
|--------|-------------|
| `add_branch(chain, gain=1.0)` | Add a `PluginChain` branch with a linear summing gain. Returns the branch index. The bus keeps the branch alive |
| `set_branch_gain(branch_index, gain)` | Set a branch's summing gain (0.0 mutes; muted branches skip processing) |
| `get_branch_gain(branch_index)` | Get a branch's summing gain |
| `process(input, output)` | Fan input to every branch, sum (per-branch gain) into output |
| `process_midi(input, output, midi_in)` | Fan input audio **and** the same MIDI to every branch (MIDI to each branch's first plugin), then sum. The layering primitive. Branch MIDI output is not collected |
| `close()` | Release internal resources (idempotent). Branches are not closed |

Supports the context-manager protocol (`with minihost.PluginBus(...) as bus:`).

---

## PluginGraph

General-DAG executor: arbitrary node-to-node audio and MIDI routing (plugin,
input, output, mix, channel pick/merge, and MIDI nodes). It backs project
files (`load_project` / `render_project`); most users reach it through those
rather than wiring nodes by hand. Build the graph (`add_*`, `connect`,
`set_mix_gain`), call `compile()`, then `render_block()`.

```python
g = minihost.PluginGraph(max_block_size, sample_rate)
src = g.add_input(2); fx = g.add_plugin(plugin); out = g.add_output(2)
g.connect(src, fx); g.connect(fx, out)
g.compile()
g.render_block([in_buf], [out_buf], nframes)
```

Key methods: `add_input`, `add_output`, `add_plugin`, `add_mix`,
`add_pick_channel`, `add_merge_channels`, `add_midi_input`, `add_midi_output`,
`add_midi_processor`, `add_midi_merge`, `connect`, `connect_midi`,
`connect_midi_port`, `set_mix_gain`, `set_node_automation`,
`set_midi_input_events`, `get_midi_output_events`, `compile`, `render_block`,
`close`. See `_core.pyi` for full signatures.

> Renamed in 0.2.0: this is the former `GraphV2`; the former `PluginGraph`
> (parallel bus) is now `PluginBus`. See the [migration guide](migration.md).

---

## AudioDevice

Real-time audio device for plugin playback. Supports context manager protocol.

### Constructor

```python
AudioDevice(
    plugin: Plugin | PluginChain,
    sample_rate: float = 0.0,          # 0 = use system default
    buffer_frames: int = 0,            # 0 = use system default
    output_channels: int = 0,          # 0 = use plugin channels
    midi_input_port: int = -1,         # -1 = no MIDI input
    midi_output_port: int = -1,        # -1 = no MIDI output
    capture: bool = False,             # True = duplex mode (system audio input)
    playback_device_index: int = -1,   # -1 = system default playback device
    capture_device_index: int = -1,    # -1 = system default capture device
)
```

When `capture=True`, the audio device opens in duplex mode: system audio input is captured, processed through the plugin, and played back through speakers. Useful for guitar amp sims, vocal effects, and live processing.

`playback_device_index` and `capture_device_index` accept 0-based indices into the lists returned by `audio_get_playback_devices()` / `audio_get_capture_devices()`. Use `-1` for the system default.

### Properties

| Property | Type | Description |
|----------|------|-------------|
| `is_playing` | `bool` | Whether audio is currently playing |
| `sample_rate` | `float` | Actual device sample rate |
| `buffer_frames` | `int` | Actual buffer size in frames |
| `channels` | `int` | Number of output channels |
| `midi_input_port` | `int` | Connected MIDI input port index (-1 if none) |
| `midi_output_port` | `int` | Connected MIDI output port index (-1 if none) |
| `is_midi_input_virtual` | `bool` | Whether MIDI input is a virtual port |
| `is_midi_output_virtual` | `bool` | Whether MIDI output is a virtual port |
| `input_available` | `int` | Frames available in input ring buffer (0 if not enabled) |

### Methods

| Method | Description |
|--------|-------------|
| `start()` | Start audio playback |
| `stop()` | Stop audio playback |
| `send_midi(status, data1, data2)` | Send MIDI event programmatically |
| `connect_midi_input(port_index)` | Connect MIDI input port |
| `connect_midi_output(port_index)` | Connect MIDI output port |
| `disconnect_midi_input()` | Disconnect MIDI input |
| `disconnect_midi_output()` | Disconnect MIDI output |
| `create_virtual_midi_input(port_name)` | Create virtual MIDI input port |
| `create_virtual_midi_output(port_name)` | Create virtual MIDI output port |
| `enable_input(capacity_frames=0)` | Enable ring buffer audio input. `capacity_frames=0` uses ~0.5s default |
| `disable_input()` | Disable ring buffer audio input (revert to silence) |
| `write_input(data)` | Write `(channels, frames)` audio data into input ring buffer. Accepts AudioBuffer, numpy ndarray, or any 2D float32 c-contig buffer-protocol producer. Returns frames written. Thread-safe |

---

## MidiFile

MIDI file reader/writer.

### Constructor

```python
MidiFile()
```

### Properties

| Property | Type | Writable | Description |
|----------|------|----------|-------------|
| `num_tracks` | `int` | No | Number of tracks |
| `ticks_per_quarter` | `int` | Yes | Ticks per quarter note (resolution) |
| `duration_seconds` | `float` | No | Total duration in seconds |

### Methods

| Method | Description |
|--------|-------------|
| `load(path)` | Load MIDI file. Returns `True` on success |
| `save(path)` | Save MIDI file. Returns `True` on success |
| `add_track()` | Add a new track. Returns track index |
| `add_tempo(track, tick, bpm)` | Add tempo event |
| `add_note_on(track, tick, channel, pitch, velocity)` | Add note on event |
| `add_note_off(track, tick, channel, pitch, velocity=0)` | Add note off event |
| `add_control_change(track, tick, channel, controller, value)` | Add CC event |
| `add_program_change(track, tick, channel, program)` | Add program change |
| `add_pitch_bend(track, tick, channel, value)` | Add pitch bend event |
| `get_events(track)` | Get all events from track as list of dicts |
| `make_absolute_ticks()` | Convert delta ticks to absolute |
| `make_delta_ticks()` | Convert absolute ticks to delta |
| `join_tracks()` | Merge all tracks into one |
| `split_tracks()` | Split tracks by MIDI channel |

### Event Dict Keys

Events returned by `get_events()` have these keys depending on type:

| Key | Types | Description |
|-----|-------|-------------|
| `type` | all | Event type: `note_on`, `note_off`, `control_change`, `program_change`, `pitch_bend`, `tempo` |
| `tick` | all | Tick position |
| `seconds` | all | Time in seconds |
| `channel` | note/CC/program/pitch_bend | MIDI channel (0-15) |
| `pitch` | note_on, note_off | MIDI note number (0-127) |
| `velocity` | note_on, note_off | Velocity (0-127) |
| `controller` | control_change | CC number (0-127) |
| `value` | control_change, pitch_bend | Value |
| `program` | program_change | Program number (0-127) |
| `bpm` | tempo | Tempo in BPM |

---

## MidiIn

Standalone MIDI input for monitoring raw MIDI messages without loading a plugin.

### Static Methods

| Method | Description |
|--------|-------------|
| `MidiIn.open(port_index, callback)` | Open hardware MIDI input port. Callback receives `bytes` |
| `MidiIn.open_virtual(name, callback)` | Create virtual MIDI input port. Callback receives `bytes` |

### Methods

| Method | Description |
|--------|-------------|
| `close()` | Close the MIDI input |

Supports context manager: `with MidiIn.open(0, cb) as m: ...`

---

## MIDI Rendering

Render MIDI files through plugins to produce audio.

### Functions

```python
render_midi(
    plugin: Plugin | PluginChain,
    midi_file: MidiFile | str,
    block_size: int = 512,
    tail_seconds: float | str | None = None,
    dtype: type | None = None,
    tail_threshold: float = 1e-4,
    max_tail_seconds: float = 30.0,
    as_: type | None = None,           # AudioBuffer (default) or numpy.ndarray
) -> AudioBuffer | np.ndarray
```

Render MIDI to a single buffer of shape `(channels, total_samples)`.
Returns `AudioBuffer` by default; pass `as_=numpy.ndarray` for numpy.

```python
render_midi_stream(
    plugin: Plugin | PluginChain,
    midi_file: MidiFile | str,
    block_size: int = 512,
    tail_seconds: float | str | None = None,
    tail_threshold: float = 1e-4,
    max_tail_seconds: float = 30.0,
    as_: type | None = None,           # AudioBuffer (default) or numpy.ndarray
) -> Iterator[AudioBuffer | np.ndarray]
```

Generator yielding audio blocks of shape `(channels, n)` where `n <= block_size`.
Yields `AudioBuffer` by default; pass `as_=numpy.ndarray` for numpy.

```python
render_midi_to_file(
    plugin: Plugin | PluginChain,
    midi_file: MidiFile | str,
    output_path: str,
    block_size: int = 512,
    tail_seconds: float | str | None = None,
    bit_depth: int = 24,
    tail_threshold: float = 1e-4,
    max_tail_seconds: float = 30.0,
) -> int
```

Render MIDI to WAV file. Returns number of samples written.

#### Auto-tail Detection

Pass `tail_seconds="auto"` to automatically detect when the plugin's output (reverb/delay tail) has decayed below a threshold, instead of using a fixed tail duration:

```python
audio = render_midi(plugin, "song.mid", tail_seconds="auto")
audio = render_midi(plugin, "song.mid", tail_seconds="auto", tail_threshold=1e-2)  # -40 dB
```

- `tail_threshold`: peak amplitude threshold in linear (default: `1e-4`, ~-80 dB)
- `max_tail_seconds`: safety cap (default: 30s)
- Rendering stops after 4 consecutive blocks below threshold

### MidiRenderer Class

```python
MidiRenderer(
    plugin: Plugin | PluginChain,
    midi_file: MidiFile | str,
    block_size: int = 512,
    tail_seconds: float | str | None = None,
    tail_threshold: float = 1e-4,
    max_tail_seconds: float = 30.0,
)
```

Stateful renderer for fine-grained control. Supports `tail_seconds="auto"`.

| Property | Type | Description |
|----------|------|-------------|
| `duration_seconds` | `float` | Total duration including tail |
| `midi_duration_seconds` | `float` | MIDI content duration (excluding tail) |
| `total_samples` | `int` | Total samples to render |
| `current_sample` | `int` | Current sample position |
| `current_time` | `float` | Current time in seconds |
| `progress` | `float` | Progress fraction (0.0--1.0) |
| `is_finished` | `bool` | Whether rendering is complete |
| `channels` | `int` | Number of output channels |

| Method | Description |
|--------|-------------|
| `render_block()` | Render next block. Returns `AudioBuffer` (or `None` if finished / fully consumed by latency-comp skip) |
| `render_all(dtype=None, as_=None)` | Render all remaining audio. Returns `AudioBuffer` by default; pass `as_=numpy.ndarray` for numpy |
| `reset()` | Reset renderer to beginning |

For all rendering functions: `tail_seconds=None` uses the plugin's reported tail length (clamped to 2s default if 0 or >30s). `tail_seconds="auto"` enables auto-detection. A numeric value uses that exact duration.

---

## Audio File I/O

Read and write audio files via miniaudio.

### Functions

```python
read_audio(
    path: str | Path,
    as_: type | None = None,           # AudioBuffer (default) or numpy.ndarray
) -> tuple[AudioBuffer | np.ndarray, int]
```

Read audio file. Returns `(data, sample_rate)` where data has shape
`(channels, samples)` and float32 dtype. Default container is
`AudioBuffer`; pass `as_=numpy.ndarray` to receive a numpy array
(requires numpy installed).

Supported formats: WAV, FLAC, MP3, Vorbis.

```python
write_audio(
    path: str | Path,
    data: AudioBuffer | np.ndarray | Any,
    sample_rate: int,
    bit_depth: int = 24,
) -> None
```

Write WAV or FLAC file. Data shape: `(channels, samples)`. Accepts
`AudioBuffer`, numpy ndarray, or any 2D float32 c-contiguous
buffer-protocol producer (DLPack-compatible). Bit depth 16 and 24 write
integer PCM; 32 writes IEEE float.

```python
get_audio_info(path: str | Path) -> dict
```

Get file metadata without decoding. Returns dict with keys: `channels`, `sample_rate`, `frames`, `duration`.

### Supported Formats

| Format | Read | Write |
|--------|------|-------|
| WAV | Yes | Yes (16/24/32-bit) |
| FLAC | Yes | Yes (16/24-bit) |
| MP3 | Yes | No |
| Vorbis | Yes | No |

### Resampling

```python
resample(
    data: AudioBuffer | np.ndarray | Any,
    sample_rate_in: int,
    sample_rate_out: int,
) -> AudioBuffer | np.ndarray
```

Resample audio data to a different sample rate. Input/output shape:
`(channels, frames)`, float32. Accepts AudioBuffer, numpy ndarray, or
any 2D float32 c-contiguous buffer-protocol producer. Return type
matches the input type (AudioBuffer in -> AudioBuffer out;
numpy.ndarray in -> numpy.ndarray out). Uses miniaudio's linear
resampler with 4th-order low-pass anti-aliasing. Returns a copy when
rates are equal.

---

## Offline Processing

High-level helpers that collapse the typical block-iteration loop. See
the `process_audio` / `process_audio_to_file` source for the exact
contract; the headline form is:

```python
process_audio(
    plugin_or_chain: Plugin | PluginChain,
    audio: AudioBuffer | np.ndarray | Any,
    tail_seconds: float = 0.0,
    block_size: int | None = None,
    compensate_latency: bool = True,
) -> AudioBuffer
```

Process in-memory audio through a plugin or chain. Returns a new
`AudioBuffer`. Handles latency compensation (renders extra
`latency_samples` and trims the matching head from output) and tail
rendering (input is zero-padded past the source by `tail_seconds`).

```python
process_audio_to_file(
    plugin_or_chain: Plugin | PluginChain,
    input_path: str | Path,
    output_path: str | Path,
    tail_seconds: float = 0.0,
    block_size: int | None = None,
    bit_depth: int = 24,
    resample_to_plugin_rate: bool = True,
    duplicate_to_stereo: bool = True,
    compensate_latency: bool = True,
) -> int
```

Read, process, and write in one call. Auto-resamples the input to the
plugin's sample rate when they differ; auto-duplicates a mono source to
match the plugin's expected channel count when it expects more.
Returns the number of frames written.

---

## Automation

Utilities for parameter automation and CLI parameter parsing.

### Functions

```python
find_param_by_name(plugin: Plugin, name: str) -> int
```

Find parameter index by name (case-insensitive). Raises `ValueError` if not found.

```python
parse_param_arg(arg_str: str, plugin: Plugin) -> tuple[int, float]
```

Parse CLI `--param` argument string. Formats: `"Name:value"`, `"Name:value:n"` (normalized), `"Name:TextValue"`. Returns `(param_index, normalized_value)`.

```python
parse_automation_file(
    path: str | Path,
    plugin: Plugin,
    sample_rate: int,
    total_length_samples: int,
    block_size: int = 512,
) -> list[tuple[int, int, float]]
```

Parse JSON automation file into parameter change events compatible with `Plugin.process_auto()`. Returns sorted list of `(sample_offset, param_index, value)` tuples.

### Automation File Format

```json
{
    "Param Name": 0.5,
    "Param Name": "TextValue",
    "Param Name": {"0": 0.5, "1.5s": 0.7, "50%": 1.0}
}
```

Keyframe time formats: `"1000"` (sample offset), `"1.5s"` (seconds), `"50%"` (percentage of total length). Linear interpolation between keyframes at block boundaries.

---

## VST3 Presets

Parse, load, and write Steinberg `.vstpreset` files.

### VstPreset Class

| Attribute | Type | Description |
|-----------|------|-------------|
| `class_id` | `str` | Processor component FUID (32-char ASCII) |
| `component_state` | `bytes | None` | Raw processor state (`Comp` chunk) |
| `controller_state` | `bytes | None` | Raw controller state (`Cont` chunk) |

### Functions

```python
read_vstpreset(path: str | Path) -> VstPreset
```

Read and parse a `.vstpreset` file.

```python
load_vstpreset(path: str | Path, plugin: Plugin) -> None
```

Load a `.vstpreset` file into a plugin via `plugin.set_state()`.

```python
write_vstpreset(
    path: str | Path,
    class_id: str,
    component_state: bytes,
    controller_state: bytes | None = None,
) -> None
```

Write a `.vstpreset` file from raw chunk bytes and a processor `class_id` (32-char FUID).

```python
read_class_id_from_bundle(vst3_path: str | Path) -> str
```

Read the processor class ID (FUID) from a VST3 bundle's `Contents/Resources/moduleinfo.json`. Returns a 32-character uppercase hex string. Picks the first entry whose `Category` is `"Audio Module Class"`.

Raises `ValueError` if `moduleinfo.json` is missing (plugin predates VST3 SDK 3.7.5), malformed, or contains no Audio Module Class entry. VST3 only.

```python
save_vstpreset(
    path: str | Path,
    plugin: Plugin,
    class_id: str | None = None,
) -> None
```

Save the plugin's current state to a `.vstpreset` file.

When `class_id` is `None` (the default), the FUID is auto-detected from the plugin bundle's `moduleinfo.json` via `read_class_id_from_bundle(plugin.path)`. This requires the plugin to be VST3 and built against VST3 SDK 3.7.5 or newer (which all modern plugins ship).

For legacy plugins without `moduleinfo.json`, or for non-VST3 formats, pass `class_id` explicitly or use `load_vstpreset()` to inherit one from an existing preset. Raises `ValueError` if `class_id` is `None` and cannot be auto-detected -- the function never silently writes a placeholder.

---

## Async Plugin Loading

```python
open_async(
    path: str,
    sample_rate: float = 48000.0,
    max_block_size: int = 512,
    in_channels: int = 2,
    out_channels: int = 2,
    sidechain_channels: int = 0,
) -> concurrent.futures.Future
```

Load a plugin in a background thread. Returns a `concurrent.futures.Future` whose `.result()` is the loaded `Plugin`. Useful for large sample-library plugins that take seconds to load.

```python
future = minihost.open_async("/path/to/heavy_synth.vst3")
# ... do other work ...
plugin = future.result()  # blocks until ready
```

---

## Plugin Discovery

### Functions

```python
probe(path: str) -> dict
```

Get plugin metadata without full instantiation. Returns dict with plugin info.

```python
scan_directory(directory_path: str) -> list[dict]
```

Recursively scan a directory for plugins (VST3, AudioUnit). Returns list of plugin info dicts.

---

## MIDI Port Enumeration

### Functions

```python
midi_get_input_ports() -> list[dict]
```

Get list of available MIDI input ports. Each dict has `index` and `name`.

```python
midi_get_output_ports() -> list[dict]
```

Get list of available MIDI output ports. Each dict has `index` and `name`.

---

## Audio Device Enumeration

### Functions

```python
audio_get_playback_devices() -> list[dict]
```

Get list of available audio playback devices. Each dict has keys `index`, `name`, and `is_default`.

```python
audio_get_capture_devices() -> list[dict]
```

Get list of available audio capture devices (for duplex / `capture=True` mode). Each dict has keys `index`, `name`, and `is_default`.

Pass the `index` value into `AudioDevice(..., playback_device_index=N, capture_device_index=N)` to target a specific device.

---

## Constants

| Constant | Description |
|----------|-------------|
| `MH_CHANGE_LATENCY` | Change flag: plugin latency changed |
| `MH_CHANGE_PARAM_INFO` | Change flag: parameter info changed |
| `MH_CHANGE_PROGRAM` | Change flag: current program changed |
| `MH_CHANGE_NON_PARAM_STATE` | Change flag: non-parameter state changed |
| `MH_PRECISION_SINGLE` | 32-bit float processing |
| `MH_PRECISION_DOUBLE` | 64-bit double processing |
