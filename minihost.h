// minihost.h
// Minimal audio plugin host library using JUCE
//
// Thread Safety:
//   - mh_process, mh_process_midi, mh_process_midi_io: Call from audio thread only.
//     These functions do NOT lock to avoid blocking the realtime audio thread.
//   - All other functions are thread-safe and can be called from any thread.
//     They use internal locking to protect plugin state.
//   - Do not call mh_close while another thread is using the plugin.
//
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MH_Plugin MH_Plugin;

typedef struct MH_Info {
    int num_params;
    int num_input_ch;
    int num_output_ch;
    int latency_samples;
} MH_Info;

typedef struct MH_MidiEvent {
    int sample_offset;           // sample position within the block (0 to nframes-1)
    unsigned char status;        // MIDI status byte (e.g., 0x90 = note on, 0x80 = note off)
    unsigned char data1;         // first data byte (e.g., note number)
    unsigned char data2;         // second data byte (e.g., velocity)
} MH_MidiEvent;

#define MH_PARAM_NAME_LEN 128

typedef struct MH_ParamInfo {
    char name[MH_PARAM_NAME_LEN];          // parameter name
    char label[MH_PARAM_NAME_LEN];         // unit label (e.g., "dB", "Hz", "%")
    char current_value_str[MH_PARAM_NAME_LEN]; // current value as display string
    float min_value;                       // minimum normalized value (usually 0.0)
    float max_value;                       // maximum normalized value (usually 1.0)
    float default_value;                   // default normalized value
    int num_steps;                         // number of discrete steps (0 = continuous)
    int is_automatable;                    // 1 if parameter can be automated
    int is_boolean;                        // 1 if parameter is a toggle/switch
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

// State save/load (for presets and session recall)
// Returns size in bytes needed to store state, or 0 on error
int mh_get_state_size(MH_Plugin* p);

// Copy state into buffer. Returns 1 on success, 0 on failure.
// buffer must be at least mh_get_state_size() bytes
int mh_get_state(MH_Plugin* p, void* buffer, int buffer_size);

// Restore state from buffer. Returns 1 on success, 0 on failure.
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

#ifdef __cplusplus
}
#endif

