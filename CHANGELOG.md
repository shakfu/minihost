# Changelog

## [Unreleased]

## [0.1.5]

### Added

- **Audio device selection** -- enumerate and select specific playback/capture audio devices
  - C API: `MH_AudioDeviceInfo`, `mh_audio_enumerate_playback_devices()`, `mh_audio_enumerate_capture_devices()`; new `playback_device_index` and `capture_device_index` fields on `MH_AudioConfig`
  - Python: `minihost.audio_get_playback_devices()`, `minihost.audio_get_capture_devices()`; `AudioDevice(..., playback_device_index=N, capture_device_index=N)`
  - Python CLI: new `minihost devices` subcommand lists available devices; `minihost play --playback-device` and `--capture-device` accept either an index or a case-insensitive substring of the device name
  - C/C++ CLIs: new `devices` subcommand (text + `-j`/`--json` output). The C/C++ CLIs have no real-time `play` command, so device-selection flags are not applicable there.
- **Preset management CLI** -- `presets <plugin>` subcommand extended across all three frontends
  - Default: lists all factory presets (no longer truncated at 10 like `info`); `-j/--json` for structured output
  - `--save FILE.vstpreset` saves the current plugin state as a `.vstpreset`
  - Combinable with `--program N`, `-s/--state FILE`, or `--load-vstpreset FILE` to apply an input state before saving; when `--load-vstpreset` is used, the source file's class_id is preserved in the output
  - `-y`/`--overwrite` allows overwriting an existing `--save` target
  - Byte-exact round-trip verified between Python, C, and C++ CLIs writing/reading the same `.vstpreset`

- **`minihost_vstpreset.h/.c` in libminihost** -- new C module exposing `mh_vstpreset_read()`, `mh_vstpreset_write()`, and `mh_vstpreset_free()` for programmatic .vstpreset I/O from C/C++ callers (portable little-endian packing, no external dependencies)
- **`write_vstpreset()` / `save_vstpreset()` in `minihost.vstpreset`** -- programmatic .vstpreset writer to complement the existing reader

- **C/C++ CLI feature parity with Python frontend** -- brought `minihost_c` and `minihost_cpp` up to date with features already available in libminihost and the Python CLI
  - `process` (both CLIs): audio file I/O via `minihost_audiofile.h` -- process WAV, FLAC, MP3 input and write WAV/FLAC output (raw float32 retained as fallback for non-audio extensions)
  - `process` (both CLIs): `-i`/`--input` and `-o`/`--output` named arguments for input/output files (C CLI retains legacy positional syntax)
  - `process` (both CLIs): latency compensation -- output automatically trimmed using `mh_get_latency_samples()`
  - `process` (both CLIs): `-p`/`--preset N` -- load factory preset before processing via `mh_set_program()`
  - `process` (both CLIs): `--param "Name:value"` -- set parameters by name or index (repeatable), dispatched via `mh_process_auto()` for sample-accurate application
  - `process` (both CLIs): `--sidechain FILE` -- sidechain input via `mh_open_ex()` + `mh_process_sidechain()`
  - `process` (both CLIs): `--non-realtime` -- enable higher-quality offline processing via `mh_set_non_realtime()`
  - `process` (both CLIs): `--bpm BPM` -- set transport tempo for tempo-synced plugins via `mh_set_transport()`
  - `process` (both CLIs): `--bit-depth 16|24|32` -- control output bit depth (default: 24)
  - `process` (C++ CLI only): `-m`/`--midi-input FILE` -- render MIDI files through synth/effect plugins via midifile library + `mh_process_midi()`
  - `process` (C++ CLI only): `-t`/`--tail SECONDS` -- configurable tail length for MIDI-only rendering (default: 2.0s)
  - `params` (both CLIs): `-V`/`--verbose` -- extended parameter display with ranges, defaults, and flags using `mh_param_to_text()`
  - `info` (both CLIs): `--probe` -- lightweight metadata-only mode (no full plugin load)
  - `info` (both CLIs): `-j`/`--json` -- JSON output with merged probe and runtime info

