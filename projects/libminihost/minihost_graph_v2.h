// minihost_graph_v2.h
//
// General-DAG graph executor for minihost. v1 scope:
//
//   - Plugin nodes (wrapping an existing MH_Plugin*).
//   - Built-in non-plugin nodes: input, output, mix.
//   - Block-level scheduling via Kahn topological sort.
//   - One edge per (dst_node, dst_port); fan-out from a source is
//     allowed. Fan-in requires an explicit mix node.
//   - Channel-count validation at connect / compile time.
//   - Per-node output buffer pool, allocation-free after compile.
//   - No automation (defer to a follow-up: graph-time
//     MH_ParamChange lists per plugin node).
//   - No MIDI routing (audio only).
//   - No latency compensation across fan-in paths.
//   - No feedback loops.
//
// Threading:
//   - mh_graph_v2_render_block: audio-thread-only, no internal lock.
//   - All other functions: thread-safe (do not call concurrently
//     with render_block on the same graph).
//
// The existing MH_PluginGraph (parallel-branches-summed) is unchanged
// and remains the right tool for that specific shape.

#pragma once

#include "minihost.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MH_GraphV2 MH_GraphV2;

typedef enum MH_NodeKind {
    MH_NODE_PLUGIN = 0,
    MH_NODE_INPUT  = 1,
    MH_NODE_OUTPUT = 2,
    MH_NODE_MIX    = 3,
} MH_NodeKind;

// Opaque node handle. Returned by add_* in topological-add order.
typedef int MH_NodeId;

// Create a graph configured for a fixed max block size and sample
// rate. Plugin nodes added later must be opened at this sample rate
// and accept up to max_block_size frames per process call.
//
// Returns NULL on failure; err_buf receives a message.
MH_GraphV2* mh_graph_v2_create(int max_block_size,
                               double sample_rate,
                               char* err_buf, size_t err_buf_size);

// Close the graph. Does NOT close any MH_Plugin nodes -- those remain
// owned by the caller.
void mh_graph_v2_close(MH_GraphV2* g);

// Add a plugin node. The graph borrows the MH_Plugin*; caller keeps
// it alive until after mh_graph_v2_close. Plugin's input + output
// channel counts (from mh_get_info) drive edge validation.
//
// Returns the new node id (>= 0) or -1 on failure.
MH_NodeId mh_graph_v2_add_plugin(MH_GraphV2* g, MH_Plugin* p,
                                 char* err_buf, size_t err_buf_size);

// Add an input node: produces `channels` channels from a caller-
// supplied buffer at render time.
MH_NodeId mh_graph_v2_add_input(MH_GraphV2* g, int channels,
                                char* err_buf, size_t err_buf_size);

// Add an output node: consumes `channels` channels, writes to a
// caller-supplied buffer at render time.
MH_NodeId mh_graph_v2_add_output(MH_GraphV2* g, int channels,
                                 char* err_buf, size_t err_buf_size);

// Add a mix node: sums `num_inputs` inputs (each `channels` channels)
// with per-input gains (default 1.0) into one `channels`-channel
// output. Use mh_graph_v2_set_mix_gain to override gains.
MH_NodeId mh_graph_v2_add_mix(MH_GraphV2* g,
                              int num_inputs, int channels,
                              char* err_buf, size_t err_buf_size);

// Connect src.out[src_port] -> dst.in[dst_port].
//
// Port semantics:
//   - All nodes have output port 0 only. src_port must be 0.
//   - plugin / output: input port 0 only. dst_port must be 0.
//   - mix: input ports 0 .. num_inputs-1.
//   - input nodes have no inputs and cannot be a dst.
//
// One edge per (dst_node, dst_port): reconnecting overwrites the
// existing edge. Fan-out from a source is allowed.
//
// Returns 1 on success, 0 on failure (err_buf describes the error:
// bad node ids, bad ports, channel-count mismatch, etc.).
int mh_graph_v2_connect(MH_GraphV2* g,
                        MH_NodeId src, int src_port,
                        MH_NodeId dst, int dst_port,
                        char* err_buf, size_t err_buf_size);

// Set the per-input gain on a mix node (default 1.0). Linear gain.
// Returns 1 on success, 0 on failure.
int mh_graph_v2_set_mix_gain(MH_GraphV2* g, MH_NodeId mix_node,
                             int input_index, float gain);

// Compile: validate topology (acyclic, all required inputs connected,
// channel counts match across edges), produce a topological schedule,
// and allocate the per-node output buffer pool.
//
// After compile succeeds, no further add_* / connect calls are
// permitted (they return 0). Re-compile is not supported in v1.
//
// Returns 1 on success, 0 on failure.
int mh_graph_v2_compile(MH_GraphV2* g, char* err_buf, size_t err_buf_size);

// Render one block.
//
//   input_buffers[i][c] = pointer to the c-th channel of the i-th
//   input node (in the order add_input was called). Each pointer
//   must address at least nframes valid frames.
//
//   output_buffers[i][c] = pointer to the c-th channel of the i-th
//   output node. Each pointer must address writeable storage for at
//   least nframes frames.
//
// nframes must satisfy 0 < nframes <= max_block_size.
//
// Returns 1 on success, 0 on failure (e.g. graph not compiled,
// nframes out of range, plugin process failure).
int mh_graph_v2_render_block(MH_GraphV2* g,
                             const float* const* const* input_buffers,
                             int num_input_nodes,
                             float* const* const* output_buffers,
                             int num_output_nodes,
                             int nframes);

// Stage sample-accurate parameter automation for a plugin node. The
// graph borrows the changes pointer until the next render_block call;
// the caller keeps it alive that long. Cleared after each
// render_block. Plugin nodes with automation set are dispatched via
// `mh_process_auto` (rather than `mh_process` / `mh_process_midi`).
//
// MIDI + automation combine: if both are set on a node, automation
// applies and the MIDI events are passed alongside.
//
// Returns 1 on success, 0 if node is not a plugin / out of range.
int mh_graph_v2_set_node_automation(MH_GraphV2* g, MH_NodeId node,
                                    const MH_ParamChange* changes,
                                    int num_changes);

// Stage MIDI events to deliver to a plugin node on the next
// mh_graph_v2_render_block call. The graph borrows the events
// pointer until render_block returns; the caller must keep it alive
// until then. Cleared automatically after render_block.
//
// Pass num_events=0 (or events=NULL) to clear without setting.
//
// Must NOT be called concurrently with render_block on the same
// graph. LiveEngine serializes both on its audio thread.
//
// Returns 1 on success, 0 if node is not a plugin or is out of range.
int mh_graph_v2_set_node_midi(MH_GraphV2* g, MH_NodeId node,
                              const MH_MidiEvent* events,
                              int num_events);

// Introspection.
int mh_graph_v2_num_nodes(MH_GraphV2* g);
int mh_graph_v2_num_input_nodes(MH_GraphV2* g);
int mh_graph_v2_num_output_nodes(MH_GraphV2* g);
int mh_graph_v2_is_compiled(MH_GraphV2* g);

#ifdef __cplusplus
}
#endif
