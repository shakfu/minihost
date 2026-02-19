# minihost

*minihost* is a headless audio plugin host for VST3, AudioUnit, and LV2 plugins. Built on JUCE, it provides both a C API for embedding in audio applications and a Python API via nanobind.

## Features

- Load VST3 plugins (macOS, Windows, Linux)
- Load AudioUnit plugins (macOS only)
- Load LV2 plugins (macOS, Windows, Linux)
- **Headless mode** (default) - no GUI dependencies, uses JUCE's `juce_audio_processors_headless` module
- **Plugin chaining** - connect multiple plugins in series (synth -> reverb -> limiter)
- **Audio file I/O** via miniaudio -- read WAV/FLAC/MP3/Vorbis, write WAV (16/24/32-bit)
- **Real-time audio playback** via miniaudio (cross-platform)
- **Real-time MIDI I/O** via libremidi (cross-platform)
- **Virtual MIDI ports** - create named ports that DAWs can connect to (macOS, Linux)
- **Standalone MIDI input** - monitor raw MIDI messages without a plugin (`MidiIn` class)
- Process audio with sample-accurate parameter automation
- Single and double precision processing
- MIDI input/output support
- Transport info for tempo-synced plugins
- State save/restore for presets and per-program state
- Thread-safe parameter access
- Change notifications (latency, parameter info, program, non-parameter state)
- Parameter gestures for automation bracketing
- Bus layout validation and sidechain support
- Track name/color metadata forwarding to plugins
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

### macOS / Linux

```bash
# Clone the repository
git clone https://github.com/shakfu/minihost.git
cd minihost

# Build (JUCE will be downloaded automatically)
make

# Or with a custom JUCE path
cmake -B build -DJUCE_PATH=/path/to/JUCE
cmake --build build

# Disable headless mode (enables GUI support)
cmake -B build -DMINIHOST_HEADLESS=OFF
cmake --build build
```

### Windows

```powershell
# Clone the repository
git clone https://github.com/shakfu/minihost.git
cd minihost

# Download JUCE
python scripts/download_juce.py

# Configure and build
cmake -B build
cmake --build build --config Release
```

### JUCE Setup

JUCE is downloaded automatically by `make` (macOS/Linux). You can also download it manually:

```bash
# Cross-platform (recommended) - works on Windows, macOS, Linux
python scripts/download_juce.py

# Unix only (bash)
./scripts/download_juce.sh
```

To use a different version or existing installation:

```bash
# Download specific version (macOS/Linux)
JUCE_VERSION=8.0.6 python scripts/download_juce.py

# Download specific version (Windows PowerShell)
$env:JUCE_VERSION="8.0.6"; python scripts/download_juce.py

# Or point to existing JUCE
cmake -B build -DJUCE_PATH=/path/to/your/JUCE
```

## Command Line Interface

The `minihost` command provides a CLI for common plugin operations:

```bash
# Install (from source)
uv sync

# Available commands
minihost --help
usage: minihost [-h] [-r SAMPLE_RATE] [-b BLOCK_SIZE]
                {scan,info,params,midi,play,process} ...

Audio plugin hosting CLI

positional arguments:
  {scan,info,params,midi,play,process}
                        Commands
    scan                Scan directory for plugins
    info                Show plugin info
    params              List plugin parameters
    midi                List or monitor MIDI ports
    play                Play plugin with real-time audio/MIDI
    process             Process audio through plugin (offline)

options:
  -h, --help            show this help message and exit
  -r, --sample-rate SAMPLE_RATE
                        Sample rate in Hz (default: 48000)
  -b, --block-size BLOCK_SIZE
                        Block size in samples (default: 512)
```

### Commands

#### `minihost info` - Show plugin info
```bash
minihost info /path/to/plugin.vst3          # full info (loads plugin)
minihost info /path/to/plugin.vst3 --probe  # lightweight metadata only
minihost info /path/to/plugin.vst3 --json   # JSON output
```
By default shows full runtime details (sample rate, channels, latency, buses, presets). Use `--probe` for fast metadata-only mode without fully loading the plugin.