### Changed

- `minihost_cpp` now links against `minihost_audio` and `midifile` libraries
- `minihost_c` now links against `minihost_audio` library
- **`save_vstpreset` now produces valid VST3 FUIDs.** When called with `class_id=None` (the default), the FUID is auto-detected from the plugin bundle's `Contents/Resources/moduleinfo.json` instead of writing a placeholder string. This requires the plugin to be built against VST3 SDK 3.7.5+ (which all modern plugins ship). For legacy plugins, callers must pass `class_id` explicitly or use `load_vstpreset()` to inherit one from an existing preset; there is no silent fallback. The same change applies to the `presets <plugin> --save` subcommand across all three CLI frontends.
- `Plugin` Python class and `MH_Plugin` C struct now expose the constructor's plugin path via `Plugin.path` (Python) / `mh_get_path()` (C).

### Fixed

- **`.vstpreset` files written by `save_vstpreset()` (and the `presets --save` CLI) previously contained a bogus class ID** -- either the literal `"minihost_unknown"` or an 8-character hash from JUCE's `PluginDescription.uniqueId`, neither of which is a valid 32-character VST3 FUID. Files written this way round-tripped through minihost's own loader but were unrecognised by other VST3 hosts. Fixed by reading the real processor FUID from the plugin bundle's `moduleinfo.json` (see Changed). New `mh_vstpreset_read_class_id_from_bundle()` C function and `minihost.vstpreset.read_class_id_from_bundle()` Python helper expose the underlying lookup.

## [0.1.4]

### Added

- **Audio input for effect processing** -- lock-free ring buffer API for feeding audio through effect plugins in real time, without GIL contention on the audio thread
  - C API: `mh_audio_enable_input()`, `mh_audio_disable_input()`, `mh_audio_write_input()`, `mh_audio_input_available()`
  - C internals: `MH_AudioRingBuffer` -- SPSC lock-free ring buffer (`audio_ringbuffer.h/.cpp`)
  - Python: `AudioDevice.enable_input(capacity_frames=0)`, `disable_input()`, `write_input(data)`, `input_available` property
  - Example: `examples/audio_input.py` -- sine wave and file-through-effect demos

- **Offline bounce with automatic tail detection** -- `tail_seconds="auto"` in render functions monitors output amplitude after MIDI ends and stops when it decays below a threshold
  - `render_midi_stream()`, `render_midi()`, `render_midi_to_file()`, `MidiRenderer` all accept `tail_seconds="auto"`
  - Configurable `tail_threshold` (default: -80 dB / `1e-4` linear) and `max_tail_seconds` (default: 30s safety cap)
  - Stops after 4 consecutive blocks below threshold to avoid cutting during brief silences
  - Example: `examples/auto_tail.py` -- compares fixed vs auto tail at different thresholds

- **Duplex audio (capture) for real-time effect processing** -- system audio input routed through plugin via miniaudio duplex mode
  - C layer: `MH_AudioConfig.capture` field; when set, audio device opens in duplex mode and the audio callback de-interleaves capture input directly into the plugin's input buffers (zero additional latency)
  - Python: `AudioDevice(plugin, capture=True)` -- new `capture` parameter on both `Plugin` and `PluginChain` constructors
  - CLI: `minihost play /path/to/effect.vst3 --input` (`-i`) enables duplex mode for live effect processing (guitar through amp sim, vocal processing, etc.)

- **Batch / multi-file processing in CLI** -- glob pattern expansion and directory output for `minihost process`
  - `-i "*.wav"` expands glob patterns; `-o output/` writes each result to the output directory
  - Batch mode detected when output path is a directory (exists or ends with `/`) and input contains audio files (no MIDI)
  - Plugin loaded once, state saved and restored between files for consistent processing
  - Skips existing output files unless `-y`/`--overwrite` is set
  - Example: `minihost process reverb.vst3 -i "drums/*.wav" -o processed/`

