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
    plugin.process_midi(None, None, [(0, 0x90, 60, 100)])  # Note on
    time.sleep(1)
    plugin.process_midi(None, None, [(0, 0x80, 60, 0)])    # Note off
    time.sleep(0.5)

# Or manual control
audio = minihost.AudioDevice(plugin)
audio.start()
# ... do stuff ...
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

## License

GPL3
