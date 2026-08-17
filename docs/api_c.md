# C API Reference

## Plugin Functions (minihost.h)

| Function | Description |
|----------|-------------|
| `mh_open` | Load a plugin |
| `mh_open_ex` | Load a plugin with sidechain channel configuration |
| `mh_open_async` | Load a plugin in a background thread |
| `mh_close` | Unload a plugin |
| `mh_get_path` | Get the plugin file path passed to `mh_open` / `mh_open_ex` |
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

### Parameter Morphing

A/B interpolation over the normalized parameter values (a "snapshot" is one value per parameter). Operates on parameters, not opaque state blobs, since only parameters interpolate. Only continuous parameters glide; stepped/boolean parameters are quantized by the plugin.

| Function | Description |
|----------|-------------|
| `mh_morph_capture` | Capture every parameter's current normalized value into an array (returns the count) |
| `mh_morph_apply` | Set every parameter from a snapshot array (values clamped to 0..1) |
| `mh_morph_lerp` | Interpolate two snapshots with one blend amount `t`: `out = clamp01(a + (b - a) * t)` (pure math) |
| `mh_morph_lerp_per_param` | Interpolate two snapshots with a per-parameter `t` array |
| `mh_morph` | Interpolate two snapshots at scalar `t` and apply to the plugin in one call |

```c
int n = mh_get_num_params(p);
float a[512], b[512];               // n <= 512 for this example
mh_set_program(p, 0); mh_morph_capture(p, a, n);   // snapshot A
mh_set_program(p, 1); mh_morph_capture(p, b, n);   // snapshot B
mh_morph(p, a, b, n, 0.5f);         // apply the 50% blend
```

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

`MH_Info.num_input_ch` is the **main** input bus only -- the width of the
`inputs` array the `mh_process*` functions expect. Sidechain channels are
reported by `mh_get_sidechain_channels` and supplied separately to
`mh_process_sidechain`. Before C ABI 2.4.0 `num_input_ch` summed every input
bus, which made callers over-provision the main buffer and pushed the
sidechain past the channels the plugin reads.

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

Set `MH_AudioConfig.capture = 1` to open the device in duplex mode (system audio input routed through the plugin). The audio callback de-interleaves captured audio directly into the plugin's input buffers.

Set `MH_AudioConfig.playback_device_index` / `capture_device_index` to a 0-based device index to target a specific audio device. Use `-1` (the default) to use the system default.

### Device Enumeration

| Function | Description |
|----------|-------------|
| `mh_audio_enumerate_playback_devices` | Fill an array of `MH_AudioDeviceInfo` with available playback devices. Returns count |
| `mh_audio_enumerate_capture_devices` | Fill an array of `MH_AudioDeviceInfo` with available capture devices. Returns count |

**`MH_AudioDeviceInfo`** struct:

- `char name[256]` -- device name (null-terminated)

- `int is_default` -- `1` if this is the system default, else `0`

Typical usage:

```c
MH_AudioDeviceInfo devices[32];
int count = mh_audio_enumerate_playback_devices(devices, 32);
for (int i = 0; i < count; i++) {
    printf("[%d]%s %s\n", i, devices[i].is_default ? "*" : " ", devices[i].name);
}
```

### Audio Input (Ring Buffer)

| Function | Description |
|----------|-------------|
| `mh_audio_enable_input` | Enable ring buffer audio input with given capacity |
| `mh_audio_disable_input` | Disable ring buffer input (revert to silence) |
| `mh_audio_write_input` | Write interleaved float32 frames into input ring buffer (thread-safe) |
| `mh_audio_input_available` | Get number of frames available for reading |

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
| `mh_audio_write` | Write interleaved float32 data to WAV or FLAC file |
| `mh_audio_get_file_info` | Get audio file metadata without decoding |
| `mh_audio_resample` | Resample interleaved float32 audio between any two sample rates |

### Supported Formats

