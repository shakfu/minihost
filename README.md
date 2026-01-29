# minihost

A minimal audio plugin host library for loading and processing VST3 and AudioUnit plugins. Provides a simple C API suitable for embedding in audio applications, and also Python bindings.

## Features

- Load VST3 plugins (macOS, Windows, Linux)
- Load AudioUnit plugins (macOS only)
- **Real-time audio playback** via miniaudio (cross-platform)
- **Real-time MIDI I/O** via libremidi (cross-platform)
- **Virtual MIDI ports** - create named ports that DAWs can connect to (macOS, Linux)
- Process audio with sample-accurate parameter automation
- MIDI input/output support
- Transport info for tempo-synced plugins
- State save/restore for presets
- Thread-safe parameter access
- Latency and tail time reporting

## Requirements

- CMake 3.20+
- C++17 compiler
- JUCE framework (automatically downloaded if not present)

### Platform-specific

- **macOS**: Xcode command line tools
- **Windows**: Visual Studio 2019+ or MinGW
- **Linux**: Install the following development libraries:
  ```bash
  sudo apt install libasound2-dev libfreetype-dev libfontconfig1-dev \
      libwebkit2gtk-4.1-dev libgtk-3-dev libgl-dev libcurl4-openssl-dev
  ```

## Building

```bash
# Clone the repository
git clone https://github.com/user/minihost.git
cd minihost

# Build (JUCE will be downloaded automatically)
make

# Or with a custom JUCE path
cmake -B build -DJUCE_PATH=/path/to/JUCE
cmake --build build
```

### JUCE Setup

JUCE is downloaded automatically by `make` or `./scripts/download_juce.sh`. To use a different version or existing installation:

```bash
# Download specific version
JUCE_VERSION=8.0.6 ./scripts/download_juce.sh

# Or point to existing JUCE
cmake -B build -DJUCE_PATH=/path/to/your/JUCE
```

## C API Usage

```c
#include "minihost.h"

// Load a plugin
char err[256];
MH_Plugin* plugin = mh_open("/path/to/plugin.vst3",
                            48000.0,  // sample rate
                            512,      // max block size
                            2, 2,     // in/out channels
                            err, sizeof(err));

// Process audio
float* inputs[2] = { in_left, in_right };
float* outputs[2] = { out_left, out_right };
mh_process(plugin, inputs, outputs, 512);

// Process with MIDI
MH_MidiEvent midi[] = {
    { 0, 0x90, 60, 100 },   // Note on at sample 0
    { 256, 0x80, 60, 0 }    // Note off at sample 256
};
mh_process_midi(plugin, inputs, outputs, 512, midi, 2);

// Parameter control
int num_params = mh_get_num_params(plugin);
float value = mh_get_param(plugin, 0);
mh_set_param(plugin, 0, 0.5f);

// State save/restore
int size = mh_get_state_size(plugin);
void* state = malloc(size);
mh_get_state(plugin, state, size);
mh_set_state(plugin, state, size);

// Cleanup
mh_close(plugin);
```

### Real-time Audio Playback

```c
#include "minihost_audio.h"

// Open audio device for real-time playback
MH_AudioConfig config = { .sample_rate = 48000, .buffer_frames = 512 };
MH_AudioDevice* audio = mh_audio_open(plugin, &config, err, sizeof(err));

// Start playback
mh_audio_start(audio);

// Plugin is now producing audio through speakers
// Send MIDI, adjust parameters, etc.

// Stop and cleanup
mh_audio_stop(audio);
mh_audio_close(audio);
mh_close(plugin);
```

### Real-time MIDI I/O

```c
#include "minihost_midi.h"

// Enumerate available MIDI ports
int num_inputs = mh_midi_get_num_inputs();
int num_outputs = mh_midi_get_num_outputs();

for (int i = 0; i < num_inputs; i++) {
    char name[256];
    mh_midi_get_input_name(i, name, sizeof(name));
    printf("MIDI Input %d: %s\n", i, name);
}

// Connect MIDI to audio device
MH_AudioConfig config = {
    .sample_rate = 48000,
    .midi_input_port = 0,   // Connect to first MIDI input
    .midi_output_port = -1  // No MIDI output
};
MH_AudioDevice* audio = mh_audio_open(plugin, &config, err, sizeof(err));

// Or connect/disconnect dynamically
mh_audio_connect_midi_input(audio, 1);
mh_audio_disconnect_midi_input(audio);

// Create virtual MIDI ports (appear in system MIDI, DAWs can connect)
mh_audio_create_virtual_midi_input(audio, "minihost Input");
mh_audio_create_virtual_midi_output(audio, "minihost Output");
```