- **Sample rate conversion / resampling** -- built-in resampling using miniaudio's `ma_resampler` (linear interpolation with 4th-order low-pass anti-aliasing filter)
  - C API: `mh_audio_resample()` in `minihost_audiofile.h` -- resamples interleaved float32 audio between any two sample rates
  - Python: `minihost.resample(data, sr_in, sr_out)` -- takes `(channels, frames)` numpy array, returns resampled array
  - CLI: `minihost process` and batch mode automatically resample mismatched input files to match the plugin's sample rate; use `--no-resample` to error instead
  - CLI: `minihost resample input.wav -o output.wav -r 48000` -- standalone resampling subcommand with `--bit-depth` and `-y`/`--overwrite` options
  - No-op fast path when source and target rates are equal (memcpy, no resampler init)

### Tests

- **Render internals unit tests** (`tests/test_render_internals.py`) -- 55 tests covering `_build_tempo_map`, `_tick_to_seconds`, `_collect_midi_events`, `_event_to_midi_tuple`, `_seconds_to_samples`, and end-to-end tempo map integration
- **CLI unit tests** (`tests/test_cli.py`) -- 84 tests covering argument parsing for all 7 subcommands, global options, error paths, `--input` capture flag for `play`, glob expansion, batch output detection, batch error paths, `--no-resample` flag, and `resample` subcommand (arg parsing + functional tests)
- **Resampling tests** (`tests/test_audio_io.py`) -- 7 tests covering upsample, downsample, identity (same rate), stereo, silence preservation, large ratio (8k to 48k), and round-trip frame count

## [0.1.3]

### Added

- `mh_chain_process_auto()` for sample-accurate parameter automation across plugin chains
  - New `MH_ChainParamChange` struct with `plugin_index` field to target specific plugins in the chain
  - Python: `PluginChain.process_auto(input, output, midi_in, param_changes)` with 4-tuple param changes `(sample_offset, plugin_index, param_index, value)`

