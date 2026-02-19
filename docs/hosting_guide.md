# Plugin Hosting Guide

This guide covers how to host VST3, AudioUnit, and LV2 plugins using minihost as an embedded library.

## Table of Contents

1. [Overview](#overview)
2. [Building and Linking](#building-and-linking)
3. [Plugin Lifecycle](#plugin-lifecycle)
4. [Thread Safety](#thread-safety)
5. [Audio Processing](#audio-processing)
6. [MIDI](#midi)
7. [Parameters](#parameters)
8. [State Management](#state-management)
9. [Transport and Tempo](#transport-and-tempo)
10. [Latency Compensation](#latency-compensation)
11. [Offline Processing](#offline-processing)
12. [Plugin Discovery](#plugin-discovery)
13. [Real-time Audio I/O](#real-time-audio-io)
14. [Real-time MIDI I/O](#real-time-midi-io)
15. [Audio File I/O](#audio-file-io)
16. [Performance Guidelines](#performance-guidelines)
17. [Common Pitfalls](#common-pitfalls)

---

## Overview

minihost is a C library for hosting VST3, AudioUnit, and LV2 plugins. It builds in headless mode by default (no GUI dependencies), using JUCE's `juce_audio_processors_headless` module (available since JUCE 8.0.11). It's designed for:

- Headless audio servers
- Batch processing tools
- Embedded applications
- Any scenario where plugin UI is not needed

Headless mode can be disabled with `cmake -DMINIHOST_HEADLESS=OFF` if GUI support is needed.

The API is pure C for maximum compatibility, though the implementation uses C++ and JUCE internally.

---

## Building and Linking

### As a Static Library

```cmake
add_subdirectory(minihost)
target_link_libraries(your_app PRIVATE minihost)
```

### Required Frameworks (macOS)

```cmake
target_link_libraries(your_app PRIVATE
    "-framework AudioUnit"
    "-framework AudioToolbox"
    "-framework CoreAudioKit"
)
```

### Header

```c
#include "minihost.h"
```

---

## Plugin Lifecycle

### Opening a Plugin

```c
char err[1024];
MH_Plugin* plugin = mh_open(
    "/path/to/plugin.vst3",  // or .component for AU, .lv2 for LV2
    48000.0,                  // sample rate
    512,                      // max block size
    2,                        // input channels
    2,                        // output channels
    err, sizeof(err)
);

if (!plugin) {
    fprintf(stderr, "Failed: %s\n", err);
    return;
}
```

### Key Points

- **sample_rate**: Must match your audio system. Can be changed later with `mh_set_sample_rate()`.
- **max_block_size**: The largest buffer you'll ever pass to process. Set this once at open time. Passing larger buffers to process functions will fail.
- **channels**: Request your desired I/O configuration. The plugin may use fewer channels if it doesn't support your request.

### Querying Actual Configuration

```c
MH_Info info;
mh_get_info(plugin, &info);
// info.num_input_ch and info.num_output_ch reflect actual configuration
```

### Closing

```c
mh_close(plugin);  // Always close when done
plugin = NULL;     // Good practice
```

---

## Thread Safety

minihost has a specific threading model:

### Audio Thread Functions (NO locking)

These functions are designed for realtime audio callbacks and do NOT acquire locks:

- `mh_process()`
- `mh_process_midi()`
- `mh_process_midi_io()`
- `mh_process_auto()`
- `mh_process_sidechain()`
- `mh_process_double()`

**Call these from ONE thread only** (your audio callback thread).

### Thread-Safe Functions (with locking)

All other functions use internal mutex locking and can be called from any thread:

- `mh_get_param()` / `mh_set_param()`
- `mh_get_state()` / `mh_set_state()`
- `mh_set_transport()`
- `mh_reset()`
- etc.

### Critical Rule

**Never call `mh_close()` while another thread might be using the plugin.**

Ensure your audio thread has stopped processing before closing.

### Recommended Pattern

```c
// Main thread
atomic_bool running = true;

// Audio thread
while (atomic_load(&running)) {
    mh_process(plugin, in, out, frames);
}

// Main thread - shutdown
atomic_store(&running, false);
// Wait for audio thread to finish
join_audio_thread();
// Now safe to close
mh_close(plugin);
```

---

## Audio Processing

### Buffer Format

minihost uses **non-interleaved** audio buffers:

```c
// inputs[channel][sample]
// outputs[channel][sample]

float* inputs[2];   // 2 input channels
float* outputs[2];  // 2 output channels

inputs[0] = left_in;   // Left channel samples
inputs[1] = right_in;  // Right channel samples
outputs[0] = left_out;
outputs[1] = right_out;

mh_process(plugin, (const float* const*)inputs, outputs, num_frames);
```

### Block Size

- Never exceed the `max_block_size` specified at open time
- Smaller blocks are fine and common
- Variable block sizes are supported (within the max)

```c
// Opened with max_block_size=1024
mh_process(plugin, in, out, 512);   // OK
mh_process(plugin, in, out, 256);   // OK
mh_process(plugin, in, out, 1024);  // OK
mh_process(plugin, in, out, 2048);  // FAILS - exceeds max
```

### NULL Buffers

- Pass `NULL` for inputs to feed silence
- Pass `NULL` for outputs to discard output

```c
// Instrument with no audio input
mh_process(plugin, NULL, outputs, frames);
```

### Channel Count Mismatch

If your buffer has different channel counts than the plugin expects:

```c
MH_Info info;
mh_get_info(plugin, &info);

// Allocate buffers matching plugin's actual configuration
float** in = alloc_channels(info.num_input_ch, block_size);
float** out = alloc_channels(info.num_output_ch, block_size);
```

---

## MIDI

### Sending MIDI to Plugins

```c
MH_MidiEvent events[2];

// Note On - middle C, velocity 100, at sample 0
events[0].sample_offset = 0;
events[0].status = 0x90;  // Note on, channel 1
events[0].data1 = 60;     // Note number
events[0].data2 = 100;    // Velocity

// Note Off at sample 256
events[1].sample_offset = 256;
events[1].status = 0x80;  // Note off, channel 1
events[1].data1 = 60;
events[1].data2 = 0;

mh_process_midi(plugin, in, out, 512, events, 2);
```

### Receiving MIDI from Plugins

Some plugins generate MIDI (arpeggiators, MIDI effects):

```c
MH_MidiEvent midi_out[256];
int num_out = 0;

mh_process_midi_io(plugin, in, out, frames,
                   midi_in, num_midi_in,
                   midi_out, 256, &num_out);

for (int i = 0; i < num_out; i++) {
    // Handle midi_out[i]
}
```

### Sample Offset

The `sample_offset` field specifies when within the block the event occurs:

- `0` = first sample of the block
- `nframes-1` = last sample

This enables sample-accurate timing for tight rhythmic precision.

---

## Parameters

### Normalized Values

All parameter values are normalized to 0.0-1.0 range:

```c
float val = mh_get_param(plugin, 0);      // Returns 0.0-1.0
mh_set_param(plugin, 0, 0.5f);            // Set to midpoint
```

### Parameter Metadata

```c
MH_ParamInfo info;
if (mh_get_param_info(plugin, index, &info)) {
    printf("Name: %s\n", info.name);
    printf("Label: %s\n", info.label);           // e.g., "dB", "Hz"
    printf("Display: %s\n", info.current_value_str);  // e.g., "-6.0 dB"
    printf("Default: %.2f\n", info.default_value);
    printf("Steps: %d\n", info.num_steps);       // 0 = continuous
}
```

### Value/Text Conversion

```c
// Value to display string
char buf[64];
mh_param_to_text(plugin, 0, 0.5f, buf, sizeof(buf));
// buf might contain "2500 Hz" or "-6.0 dB"

// Display string to value (not all plugins support this)
float value;
if (mh_param_from_text(plugin, 0, "-12 dB", &value)) {
    mh_set_param(plugin, 0, value);
}
```

### Sample-Accurate Automation

For precise automation, use `mh_process_auto()`:

```c
MH_ParamChange changes[3];

changes[0] = (MH_ParamChange){.sample_offset = 0,   .param_index = 0, .value = 0.0f};
changes[1] = (MH_ParamChange){.sample_offset = 128, .param_index = 0, .value = 0.5f};
changes[2] = (MH_ParamChange){.sample_offset = 256, .param_index = 0, .value = 1.0f};

mh_process_auto(plugin, in, out, 512,
                NULL, 0, NULL, 0, NULL,  // No MIDI
                changes, 3);
```

**Important**: Parameter changes must be sorted by `sample_offset`.

---

## State Management

### Saving State

```c
int size = mh_get_state_size(plugin);
if (size > 0) {
    void* data = malloc(size);
    if (mh_get_state(plugin, data, size)) {
        // Write 'data' to file
        fwrite(data, 1, size, file);
    }
    free(data);
}
```

### Restoring State

```c
// Read data from file
void* data = read_file(path, &size);
if (mh_set_state(plugin, data, size)) {
    // State restored successfully
}
free(data);
```

### State Compatibility

Use `mh_probe()` to check plugin identity before restoring state:

```c
MH_PluginDesc desc;
mh_probe(plugin_path, &desc, err, sizeof(err));

// Compare desc.unique_id with the ID stored alongside the state
if (strcmp(desc.unique_id, saved_id) != 0) {
    // Warning: state may not be compatible
}
```

### Factory Presets

```c
int num_presets = mh_get_num_programs(plugin);
for (int i = 0; i < num_presets; i++) {
    char name[256];
    mh_get_program_name(plugin, i, name, sizeof(name));
    printf("[%d] %s\n", i, name);
}

// Load a preset
mh_set_program(plugin, 5);
```

---

## Transport and Tempo

For tempo-synced plugins (delays, LFOs, arpeggiators):

```c
MH_TransportInfo transport = {
    .bpm = 120.0,
    .time_sig_numerator = 4,
    .time_sig_denominator = 4,
    .position_samples = current_position,
    .position_beats = current_position / (sample_rate * 60.0 / bpm),
    .is_playing = 1,
    .is_recording = 0,
    .is_looping = 0,
    .loop_start_samples = 0,
    .loop_end_samples = 0
};

mh_set_transport(plugin, &transport);
```

**Call before each process block** if position changes (which it usually does).

To clear transport info:

```c
mh_set_transport(plugin, NULL);
```

---

## Latency Compensation

### Query Latency

```c
int latency = mh_get_latency_samples(plugin);
```

### When to Re-query

Latency may change after:
- Parameter changes
- Sample rate changes
- Preset changes

### Compensation Strategy

If processing through multiple plugins:

```c
// Find maximum latency
int max_latency = 0;
for (int i = 0; i < num_plugins; i++) {
    int lat = mh_get_latency_samples(plugins[i]);
    if (lat > max_latency) max_latency = lat;
}

// Delay other signals by (max_latency - their_latency)
```

---

## Offline Processing

For batch/offline processing where realtime constraints don't apply:

```c
// Enable non-realtime mode
mh_set_non_realtime(plugin, 1);

// Process entire file
while (frames_remaining > 0) {
    int to_process = min(frames_remaining, block_size);
    mh_process(plugin, in, out, to_process);
    // ...
}

// Disable when done (if reusing plugin for realtime)
mh_set_non_realtime(plugin, 0);
```

In non-realtime mode, plugins may use higher-quality algorithms that would be too expensive for realtime.

### Processing Tail

For reverbs and delays, continue processing after input ends:

```c
double tail = mh_get_tail_seconds(plugin);
int tail_samples = (int)(tail * sample_rate);

// After input ends, process tail_samples more with silence input
for (int processed = 0; processed < tail_samples; processed += block_size) {
    int n = min(block_size, tail_samples - processed);
    mh_process(plugin, NULL, out, n);  // NULL = silence
    // Write output
}
```

### Resetting Between Files

When processing unrelated audio segments:

```c
mh_reset(plugin);  // Clears delay lines, filter states, etc.
```

---

## Plugin Discovery

### Probing Without Loading

```c
MH_PluginDesc desc;
char err[1024];

if (mh_probe("/path/to/plugin.vst3", &desc, err, sizeof(err))) {
    printf("Name: %s\n", desc.name);
    printf("Vendor: %s\n", desc.vendor);
    printf("MIDI: %s\n", desc.accepts_midi ? "yes" : "no");
}
```

This is fast and doesn't fully instantiate the plugin.

### Scanning Directories

```c
void on_plugin_found(const MH_PluginDesc* desc, void* user_data) {
    printf("Found: %s at %s\n", desc->name, desc->path);
}

int count = mh_scan_directory("/Library/Audio/Plug-Ins/VST3",
                               on_plugin_found, NULL);
printf("Found %d plugins\n", count);
```

### Standard Plugin Locations

**macOS:**
- `/Library/Audio/Plug-Ins/VST3/` (system)
- `~/Library/Audio/Plug-Ins/VST3/` (user)
- `/Library/Audio/Plug-Ins/Components/` (AU system)
- `~/Library/Audio/Plug-Ins/Components/` (AU user)

**Windows:**
- `C:\Program Files\Common Files\VST3\`
- `C:\Program Files (x86)\Common Files\VST3\`

**Linux:**
- `/usr/lib/vst3/`
- `~/.vst3/`
- `/usr/lib/lv2/` (LV2 system)
- `~/.lv2/` (LV2 user)

---

## Real-time Audio I/O

minihost includes `libminihost_audio`, a companion library using [miniaudio](https://miniaud.io/) for cross-platform real-time audio playback. This handles audio device management and buffer conversion automatically.

### Headers

```c
#include "minihost.h"
#include "minihost_audio.h"
```

### Linking

```cmake
target_link_libraries(your_app PRIVATE minihost minihost_audio)
```

### Basic Usage

```c
// 1. Open plugin as usual
char err[1024];
MH_Plugin* plugin = mh_open("/path/to/synth.vst3", 48000.0, 512, 0, 2, err, sizeof(err));

// 2. Open audio device with default settings
MH_AudioDevice* audio = mh_audio_open(plugin, NULL, err, sizeof(err));
if (!audio) {
    fprintf(stderr, "Failed to open audio: %s\n", err);
    mh_close(plugin);
    return;
}

// 3. Start playback
mh_audio_start(audio);

// Plugin is now producing audio through speakers!
// The audio callback runs automatically in a separate thread.

// 4. Interact with the plugin
mh_set_param(plugin, 0, 0.5f);  // Change parameters
// Send MIDI via mh_process_midi() if needed (see Real-time MIDI I/O section)

// 5. Stop and cleanup
mh_audio_stop(audio);
mh_audio_close(audio);
mh_close(plugin);
```

### Configuration

Use `MH_AudioConfig` to customize the audio device:

```c
MH_AudioConfig config = {
    .sample_rate = 48000,      // 0 = use device default
    .buffer_frames = 256,      // 0 = auto (~256-512)
    .output_channels = 2,      // 0 = use plugin's output channels
    .midi_input_port = -1,     // -1 = no MIDI, >= 0 = port index
    .midi_output_port = -1     // -1 = no MIDI, >= 0 = port index
};

MH_AudioDevice* audio = mh_audio_open(plugin, &config, err, sizeof(err));
```

### Querying Actual Configuration

The device may negotiate different settings than requested:

```c
double actual_rate = mh_audio_get_sample_rate(audio);
int actual_buffer = mh_audio_get_buffer_frames(audio);
int actual_channels = mh_audio_get_channels(audio);

printf("Audio: %.0f Hz, %d frames, %d channels\n",
       actual_rate, actual_buffer, actual_channels);
```

The plugin's sample rate is automatically updated to match the device if they differ.

### Effect Plugins (With Audio Input)

For effect plugins that process audio input, provide an input callback:

```c
void my_input_callback(float* const* buffer, int nframes, void* user_data) {
    // Fill buffer with audio to be processed
    // buffer[channel][frame] - non-interleaved format
    MyAudioSource* source = (MyAudioSource*)user_data;

    for (int ch = 0; ch < 2; ch++) {
        for (int f = 0; f < nframes; f++) {
            buffer[ch][f] = get_sample(source, ch);
        }
    }
}

// Set the callback before starting
mh_audio_set_input_callback(audio, my_input_callback, my_audio_source);
mh_audio_start(audio);
```

Without an input callback, the plugin receives silence (appropriate for synths/instruments).

### Thread Safety

The audio device creates its own audio thread internally:

- `mh_audio_start()`, `mh_audio_stop()`, `mh_audio_is_playing()` - Safe from any thread
- `mh_audio_open()`, `mh_audio_close()` - Call from main thread
- The input callback runs on the audio thread - keep it fast, no allocations

**Critical**: Do not call `mh_audio_close()` while callbacks might be running. Always call `mh_audio_stop()` first.

### Error Handling

```c
MH_AudioDevice* audio = mh_audio_open(plugin, &config, err, sizeof(err));
if (!audio) {
    // err contains description: "No audio device found",
    // "Failed to set sample rate", etc.
    fprintf(stderr, "Audio error: %s\n", err);
}
```

---

## Real-time MIDI I/O

minihost includes [libremidi](https://github.com/jcelerier/libremidi) integration for cross-platform MIDI I/O. This allows connecting hardware MIDI controllers to plugins and receiving MIDI output.

### Headers

```c
#include "minihost_audio.h"  // AudioDevice MIDI functions
#include "minihost_midi.h"   // Port enumeration, standalone MIDI
```

### Port Enumeration

```c
// Get number of ports
int num_inputs = mh_midi_get_num_inputs();
int num_outputs = mh_midi_get_num_outputs();

// Get port names
for (int i = 0; i < num_inputs; i++) {
    char name[256];
    mh_midi_get_input_name(i, name, sizeof(name));
    printf("Input %d: %s\n", i, name);
}

for (int i = 0; i < num_outputs; i++) {
    char name[256];
    mh_midi_get_output_name(i, name, sizeof(name));
    printf("Output %d: %s\n", i, name);
}
```

### Callback-Based Enumeration

```c
void on_port(const MH_MidiPortInfo* port, void* user_data) {
    printf("[%d] %s\n", port->index, port->name);
}

printf("MIDI Inputs:\n");
mh_midi_enumerate_inputs(on_port, NULL);

printf("MIDI Outputs:\n");
mh_midi_enumerate_outputs(on_port, NULL);
```

### Connecting MIDI to AudioDevice

The easiest way to use MIDI is through `MH_AudioDevice`:

```c
// Connect at creation time
MH_AudioConfig config = {
    .sample_rate = 48000,
    .midi_input_port = 0,    // First MIDI input device
    .midi_output_port = -1   // No output
};
MH_AudioDevice* audio = mh_audio_open(plugin, &config, err, sizeof(err));

// Or connect dynamically
mh_audio_connect_midi_input(audio, 0);   // Connect to port 0
mh_audio_connect_midi_output(audio, 1);  // Connect output to port 1

// Query current connections
int in_port = mh_audio_get_midi_input_port(audio);   // -1 if not connected
int out_port = mh_audio_get_midi_output_port(audio);

// Disconnect
mh_audio_disconnect_midi_input(audio);
mh_audio_disconnect_midi_output(audio);
```

When MIDI is connected to an AudioDevice:
- **Input**: MIDI messages are buffered in a lock-free ring buffer and processed at the start of each audio callback
- **Output**: MIDI generated by the plugin is sent to the output port immediately

### Virtual MIDI Ports

Virtual ports create named MIDI ports that appear in the system's MIDI port list. Other applications (DAWs, etc.) can connect to them.

```c
// Create virtual ports
mh_audio_create_virtual_midi_input(audio, "My App Input");
mh_audio_create_virtual_midi_output(audio, "My App Output");

// Check if current connection is virtual
if (mh_audio_is_midi_input_virtual(audio)) {
    printf("Using virtual MIDI input\n");
}
```

**Platform Support**:
- **macOS**: Full support via CoreMIDI
- **Linux**: Full support via ALSA
- **Windows**: Not supported (returns failure)

### Standalone MIDI (Without AudioDevice)

For advanced use cases, you can open MIDI ports directly:

```c
// Callback for incoming MIDI
void on_midi(const unsigned char* data, size_t len, void* user_data) {
    printf("MIDI: %02X %02X %02X\n", data[0],
           len >= 2 ? data[1] : 0,
           len >= 3 ? data[2] : 0);
}

// Open input
char err[256];
MH_MidiIn* midi_in = mh_midi_in_open(0, on_midi, NULL, err, sizeof(err));

// Open output
MH_MidiOut* midi_out = mh_midi_out_open(0, err, sizeof(err));

// Send MIDI
unsigned char note_on[] = {0x90, 60, 100};
mh_midi_out_send(midi_out, note_on, 3);

// Virtual ports
MH_MidiIn* virtual_in = mh_midi_in_open_virtual("My Virtual Input",
                                                  on_midi, NULL, err, sizeof(err));
MH_MidiOut* virtual_out = mh_midi_out_open_virtual("My Virtual Output",
                                                     err, sizeof(err));

// Cleanup
mh_midi_in_close(midi_in);
mh_midi_out_close(midi_out);
```

### Complete Example: Synth with MIDI Controller

```c
#include "minihost.h"
#include "minihost_audio.h"
#include "minihost_midi.h"
#include <stdio.h>
#include <signal.h>

static volatile int running = 1;

void on_signal(int sig) {
    running = 0;
}

int main(int argc, char** argv) {
    char err[1024];

    // List MIDI inputs
    printf("Available MIDI inputs:\n");
    int num_inputs = mh_midi_get_num_inputs();
    for (int i = 0; i < num_inputs; i++) {
        char name[256];
        mh_midi_get_input_name(i, name, sizeof(name));
        printf("  [%d] %s\n", i, name);
    }

    if (num_inputs == 0) {
        printf("No MIDI inputs found. Creating virtual port.\n");
    }

    // Load synth plugin
    MH_Plugin* plugin = mh_open(argv[1], 48000.0, 512, 0, 2, err, sizeof(err));
    if (!plugin) {
        fprintf(stderr, "Failed to load plugin: %s\n", err);
        return 1;
    }

    // Open audio with MIDI
    MH_AudioConfig config = {
        .sample_rate = 48000,
        .midi_input_port = (num_inputs > 0) ? 0 : -1
    };

    MH_AudioDevice* audio = mh_audio_open(plugin, &config, err, sizeof(err));
    if (!audio) {
        fprintf(stderr, "Failed to open audio: %s\n", err);
        mh_close(plugin);
        return 1;
    }

    // Create virtual MIDI input if no hardware available
    if (num_inputs == 0) {
        if (mh_audio_create_virtual_midi_input(audio, "minihost Synth")) {
            printf("Created virtual MIDI input: 'minihost Synth'\n");
            printf("Connect your DAW or MIDI app to this port.\n");
        }
    }

    // Start audio
    mh_audio_start(audio);
    printf("Playing. Press Ctrl+C to stop.\n");

    // Handle Ctrl+C
    signal(SIGINT, on_signal);

    // Run until interrupted
    while (running) {
        // Could update parameters, display info, etc.
        usleep(100000);  // 100ms
    }

    // Cleanup
    printf("\nStopping...\n");
    mh_audio_stop(audio);
    mh_audio_close(audio);
    mh_close(plugin);

    return 0;
}
```

### Thread Safety for MIDI

- **Port enumeration**: Thread-safe, can be called from any thread
- **MIDI callbacks**: Run on the MIDI thread (libremidi's thread)
- **Ring buffer**: Lock-free SPSC (single-producer single-consumer) for MIDI -> audio transfer
- **mh_audio_connect/disconnect**: Safe to call while audio is running

The internal architecture ensures no locks are held in the audio callback path:

```
MIDI Thread                    Audio Thread
    |                              |
    v                              v
mh_midi_in callback           audio_callback
    |                              |
    v                              v
ring_buffer_push() --------> ring_buffer_pop_all()
(lock-free)                  (lock-free)
    |                              |
    |                              v
    |                         mh_process_midi_io()
```

---

## Audio File I/O

minihost includes audio file read/write capabilities via [miniaudio](https://miniaud.io/), with no external library dependencies.

### Headers

```c
#include "minihost_audiofile.h"
```

### Linking

```cmake
target_link_libraries(your_app PRIVATE minihost_audio)
```

The audio file functions are part of `libminihost_audio`.

### Reading Audio Files

```c
char err[1024];
MH_AudioData* audio = mh_audio_read("input.flac", err, sizeof(err));
if (!audio) {
    fprintf(stderr, "Read failed: %s\n", err);
    return;
}

printf("Channels: %u, Frames: %u, Rate: %u Hz\n",
       audio->channels, audio->frames, audio->sample_rate);

// audio->data is interleaved float32: [L0, R0, L1, R1, ...]
// Total samples = channels * frames

// Process the audio...
process(audio->data, audio->channels, audio->frames);

// Always free when done
mh_audio_data_free(audio);
```

**Supported read formats**: WAV, FLAC, MP3, Vorbis/OGG.

### Writing Audio Files

```c
// Write interleaved float32 data to WAV
int ok = mh_audio_write("output.wav", interleaved_data,
                         channels, num_frames, sample_rate,
                         24,  // bit_depth: 16, 24, or 32
                         err, sizeof(err));
if (!ok) {
    fprintf(stderr, "Write failed: %s\n", err);
}
```

**Bit depth options**:
- `16` -- 16-bit integer PCM (with triangle dithering from float32)
- `24` -- 24-bit integer PCM (with triangle dithering from float32)
- `32` -- 32-bit IEEE float (lossless from float32 source)

**Write format**: WAV only. FLAC, AIFF, and OGG writing are not supported.

### Getting File Info

Query metadata without decoding the entire file:

```c
MH_AudioFileInfo info;
if (mh_audio_get_file_info("song.wav", &info, err, sizeof(err))) {
    printf("Channels: %u\n", info.channels);
    printf("Sample rate: %u Hz\n", info.sample_rate);
    printf("Frames: %llu\n", info.frames);
    printf("Duration: %.2f seconds\n", info.duration);
}
```

### Complete Example: Process Audio File Through Plugin

```c
#include "minihost.h"
#include "minihost_audiofile.h"

char err[1024];

// Read input
MH_AudioData* input = mh_audio_read("input.wav", err, sizeof(err));
if (!input) { fprintf(stderr, "%s\n", err); return 1; }

// Load plugin matching the file's sample rate
MH_Plugin* plugin = mh_open("/path/to/effect.vst3",
    (double)input->sample_rate, 512,
    input->channels, input->channels, err, sizeof(err));

// De-interleave, process, re-interleave (see Audio Processing section)
// ...

// Write output
mh_audio_write("output.wav", output_interleaved,
               input->channels, input->frames, input->sample_rate,
               24, err, sizeof(err));

mh_audio_data_free(input);
mh_close(plugin);
```

---

## Performance Guidelines

### Do

1. **Pre-allocate buffers** - Allocate all audio buffers before entering the audio callback
2. **Reuse plugin instances** - Opening/closing is expensive; reuse when possible
3. **Match channel counts** - Query actual channel counts and allocate accordingly
4. **Use appropriate block sizes** - 256-1024 samples is typical; smaller increases CPU overhead
5. **Cache parameter values** - If displaying many parameters, cache values and update periodically
6. **Use `mh_probe()` for scanning** - It's much faster than `mh_open()` for building plugin lists

### Don't

1. **Don't allocate in audio callbacks** - No malloc/free/new/delete in process functions
2. **Don't call `mh_open()`/`mh_close()` from audio thread** - These are slow and may block
3. **Don't exceed max_block_size** - Process will fail
4. **Don't ignore latency** - Important for accurate timing when mixing signals
5. **Don't assume channel counts** - Always check `MH_Info` after opening

### Memory

```c
// Allocate once at startup
float** in = alloc_channels(in_ch, max_block_size);
float** out = alloc_channels(out_ch, max_block_size);
MH_MidiEvent* midi_buf = malloc(256 * sizeof(MH_MidiEvent));

// Use in audio callback
mh_process(plugin, in, out, frames);

// Free at shutdown
free_channels(in, in_ch);
free_channels(out, out_ch);
free(midi_buf);
```

---

## Common Pitfalls

### 1. Forgetting to Close Plugins

```c
// BAD - leaks resources
void process_file(const char* plugin_path) {
    MH_Plugin* p = mh_open(...);
    if (!p) return;  // Forgot to close on error paths!
    // ...
    // Forgot mh_close(p);
}

// GOOD - always close
void process_file(const char* plugin_path) {
    MH_Plugin* p = mh_open(...);
    if (!p) return;

    // ... processing ...

    mh_close(p);
}
```

### 2. Wrong Buffer Format

```c
// BAD - interleaved buffer
float interleaved[1024];  // L R L R L R ...
mh_process(plugin, &interleaved, ...);  // WRONG

// GOOD - non-interleaved
float left[512], right[512];
float* channels[2] = {left, right};
mh_process(plugin, channels, ...);
```

### 3. Exceeding Block Size

```c
// Opened with max_block_size = 512
MH_Plugin* p = mh_open(path, 48000, 512, 2, 2, err, sizeof(err));

// BAD - will fail
mh_process(p, in, out, 1024);

// GOOD - within limit
mh_process(p, in, out, 512);
mh_process(p, in, out, 256);  // Smaller is fine
```

### 4. Not Checking Return Values

```c
// BAD - ignores errors
mh_set_state(plugin, data, size);

// GOOD - handle errors
if (!mh_set_state(plugin, data, size)) {
    fprintf(stderr, "Failed to restore state\n");
}
```

### 5. Calling Close from Wrong Thread

```c
// BAD - closing while audio thread might be processing
void on_user_quit() {
    mh_close(plugin);  // Audio thread might still be in mh_process()!
}

// GOOD - stop audio first
void on_user_quit() {
    stop_audio_thread();
    wait_for_audio_thread();
    mh_close(plugin);
}
```

### 6. Assuming Instrument Needs Input

```c
// Many instruments have 0 inputs
MH_Info info;
mh_get_info(plugin, &info);

if (info.num_input_ch == 0) {
    // This is an instrument - pass NULL for input
    mh_process(plugin, NULL, out, frames);
}
```

### 7. Ignoring Tail for Reverbs/Delays

```c
// BAD - cuts off reverb tail
process_input_file();
// Done! (but reverb tail is lost)

// GOOD - process tail
process_input_file();
double tail = mh_get_tail_seconds(plugin);
process_silence_for(tail * sample_rate);
```

---

## Quick Reference

### Plugin Functions

| Task | Function |
|------|----------|
| Open plugin | `mh_open()` |
| Close plugin | `mh_close()` |
| Process audio | `mh_process()` |
| Process with MIDI | `mh_process_midi()` or `mh_process_midi_io()` |
| Process with automation | `mh_process_auto()` |
| Get/set parameter | `mh_get_param()` / `mh_set_param()` |
| Save/load state | `mh_get_state()` / `mh_set_state()` |
| Set tempo | `mh_set_transport()` |
| Get latency | `mh_get_latency_samples()` |
| Reset plugin | `mh_reset()` |
| Probe without loading | `mh_probe()` |
| Scan directory | `mh_scan_directory()` |

### Audio Device Functions (minihost_audio.h)

| Task | Function |
|------|----------|
| Open audio device | `mh_audio_open()` |
| Close audio device | `mh_audio_close()` |
| Start/stop playback | `mh_audio_start()` / `mh_audio_stop()` |
| Check playing state | `mh_audio_is_playing()` |
| Set effect input callback | `mh_audio_set_input_callback()` |
| Query sample rate | `mh_audio_get_sample_rate()` |
| Query buffer size | `mh_audio_get_buffer_frames()` |
| Query channel count | `mh_audio_get_channels()` |
| Connect MIDI input | `mh_audio_connect_midi_input()` |
| Connect MIDI output | `mh_audio_connect_midi_output()` |
| Create virtual MIDI input | `mh_audio_create_virtual_midi_input()` |
| Create virtual MIDI output | `mh_audio_create_virtual_midi_output()` |

### Audio File I/O Functions (minihost_audiofile.h)

| Task | Function |
|------|----------|
| Read audio file | `mh_audio_read()` |
| Free decoded data | `mh_audio_data_free()` |
| Write WAV file | `mh_audio_write()` |
| Get file metadata | `mh_audio_get_file_info()` |

### MIDI Functions (minihost_midi.h)

| Task | Function |
|------|----------|
| Count MIDI ports | `mh_midi_get_num_inputs()` / `mh_midi_get_num_outputs()` |
| Get port name | `mh_midi_get_input_name()` / `mh_midi_get_output_name()` |
| Enumerate ports | `mh_midi_enumerate_inputs()` / `mh_midi_enumerate_outputs()` |
| Open MIDI input | `mh_midi_in_open()` |
| Open MIDI output | `mh_midi_out_open()` |
| Open virtual input | `mh_midi_in_open_virtual()` |
| Open virtual output | `mh_midi_out_open_virtual()` |
| Send MIDI | `mh_midi_out_send()` |