## Python Bindings

```bash
uv sync
```

```python
import numpy as np
import minihost

plugin = minihost.Plugin("/path/to/plugin.vst3", sample_rate=48000)

input_audio = np.zeros((2, 512), dtype=np.float32)
output_audio = np.zeros((2, 512), dtype=np.float32)
plugin.process(input_audio, output_audio)
```

### Real-time Audio Playback

```python
import minihost
import time

plugin = minihost.Plugin("/path/to/synth.vst3", sample_rate=48000)

# Use as context manager for automatic start/stop
with minihost.AudioDevice(plugin) as audio:
    # Plugin is now producing audio through speakers
    # Send MIDI programmatically
    audio.send_midi(0x90, 60, 100)  # Note on: C4, velocity 100
    time.sleep(1)
    audio.send_midi(0x80, 60, 0)    # Note off
    time.sleep(0.5)

# Or manual control
audio = minihost.AudioDevice(plugin)
audio.start()
audio.send_midi(0x90, 64, 80)  # E4 note on
time.sleep(0.5)
audio.send_midi(0x80, 64, 0)   # E4 note off
audio.stop()
```

### Real-time MIDI I/O

```python
import minihost

# Enumerate available MIDI ports
inputs = minihost.midi_get_input_ports()
outputs = minihost.midi_get_output_ports()
print(f"MIDI Inputs: {inputs}")
print(f"MIDI Outputs: {outputs}")

# Connect MIDI when creating AudioDevice
with minihost.AudioDevice(plugin, midi_input_port=0) as audio:
    # MIDI from port 0 is now routed to the plugin
    pass

# Or connect dynamically
audio = minihost.AudioDevice(plugin)
audio.connect_midi_input(0)
audio.start()
# ...
audio.disconnect_midi_input()
audio.stop()

# Create virtual MIDI ports (appear in system MIDI, DAWs can connect)
audio = minihost.AudioDevice(plugin)
audio.create_virtual_midi_input("minihost Input")
audio.create_virtual_midi_output("minihost Output")
audio.start()
# Other apps can now send MIDI to "minihost Input"
# and receive MIDI from "minihost Output"
```

### MIDI File Read/Write

```python
import minihost

# Create a new MIDI file
mf = minihost.MidiFile()
mf.ticks_per_quarter = 480

# Add events
mf.add_tempo(0, 0, 120.0)  # 120 BPM at tick 0
mf.add_note_on(0, 0, 0, 60, 100)    # C4 note on at tick 0
mf.add_note_off(0, 480, 0, 60, 0)   # C4 note off at tick 480

# Save to file
mf.save("output.mid")

# Load existing MIDI file
mf2 = minihost.MidiFile()
mf2.load("input.mid")

# Read events
events = mf2.get_events(0)  # Get events from track 0
for event in events:
    if event['type'] == 'note_on':
        print(f"Note {event['pitch']} vel {event['velocity']} at {event['seconds']:.2f}s")
```

### MIDI File Rendering

Render MIDI files through plugins to produce audio output:

```python
import minihost

plugin = minihost.Plugin("/path/to/synth.vst3", sample_rate=48000)

# Render to numpy array
audio = minihost.render_midi(plugin, "song.mid")
print(f"Rendered {audio.shape[1] / 48000:.2f} seconds of audio")

# Render directly to WAV file
samples = minihost.render_midi_to_file(plugin, "song.mid", "output.wav", bit_depth=24)

# Stream blocks for large files or real-time processing
for block in minihost.render_midi_stream(plugin, "song.mid", block_size=512):
    # Process each block (shape: channels, block_size)
    pass

# Fine-grained control with MidiRenderer class
renderer = minihost.MidiRenderer(plugin, "song.mid")
print(f"Duration: {renderer.duration_seconds:.2f}s")

while not renderer.is_finished:
    block = renderer.render_block()
    print(f"Progress: {renderer.progress:.1%}")
```

