// minihost.h
// Minimal audio plugin host library using JUCE
//
// Thread Safety:
//   Functions fall into three thread-safety classes:
//
//   1. AUDIO-THREAD ONLY (no locks, no allocations after warmup):
//        mh_process, mh_process_midi, mh_process_midi_io,
//        mh_process_auto, mh_process_sidechain, mh_process_double
//      Call from exactly one thread (the audio callback). Concurrent calls
//      from multiple threads on the same MH_Plugin are undefined.
//
//   2. CONCURRENT WITH AUDIO (atomic / brief lock; safe to overlap mh_process):
//        mh_set_param, mh_get_param, mh_get_param_info,
//        mh_morph_capture, mh_morph_apply, mh_morph_lerp,
//        mh_morph_lerp_per_param, mh_morph,
//        mh_get_num_params, mh_get_info, mh_get_path,
//        mh_get_latency_samples, mh_get_tail_seconds,
//        mh_get_bypass, mh_set_bypass,
//        mh_set_transport, mh_param_to_text, mh_param_from_text,
//        mh_get_num_buses, mh_get_bus_info, mh_get_sidechain_channels,
//        mh_check_buses_layout, mh_set_track_properties,
//        mh_supports_double, mh_get_processing_precision,
//        mh_get_sample_rate, mh_get_num_programs, mh_get_program_name,
//        mh_get_program, mh_set_program (program-change is safe; it goes
//        through param notifications, not releaseResources),
//        mh_begin_param_gesture, mh_end_param_gesture,
//        mh_set_change_callback / mh_set_param_value_callback /
//        mh_set_param_gesture_callback,
//        mh_api_version, mh_api_version_string
//
//   3. NOT SAFE TO OVERLAP mh_process (calls releaseResources/prepareToPlay
//      or otherwise reconfigures the plugin's audio pipeline):
//        mh_set_state, mh_set_program_state, mh_get_state, mh_get_state_size,
//        mh_get_program_state, mh_get_program_state_size,
//        mh_set_sample_rate, mh_set_processing_precision,
//        mh_set_non_realtime, mh_reset
//      Stop the audio thread (or AudioDevice) before calling these. They
//      DO take the same internal lock that class 2 functions use, so
//      concurrent UI/control-thread access between class 2 and class 3 is
//      serialized -- but the audio thread does NOT acquire that lock and is
//      not protected.
//
//   Lifecycle:
//     - mh_close() is not safe to call while ANY other thread is using the
//       plugin. Callers must stop the audio device, drop callbacks, and
//       ensure no other thread holds the MH_Plugin* before closing.
//     - When using mh_audio_open / MH_AudioDevice, call mh_audio_stop() and
//       mh_audio_close() BEFORE mh_close() on the underlying plugin. The
//       AudioDevice keeps a raw MH_Plugin* pointer; closing the plugin
//       first leaves the device dangling.
//
// ABI Versioning:
// ABI Versioning:
//   The MH_API_VERSION_* macros below describe the ABI version the header
//   was generated for; mh_api_version() returns the version the linked
//   implementation was built against. Callers that ship as compiled
//   binaries against minihost should validate at startup:
//
//       if (mh_api_version() < MH_API_VERSION_NUMBER) {
//           // Linked minihost is older than the header expects.
//           return -1;
//       }
//
//   Major version bumps indicate incompatible changes (struct layout,
//   removed/renamed functions). Minor bumps are backward-compatible
//   additions (new functions, new fields appended to existing structs).
//   Patch bumps are non-API fixes.
//
//   Public structs are evolved by appending new fields at the end. Callers
//   should always zero-initialize structs they pass in (e.g. via
//   memset(&info, 0, sizeof(info))) so future field additions don't read
//   uninitialized memory if the caller is rebuilt against a newer header.
//
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// API version components. Bump per the policy described above.
// 2.8.0: added supervised scanning -- mh_plugin_cache_scan_supervised and
//   mh_plugin_scan_worker_main. Probing loads a plugin, and a plugin that
//   hangs or crashes takes the scanning process with it, so an in-process
//   scan of a large collection cannot be relied on to finish. The supervisor
//   probes each plugin in a disposable child process with a deadline and
//   records the outcome, so one bad plugin costs one entry instead of the
//   scan (additive; mh_plugin_cache_scan is unchanged).
// 2.7.0: added plugin discovery helpers -- mh_get_default_plugin_dir plus
//   mh_plugin_cache_path / _scan / _lookup / _match. Front-ends had no way to
//   ask where plugins live on this platform, and no name -> path index, so
//   every command took an absolute path typed out in full. The cache file and
//   its JSON schema are shared with the Python CLI's, so a scan from either
//   side serves the other (additive).
// 2.6.0: added mh_midi_file_load / mh_midi_file_free -- reading a standard
//   MIDI file into MH_MidiEvent form had no C entry point at all, so a C
//   consumer could drive a plugin with MIDI it built by hand but not with
//   MIDI from a file (additive).
// 2.5.0: MIDI now flows through a chain rather than stopping at its first
//   plugin. mh_chain_process_midi_io hands midi_in to the first plugin that
//   accepts MIDI and lets every plugin reporting produces_midi replace the
//   stream for the plugins behind it, so a MIDI effect can drive an
//   instrument further down; previously those events were reported to the
//   caller and dropped, and PluginChain([arpeggiator, synth]) rendered
//   silence. Consequently midi_out now carries what leaves the LAST plugin,
//   not the first. Also relaxes mh_bus_create / mh_bus_add_branch: a bus may
//   have zero input channels and a branch may read fewer than the bus carries
//   (never more), so instrument branches -- which expose no audio input bus --
//   can finally be layered, which is what mh_bus_process_midi_io exists for.
//   No symbols added or removed; no struct layout change.
// 2.4.0: MH_Info.num_input_ch now reports the MAIN input bus only, not the sum
//   of every input bus. A plugin opened with a sidechain previously reported
//   main+sidechain, so callers had to over-provision the main input buffer and
//   mh_process_sidechain wrote the sidechain past the channels the plugin
//   reads. Struct layout is unchanged; the value's meaning is not.
// 2.3.0: added mh_get_max_block_size -- lets callers (audio devices, chains,
//   graphs) validate their block size against a plugin's instead of failing
//   at process time (additive).
// 2.2.0: added parameter morphing -- mh_morph_capture / mh_morph_apply /
//   mh_morph_lerp / mh_morph_lerp_per_param / mh_morph (additive).
// 2.1.0: added mh_bus_process_midi_io -- collects and merges the MIDI
//   produced by each bus branch into one time-ordered stream (additive).
// 2.0.0: routing symbols renamed for the chain/bus/graph tier --
//   mh_graph_* (parallel bus) -> mh_bus_* / MH_PluginGraph -> MH_PluginBus,
//   and mh_graph_v2_* (DAG) -> mh_graph_* / MH_GraphV2 -> MH_PluginGraph.
#define MH_API_VERSION_MAJOR 2
#define MH_API_VERSION_MINOR 8
#define MH_API_VERSION_PATCH 0

