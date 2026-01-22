# Plugin Hosting Guide

This guide covers how to host VST3 and AudioUnit plugins using minihost as an embedded library.

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
13. [Performance Guidelines](#performance-guidelines)
14. [Common Pitfalls](#common-pitfalls)

---

## Overview

minihost is a C library for hosting VST3 and AudioUnit plugins without GUI support. It's designed for:

- Headless audio servers
- Batch processing tools
- Embedded applications
- Any scenario where plugin UI is not needed

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
    "/path/to/plugin.vst3",  // or .component for AU
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
