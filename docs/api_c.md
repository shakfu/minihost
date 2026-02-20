# C API Reference

## Plugin Functions (minihost.h)

| Function | Description |
|----------|-------------|
| `mh_open` | Load a plugin |
| `mh_open_ex` | Load a plugin with sidechain channel configuration |
| `mh_open_async` | Load a plugin in a background thread |
| `mh_close` | Unload a plugin |
| `mh_get_info` | Get plugin info (channels, params, latency, MIDI capabilities) |
| `mh_probe` | Get plugin metadata without full instantiation |
| `mh_scan_directory` | Recursively scan directory for plugins |

### Audio Processing

| Function | Description |
|----------|-------------|
| `mh_process` | Process audio (non-interleaved float32 buffers) |
| `mh_process_midi` | Process audio with MIDI input |
| `mh_process_midi_io` | Process audio with MIDI input and output |
| `mh_process_auto` | Process with sample-accurate parameter automation and MIDI |
| `mh_process_sidechain` | Process audio with sidechain input |
| `mh_process_double` | Process audio with 64-bit double precision |
| `mh_supports_double` | Check if plugin supports native double precision |

### Parameters

| Function | Description |
|----------|-------------|
| `mh_get_num_params` | Get parameter count |
| `mh_get_param` | Get parameter value (normalized 0.0-1.0) |
| `mh_set_param` | Set parameter value (normalized 0.0-1.0) |
| `mh_get_param_info` | Get parameter metadata (name, label, default, steps, ID, category) |
| `mh_param_to_text` | Convert normalized value to display string (e.g., "2500 Hz") |
| `mh_param_from_text` | Convert display string to normalized value |
| `mh_begin_param_gesture` | Signal start of parameter change gesture |
| `mh_end_param_gesture` | Signal end of parameter change gesture |

### State Management

| Function | Description |
|----------|-------------|
| `mh_get_state_size` | Get full state size in bytes |
| `mh_get_state` | Save full plugin state |
| `mh_set_state` | Restore full plugin state |
| `mh_get_program_state_size` | Get current program state size |
| `mh_get_program_state` | Save current program state |
| `mh_set_program_state` | Restore current program state |

### Factory Presets

| Function | Description |
|----------|-------------|
| `mh_get_num_programs` | Get number of factory presets |
| `mh_get_program_name` | Get preset name by index |
| `mh_get_program` | Get current preset index |
| `mh_set_program` | Load preset by index |

### Transport and Playback

| Function | Description |
|----------|-------------|
| `mh_set_transport` | Set transport info (BPM, time signature, position, play state) |
| `mh_get_bypass` | Get bypass state |
| `mh_set_bypass` | Set bypass state |
| `mh_reset` | Reset internal state (clears delay lines, filter states) |
| `mh_set_non_realtime` | Enable higher-quality algorithms for offline processing |

### Configuration

| Function | Description |
|----------|-------------|
| `mh_get_sample_rate` | Get current sample rate |
| `mh_set_sample_rate` | Change sample rate (preserves parameter state) |
| `mh_get_latency_samples` | Get plugin latency in samples |
| `mh_get_tail_seconds` | Get reverb/delay tail length in seconds |
| `mh_get_processing_precision` | Get processing precision (single/double) |
| `mh_set_processing_precision` | Set processing precision (single/double) |
| `mh_set_track_properties` | Set track name and/or color metadata |

### Bus Layout

| Function | Description |
|----------|-------------|
| `mh_get_num_buses` | Get number of input or output buses |
| `mh_get_bus_info` | Get bus info (name, channels, is_main, is_enabled) |
| `mh_check_buses_layout` | Check if a bus layout is supported |
| `mh_get_sidechain_channels` | Get configured sidechain channel count |

### Change Notifications

| Function | Description |
|----------|-------------|
| `mh_set_change_callback` | Register callback for processor-level changes (latency, param info, program, non-param state) |
| `mh_set_param_value_callback` | Register callback for plugin-initiated parameter value changes |
| `mh_set_param_gesture_callback` | Register callback for parameter gesture begin/end |

Constants: `MH_CHANGE_LATENCY`, `MH_CHANGE_PARAM_INFO`, `MH_CHANGE_PROGRAM`, `MH_CHANGE_NON_PARAM_STATE`

---

## Audio Device Functions (minihost_audio.h)

| Function | Description |
|----------|-------------|
| `mh_audio_open` | Open audio device for real-time plugin playback |
| `mh_audio_open_chain` | Open audio device for real-time chain playback |
| `mh_audio_close` | Close audio device |
| `mh_audio_start` | Start audio playback |
| `mh_audio_stop` | Stop audio playback |
| `mh_audio_is_playing` | Check if audio is currently playing |
| `mh_audio_set_input_callback` | Set input audio callback for effect plugins |
| `mh_audio_get_sample_rate` | Get actual device sample rate |
| `mh_audio_get_buffer_frames` | Get actual buffer size in frames |
| `mh_audio_get_channels` | Get number of output channels |