// Single packed integer for compile-time comparison.
// Layout: MAJOR * 10000 + MINOR * 100 + PATCH (so 1.2.3 == 10203).
#define MH_API_VERSION_NUMBER \
    ((MH_API_VERSION_MAJOR) * 10000 + (MH_API_VERSION_MINOR) * 100 + (MH_API_VERSION_PATCH))

#define MH_API_VERSION_STRINGIFY_(x) #x
#define MH_API_VERSION_STRINGIFY(x) MH_API_VERSION_STRINGIFY_(x)
#define MH_API_VERSION_STRING                          \
    MH_API_VERSION_STRINGIFY(MH_API_VERSION_MAJOR) "." \
    MH_API_VERSION_STRINGIFY(MH_API_VERSION_MINOR) "." \
    MH_API_VERSION_STRINGIFY(MH_API_VERSION_PATCH)

// Returns the API version the linked implementation was compiled against.
// Compare with MH_API_VERSION_NUMBER (header-side) to detect mismatches.
int mh_api_version(void);

// Bring up the dedicated JUCE plugin thread. Idempotent. Called automatically
// on the first plugin load (not at import), so a process that never loads a
// plugin does no JUCE initialization -- important on headless systems.
// Creates the JUCE MessageManager on a background thread (no GUI/display init)
// and marshals all plugin construction, destruction, and thread-affine control
// operations onto it, which makes plugins safe to build on one thread and
// use/close on another (e.g. open_async). Enabled by default; set
// MINIHOST_MESSAGE_THREAD=0 to disable (operations then run inline on the
// caller's thread, and cross-thread plugin use is unsafe again). Call it
// explicitly only to force the thread up early. See minihost.cpp.
void mh_message_thread_init(void);

// Stop the dedicated JUCE plugin thread and tear down the MessageManager on
// that thread. Idempotent; safe no-op if the thread was never started. Must be
// called at process exit (the Python bindings register it with atexit):
// leaving the MessageManager alive on a background thread into JUCE's static
// teardown deadlocks process exit on Linux.
void mh_message_thread_shutdown(void);

// Returns the implementation's API version as a "MAJOR.MINOR.PATCH" string.
// Storage is owned by the library; do not free.
const char* mh_api_version_string(void);

typedef struct MH_Plugin MH_Plugin;

// Plugin metadata (available without full instantiation via mh_probe)
typedef struct MH_PluginDesc {
    char name[256];
    char vendor[256];
    char version[64];
    char format[16];            // "VST3", "AU", or "LV2"
    char unique_id[64];         // for state compatibility checking
    char path[1024];            // full path to plugin file (populated by mh_scan_directory)
    int accepts_midi;           // best-effort heuristic (instruments=1, others=0); mh_get_info() is authoritative
    int produces_midi;          // not derivable at probe time; always 0 -- call mh_get_info() to query
    int num_inputs;             // default input channel count
    int num_outputs;            // default output channel count
} MH_PluginDesc;

typedef struct MH_Info {
    int num_params;
    // Channels of the MAIN input bus only -- the width of the `inputs` array
    // the process* functions expect. A sidechain (or other aux) input bus is
    // NOT counted here; query it with mh_get_sidechain_channels, and feed it
    // through mh_process_sidechain. Before ABI 2.4.0 this reported the sum of
    // every input bus, which made callers over-provision the main buffer.
    int num_input_ch;
    int num_output_ch;
    int latency_samples;
    int accepts_midi;    // 1 if plugin accepts MIDI input
    int produces_midi;   // 1 if plugin produces MIDI output
    int is_midi_effect;  // 1 if pure MIDI effect (no audio)
    int supports_mpe;    // 1 if supports MIDI Polyphonic Expression
} MH_Info;

