# Changelog

## [Unreleased]

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
- `mh_scan_directory()` - Recursively scan directory for VST3/AudioUnit plugins
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
  - `scan` - Recursively scan directory for VST3/AudioUnit plugins
  - `info` - Show detailed plugin info (buses, presets, latency)
  - `params` - List plugin parameters with current values
  - `midi-ports` - List available MIDI input/output ports
  - `play` - Real-time audio playback with MIDI input
  - `render` - Render MIDI file through plugin to WAV (16/24/32-bit)
  - `process` - Offline audio file processing through effects
- Global options: `--sample-rate`, `--block-size`
- JSON output support (`--json`) for probe, scan, params, midi-ports
- Plugin state and preset loading for render command
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
- `minihost.scan_directory(path)` - Scan directory for plugins, returns list of metadata dicts
- `Plugin` constructor now accepts `sidechain_channels` parameter
- New properties: `non_realtime`, `num_programs`, `program`, `sidechain_channels`, `num_input_buses`, `num_output_buses`, `sample_rate` (read/write), `supports_double`
- New methods: `reset()`, `param_to_text()`, `param_from_text()`, `get_program_name()`, `get_bus_info()`, `process_sidechain()`, `process_double()`
- Note: For async loading in Python, use Python's `threading` module with the regular `Plugin()` constructor
