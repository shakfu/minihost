# minihost

A minimal audio plugin host library for loading and processing VST3 and AudioUnit plugins. Provides a simple C API suitable for embedding in audio applications, and also Python bindings.

## Features

- Load VST3 plugins (macOS, Windows, Linux)
- Load AudioUnit plugins (macOS only)
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

## Thread Safety

- `mh_process`, `mh_process_midi`, `mh_process_midi_io`, `mh_process_auto`: Call from audio thread only (no locking)
- All other functions are thread-safe with internal locking
- Do not call `mh_close` while another thread is using the plugin

## API Reference

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

## License

GPL3