typedef struct MH_MidiEvent {
    int sample_offset;           // sample position within the block (0 to nframes-1)
    unsigned char status;        // MIDI status byte (e.g., 0x90 = note on, 0x80 = note off)
    unsigned char data1;         // first data byte (e.g., note number)
    unsigned char data2;         // second data byte (e.g., velocity)
} MH_MidiEvent;

#define MH_PARAM_NAME_LEN 128

// Parameter category constants (matches JUCE AudioProcessorParameter::Category)
#define MH_PARAM_CATEGORY_GENERIC          0
#define MH_PARAM_CATEGORY_INPUT_GAIN       0x10000
#define MH_PARAM_CATEGORY_OUTPUT_GAIN      0x10001
#define MH_PARAM_CATEGORY_INPUT_METER      0x20000
#define MH_PARAM_CATEGORY_OUTPUT_METER     0x20001
#define MH_PARAM_CATEGORY_COMPRESSOR_METER 0x20002
#define MH_PARAM_CATEGORY_EXPANDER_METER   0x20003
#define MH_PARAM_CATEGORY_ANALYSIS_METER   0x20004
#define MH_PARAM_CATEGORY_OTHER_METER      0x20005

typedef struct MH_ParamInfo {
    char name[MH_PARAM_NAME_LEN];          // parameter name
    char id[MH_PARAM_NAME_LEN];            // stable unique parameter ID string
    char label[MH_PARAM_NAME_LEN];         // unit label (e.g., "dB", "Hz", "%")
    char current_value_str[MH_PARAM_NAME_LEN]; // current value as display string
    float min_value;                       // minimum normalized value (usually 0.0)
    float max_value;                       // maximum normalized value (usually 1.0)
    float default_value;                   // default normalized value
    int num_steps;                         // number of discrete steps (0 = continuous)
    int is_automatable;                    // 1 if parameter can be automated
    int is_boolean;                        // 1 if parameter is a toggle/switch
    int category;                          // parameter category (MH_PARAM_CATEGORY_*)
} MH_ParamInfo;

typedef struct MH_TransportInfo {
    double bpm;                            // tempo in beats per minute
    int time_sig_numerator;                // time signature numerator (e.g., 4 for 4/4)
    int time_sig_denominator;              // time signature denominator (e.g., 4 for 4/4)
    long long position_samples;            // playhead position in samples
    double position_beats;                 // playhead position in quarter notes
    int is_playing;                        // 1 if transport is playing
    int is_recording;                      // 1 if transport is recording
    int is_looping;                        // 1 if loop is enabled
    long long loop_start_samples;          // loop start in samples
    long long loop_end_samples;            // loop end in samples
} MH_TransportInfo;

// Sample-accurate parameter automation
typedef struct MH_ParamChange {
    int sample_offset;                     // sample position within the block (0 to nframes-1)
    int param_index;                       // parameter index
    float value;                           // normalized value (0.0 to 1.0)
} MH_ParamChange;

// Bus information for understanding plugin I/O topology
typedef struct MH_BusInfo {
    char name[64];                         // bus name (e.g., "Main", "Sidechain")
    int num_channels;                      // number of channels in this bus
    int is_main;                           // 1 if main bus, 0 if aux/sidechain
    int is_enabled;                        // 1 if bus is currently enabled
} MH_BusInfo;

// plugin_path: .vst3 bundle on macOS, .vst3 folder on Win/Linux, .component for AU (mac)
// returns NULL on failure
MH_Plugin* mh_open(const char* plugin_path,
                   double sample_rate,
                   int max_block_size,
                   int requested_in_ch,
                   int requested_out_ch,
                   char* err_buf,
                   size_t err_buf_size);

void mh_close(MH_Plugin* p);

// Returns the plugin file path passed to mh_open / mh_open_ex.
// Owned by the MH_Plugin; valid until mh_close. Returns "" for NULL.
const char* mh_get_path(const MH_Plugin* p);

// Escape hatch for GUI hosts: returns the underlying juce::AudioProcessor*
// as an opaque pointer. Typed as void* so this header stays valid C and
// translation units that don't link JUCE are unaffected. The desktop app
// (which does link JUCE) casts the result back to juce::AudioProcessor*
// to obtain an AudioProcessorEditor and hand it to a DocumentWindow.
//
// Lifetime: pointer is owned by the MH_Plugin; do not delete. Valid
// until mh_close. Returns NULL for NULL input.
//
// Threading: see the file-level threading notes. The caller is
// responsible for not using the returned pointer concurrently with
// mh_close. Calls that go through the JUCE AudioProcessor API
// (parameter writes, editor creation, etc.) do not coordinate with
// libminihost's internal mutex -- editor edits reach the audio thread
// through JUCE's own parameter machinery, which libminihost already
// listens to via mh_register_param_listener.
void* mh_get_juce_processor(MH_Plugin* p);

int mh_get_info(MH_Plugin* p, MH_Info* out_info);

