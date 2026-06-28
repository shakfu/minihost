// minihost_graph.h
// Parallel plugin routing for minihost (the "bus").
//
// NOTE: the file name is retained for git history; as of ABI 2.0.0 the
// symbols here are the mh_bus_* / MH_PluginBus family (Python: PluginBus).
// The general DAG executor lives in minihost_graph_v2.h (mh_graph_* /
// MH_PluginGraph; Python: PluginGraph).
//
// MH_PluginBus runs N PluginChain branches in parallel against the
// same input, then sums their outputs with per-branch gains. This
// covers the common cases that a serial PluginChain can't handle:
// parallel compression, dry-bus + reverb-send, multi-band processing
// when each band is wrapped in a chain, etc.
//
// Scope (v1): fan-out + summed mix. Not a full DAG -- there's no
// arbitrary node-to-node routing, no sidechain inputs into branches,
// and no per-edge connection topology. A richer graph type is a
// possible v2.
//
// Thread Safety:
//   - mh_bus_process: call from the audio thread only.
//   - All other functions are thread-safe (do not call while a process
//     call is in flight on the same graph from another thread).
//
#pragma once
#include <stddef.h>
#include "minihost.h"   // MH_MidiEvent

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct MH_PluginChain MH_PluginChain;
typedef struct MH_PluginBus MH_PluginBus;

// Create a graph configured for a fixed I/O channel layout and max
// block size. Every branch added later must accept exactly
// num_in_channels input channels and produce exactly num_out_channels
// output channels; sample rates across branches must agree with the
// graph's sample_rate. Branches will be processed at up to
// max_block_size frames per call.
//
// Returns NULL on failure.
MH_PluginBus* mh_bus_create(int num_in_channels,
                                 int num_out_channels,
                                 int max_block_size,
                                 double sample_rate,
                                 char* err_buf,
                                 size_t err_buf_size);

// Close the graph and free its scratch buffers. Does NOT close the
// PluginChain branches -- they are still owned by the caller.
void mh_bus_close(MH_PluginBus* graph);

// Add a PluginChain branch to the graph. The graph stores the chain
// pointer but does not take ownership; the caller must keep the chain
// alive (and not close it) while the graph is in use.
//
// gain is the per-branch summing gain (linear; default 1.0). Pass
// 0.0 to mute a branch without removing it.
//
// Returns the branch index (>= 0) on success, or -1 on failure (e.g.,
// NULL chain, channel-count mismatch with the graph, sample-rate
// mismatch).
int mh_bus_add_branch(MH_PluginBus* graph,
                         MH_PluginChain* chain,
                         float gain,
                         char* err_buf,
                         size_t err_buf_size);

// Get / set a branch's summing gain. Returns 1 on success, 0 on
// failure (NULL graph, index out of range).
int mh_bus_set_branch_gain(MH_PluginBus* graph, int branch_index, float gain);

// Returns gain on success, or NaN on failure.
float mh_bus_get_branch_gain(MH_PluginBus* graph, int branch_index);

int mh_bus_get_num_branches(MH_PluginBus* graph);

// Process audio through the graph: fan inputs to every branch, sum
// each branch's output (scaled by its gain) into outputs.
//
// inputs / outputs: planar audio buffers, sized to the graph's
// num_in_channels / num_out_channels and at least nframes frames each.
// nframes must be in (0, max_block_size].
//
// Returns 1 on success, 0 on failure.
int mh_bus_process(MH_PluginBus* graph,
                      const float* const* inputs,
                      float* const* outputs,
                      int nframes);

// Process audio with MIDI fanned out to every branch. The same
// midi_in events are delivered to each branch (to that branch's first
// plugin, per mh_chain_process_midi_io semantics), which is what makes
// the bus a layering primitive: one MIDI part drives N parallel
// instruments whose audio is summed.
//
// midi_in / num_midi_in: input MIDI events (pass NULL/0 to process
// audio only -- equivalent to mh_bus_process). Muted branches
// (gain 0.0) are skipped and receive no MIDI.
//
// MIDI produced by branches is NOT collected by this entry point (it is
// an audio-summing bus). To collect and merge branch MIDI output, use
// mh_bus_process_midi_io.
//
// Returns 1 on success, 0 on failure.
int mh_bus_process_midi(MH_PluginBus* graph,
                          const float* const* inputs,
                          float* const* outputs,
                          int nframes,
                          const MH_MidiEvent* midi_in,
                          int num_midi_in);

// Process audio with MIDI fanned out to every branch (as
// mh_bus_process_midi), and additionally collect and merge the MIDI
// produced by each branch (from each branch's first plugin, per
// mh_chain_process_midi_io semantics) into a single time-ordered
// stream. This completes the bus for parallel MIDI effects (e.g. a
// layer of arpeggiators driven by one part).
//
// midi_in / num_midi_in: input MIDI fanned to every branch (NULL/0 ok).
//
// midi_out: caller-owned buffer receiving the merged branch MIDI,
//   stably sorted by sample_offset (events at the same offset keep
//   branch order: branch 0 before branch 1, etc.). May be NULL only if
//   midi_out_capacity is 0.
// midi_out_capacity: capacity of midi_out in events.
// num_midi_out: receives the number of merged events written (may be
//   NULL). Always <= midi_out_capacity.
// midi_out_overflow: set to 1 if the merged buffer filled to capacity
//   and events may have been dropped, else 0 (may be NULL). This is
//   conservative: it can be 1 when the buffer filled exactly without
//   loss, but it is never 0 when events were dropped.
//
// Processing (audio fan-out-and-sum) is identical to mh_bus_process_midi
// regardless of whether MIDI output is collected.
//
// Returns 1 on success, 0 on failure.
int mh_bus_process_midi_io(MH_PluginBus* graph,
                           const float* const* inputs,
                           float* const* outputs,
                           int nframes,
                           const MH_MidiEvent* midi_in,
                           int num_midi_in,
                           MH_MidiEvent* midi_out,
                           int midi_out_capacity,
                           int* num_midi_out,
                           int* midi_out_overflow);

// Properties
int mh_bus_get_num_input_channels(MH_PluginBus* graph);
int mh_bus_get_num_output_channels(MH_PluginBus* graph);
double mh_bus_get_sample_rate(MH_PluginBus* graph);
int mh_bus_get_max_block_size(MH_PluginBus* graph);

// Total latency / tail: maximum across branches (parallel branches
// don't accumulate latency the way serial chains do).
int mh_bus_get_latency_samples(MH_PluginBus* graph);
double mh_bus_get_tail_seconds(MH_PluginBus* graph);

#ifdef __cplusplus
}
#endif