### MIDI Connections

| Function | Description |
|----------|-------------|
| `mh_audio_connect_midi_input` | Connect MIDI input port to device |
| `mh_audio_connect_midi_output` | Connect MIDI output port to device |
| `mh_audio_disconnect_midi_input` | Disconnect MIDI input |
| `mh_audio_disconnect_midi_output` | Disconnect MIDI output |
| `mh_audio_get_midi_input_port` | Get connected MIDI input port index (-1 if none) |
| `mh_audio_get_midi_output_port` | Get connected MIDI output port index (-1 if none) |
| `mh_audio_create_virtual_midi_input` | Create virtual MIDI input port |
| `mh_audio_create_virtual_midi_output` | Create virtual MIDI output port |
| `mh_audio_is_midi_input_virtual` | Check if MIDI input is a virtual port |
| `mh_audio_is_midi_output_virtual` | Check if MIDI output is a virtual port |
| `mh_audio_send_midi` | Send MIDI event programmatically to plugin |

---

## Audio File I/O Functions (minihost_audiofile.h)

| Function | Description |
|----------|-------------|
| `mh_audio_read` | Read audio file to interleaved float32 buffer |
| `mh_audio_data_free` | Free decoded audio data returned by `mh_audio_read` |
| `mh_audio_write` | Write interleaved float32 data to WAV file |
| `mh_audio_get_file_info` | Get audio file metadata without decoding |

### Supported Formats

| Format | Read | Write |
|--------|------|-------|
| WAV | Yes | Yes (16/24/32-bit) |
| FLAC | Yes | No |
| MP3 | Yes | No |
| Vorbis | Yes | No |

### Structs

**`MH_AudioData`** -- returned by `mh_audio_read()`:
- `float* data` -- interleaved float32 samples
- `unsigned int channels`
- `unsigned int frames`
- `unsigned int sample_rate`

**`MH_AudioFileInfo`** -- populated by `mh_audio_get_file_info()`:
- `unsigned int channels`
- `unsigned int sample_rate`
- `unsigned long long frames`
- `double duration`

---

## Plugin Chain Functions (minihost_chain.h)

| Function | Description |
|----------|-------------|
| `mh_chain_create` | Create chain from array of plugins (all must share sample rate) |
| `mh_chain_close` | Close chain (does not close individual plugins) |
| `mh_chain_process` | Process audio through chain |
| `mh_chain_process_midi_io` | Process with MIDI I/O (MIDI goes to first plugin) |
| `mh_chain_process_auto` | Process with sample-accurate parameter automation and MIDI |
| `mh_chain_get_latency_samples` | Get total chain latency (sum of all plugins) |
| `mh_chain_get_num_plugins` | Get number of plugins in chain |
| `mh_chain_get_plugin` | Get plugin by index |
| `mh_chain_get_num_input_channels` | Get input channel count (from first plugin) |
| `mh_chain_get_num_output_channels` | Get output channel count (from last plugin) |
| `mh_chain_get_sample_rate` | Get sample rate (shared by all plugins) |
| `mh_chain_get_max_block_size` | Get maximum block size |
| `mh_chain_reset` | Reset all plugins in chain |
| `mh_chain_set_non_realtime` | Set non-realtime mode for all plugins |
| `mh_chain_get_tail_seconds` | Get maximum tail length (max of all plugins) |

---

## MIDI Functions (minihost_midi.h)

### Port Enumeration

| Function | Description |
|----------|-------------|
| `mh_midi_get_num_inputs` | Get number of available MIDI input ports |
| `mh_midi_get_num_outputs` | Get number of available MIDI output ports |
| `mh_midi_get_input_name` | Get MIDI input port name by index |
| `mh_midi_get_output_name` | Get MIDI output port name by index |
| `mh_midi_enumerate_inputs` | Enumerate input ports via callback |
| `mh_midi_enumerate_outputs` | Enumerate output ports via callback |

### Standalone MIDI I/O

| Function | Description |
|----------|-------------|
| `mh_midi_in_open` | Open MIDI input port with message callback |
| `mh_midi_in_open_virtual` | Create virtual MIDI input port with callback |
| `mh_midi_in_close` | Close MIDI input |
| `mh_midi_out_open` | Open MIDI output port |
| `mh_midi_out_open_virtual` | Create virtual MIDI output port |
| `mh_midi_out_close` | Close MIDI output |
| `mh_midi_out_send` | Send raw MIDI message on output port |