// Non-interleaved buffers: inputs[ch][nframes], outputs[ch][nframes]
// If in/out pointers are NULL, the host will supply silence / discard output.
int mh_process(MH_Plugin* p,
               const float* const* inputs,
               float* const* outputs,
               int nframes);

// Process with MIDI input
// midi_events: array of MIDI events to send to the plugin (can be NULL if num_midi_events is 0)
// num_midi_events: number of events in the array
int mh_process_midi(MH_Plugin* p,
                    const float* const* inputs,
                    float* const* outputs,
                    int nframes,
                    const MH_MidiEvent* midi_events,
                    int num_midi_events);

// Process with MIDI input and output
// midi_in/num_midi_in: input MIDI events (can be NULL/0)
// midi_out: buffer to receive output MIDI events (can be NULL to ignore)
// midi_out_capacity: size of midi_out buffer
// num_midi_out: receives actual number of output events (can be NULL)
// Returns 1 on success, 0 on failure
int mh_process_midi_io(MH_Plugin* p,
                       const float* const* inputs,
                       float* const* outputs,
                       int nframes,
                       const MH_MidiEvent* midi_in,
                       int num_midi_in,
                       MH_MidiEvent* midi_out,
                       int midi_out_capacity,
                       int* num_midi_out);

// Params by index (JUCE parameter ordering)
int   mh_get_num_params(MH_Plugin* p);
float mh_get_param(MH_Plugin* p, int index);
int   mh_set_param(MH_Plugin* p, int index, float normalized_0_1);

// Get parameter metadata (returns 1 on success, 0 on failure)
int   mh_get_param_info(MH_Plugin* p, int index, MH_ParamInfo* out_info);

// Parameter morphing (A/B interpolation over normalized parameter values)
//
// A "snapshot" is an array of normalized parameter values (each in [0, 1]),
// one entry per plugin parameter. Morphing linearly interpolates two snapshots
// so a whole patch can be blended or swept along a single control. It operates
// on the normalized per-parameter values, NOT on opaque state blobs
// (mh_get_state / mh_set_state), which are not interpolatable.
//
// Only continuous parameters interpolate smoothly. Stepped / boolean / enum
// parameters are quantized by the plugin, so intermediate normalized values
// may jump rather than glide.

// Capture the current normalized value of every parameter into out_values.
// capacity is the number of floats available in out_values; it must be at
// least mh_get_num_params(p). Returns the number of values written (the
// parameter count) on success, or -1 on error (NULL args or capacity too
// small).
int mh_morph_capture(MH_Plugin* p, float* out_values, int capacity);

// Apply a snapshot: set every parameter from values[i], clamped to [0, 1].
// count must equal mh_get_num_params(p). Returns 1 on success, 0 on failure
// (NULL args or count mismatch).
int mh_morph_apply(MH_Plugin* p, const float* values, int count);

// Linearly interpolate two snapshots with one blend amount t:
//   out[i] = clamp01(a[i] + (b[i] - a[i]) * t)
// t=0 yields a, t=1 yields b; results are clamped so an extrapolated t (outside
// [0, 1]) still yields valid normalized values. Pure array math, no plugin
// access; out may alias a or b. Returns 1 on success, 0 on failure (NULL args
// or negative count).
int mh_morph_lerp(const float* a, const float* b, float* out, int count, float t);

// Per-parameter interpolation: out[i] = clamp01(a[i] + (b[i]-a[i]) * t[i]).
// t is an array of count blend amounts. Returns 1 on success, 0 on failure.
int mh_morph_lerp_per_param(const float* a, const float* b, float* out,
                            int count, const float* t);

// Convenience: interpolate snapshots a and b at scalar t and apply the result
// to the plugin. count must equal mh_get_num_params(p). Returns 1 on success,
// 0 on failure.
int mh_morph(MH_Plugin* p, const float* a, const float* b, int count, float t);

// State save/load (for presets and session recall)
// Returns size in bytes needed to store state, or 0 on error
int mh_get_state_size(MH_Plugin* p);

// Copy state into buffer. Returns 1 on success, 0 on failure.
// buffer must be at least mh_get_state_size() bytes
int mh_get_state(MH_Plugin* p, void* buffer, int buffer_size);

// Restore state from buffer. Returns 1 on success, 0 on failure.
//
// JUCE's setStateInformation is void, so a plugin that rejects a blob does so
// silently -- juce::VST3PluginInstance, for example, ignores anything that is
// not in its own container format. This function therefore verifies that the
// call had an observable effect (parameter values, else the serialized state)
// and returns 0 when the plugin demonstrably ignored `data`. It never reports
// failure for a state that was applied, but a plugin whose serialized state is
// non-deterministic may still yield a false success.
//
// Cost: one extra getStateInformation() before the call, and a second one only
// when parameter values alone cannot distinguish "applied" from "ignored".
int mh_set_state(MH_Plugin* p, const void* data, int data_size);

// Set transport info (call before mh_process to update tempo/position for plugins)
// Pass NULL to clear transport info (plugins will see "no transport")
int mh_set_transport(MH_Plugin* p, const MH_TransportInfo* transport);

// Get plugin tail length in seconds (for reverbs, delays - time needed after input stops)
// Returns 0.0 if plugin has no tail or on error
double mh_get_tail_seconds(MH_Plugin* p);