#### `minihost scan` - Scan directory for plugins
```bash
minihost scan /Library/Audio/Plug-Ins/VST3/
minihost scan ~/Music/Plugins --json
```

#### `minihost params` - List plugin parameters
```bash
minihost params /path/to/plugin.vst3
minihost params /path/to/plugin.vst3 --json
```

#### `minihost midi` - List or monitor MIDI ports
```bash
minihost midi                          # list all MIDI ports
minihost midi --json                   # list as JSON
minihost midi -m 0                     # monitor MIDI input port 0
minihost midi --virtual-midi "Monitor" # create virtual port and monitor
```

#### `minihost play` - Play plugin with real-time audio/MIDI
```bash
# Connect to MIDI input port 0
minihost play /path/to/synth.vst3 --midi 0

# Create a virtual MIDI port (macOS/Linux)
minihost play /path/to/synth.vst3 --virtual-midi "My Synth"
```

#### `minihost process` - Process audio/MIDI offline
```bash
# Process audio through effect
minihost process /path/to/effect.vst3 -i input.wav -o output.wav

# With parameter control
minihost process /path/to/effect.vst3 -i input.wav -o output.wav --param "Mix:0.5"

# Render MIDI through synth
minihost process /path/to/synth.vst3 -m song.mid -o output.wav --tail 3.0

# With preset and bit depth
minihost process /path/to/synth.vst3 -m song.mid -o output.wav --preset 5 --bit-depth 16

# Sidechain processing (second -i is sidechain)
minihost process /path/to/compressor.vst3 -i main.wav -i sidechain.wav -o output.wav
```

### Global Options

| Option | Description |
|--------|-------------|
| `-r, --sample-rate` | Sample rate in Hz (default: 48000) |
| `-b, --block-size` | Block size in samples (default: 512) |

## Python API

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

### Standalone MIDI Input

Monitor MIDI messages without loading a plugin:

```python
import minihost

def on_midi(data: bytes):
    status = data[0]
    if status & 0xF0 == 0x90 and data[2] > 0:
        print(f"Note On: {data[1]} vel={data[2]}")

# Open hardware MIDI port
with minihost.MidiIn.open(0, on_midi) as midi_in:
    input("Press Enter to stop...\n")

# Or create a virtual MIDI port
with minihost.MidiIn.open_virtual("My Monitor", on_midi) as midi_in:
    input("Press Enter to stop...\n")
```

### Audio File I/O

```python
import minihost

# Read audio files (WAV, FLAC, MP3, Vorbis)
data, sample_rate = minihost.read_audio("input.wav")
# data shape: (channels, samples), dtype: float32

# Write WAV files (16, 24, or 32-bit)
minihost.write_audio("output.wav", data, sample_rate, bit_depth=24)

# Get file info without decoding
info = minihost.get_audio_info("song.wav")
print(f"{info['channels']}ch, {info['sample_rate']}Hz, {info['duration']:.2f}s")
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

### Plugin Chaining

Chain multiple plugins together for serial processing:

```python
import minihost
import time

# Load plugins (all must have same sample rate)
synth = minihost.Plugin("/path/to/synth.vst3", sample_rate=48000)
reverb = minihost.Plugin("/path/to/reverb.vst3", sample_rate=48000)
limiter = minihost.Plugin("/path/to/limiter.vst3", sample_rate=48000)

# Create chain
chain = minihost.PluginChain([synth, reverb, limiter])
print(f"Total latency: {chain.latency_samples} samples")
print(f"Tail length: {chain.tail_seconds:.2f} seconds")

# Real-time playback through chain
with minihost.AudioDevice(chain) as audio:
    audio.send_midi(0x90, 60, 100)  # Note on to synth
    time.sleep(2)
    audio.send_midi(0x80, 60, 0)    # Note off
    time.sleep(1)  # Let reverb tail fade

# Offline processing
import numpy as np
input_audio = np.zeros((2, 512), dtype=np.float32)
output_audio = np.zeros((2, 512), dtype=np.float32)
chain.process(input_audio, output_audio)

