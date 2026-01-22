# minihost Feature Review

Analysis of feature coverage for headless plugin hosting in small applications and servers.

## Current Capabilities

| Category | Feature | Status |
|----------|---------|--------|
| **Loading** | VST3 support | Yes |
| | AudioUnit support (macOS) | Yes |
| | Path-based loading | Yes |
| **Audio** | Non-interleaved float processing | Yes |
| | Variable block sizes (up to max) | Yes |
| | Null input/output handling | Yes |
| **MIDI** | Input events | Yes |
| | Output events | Yes |
| | Sample-accurate timing | Yes |
| **Parameters** | Get/set by index (normalized) | Yes |
| | Metadata (name, label, steps, flags) | Yes |
| | Sample-accurate automation | Yes |
| **State** | Binary save/restore | Yes |
| **Transport** | Tempo, time signature | Yes |
| | Position (samples & beats) | Yes |
| | Play/record/loop flags | Yes |
| **Utilities** | Latency reporting | Yes |
| | Tail time reporting | Yes |
| | Bypass control | Yes |
| **Threading** | Lock-free audio path | Yes |
| | Thread-safe state access | Yes |

The core use case of "load plugin, process audio, control parameters" is well covered.

## Missing Features

### High Priority (common server/batch use cases)

**1. Plugin metadata without instantiation**
```c
// Proposed API
int mh_probe(const char* path, MH_PluginDesc* desc, char* err, size_t err_size);

typedef struct MH_PluginDesc {
    char name[256];
    char vendor[256];
    char version[64];
    char format[16];        // "VST3" or "AU"
    char unique_id[64];     // for state compatibility checking
    int accepts_midi;
    int produces_midi;
    int num_inputs;         // default channel count
    int num_outputs;
} MH_PluginDesc;
```
Use case: Server startup validation, plugin inventory, state file compatibility checks.

**2. Offline/non-realtime mode**
```c
int mh_set_non_realtime(MH_Plugin* p, int non_realtime);
```
Use case: Batch rendering. Many plugins use higher-quality algorithms when they know they're not running in realtime (better interpolation, oversampling, etc.).

**3. Reset/flush internal state**
```c
int mh_reset(MH_Plugin* p);
```
Use case: Clear delay lines, reverb tails, filter states between unrelated audio segments. Currently requires close/reopen. JUCE exposes `reset()` on AudioProcessor.

**4. Parameter value text conversion**
```c
// normalized -> display string (already have via get_param_info, but requires current value)
int mh_param_to_text(MH_Plugin* p, int index, float value, char* buf, size_t buf_size);
// display string -> normalized
int mh_param_from_text(MH_Plugin* p, int index, const char* text, float* out_value);
```
Use case: Config files, APIs, logs. "cutoff=2500Hz" instead of "cutoff=0.347".

### Medium Priority (useful but less common)

**5. Factory preset enumeration**
```c
int mh_get_num_programs(MH_Plugin* p);
int mh_get_program_name(MH_Plugin* p, int index, char* buf, size_t buf_size);
int mh_set_program(MH_Plugin* p, int index);
int mh_get_program(MH_Plugin* p);
```
Use case: Expose built-in presets to users without shipping separate state files.

**6. Sidechain input support**
```c
// Extended open with sidechain channels
MH_Plugin* mh_open_ex(const char* path, double sr, int block,
                      int main_in, int main_out,
                      int sidechain_in,  // 0 to disable
                      char* err, size_t err_size);

// Process with sidechain
int mh_process_sidechain(MH_Plugin* p,
                         const float* const* main_in,
                         float* const* main_out,
                         const float* const* sidechain_in,
                         int nframes);
```
Use case: Compressors with external key input, vocoders, ducking effects.

**7. Bus layout query**
```c
typedef struct MH_BusInfo {
    char name[64];
    int num_channels;
    int is_main;           // vs aux/sidechain
} MH_BusInfo;

int mh_get_num_buses(MH_Plugin* p, int is_input);
int mh_get_bus_info(MH_Plugin* p, int is_input, int bus_index, MH_BusInfo* info);
```
Use case: Understanding plugin I/O topology for complex routing.

**8. Sample rate change without reload**
```c
int mh_set_sample_rate(MH_Plugin* p, double new_rate);
```
Use case: Processing files at various sample rates. Current workaround (close/reopen) loses parameter state.

### Lower Priority (edge cases)

**9. Double precision processing**
```c
int mh_process_double(MH_Plugin* p, const double* const* in, double* const* out, int n);
```
Some plugins support 64-bit processing internally. Marginal benefit for most use cases.

**10. Async plugin loading**
```c
typedef void (*MH_LoadCallback)(MH_Plugin* p, const char* error, void* user);
int mh_open_async(const char* path, double sr, int block, int in, int out,
                  MH_LoadCallback cb, void* user);
```
Use case: Server startup with many plugins. Could be handled at application level with threads.

**11. Plugin scanning**
```c
typedef void (*MH_ScanCallback)(const MH_PluginDesc* desc, void* user);
int mh_scan_directory(const char* path, MH_ScanCallback cb, void* user);
```
Use case: Plugin discovery. Arguably out of scope for a "minimal" host - can be built on top of `mh_probe`.

## Implementation Notes

### Non-realtime mode
JUCE's `AudioProcessor::setNonRealtime(bool)` is already available. Trivial to expose.

### Reset
`AudioProcessor::reset()` exists. One-liner to expose.

### Factory presets
JUCE provides `getNumPrograms()`, `getProgramName()`, `setCurrentProgram()`, `getCurrentProgram()`. Straightforward mapping.

### Parameter text conversion
JUCE's `AudioProcessorParameter` has `getText(float, int)` and `getValueForText(String)`. Some plugins implement these poorly, so fallback handling may be needed.

### Sidechain
Requires changes to bus configuration in `tryConfigureBuses()`. The current implementation calls `enableAllBuses()` but doesn't expose sidechain buses to the caller.

## Recommendations

**Phase 1** (most impact, low effort):
1. `mh_reset()` - single line, commonly needed
2. `mh_set_non_realtime()` - single line, important for batch
3. `mh_probe()` - moderate effort, high value for servers

**Phase 2** (moderate effort):
4. Parameter text conversion
5. Factory preset access

**Phase 3** (if needed):
6. Sidechain support
7. Bus layout queries

Items 8-11 could remain out of scope unless specific use cases emerge.

## Non-goals

Given the "small applications and servers, no UI" focus, these are correctly omitted:
- Editor window management
- GUI hosting
- Preset browser UI
- Parameter change notifications (callbacks)
- MIDI learn
- Plugin shell/multi-instrument handling