// Bypass control
// When bypassed, plugin passes audio through unchanged
int mh_get_bypass(MH_Plugin* p);                    // Returns 1 if bypassed, 0 if not
int mh_set_bypass(MH_Plugin* p, int bypass);        // Set bypass state (1 = bypassed)

// Latency compensation
// Returns plugin latency in samples (use to align with other signals)
// Note: Latency may change after parameter changes - re-query if needed
int mh_get_latency_samples(MH_Plugin* p);

// Process with sample-accurate parameter automation
// param_changes: array of parameter changes sorted by sample_offset
// num_param_changes: number of parameter changes
// Splits processing at change points for sample-accurate automation
// Also supports MIDI I/O (pass NULL/0 to ignore)
int mh_process_auto(MH_Plugin* p,
                    const float* const* inputs,
                    float* const* outputs,
                    int nframes,
                    const MH_MidiEvent* midi_in,
                    int num_midi_in,
                    MH_MidiEvent* midi_out,
                    int midi_out_capacity,
                    int* num_midi_out,
                    const MH_ParamChange* param_changes,
                    int num_param_changes);

// Reset plugin internal state (clears delay lines, reverb tails, filter states)
// Call between unrelated audio segments to avoid artifacts
// Returns 1 on success, 0 on failure
int mh_reset(MH_Plugin* p);

// Set non-realtime mode for offline/batch processing
// When non_realtime=1, plugins may use higher-quality algorithms
// Returns 1 on success, 0 on failure
int mh_set_non_realtime(MH_Plugin* p, int non_realtime);

// Get plugin metadata without full instantiation
// Useful for validation, inventory, state file compatibility checks
// Returns 1 on success, 0 on failure (with error message in err_buf)
int mh_probe(const char* plugin_path,
             MH_PluginDesc* out_desc,
             char* err_buf,
             size_t err_buf_size);

// Parameter value text conversion
// Convert normalized value (0-1) to display string (e.g., "2500 Hz", "-6.0 dB")
// Returns 1 on success, 0 on failure
int mh_param_to_text(MH_Plugin* p, int index, float value, char* buf, size_t buf_size);

// Convert display string to normalized value (0-1)
// Returns 1 on success, 0 on failure (e.g., invalid text format)
// Note: Not all plugins implement text-to-value conversion
int mh_param_from_text(MH_Plugin* p, int index, const char* text, float* out_value);

// Factory preset (program) access
// Returns number of factory presets, or 0 if none
int mh_get_num_programs(MH_Plugin* p);

// Get name of factory preset at index
// Returns 1 on success, 0 on failure
int mh_get_program_name(MH_Plugin* p, int index, char* buf, size_t buf_size);

// Get currently selected program index
// Returns -1 if no program selected or on error
int mh_get_program(MH_Plugin* p);

// Select a factory preset by index
// Returns 1 on success, 0 on failure
int mh_set_program(MH_Plugin* p, int index);

// Bus layout query
// Get number of input or output buses
// is_input: 1 for input buses, 0 for output buses
int mh_get_num_buses(MH_Plugin* p, int is_input);

// Get information about a specific bus
// Returns 1 on success, 0 on failure
int mh_get_bus_info(MH_Plugin* p, int is_input, int bus_index, MH_BusInfo* out_info);

// Extended open with sidechain support
// sidechain_in_ch: number of sidechain input channels (0 to disable)
// returns NULL on failure
MH_Plugin* mh_open_ex(const char* plugin_path,
                      double sample_rate,
                      int max_block_size,
                      int main_in_ch,
                      int main_out_ch,
                      int sidechain_in_ch,
                      char* err_buf,
                      size_t err_buf_size);

// Open a plugin from a serialized juce::PluginDescription (its createXml()
// form, as a NUL-terminated UTF-8 string). Unlike mh_open, this needs no file
// path, so it is the way to load AudioUnits (identified by an AU id, not a
// path). requested_in_ch / requested_out_ch mirror mh_open (0 = plugin default,
// no sidechain). Returns NULL on failure (err_buf gets the reason).
MH_Plugin* mh_open_desc(const char* pd_xml,
                        double sample_rate,
                        int max_block_size,
                        int requested_in_ch,
                        int requested_out_ch,
                        char* err_buf,
                        size_t err_buf_size);

// Process with sidechain input
// main_in: main input channels [num_input_ch][nframes] (MH_Info.num_input_ch,
//          i.e. the main bus only -- do NOT include sidechain channels here)
// main_out: main output channels [num_output_ch][nframes]
// sidechain_in: sidechain channels [mh_get_sidechain_channels()][nframes]
//               (can be NULL, in which case the sidechain is fed silence)
// Returns 1 on success, 0 on failure
int mh_process_sidechain(MH_Plugin* p,
                         const float* const* main_in,
                         float* const* main_out,
                         const float* const* sidechain_in,
                         int nframes);

// Get number of sidechain input channels configured for this plugin
// Returns 0 if no sidechain or plugin opened with mh_open() instead of mh_open_ex()
int mh_get_sidechain_channels(MH_Plugin* p);