# Process with MIDI (MIDI goes to first plugin)
midi_events = [(0, 0x90, 60, 100)]
chain.process_midi(input_audio, output_audio, midi_events)

# Render MIDI file through chain
audio = minihost.render_midi(chain, "song.mid")
minihost.render_midi_to_file(chain, "song.mid", "output.wav")

# Access individual plugins in chain
for i in range(chain.num_plugins):
    plugin = chain.get_plugin(i)
    print(f"Plugin {i}: {plugin.num_params} params")
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

### Plugin Chaining

Chain multiple plugins together for processing (e.g., synth -> reverb -> limiter):

```c
#include "minihost_chain.h"

// Load plugins
MH_Plugin* synth = mh_open("/path/to/synth.vst3", 48000, 512, 0, 2, err, sizeof(err));
MH_Plugin* reverb = mh_open("/path/to/reverb.vst3", 48000, 512, 2, 2, err, sizeof(err));
MH_Plugin* limiter = mh_open("/path/to/limiter.vst3", 48000, 512, 2, 2, err, sizeof(err));

// Create chain (all plugins must have same sample rate)
MH_Plugin* plugins[] = { synth, reverb, limiter };
MH_PluginChain* chain = mh_chain_create(plugins, 3, err, sizeof(err));

// Get combined latency
int latency = mh_chain_get_latency_samples(chain);

// Process audio through chain
float* inputs[2] = { in_left, in_right };
float* outputs[2] = { out_left, out_right };
mh_chain_process(chain, inputs, outputs, 512);

// Process with MIDI (MIDI goes to first plugin only)
MH_MidiEvent midi[] = { { 0, 0x90, 60, 100 } };
mh_chain_process_midi_io(chain, inputs, outputs, 512, midi, 1, NULL, 0, NULL);

// Real-time playback through chain
MH_AudioConfig config = { .sample_rate = 48000, .buffer_frames = 512 };
MH_AudioDevice* audio = mh_audio_open_chain(chain, &config, err, sizeof(err));
mh_audio_start(audio);
// ...
mh_audio_stop(audio);
mh_audio_close(audio);

// Cleanup
mh_chain_close(chain);  // Does not close individual plugins
mh_close(synth);
mh_close(reverb);
mh_close(limiter);
```

### Audio File I/O

Read and write audio files without external dependencies:

```c
#include "minihost_audiofile.h"

// Read any supported format (WAV, FLAC, MP3, Vorbis)
char err[1024];
MH_AudioData* audio = mh_audio_read("input.flac", err, sizeof(err));
if (audio) {
    printf("Channels: %u, Frames: %u, Rate: %u\n",
           audio->channels, audio->frames, audio->sample_rate);
    // audio->data is interleaved float32
    mh_audio_data_free(audio);
}

// Write WAV file (16, 24, or 32-bit)
mh_audio_write("output.wav", interleaved_data,
               2, num_frames, 48000, 24, err, sizeof(err));

// Get file info without decoding
MH_AudioFileInfo info;
mh_audio_get_file_info("song.wav", &info, err, sizeof(err));
printf("Duration: %.2f seconds\n", info.duration);
```

## Thread Safety

- `mh_process`, `mh_process_midi`, `mh_process_midi_io`, `mh_process_auto`: Call from audio thread only (no locking)
- All other functions are thread-safe with internal locking
- Do not call `mh_close` while another thread is using the plugin

## API Reference

Detailed API documentation:

- [C API Reference](docs/api_c.md) -- `minihost.h`, `minihost_audio.h`, `minihost_audiofile.h`, `minihost_chain.h`, `minihost_midi.h`
- [Python API Reference](docs/api_python.md) -- `Plugin`, `PluginChain`, `AudioDevice`, `MidiFile`, `MidiIn`, audio I/O, MIDI rendering, automation, VST3 presets
- [Hosting Guide](docs/hosting_guide.md) -- practical guide with extended examples

## License

GPL3