| Format | Read | Write |
|--------|------|-------|
| WAV | Yes | Yes (16/24/32-bit) |
| FLAC | Yes | Yes (16/24-bit) |
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
| `mh_chain_process_midi_io` | Process with MIDI I/O. MIDI enters the first plugin that accepts it and is carried onward by any plugin reporting `produces_midi`, so a MIDI effect can drive an instrument behind it; `midi_out` is what leaves the last plugin |
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

## Plugin Bus Functions (minihost_graph.h)

Parallel-branches-summed routing: fan one input to N `MH_PluginChain` branches and sum their outputs with per-branch gain. Python: `PluginBus`. (File name retained from before the 0.2.0 rename; symbols are `mh_bus_*`.)

| Function | Description |
|----------|-------------|
| `mh_bus_create` | Create a bus for a fixed I/O channel layout, block size, and sample rate |
| `mh_bus_close` | Close the bus (does not close the branches) |
| `mh_bus_add_branch` | Add a `MH_PluginChain` branch with a linear gain; returns branch index. Rejects channel/sample-rate mismatch |
| `mh_bus_set_branch_gain` / `mh_bus_get_branch_gain` | Set/get a branch's summing gain (0.0 mutes; muted branches skip processing) |
| `mh_bus_get_num_branches` | Number of branches |
| `mh_bus_process` | Fan input to every branch, sum (per-branch gain) into outputs |
| `mh_bus_process_midi` | As `mh_bus_process`, but also fan the same MIDI to every branch (to each branch's first plugin). Branch MIDI output is not collected |
| `mh_bus_get_num_input_channels` / `mh_bus_get_num_output_channels` | Configured channel counts |
| `mh_bus_get_sample_rate` / `mh_bus_get_max_block_size` | Configured sample rate / block size |
| `mh_bus_get_latency_samples` / `mh_bus_get_tail_seconds` | Maximum latency / tail across branches |

The general DAG executor (Python `PluginGraph`) lives in `minihost_graph_v2.h` as the `mh_graph_*` / `MH_PluginGraph` family, with a C++ RAII wrapper `minihost::PluginGraph` in `minihost_graph_v2.hpp`. Its surface is large (node add/connect, MIDI routing, automation, `mh_graph_compile`, `mh_graph_render_block`); see the header for the full API.

---

## Plugin Discovery and the Scan Cache (minihost.h)

| Function | Description |
|----------|-------------|
| `mh_get_default_plugin_dir` | Canonical plugin directory for this platform, by index |
| `mh_plugin_cache_path` | Absolute path of the shared scan cache |
| `mh_plugin_cache_scan` | Scan directories (or the canonical ones) and write the cache |
| `mh_plugin_cache_scan_supervised` | The same, probing each plugin in a disposable child process |
| `mh_plugin_scan_worker_main` | Answer the worker flag; call first thing in `main` |
| `mh_plugin_cache_lookup` | Resolve a plugin name to a path, ignoring case |
| `mh_plugin_cache_match` | Nth match for a name, for reporting an ambiguity |

Both take a `format` preference (`"vst3"`, `"au"`/`"audiounit"`, or `NULL`) and an
`allow_substring` flag.

`mh_get_default_plugin_dir` enumerates until it returns 0, and skips
directories that do not exist, so a caller sees what is actually installed:

```c
char dir[1024];
for (int i = 0; mh_get_default_plugin_dir(i, dir, sizeof(dir)); i++)
    printf("%s\n", dir);
```

| Platform | Directories |
|----------|-------------|
| macOS | `/Library/Audio/Plug-Ins/{VST3,Components}` and the same two under `~/Library` |
| Windows | `C:\Program Files\Common Files\VST3`, and the x86 equivalent |
| Linux | `/usr/lib/vst3`, `/usr/local/lib/vst3`, `~/.vst3` |

Scanning probes each plugin, which means loading it, so a first full scan of
a large collection takes minutes. The result is cached and keyed by path with
an mtime + size fingerprint, so a repeat scan re-probes only what changed;
pass `refresh = 1` to force. A plugin that fails to probe is remembered as an
error rather than retried on every scan. The cache is written as the scan
proceeds, so a scan that is interrupted keeps what it had and a re-run resumes.

Probing is dispatched to the message thread, since instantiating an AudioUnit
is thread-affine on macOS; callers need do nothing.

`mh_plugin_cache_scan` probes in the calling process, so a plugin that hangs
or crashes on load ends the scan. `mh_plugin_cache_scan_supervised` gives each
plugin its own child process and a deadline instead:

```c
char err[512] = {0};
int n = mh_plugin_cache_scan_supervised(NULL, 0, /*refresh=*/0,
                                        NULL, 0,      /* worker: this binary */
                                        /*timeout_ms=*/0,   /* 0 -> 60000 */
                                        NULL, NULL, err, sizeof(err));
```

Passing `NULL` for the worker spawns this process's own executable, which
works when the program answers the worker flag first thing in `main`:

```c
int main(int argc, char** argv) {
    if (mh_plugin_scan_worker_main(argc, argv)) return 0;   /* was a worker */
    ...
}
```

An embedder whose executable is not ours -- a DAW, or Python -- passes its own
worker command instead (`{"python3", "-m", "yourpkg.worker"}`), implementing
the protocol documented in the header: one JSON object on stdout between
`MH_SCAN_WORKER_BEGIN` and `MH_SCAN_WORKER_END`. The markers exist because
plugins print to stdout while loading, so the answer has to be findable inside
that noise. `MINIHOST_SCAN_WORKER` overrides the command, and
`MINIHOST_SCAN_TIMEOUT_MS` the deadline.

Cache entries gain two outcomes only the supervised path can report --
`timeout` and `crash` -- both fingerprinted like any other entry, so a re-scan
skips those plugins rather than paying for them again.

```c
char err[512] = {0};
int cached = mh_plugin_cache_scan(NULL, 0, /*refresh=*/0, NULL, NULL,
                                  err, sizeof(err));   /* NULL: canonical dirs */
if (cached < 0) fprintf(stderr, "%s\n", err);
```

Name lookup ignores case and matches the whole name; pass `allow_substring = 1`
to match part of one, which on a real collection is usually ambiguous. Entries
that failed to probe are never offered. When every match is the same name in
more than one format -- a common way to install a plugin -- one is chosen
instead of reporting an ambiguity: the requested `format` if given, else VST3.
The return value distinguishes the three outcomes:

```c
char path[1024];
int n = mh_plugin_cache_lookup("dexed", NULL, /*allow_substring=*/0,
                               path, sizeof(path));
if (n == 1) {
    /* unique match: path is filled in */
} else if (n == 0) {
    /* nothing matched -- scan first, or use a path */
} else {
    /* ambiguous: n candidates, path holds the first */
    char one[1024];
    for (int i = 0; mh_plugin_cache_match("dexed", NULL, 0, i,
                                          one, sizeof(one)); i++)
        fprintf(stderr, "  %s\n", one);
}
```

The cache file and its JSON schema are shared with the Python CLI
(`minihost.plugincache`), so a scan from either front-end serves the other.
Location, honouring `MINIHOST_CACHE_DIR`:

| Platform | Cache file |
|----------|------------|
| macOS | `~/Library/Caches/minihost/plugins.json` |
| Windows | `%LOCALAPPDATA%/minihost/Cache/plugins.json` |
| other | `$XDG_CACHE_HOME/minihost/plugins.json` (or `~/.cache/...`) |

Added in C ABI 2.7.0.

---

## MIDI File Reading (minihost.h)

| Function | Description |
|----------|-------------|
| `mh_midi_file_load` | Read a standard MIDI file into one time-ordered `MH_MidiEvent` array |
| `mh_midi_file_free` | Release an array returned by `mh_midi_file_load` |

Tracks are merged, the file's tempo map is applied, and meta events are
dropped (they have no `MH_MidiEvent` form). `sample_offset` is absolute --
measured in samples from the start of the file at the rate you pass in --
so rebase it per block before handing events to a plugin:

```c
MH_MidiEvent* events = NULL;
int count = 0;
double duration = 0.0;
char err[512] = {0};

if (!mh_midi_file_load("song.mid", 48000.0, &events, &count, &duration,
                       err, sizeof(err))) {
    fprintf(stderr, "%s\n", err);
    return 1;
}

int cursor = 0;
for (int start = 0; start < total_frames; start += block) {
    int end = start + block;
    MH_MidiEvent block_midi[256];
    int n = 0;
    while (cursor < count && events[cursor].sample_offset < end && n < 256) {
        block_midi[n] = events[cursor];
        block_midi[n].sample_offset -= start;   /* rebase to this block */
        n++;
        cursor++;
    }
    mh_process_midi(p, inputs, outputs, block, block_midi, n);
}

mh_midi_file_free(events);
```

An empty file succeeds with `count == 0` and a NULL array. Added in C ABI
2.6.0; before that, reading a MIDI file was possible only from Python.

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

---

## VST3 Preset Functions (minihost_vstpreset.h)

Portable Steinberg `.vstpreset` reader/writer with no external dependencies (little-endian packing built in). Callable from both C and C++.

| Function | Description |
|----------|-------------|
| `mh_vstpreset_read` | Read a `.vstpreset` file into an `MH_VstPreset` struct |
| `mh_vstpreset_write` | Write `class_id` + component/controller state blobs to a `.vstpreset` file |
| `mh_vstpreset_free` | Free heap-allocated state blobs inside an `MH_VstPreset` populated by `mh_vstpreset_read` |
| `mh_vstpreset_read_class_id_from_bundle` | Read the processor FUID from a VST3 bundle's `Contents/Resources/moduleinfo.json` |

### Structs

**`MH_VstPreset`** -- populated by `mh_vstpreset_read()`:

- `char class_id[33]` -- 32-char FUID plus null terminator (`MH_VSTPRESET_CLASS_ID_LEN` = 32)

- `void* component_state` -- processor (`Comp`) chunk, heap-allocated

- `int component_size`

- `void* controller_state` -- controller (`Cont`) chunk, heap-allocated (may be `NULL`)

- `int controller_size`

Typical usage:

```c
#include "minihost_vstpreset.h"

// Read
MH_VstPreset preset;
char err[256];
if (mh_vstpreset_read("in.vstpreset", &preset, err, sizeof(err))) {
    // apply preset.component_state to plugin via mh_set_state(), etc.
    mh_vstpreset_free(&preset);
}

// Auto-detect the processor FUID from the plugin bundle (no plugin
// instantiation required). Works for any VST3 plugin built against
// SDK 3.7.5 or newer (which ships moduleinfo.json in the bundle).
char class_id[MH_VSTPRESET_CLASS_ID_LEN + 1];
if (!mh_vstpreset_read_class_id_from_bundle(
        "/path/to/synth.vst3", class_id, err, sizeof(err))) {
    fprintf(stderr, "%s\n", err);
    // Legacy plugin: caller must supply class_id another way
    // (e.g., copy from an existing .vstpreset).
}

// Write
int state_size = mh_get_state_size(plugin);
void* state = malloc(state_size);
mh_get_state(plugin, state, state_size);
mh_vstpreset_write("out.vstpreset",
                   class_id,
                   state, state_size,
                   NULL, 0,  // no separate controller state
                   err, sizeof(err));
free(state);
```

### Notes on `mh_vstpreset_read_class_id_from_bundle`

- Returns 1 on success and writes a 32-character uppercase hex FUID into `out_class_id` (which must be at least `MH_VSTPRESET_CLASS_ID_LEN + 1` bytes).

- Selects the first entry in the bundle's `Classes` array whose `Category` is `"Audio Module Class"` (the processor component, not the controller).

- Tolerates JSON5-style trailing commas, which appear in real-world bundles (e.g., Dexed, Strokes).

- Returns 0 with a descriptive error in `err_buf` for: missing `moduleinfo.json` (plugin predates VST3 SDK 3.7.5), file unreadable, file > 1 MB, JSON malformed, no Audio Module Class entry, or CID not exactly 32 hex characters.

- VST3 only -- AudioUnit and LV2 plugins do not have `.vstpreset` files or moduleinfo.json.