// Check if a bus layout is supported before attempting to apply it
// input_channels/output_channels: array of channel counts, one per bus
// Returns 1 if supported, 0 if not supported or on error
int mh_check_buses_layout(MH_Plugin* p,
                          const int* input_channels, int num_input_buses,
                          const int* output_channels, int num_output_buses);

// Change notifications
// Flags for MH_ChangeCallback
#define MH_CHANGE_LATENCY         0x01
#define MH_CHANGE_PARAM_INFO      0x02
#define MH_CHANGE_PROGRAM         0x04
#define MH_CHANGE_NON_PARAM_STATE 0x08

// Callback: processor-level changes (latency, param info, program, non-param state)
// flags: bitmask of MH_CHANGE_* values
typedef void (*MH_ChangeCallback)(MH_Plugin* plugin, int flags, void* user_data);

// Callback: parameter value changed (plugin-initiated, e.g. preset load, internal modulation)
typedef void (*MH_ParamValueCallback)(MH_Plugin* plugin, int param_index, float new_value, void* user_data);

// Callback: parameter gesture began (gesture_starting=1) or ended (gesture_starting=0) from plugin UI
typedef void (*MH_ParamGestureCallback)(MH_Plugin* plugin, int param_index, int gesture_starting, void* user_data);

// Register notification callbacks (pass NULL callback to clear)
// Returns 1 on success, 0 on failure
int mh_set_change_callback(MH_Plugin* p, MH_ChangeCallback cb, void* user_data);
int mh_set_param_value_callback(MH_Plugin* p, MH_ParamValueCallback cb, void* user_data);
int mh_set_param_gesture_callback(MH_Plugin* p, MH_ParamGestureCallback cb, void* user_data);

// Signal start of a parameter change gesture (call before a sequence of mh_set_param calls)
int mh_begin_param_gesture(MH_Plugin* p, int index);

// Signal end of a parameter change gesture
int mh_end_param_gesture(MH_Plugin* p, int index);

// Current program state save/load (lighter-weight per-program state)
// Returns size in bytes, or 0 on error
int mh_get_program_state_size(MH_Plugin* p);

// Copy current program state into buffer. Returns 1 on success, 0 on failure.
int mh_get_program_state(MH_Plugin* p, void* buffer, int buffer_size);

// Restore current program state from buffer. Returns 1 on success, 0 on failure.
// Rejection is detected the same way as mh_set_state -- see the note there.
int mh_set_program_state(MH_Plugin* p, const void* data, int data_size);

// Change sample rate without reloading the plugin
// Preserves parameter state across the change
// Returns 1 on success, 0 on failure
int mh_set_sample_rate(MH_Plugin* p, double new_sample_rate);

// Get current sample rate
double mh_get_sample_rate(MH_Plugin* p);

// Largest block (in frames) this plugin was prepared for, i.e. the
// max_block_size passed to mh_open / mh_open_ex / mh_open_desc. Every
// mh_process* call rejects nframes above it. Callers that own the block size
// (audio devices, chains, graphs) should validate against this at setup time
// rather than discovering the mismatch as a per-block process failure.
// Returns 0 for NULL.
int mh_get_max_block_size(MH_Plugin* p);

// Plugin directory scanning callback
// Called for each valid plugin found in the directory
// desc: plugin metadata (includes path field with full path to plugin)
// user_data: user-provided context pointer
typedef void (*MH_ScanCallback)(const MH_PluginDesc* desc, void* user_data);

// Scan a directory for plugins
// Recursively searches for .vst3 and .component (AU) files
// Calls callback for each valid plugin found (invalid plugins are silently skipped)
// Returns number of plugins found, or -1 on error (e.g., directory doesn't exist)
int mh_scan_directory(const char* directory_path,
                      MH_ScanCallback callback,
                      void* user_data);

// Double precision processing
// Process audio using 64-bit floating point samples
// Returns 1 on success, 0 on failure
// Note: Internally converts to/from float if plugin doesn't support double precision
int mh_process_double(MH_Plugin* p,
                      const double* const* inputs,
                      double* const* outputs,
                      int nframes);

// Check if plugin supports double precision processing natively
// Returns 1 if plugin supports double precision, 0 otherwise
int mh_supports_double(MH_Plugin* p);

// Processing precision selection
#define MH_PRECISION_SINGLE 0
#define MH_PRECISION_DOUBLE 1

// Get current processing precision (MH_PRECISION_SINGLE or MH_PRECISION_DOUBLE)
int mh_get_processing_precision(MH_Plugin* p);

// Set processing precision. Re-prepares the plugin with the new precision.
// Only MH_PRECISION_DOUBLE is valid if mh_supports_double() returns 1.
// Returns 1 on success, 0 on failure (e.g., plugin doesn't support double)
int mh_set_processing_precision(MH_Plugin* p, int precision);

// Track properties
// Pass track name and/or color metadata to the plugin
// name: track name string (NULL to clear)
// has_colour: 1 to set colour, 0 to clear
// colour_argb: track colour as 0xAARRGGBB (only used if has_colour=1)
// Returns 1 on success, 0 on failure
int mh_set_track_properties(MH_Plugin* p, const char* name,
                            int has_colour, unsigned int colour_argb);