## Command Line Interface

The `minihost` command provides a CLI for common plugin operations:

```bash
# Install (from source)
uv sync

# Available commands
minihost --help
```

### Commands

#### `minihost probe` - Get plugin metadata
```bash
minihost probe /path/to/plugin.vst3
minihost probe /path/to/plugin.vst3 --json
```

#### `minihost scan` - Scan directory for plugins
```bash
minihost scan /Library/Audio/Plug-Ins/VST3/
minihost scan ~/Music/Plugins --json
```

#### `minihost info` - Show detailed plugin info
```bash
minihost info /path/to/plugin.vst3
```
Shows runtime information including sample rate, channels, latency, buses, and factory presets.

#### `minihost params` - List plugin parameters
```bash
minihost params /path/to/plugin.vst3
minihost params /path/to/plugin.vst3 --json
```

#### `minihost midi-ports` - List available MIDI ports
```bash
minihost midi-ports
minihost midi-ports --json
```

#### `minihost play` - Play plugin with real-time audio/MIDI
```bash
# Connect to MIDI input port 0
minihost play /path/to/synth.vst3 --midi 0

# Create a virtual MIDI port (macOS/Linux)
minihost play /path/to/synth.vst3 --virtual-midi "My Synth"
```

#### `minihost render` - Render MIDI file through plugin
```bash
# Basic render to WAV
minihost render /path/to/synth.vst3 song.mid output.wav

# With preset and tail length
minihost render /path/to/synth.vst3 song.mid output.wav --preset 5 --tail 3.0

# With custom bit depth
minihost render /path/to/synth.vst3 song.mid output.wav --bit-depth 16

# Load plugin state
minihost render /path/to/synth.vst3 song.mid output.wav --state preset.fxp
```

#### `minihost process` - Process audio file offline
```bash
# Process raw float32 audio through effect
minihost process /path/to/effect.vst3 input.raw output.raw

# Use double precision
minihost process /path/to/effect.vst3 input.raw output.raw --double
```

### Global Options

| Option | Description |
|--------|-------------|
| `-r, --sample-rate` | Sample rate in Hz (default: 48000) |
| `-b, --block-size` | Block size in samples (default: 512) |

## Thread Safety

- `mh_process`, `mh_process_midi`, `mh_process_midi_io`, `mh_process_auto`: Call from audio thread only (no locking)
- All other functions are thread-safe with internal locking
- Do not call `mh_close` while another thread is using the plugin

## API Reference

### Plugin Functions

| Function | Description |
|----------|-------------|
| `mh_open` | Load a plugin |
| `mh_close` | Unload a plugin |
| `mh_get_info` | Get plugin info (channels, params, latency) |
| `mh_process` | Process audio |
| `mh_process_midi` | Process audio with MIDI input |
| `mh_process_midi_io` | Process audio with MIDI input/output |
| `mh_process_auto` | Process with sample-accurate automation |
| `mh_get_num_params` | Get parameter count |
| `mh_get_param` | Get parameter value (0-1) |
| `mh_set_param` | Set parameter value (0-1) |
| `mh_get_param_info` | Get parameter metadata |
| `mh_get_state_size` | Get state size for save |
| `mh_get_state` | Save plugin state |
| `mh_set_state` | Restore plugin state |
| `mh_set_transport` | Set transport info (tempo, position) |
| `mh_get_tail_seconds` | Get reverb/delay tail length |
| `mh_get_bypass` | Get bypass state |
| `mh_set_bypass` | Set bypass state |
| `mh_get_latency_samples` | Get plugin latency |

### Audio Device Functions (minihost_audio.h)

