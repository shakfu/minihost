// minihost_chain.h
// Plugin chaining support for minihost
//
// Thread Safety:
//   - mh_chain_process, mh_chain_process_midi_io: Call from audio thread only.
//     These functions do NOT lock to avoid blocking the realtime audio thread.
//   - All other functions are thread-safe and can be called from any thread.
//
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct MH_Plugin MH_Plugin;
typedef struct MH_MidiEvent MH_MidiEvent;
typedef struct MH_PluginChain MH_PluginChain;

// Sample-accurate parameter automation for plugin chains
typedef struct MH_ChainParamChange {
    int sample_offset;   // sample position within block (0 to nframes-1)
    int plugin_index;    // which plugin in the chain (0-based)
    int param_index;     // parameter index on that plugin
    float value;         // normalized value (0.0 to 1.0)
} MH_ChainParamChange;

// Create a plugin chain from an array of plugins.
// All plugins must have the same sample rate.
// The chain takes ownership references to the plugins - they must remain valid
// while the chain is in use, but will not be closed when the chain is closed.
//
// plugins: array of plugin pointers (first plugin receives MIDI, output flows sequentially)
// num_plugins: number of plugins in the array (must be > 0)
// err_buf: buffer to receive error message on failure
// err_buf_size: size of error buffer
//
// Returns NULL on failure (e.g., empty array, sample rate mismatch)
MH_PluginChain* mh_chain_create(MH_Plugin** plugins, int num_plugins,
                                 char* err_buf, size_t err_buf_size);

// Close the chain and free resources.
// Does NOT close the individual plugins - caller is responsible for those.
void mh_chain_close(MH_PluginChain* chain);

// Process audio through the chain (no MIDI).
// Input goes to the first plugin, output comes from the last plugin.
// Audio flows sequentially through all plugins.
//
// inputs: input audio buffers [channel][nframes] (can be NULL for silence)
// outputs: output audio buffers [channel][nframes] (can be NULL to discard)
// nframes: number of frames to process
//
// Returns 1 on success, 0 on failure
int mh_chain_process(MH_PluginChain* chain,
                     const float* const* inputs,
                     float* const* outputs,
                     int nframes);

// Process audio through the chain with MIDI I/O.
// MIDI is sent to the first plugin only (typical synth -> effects pattern).
// Audio flows sequentially through all plugins.
//
// inputs: input audio buffers (can be NULL)
// outputs: output audio buffers (can be NULL)
// nframes: number of frames to process
// midi_in: input MIDI events (sent to first plugin)
// num_midi_in: number of input MIDI events
// midi_out: buffer for output MIDI from first plugin (can be NULL)
// midi_out_capacity: size of midi_out buffer
// num_midi_out: receives actual number of output events (can be NULL)
//
// Returns 1 on success, 0 on failure
int mh_chain_process_midi_io(MH_PluginChain* chain,
                             const float* const* inputs,
                             float* const* outputs,
                             int nframes,
                             const MH_MidiEvent* midi_in,
                             int num_midi_in,
                             MH_MidiEvent* midi_out,
                             int midi_out_capacity,
                             int* num_midi_out);

// Get total latency of the chain in samples (sum of all plugin latencies).
int mh_chain_get_latency_samples(MH_PluginChain* chain);

// Get number of plugins in the chain.
int mh_chain_get_num_plugins(MH_PluginChain* chain);

// Get a plugin from the chain by index.
// Returns NULL if index is out of range.
MH_Plugin* mh_chain_get_plugin(MH_PluginChain* chain, int index);

// Get the number of input channels (from first plugin).
int mh_chain_get_num_input_channels(MH_PluginChain* chain);

// Get the number of output channels (from last plugin).
int mh_chain_get_num_output_channels(MH_PluginChain* chain);

// Get the sample rate (all plugins in chain have the same rate).
double mh_chain_get_sample_rate(MH_PluginChain* chain);

// Get the maximum block size the chain can process.
int mh_chain_get_max_block_size(MH_PluginChain* chain);

// Reset all plugins in the chain (clears delay lines, reverb tails, etc.).
// Returns 1 on success, 0 on failure
int mh_chain_reset(MH_PluginChain* chain);

// Set non-realtime mode for all plugins in the chain.
// Returns 1 on success, 0 on failure
int mh_chain_set_non_realtime(MH_PluginChain* chain, int non_realtime);

// Process audio through the chain with sample-accurate parameter automation.
// param_changes: array of parameter changes sorted by sample_offset
// num_param_changes: number of parameter changes
// Splits processing at change points for sample-accurate automation.
// MIDI is sent to the first plugin only. Audio flows sequentially.
//
// Returns 1 on success, 0 on failure
int mh_chain_process_auto(MH_PluginChain* chain,
                           const float* const* inputs,
                           float* const* outputs,
                           int nframes,
                           const MH_MidiEvent* midi_in,
                           int num_midi_in,
                           MH_MidiEvent* midi_out,
                           int midi_out_capacity,
                           int* num_midi_out,
                           const MH_ChainParamChange* param_changes,
                           int num_param_changes);

// Get total tail length of the chain in seconds (maximum of all plugin tails).
// Note: This is the max, not sum, since tails overlap temporally.
double mh_chain_get_tail_seconds(MH_PluginChain* chain);

#ifdef __cplusplus
}
#endif