// Async plugin loading callback
// Called when plugin loading completes (on success or failure)
// plugin: the loaded plugin (NULL on failure)
// error: error message (NULL on success, non-NULL on failure)
// user_data: user-provided context pointer
typedef void (*MH_LoadCallback)(MH_Plugin* plugin, const char* error, void* user_data);

// Asynchronously load a plugin in a background thread
// Callback is invoked from the background thread when loading completes
// Returns 1 if async load started successfully, 0 if failed to start
// Note: On success, caller must NOT use the plugin until callback is invoked
int mh_open_async(const char* plugin_path,
                  double sample_rate,
                  int max_block_size,
                  int requested_in_ch,
                  int requested_out_ch,
                  MH_LoadCallback callback,
                  void* user_data);

// ---------------------------------------------------------------------------
// Session: shared plugin-format-manager state across loads/probes/scans
// ---------------------------------------------------------------------------
//
// A session holds one JUCE AudioPluginFormatManager and reuses it
// across many mh_session_* calls. The non-session entry points
// (mh_open, mh_probe, mh_scan_directory) each construct and register
// formats internally on every call; for multi-plugin and
// directory-scanning workflows this is wasteful.
//
// All non-session entry points continue to work unchanged.
//
// Sessions are thread-safe for concurrent use of mh_session_open /
// mh_session_probe / mh_session_scan_directory from multiple threads
// against the same session (internal lock).
typedef struct MH_Session MH_Session;

// Create a session. Returns NULL on failure.
MH_Session* mh_session_create(char* err_buf, size_t err_buf_size);

// Close the session and release its format manager.
// Does NOT close plugins previously created with mh_session_open --
// they remain valid (the plugin owns its own state once loaded).
void mh_session_close(MH_Session* session);

// Load a plugin using the session's format manager. Same semantics as
// mh_open_ex but reuses the session's pre-initialized formats.
MH_Plugin* mh_session_open(MH_Session* session,
                            const char* plugin_path,
                            double sample_rate,
                            int max_block_size,
                            int main_in_ch,
                            int main_out_ch,
                            int sidechain_in_ch,
                            char* err_buf,
                            size_t err_buf_size);

// Load a plugin from a serialized PluginDescription (see mh_open_desc) using
// the session's format manager.
MH_Plugin* mh_session_open_desc(MH_Session* session,
                                const char* pd_xml,
                                double sample_rate,
                                int max_block_size,
                                int requested_in_ch,
                                int requested_out_ch,
                                char* err_buf,
                                size_t err_buf_size);

// Probe a plugin file using the session's format manager.
int mh_session_probe(MH_Session* session,
                      const char* plugin_path,
                      MH_PluginDesc* desc,
                      char* err_buf,
                      size_t err_buf_size);

// Scan a directory for plugins using the session's format manager.
int mh_session_scan_directory(MH_Session* session,
                               const char* directory_path,
                               MH_ScanCallback callback,
                               void* user_data);

// ---------------------------------------------------------------------------
// Plugin locations and the shared scan cache
// ---------------------------------------------------------------------------
//
// Two conveniences for front-ends: where plugins live on this platform,
// and a name -> path index so a user can say "dexed" instead of pasting
// an absolute path.
//
// The cache file and its JSON schema are shared with the Python CLI
// (minihost.plugincache), so a scan from either side is visible to the
// other. Location, honouring MINIHOST_CACHE_DIR:
//   macOS:   ~/Library/Caches/minihost/plugins.json
//   Windows: %LOCALAPPDATA%/minihost/Cache/plugins.json
//   other:   $XDG_CACHE_HOME/minihost/plugins.json (or ~/.cache/...)

// Canonical plugin directories for this platform, by index. Returns 1 and
// fills buf while index is in range, 0 once it is past the end -- so a
// caller loops until it returns 0. Directories that do not exist on this
// machine are skipped, so the enumeration is what is actually installed.
int mh_get_default_plugin_dir(int index, char* buf, size_t buf_size);

// Absolute path of the shared cache file. Returns 1 on success.
int mh_plugin_cache_path(char* buf, size_t buf_size);

// Scan for plugins and write the cache.
//
// dirs == NULL scans the canonical directories above; otherwise the given
// directories are scanned. Entries whose file is unchanged (mtime + size)
// are reused rather than re-probed, unless refresh is 1. A plugin that
// fails to probe is remembered as an error so it is not retried on every
// scan.
//
// Returns the number of cached plugins, or -1 on failure.
int mh_plugin_cache_scan(const char* const* dirs, int num_dirs, int refresh,
                         MH_ScanCallback progress, void* user_data,
                         char* err_buf, size_t err_buf_size);