| Function | Description |
|----------|-------------|
| `mh_audio_open` | Open audio device for real-time playback |
| `mh_audio_close` | Close audio device |
| `mh_audio_start` | Start audio playback |
| `mh_audio_stop` | Stop audio playback |
| `mh_audio_is_playing` | Check if audio is playing |
| `mh_audio_set_input_callback` | Set input callback for effects |
| `mh_audio_get_sample_rate` | Get actual sample rate |
| `mh_audio_get_buffer_frames` | Get actual buffer size |
| `mh_audio_get_channels` | Get number of output channels |
| `mh_audio_connect_midi_input` | Connect MIDI input port to device |
| `mh_audio_connect_midi_output` | Connect MIDI output port to device |
| `mh_audio_disconnect_midi_input` | Disconnect MIDI input |
| `mh_audio_disconnect_midi_output` | Disconnect MIDI output |
| `mh_audio_get_midi_input_port` | Get connected MIDI input port index |
| `mh_audio_get_midi_output_port` | Get connected MIDI output port index |
| `mh_audio_create_virtual_midi_input` | Create virtual MIDI input port |
| `mh_audio_create_virtual_midi_output` | Create virtual MIDI output port |
| `mh_audio_is_midi_input_virtual` | Check if MIDI input is virtual |
| `mh_audio_is_midi_output_virtual` | Check if MIDI output is virtual |
| `mh_audio_send_midi` | Send MIDI event programmatically |

### MIDI Functions (minihost_midi.h)

| Function | Description |
|----------|-------------|
| `mh_midi_enumerate_inputs` | Enumerate MIDI input ports via callback |
| `mh_midi_enumerate_outputs` | Enumerate MIDI output ports via callback |
| `mh_midi_get_num_inputs` | Get number of MIDI input ports |
| `mh_midi_get_num_outputs` | Get number of MIDI output ports |
| `mh_midi_get_input_name` | Get MIDI input port name by index |
| `mh_midi_get_output_name` | Get MIDI output port name by index |
| `mh_midi_in_open_virtual` | Create virtual MIDI input port |
| `mh_midi_out_open_virtual` | Create virtual MIDI output port |

### MidiFile Class (Python)

| Method/Property | Description |
|-----------------|-------------|
| `MidiFile()` | Create new MIDI file |
| `load(path)` | Load MIDI file from path |
| `save(path)` | Save MIDI file to path |
| `num_tracks` | Number of tracks (read-only) |
| `ticks_per_quarter` | Ticks per quarter note (resolution) |
| `duration_seconds` | Total duration in seconds |
| `add_track()` | Add a new track |
| `add_tempo(track, tick, bpm)` | Add tempo event |
| `add_note_on(track, tick, channel, pitch, velocity)` | Add note on event |
| `add_note_off(track, tick, channel, pitch, velocity)` | Add note off event |
| `add_control_change(track, tick, channel, controller, value)` | Add CC event |
| `add_program_change(track, tick, channel, program)` | Add program change |
| `add_pitch_bend(track, tick, channel, value)` | Add pitch bend event |
| `get_events(track)` | Get all events from track as list of dicts |
| `join_tracks()` | Merge all tracks into track 0 |
| `split_tracks()` | Split by channel into separate tracks |

### MIDI Rendering (Python)

| Function/Class | Description |
|----------------|-------------|
| `render_midi(plugin, midi_file, ...)` | Render MIDI to numpy array |
| `render_midi_stream(plugin, midi_file, ...)` | Generator yielding audio blocks |
| `render_midi_to_file(plugin, midi_file, output_path, ...)` | Render MIDI to WAV file |
| `MidiRenderer(plugin, midi_file, ...)` | Stateful renderer for fine-grained control |

**MidiRenderer properties:**

| Property | Description |
|----------|-------------|
| `duration_seconds` | Total duration including tail |
| `midi_duration_seconds` | MIDI content duration (no tail) |
| `total_samples` | Total samples to render |
| `current_sample` | Current position |
| `current_time` | Current time in seconds |
| `progress` | Progress as fraction (0.0-1.0) |
| `is_finished` | True when rendering complete |

**MidiRenderer methods:**

| Method | Description |
|--------|-------------|
| `render_block()` | Render next block, returns numpy array |
| `render_all()` | Render all remaining audio |
| `reset()` | Reset to beginning |

## License

GPL3
