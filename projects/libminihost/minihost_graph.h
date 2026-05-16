// minihost_graph.h
// Parallel plugin routing for minihost.
//
// MH_PluginGraph runs N PluginChain branches in parallel against the
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
//   - mh_graph_process: call from the audio thread only.
//   - All other functions are thread-safe (do not call while a process
//     call is in flight on the same graph from another thread).
//
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations
typedef struct MH_PluginChain MH_PluginChain;
typedef struct MH_PluginGraph MH_PluginGraph;

// Create a graph configured for a fixed I/O channel layout and max
// block size. Every branch added later must accept exactly
// num_in_channels input channels and produce exactly num_out_channels
// output channels; sample rates across branches must agree with the
// graph's sample_rate. Branches will be processed at up to
// max_block_size frames per call.
//
// Returns NULL on failure.
MH_PluginGraph* mh_graph_create(int num_in_channels,
                                 int num_out_channels,
                                 int max_block_size,
                                 double sample_rate,
                                 char* err_buf,
                                 size_t err_buf_size);

// Close the graph and free its scratch buffers. Does NOT close the
// PluginChain branches -- they are still owned by the caller.
void mh_graph_close(MH_PluginGraph* graph);

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
int mh_graph_add_branch(MH_PluginGraph* graph,
                         MH_PluginChain* chain,
                         float gain,
                         char* err_buf,
                         size_t err_buf_size);

// Get / set a branch's summing gain. Returns 1 on success, 0 on
// failure (NULL graph, index out of range).
int mh_graph_set_branch_gain(MH_PluginGraph* graph, int branch_index, float gain);

// Returns gain on success, or NaN on failure.
float mh_graph_get_branch_gain(MH_PluginGraph* graph, int branch_index);

int mh_graph_get_num_branches(MH_PluginGraph* graph);

// Process audio through the graph: fan inputs to every branch, sum
// each branch's output (scaled by its gain) into outputs.
//
// inputs / outputs: planar audio buffers, sized to the graph's
// num_in_channels / num_out_channels and at least nframes frames each.
// nframes must be in (0, max_block_size].
//
// Returns 1 on success, 0 on failure.
int mh_graph_process(MH_PluginGraph* graph,
                      const float* const* inputs,
                      float* const* outputs,
                      int nframes);

// Properties
int mh_graph_get_num_input_channels(MH_PluginGraph* graph);
int mh_graph_get_num_output_channels(MH_PluginGraph* graph);
double mh_graph_get_sample_rate(MH_PluginGraph* graph);
int mh_graph_get_max_block_size(MH_PluginGraph* graph);

// Total latency / tail: maximum across branches (parallel branches
// don't accumulate latency the way serial chains do).
int mh_graph_get_latency_samples(MH_PluginGraph* graph);
double mh_graph_get_tail_seconds(MH_PluginGraph* graph);

#ifdef __cplusplus
}
#endif