// Supervised scan: same result as mh_plugin_cache_scan, but each plugin is
// probed in a child process that is discarded afterwards.
//
// This is the difference between a scan that finishes and one that might.
// Probing means instantiating, and an installed collection can be relied on
// to contain a plugin that spins forever or corrupts its heap on load; on
// the development machine 5 of ~350 did. In process, the first of those ends
// the scan. Here it costs one entry: the child is killed at the deadline or
// dies on its own, the outcome is recorded, and the scan continues.
//
// worker_argv is the command to spawn, WITHOUT the plugin path -- the
// library appends MH_SCAN_WORKER_FLAG and the path. Pass NULL to use this
// process's own executable, which is right for a program that calls
// mh_plugin_scan_worker_main from main() (see below). An embedder whose
// executable is not ours -- a DAW, or Python -- passes its own worker
// instead, e.g. {"/usr/bin/python3", "-m", "minihost._scan_worker"}.
// MINIHOST_SCAN_WORKER overrides both, as a command line split on spaces.
//
// timeout_ms bounds one probe; <= 0 uses a default of 60000, and
// MINIHOST_SCAN_TIMEOUT_MS overrides both. A plugin that
// exceeds it is recorded with status "timeout"; one whose child dies without
// answering is recorded as "crash". Both are remembered like any other entry,
// so the next scan skips them rather than paying for them again.
//
// Returns the number of cached plugins, or -1 on failure (which means the
// scan could not be set up at all -- a bad worker command, say -- never a
// misbehaving plugin).
int mh_plugin_cache_scan_supervised(const char* const* dirs, int num_dirs,
                                    int refresh,
                                    const char* const* worker_argv,
                                    int worker_argc, int timeout_ms,
                                    MH_ScanCallback progress, void* user_data,
                                    char* err_buf, size_t err_buf_size);

// The argument that marks a process as a scan worker. A front-end that wants
// to be its own worker calls mh_plugin_scan_worker_main first thing in main()
// and exits immediately if it returns 1:
//
//   int main(int argc, char** argv) {
//       if (mh_plugin_scan_worker_main(argc, argv)) return 0;
//       ...
//   }
//
// In the worker case it has already probed the one plugin named on the
// command line and written the result to stdout, so there is nothing for the
// caller to do but exit. Returns 0 in the ordinary case, leaving argv alone.
#define MH_SCAN_WORKER_FLAG "--mh-probe-one"

int mh_plugin_scan_worker_main(int argc, char** argv);

// The worker protocol, for a worker that is not one of our binaries. Write
// one JSON object between these two markers on stdout, then exit:
//
//   {"ok": true, "name": ..., "vendor": ..., "version": ..., "format": ...,
//    "unique_id": ..., "num_inputs": N, "num_outputs": N,
//    "accepts_midi": bool, "produces_midi": bool}
//   {"ok": false, "error": "..."}
//
// The markers exist because plugins write to stdout as they load -- some
// print pages of it -- so the answer has to be findable inside that noise
// rather than being assumed to be all of it. Anything outside the markers is
// ignored, and a worker that exits without writing them is treated as having
// died on the plugin.
#define MH_SCAN_WORKER_BEGIN "<<<MH_PROBE>>>"
#define MH_SCAN_WORKER_END   "<<<MH_PROBE_END>>>"


// Resolve a plugin name to a path using the cache. Matching ignores case.
//
// By default only a whole-name match counts. Substring matching is
// opt-in (allow_substring = 1) because it is rarely decisive on a real
// collection: on a machine with 343 plugins, "reverb" matches 5,
// "delay" 9 and "filter" 31, so the convenience mostly buys an
// ambiguity error.
//
// The same plugin is often installed in two formats under one name --
// 16 of those 343 were -- which would make even an exact name
// ambiguous. So when every match is the same name differing only by
// format, one is chosen rather than refused: `format` if given ("vst3",
// "au"/"audiounit"/"component"), else VST3 in preference to AudioUnit.
// A non-NULL `format` also filters the candidate set, so a name can be
// pinned to one format explicitly.
//
// Returns 1 on a unique match (path written to out_path), 0 if nothing
// matched, or the number of matches (>= 2) when genuinely ambiguous --
// in which case out_path holds the first and mh_plugin_cache_match
// lists the rest.
int mh_plugin_cache_lookup(const char* name, const char* format,
                           int allow_substring,
                           char* out_path, size_t out_size);

// The index-th match for `name` under the same rules as
// mh_plugin_cache_lookup, for reporting an ambiguity. Returns 1 while the
// index is in range, 0 once past the end.
int mh_plugin_cache_match(const char* name, const char* format,
                          int allow_substring, int index,
                          char* out_path, size_t out_size);

// ---------------------------------------------------------------------------
// Standard MIDI file reading
// ---------------------------------------------------------------------------
//
// Flattens a MIDI file to one time-ordered array of MH_MidiEvent with
// sample_offset measured in samples from the start of the file at
// `sample_rate` -- the form the mh_process* entry points consume, so a
// caller can slice it per block and feed an instrument. Tracks are
// merged and the file's tempo map is applied; meta events are dropped,
// having no MH_MidiEvent representation.
//
// Note that sample_offset here is an absolute position in the render,
// not the within-block offset the process functions expect. Rebase it
// per block before handing events to a plugin.
//
// On success returns 1 and sets *out_events (free with
// mh_midi_file_free) and *out_count. Returns 0 on failure and fills
// err_buf. An empty file succeeds with *out_count == 0 and
// *out_events == NULL.
int mh_midi_file_load(const char* path,
                      double sample_rate,
                      MH_MidiEvent** out_events,
                      int* out_count,
                      double* out_duration_seconds,
                      char* err_buf,
                      size_t err_buf_size);

// Release an event array from mh_midi_file_load. NULL is a no-op.
void mh_midi_file_free(MH_MidiEvent* events);

#ifdef __cplusplus
}
#endif