- FLAC write support in `mh_audio_write()` via vendored [tflac](https://github.com/jprjr/tflac) encoder (BSD-0, single-header C89)
  - Supports 16-bit and 24-bit FLAC output; 32-bit raises an error (FLAC max is 24-bit)
  - Format selected by file extension: `.wav` for WAV, `.flac` for FLAC
  - Python: `write_audio("out.flac", data, sr, bit_depth=24)` works without any API changes

### Fixed

- Fixed stale version assertion in `test_minihost.py` (`"0.1.1"` -> `"0.1.2"`)
- Removed dead bit-depth auto-detection code in CLI `process` subcommand that relied on `get_audio_info()` returning a `subtype` key (removed in 0.1.2); now defaults to 24-bit when `--bit-depth` is not specified
- Fixed `render_midi_to_file()` docstring listing unsupported output formats (FLAC, AIFF, OGG); only WAV is supported

## [0.1.2]

### Added

- Audio file I/O via miniaudio (C layer): `mh_audio_read()`, `mh_audio_write()`, `mh_audio_get_file_info()`
  - Python: `minihost.read_audio()`, `minihost.write_audio()`, `minihost.get_audio_info()` now backed by miniaudio
  - Read support: WAV, FLAC, MP3, Vorbis
  - Write support: WAV only (16-bit PCM, 24-bit PCM, 32-bit float)
- `MidiIn` class for standalone MIDI input monitoring (no plugin required)
  - `MidiIn.open(port_index, callback)` -- open a hardware MIDI input port with raw bytes callback
  - `MidiIn.open_virtual(name, callback)` -- create a virtual MIDI input port with raw bytes callback
  - `close()` method and context manager (`with`) support
  - Python: `minihost.MidiIn`
- MIDI monitor mode in `minihost midi` CLI subcommand
  - `minihost midi -m N` -- monitor incoming MIDI on hardware port N
  - `minihost midi --virtual-midi NAME` -- create a virtual MIDI port and monitor it
  - Human-readable output: Note On/Off (with note names), CC, Pitch Bend, Program Change, Channel Pressure, Poly Aftertouch, SysEx

### Changed

- Merged `probe` CLI subcommand into `info` -- use `minihost info <plugin> --probe` for lightweight metadata-only mode
  - `minihost info` now shows full runtime details by default (was already doing this)
  - `minihost info --probe` replaces the old `minihost probe` (no full plugin load)
  - `minihost info --json` outputs combined probe + runtime data as JSON
- Merged `render` CLI subcommand into `process` -- use `minihost process <plugin> -m song.mid -o output.wav` instead of `minihost render`
- Added `-t, --tail` option to `process` subcommand (default: 2.0s) for configurable tail length in MIDI-only synth mode
- Renamed `midi-ports` CLI subcommand to `midi`

### Removed

- `soundfile` runtime dependency -- audio file I/O now uses miniaudio (already vendored)
  - AIFF and OGG write support removed (miniaudio encoder is WAV-only)
  - `get_audio_info()` no longer returns `format` or `subtype` keys

## [0.1.1]

### Added

- MIDI capability queries on live plugin instances via `MH_Info` fields: `accepts_midi`, `produces_midi`, `is_midi_effect`, `supports_mpe`
  - Python: `Plugin.accepts_midi`, `Plugin.produces_midi`, `Plugin.is_midi_effect`, `Plugin.supports_mpe` read-only properties
- Stable parameter IDs via `MH_ParamInfo.id` field (uses `getParameterID()` for version-safe state management)
  - Python: `get_param_info()` dict now includes `"id"` key
- Parameter categories via `MH_ParamInfo.category` field with `MH_PARAM_CATEGORY_*` constants
  - Python: `get_param_info()` dict now includes `"category"` key
- Bus layout validation via `mh_check_buses_layout()` -- query whether a bus layout is supported before attempting to apply it
  - Python: `Plugin.check_buses_layout(input_channels, output_channels) -> bool`
- Change notifications via `AudioProcessorListener` integration
  - `mh_set_change_callback()` -- processor-level changes (latency, param info, program, non-param state) with `MH_CHANGE_*` bitmask flags
  - `mh_set_param_value_callback()` -- plugin-initiated parameter value changes
  - `mh_set_param_gesture_callback()` -- parameter gesture begin/end from plugin UI
  - Python: `Plugin.set_change_callback()`, `Plugin.set_param_value_callback()`, `Plugin.set_param_gesture_callback()`
  - Python: module-level constants `MH_CHANGE_LATENCY`, `MH_CHANGE_PARAM_INFO`, `MH_CHANGE_PROGRAM`, `MH_CHANGE_NON_PARAM_STATE`
- Parameter gesture bracketing via `mh_begin_param_gesture()` / `mh_end_param_gesture()` -- signal start/end of automation drags to plugins
  - Python: `Plugin.begin_param_gesture(index)`, `Plugin.end_param_gesture(index)`
- Current program state save/restore via `mh_get_program_state_size()` / `mh_get_program_state()` / `mh_set_program_state()` -- lighter-weight per-program state
  - Python: `Plugin.get_program_state() -> bytes`, `Plugin.set_program_state(data)`
- Processing precision selection via `mh_get_processing_precision()` / `mh_set_processing_precision()` -- explicitly select float vs double processing mode
  - Python: `Plugin.processing_precision` read/write property
  - Module-level constants `MH_PRECISION_SINGLE`, `MH_PRECISION_DOUBLE`
- Track properties via `mh_set_track_properties()` -- pass track name/color metadata to plugins
  - Python: `Plugin.set_track_properties(name=None, colour=None)`

- LV2 plugin format support (`JUCE_PLUGINHOST_LV2=1`)
  - Load and process LV2 plugins on all platforms
  - `mh_scan_directory()` now finds `.lv2` bundles
  - CLI updated with LV2 examples for `probe`, `info`, and `scan` commands
- Headless build mode (`MINIHOST_HEADLESS`, default ON)
  - Builds without GUI dependencies using JUCE's `juce_audio_processors_headless` module (requires JUCE 8.0.11+)
  - Uses headless format classes (`VST3PluginFormatHeadless`, `AudioUnitPluginFormatHeadless`, `LV2PluginFormatHeadless`)
  - Disable with `cmake -DMINIHOST_HEADLESS=OFF`

### Changed

- `addFormat()` calls now use `std::make_unique` instead of raw `new`
- Removed unused `KnownPluginList` variable from `findFirstTypeForFile`
- Added cross-platform Python script (`scripts/download_juce.py`) for downloading JUCE
  - Works on Windows, macOS, and Linux without requiring bash
  - Uses only Python standard library (no external dependencies)
  - Handles Python 3.14+ tarfile deprecation warning
- Bumped default JUCE version from 8.0.6 to 8.0.12 (required for `juce_audio_processors_headless`)
- Updated CI workflow to use Python script instead of bash for JUCE download
- Updated Makefile to prefer Python script with bash fallback

## [0.1.0]

### Added

#### Plugin Chaining

- `MH_PluginChain` opaque struct for managing chains of plugins
- `mh_chain_create()` - Create a chain from an array of plugins (all must have same sample rate)
- `mh_chain_close()` - Close chain and free resources (does not close individual plugins)
- `mh_chain_process()` - Process audio through the chain
- `mh_chain_process_midi_io()` - Process audio with MIDI I/O (MIDI goes to first plugin only)
- `mh_chain_get_latency_samples()` - Get total chain latency (sum of all plugin latencies)
- `mh_chain_get_num_plugins()` - Get number of plugins in the chain
- `mh_chain_get_plugin()` - Get plugin from chain by index
- `mh_chain_get_num_input_channels()` - Get input channel count (from first plugin)
- `mh_chain_get_num_output_channels()` - Get output channel count (from last plugin)
- `mh_chain_get_sample_rate()` - Get sample rate (all plugins share same rate)
- `mh_chain_get_max_block_size()` - Get maximum block size
- `mh_chain_reset()` - Reset all plugins in the chain
- `mh_chain_set_non_realtime()` - Set non-realtime mode for all plugins
- `mh_chain_get_tail_seconds()` - Get maximum tail length (max of all plugin tails)
- `mh_audio_open_chain()` - Open audio device for real-time playback through a plugin chain
- Python `PluginChain` class with `process()`, `process_midi()`, `reset()`, `set_non_realtime()`, `get_plugin()` methods
- Python `PluginChain` properties: `num_plugins`, `latency_samples`, `num_input_channels`, `num_output_channels`, `sample_rate`, `tail_seconds`
- `AudioDevice` now accepts either `Plugin` or `PluginChain`
- `render_midi()`, `render_midi_stream()`, `render_midi_to_file()`, and `MidiRenderer` now accept either `Plugin` or `PluginChain`

#### Real-time Audio Playback (miniaudio integration)

- `MH_AudioDevice` opaque struct for audio device management
- `MH_AudioConfig` struct for device configuration (sample_rate, buffer_frames, output_channels, midi_input_port, midi_output_port)
- `MH_AudioInputCallback` typedef for effect plugin input audio
- `mh_audio_open()` - Open audio device for real-time playback through a plugin
- `mh_audio_close()` - Close audio device
- `mh_audio_start()` / `mh_audio_stop()` - Start/stop audio playback
- `mh_audio_is_playing()` - Check if audio is currently playing
- `mh_audio_set_input_callback()` - Set input callback for effect plugins
- `mh_audio_get_sample_rate()` - Get actual device sample rate
- `mh_audio_get_buffer_frames()` - Get actual buffer size
- `mh_audio_get_channels()` - Get number of output channels
- New `libminihost_audio` library using miniaudio for cross-platform audio I/O

#### Real-time MIDI I/O (libremidi integration)

- `MH_MidiPortInfo` struct for MIDI port information
- `MH_MidiPortCallback` typedef for port enumeration callbacks
- `mh_midi_enumerate_inputs()` / `mh_midi_enumerate_outputs()` - Enumerate available MIDI ports
- `mh_midi_get_num_inputs()` / `mh_midi_get_num_outputs()` - Get MIDI port count
- `mh_midi_get_input_name()` / `mh_midi_get_output_name()` - Get MIDI port name by index
- `mh_audio_connect_midi_input()` / `mh_audio_connect_midi_output()` - Connect MIDI ports to AudioDevice
- `mh_audio_disconnect_midi_input()` / `mh_audio_disconnect_midi_output()` - Disconnect MIDI
- `mh_audio_get_midi_input_port()` / `mh_audio_get_midi_output_port()` - Query connected MIDI ports
- Lock-free ring buffer for thread-safe MIDI transfer between MIDI and audio threads

#### Virtual MIDI Ports

- `mh_midi_in_open_virtual()` - Create a virtual MIDI input port (other apps can send MIDI to it)
- `mh_midi_out_open_virtual()` - Create a virtual MIDI output port (other apps can receive MIDI from it)
- `mh_audio_create_virtual_midi_input()` - Create virtual MIDI input for AudioDevice
- `mh_audio_create_virtual_midi_output()` - Create virtual MIDI output for AudioDevice
- `mh_audio_is_midi_input_virtual()` / `mh_audio_is_midi_output_virtual()` - Check if MIDI port is virtual
- `mh_audio_send_midi()` - Send MIDI events programmatically during real-time playback
- Virtual ports appear in system MIDI port lists, allowing DAWs and other apps to connect
- Platform support: macOS (CoreMIDI), Linux (ALSA); not supported on Windows

#### MIDI File Read/Write (midifile integration)

- Integrated `midifile` library for Standard MIDI File (SMF) read/write capability
- Python `MidiFile` class for creating, loading, and saving MIDI files
  - Create MIDI files programmatically with note on/off, tempo, control change, program change, pitch bend events
  - Load existing MIDI files and iterate through events
  - Save MIDI files to disk
  - Access event timing in both ticks and seconds

#### MIDI File Rendering

- `render_midi()` - Render MIDI file through plugin to numpy array
- `render_midi_stream()` - Generator yielding audio blocks for streaming/large files
- `render_midi_to_file()` - Render MIDI file directly to WAV file (16/24/32-bit)
- `MidiRenderer` class - Stateful renderer with progress tracking and fine-grained control
  - Properties: `duration_seconds`, `progress`, `is_finished`, `current_time`
  - Methods: `render_block()`, `render_all()`, `reset()`
- Automatic tempo map handling for correct timing
- Configurable tail length for reverb/delay tails

#### Core Utilities

- `mh_reset()` - Reset plugin internal state (clears delay lines, reverb tails, filter states)
- `mh_set_non_realtime()` - Enable higher-quality algorithms for offline/batch processing
- `mh_probe()` - Get plugin metadata without full instantiation
- `MH_PluginDesc` struct for plugin metadata (name, vendor, version, format, unique_id, MIDI flags, channel counts)
- `mh_set_sample_rate()` - Change sample rate without reloading plugin (preserves parameter state)
- `mh_get_sample_rate()` - Query current sample rate
- `MH_ScanCallback` typedef for plugin scanning callback
- `mh_scan_directory()` - Recursively scan directory for VST3/AudioUnit/LV2 plugins
- `MH_PluginDesc.path` field added for scan results
- `mh_process_double()` - Process audio with 64-bit double precision
- `mh_supports_double()` - Check if plugin supports native double precision
- `MH_LoadCallback` typedef for async loading callback
- `mh_open_async()` - Load plugin in background thread

#### Parameter & Preset Access

- `mh_param_to_text()` - Convert normalized parameter value to display string (e.g., "2500 Hz")
- `mh_param_from_text()` - Convert display string to normalized value
- `mh_get_num_programs()` - Get number of factory presets
- `mh_get_program_name()` - Get factory preset name by index
- `mh_get_program()` / `mh_set_program()` - Get/set current factory preset

#### Bus Layout & Sidechain

- `MH_BusInfo` struct for bus information (name, channels, is_main, is_enabled)
- `mh_get_num_buses()` - Query number of input/output buses
- `mh_get_bus_info()` - Get detailed bus information
- `mh_open_ex()` - Open plugin with sidechain channel configuration
- `mh_process_sidechain()` - Process audio with sidechain input
- `mh_get_sidechain_channels()` - Query configured sidechain channel count

### Fixed

#### Linux Compilation

- Added Linux build dependencies to README.md (JUCE requires freetype, fontconfig, webkit2gtk, gtk3, etc.)
- Fixed `addFormat()` calls to use raw pointers instead of `std::make_unique<>()` (JUCE's API expects raw pointers)
- Added `POSITION_INDEPENDENT_CODE ON` to libminihost CMakeLists.txt for linking into shared libraries (e.g., Python module)

### Command Line Interface

- `minihost` CLI tool with subcommands for common operations:
  - `probe` - Get plugin metadata without full instantiation
  - `scan` - Recursively scan directory for VST3/AudioUnit/LV2 plugins
  - `info` - Show detailed plugin info (buses, presets, latency)
  - `params` - List plugin parameters with current values
  - `midi` - List available MIDI input/output ports
  - `play` - Real-time audio playback with MIDI input
  - `process` - Offline audio processing through effects, or MIDI-to-audio rendering for synths
- Global options: `--sample-rate`, `--block-size`
- JSON output support (`--json`) for probe, scan, params, midi
- Plugin state and preset loading for process command
- Virtual MIDI port creation for play command

### Python Bindings

All C API additions are exposed in the Python `minihost` module:

- `minihost.AudioDevice` class for real-time audio playback with MIDI
  - Constructor: `AudioDevice(plugin, sample_rate=0, buffer_frames=0, output_channels=0, midi_input_port=-1, midi_output_port=-1)`
  - Methods: `start()`, `stop()`, `connect_midi_input()`, `connect_midi_output()`, `disconnect_midi_input()`, `disconnect_midi_output()`, `create_virtual_midi_input()`, `create_virtual_midi_output()`, `send_midi()`
  - Properties: `is_playing`, `sample_rate`, `buffer_frames`, `channels`, `midi_input_port`, `midi_output_port`, `is_midi_input_virtual`, `is_midi_output_virtual`
  - Context manager support (`with AudioDevice(plugin) as audio:`)
- `minihost.midi_get_input_ports()` - Get list of available MIDI input ports
- `minihost.midi_get_output_ports()` - Get list of available MIDI output ports
- `minihost.MidiFile` class for MIDI file read/write
  - Methods: `load()`, `save()`, `add_track()`, `add_tempo()`, `add_note_on()`, `add_note_off()`, `add_control_change()`, `add_program_change()`, `add_pitch_bend()`, `get_events()`, `join_tracks()`, `split_tracks()`
  - Properties: `num_tracks`, `ticks_per_quarter`, `duration_seconds`
- `minihost.probe(path)` - Module-level function for plugin metadata
- `minihost.scan_directory(path)` - Scan directory for VST3/AudioUnit/LV2 plugins, returns list of metadata dicts
- `Plugin` constructor now accepts `sidechain_channels` parameter
- New properties: `non_realtime`, `num_programs`, `program`, `sidechain_channels`, `num_input_buses`, `num_output_buses`, `sample_rate` (read/write), `supports_double`
- New methods: `reset()`, `param_to_text()`, `param_from_text()`, `get_program_name()`, `get_bus_info()`, `process_sidechain()`, `process_double()`
- Note: For async loading in Python, use Python's `threading` module with the regular `Plugin()` constructor
