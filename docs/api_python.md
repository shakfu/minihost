# Python API Reference

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

| Method | Description |
|--------|-------------|
| `process(input, output)` | Process audio. Arrays shape: `(channels, frames)`, dtype: `float32` |
| `process_midi(input, output, midi_in)` | Process with MIDI. Returns list of output MIDI events |
| `process_auto(input, output, midi_in, param_changes)` | Process with sample-accurate automation and MIDI |
| `process_sidechain(main_in, main_out, sidechain_in)` | Process with sidechain input |
| `process_double(input, output)` | Process with 64-bit double precision. Arrays dtype: `float64` |

MIDI events are tuples of `(sample_offset, status, data1, data2)`.
Parameter changes are tuples of `(sample_offset, param_index, value)`.

### Parameters

| Method | Description |
|--------|-------------|
| `get_param(index)` | Get normalized value (0.0--1.0) |
| `set_param(index, value)` | Set normalized value (0.0--1.0) |
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

| Method | Description |
|--------|-------------|
| `set_change_callback(callback)` | Register callback `(change_flags: int) -> None` for processor-level changes |
| `set_param_value_callback(callback)` | Register callback `(index: int, value: float) -> None` for plugin-initiated value changes |
| `set_param_gesture_callback(callback)` | Register callback `(index: int, is_begin: bool) -> None` for gesture begin/end |

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
| `process_midi(input, output, midi_events)` | Process with MIDI (MIDI goes to first plugin). Returns output MIDI events |
| `process_auto(input, output, midi_in, param_changes)` | Process with sample-accurate automation and MIDI |
| `get_plugin(index)` | Get plugin by index |
| `reset()` | Reset all plugins |
| `set_non_realtime(enabled)` | Set non-realtime mode for all plugins |

MIDI events are tuples of `(sample_offset, status, data1, data2)`.
Chain parameter changes are tuples of `(sample_offset, plugin_index, param_index, value)` -- the extra `plugin_index` field (0-based) targets a specific plugin in the chain.

---

## AudioDevice

Real-time audio device for plugin playback. Supports context manager protocol.

### Constructor

```python
AudioDevice(
    plugin: Plugin | PluginChain,
    sample_rate: float = 0.0,      # 0 = use system default
    buffer_frames: int = 0,        # 0 = use system default
    output_channels: int = 0,      # 0 = use plugin channels
    midi_input_port: int = -1,     # -1 = no MIDI input
    midi_output_port: int = -1,    # -1 = no MIDI output
)
```

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
    tail_seconds: float | None = None,
    dtype: type | None = None,
) -> np.ndarray
```

Render MIDI to numpy array. Returns shape `(channels, total_samples)`.

```python
render_midi_stream(
    plugin: Plugin | PluginChain,
    midi_file: MidiFile | str,
    block_size: int = 512,
    tail_seconds: float | None = None,
) -> Iterator[np.ndarray]
```

Generator yielding audio blocks of shape `(channels, block_size)`.

```python
render_midi_to_file(
    plugin: Plugin | PluginChain,
    midi_file: MidiFile | str,
    output_path: str,
    block_size: int = 512,
    tail_seconds: float | None = None,
    bit_depth: int = 24,
) -> int
```

Render MIDI to WAV file. Returns number of samples written.

### MidiRenderer Class

```python
MidiRenderer(
    plugin: Plugin | PluginChain,
    midi_file: MidiFile | str,
    block_size: int = 512,
    tail_seconds: float | None = None,
)
```

Stateful renderer for fine-grained control.

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
| `render_block()` | Render next block. Returns `ndarray` or `None` if finished |
| `render_all(dtype=None)` | Render all remaining audio. Returns `ndarray` |
| `reset()` | Reset renderer to beginning |

For all rendering functions, `tail_seconds=None` uses the plugin's reported tail length (clamped to 2s default if 0 or >30s).

---

## Audio File I/O

Read and write audio files via miniaudio.

### Functions

```python
read_audio(path: str | Path) -> tuple[np.ndarray, int]
```

Read audio file. Returns `(data, sample_rate)` where data has shape `(channels, samples)`, dtype `float32`.

Supported formats: WAV, FLAC, MP3, Vorbis.

```python
write_audio(
    path: str | Path,
    data: np.ndarray,
    sample_rate: int,
    bit_depth: int = 24,
) -> None
```

Write WAV file. Data shape: `(channels, samples)`. Bit depth 16 and 24 write integer PCM; 32 writes IEEE float.

```python
get_audio_info(path: str | Path) -> dict
```

Get file metadata without decoding. Returns dict with keys: `channels`, `sample_rate`, `frames`, `duration`.

### Supported Formats

| Format | Read | Write |
|--------|------|-------|
| WAV | Yes | Yes (16/24/32-bit) |
| FLAC | Yes | No |
| MP3 | Yes | No |
| Vorbis | Yes | No |

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

Parse and load Steinberg `.vstpreset` files.

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

## Constants

| Constant | Description |
|----------|-------------|
| `MH_CHANGE_LATENCY` | Change flag: plugin latency changed |
| `MH_CHANGE_PARAM_INFO` | Change flag: parameter info changed |
| `MH_CHANGE_PROGRAM` | Change flag: current program changed |
| `MH_CHANGE_NON_PARAM_STATE` | Change flag: non-parameter state changed |
| `MH_PRECISION_SINGLE` | 32-bit float processing |
| `MH_PRECISION_DOUBLE` | 64-bit double processing |
