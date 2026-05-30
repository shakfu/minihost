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
//   - Per-node parameter automation (MH_ParamChange lists) via
//     mh_graph_v2_set_node_automation.
//   - MIDI routing: dedicated MIDI_INPUT / MIDI_OUTPUT node kinds and
//     a separate MIDI edge list (mh_graph_v2_connect_midi). One MIDI
//     edge per dst node; fan-out allowed. Plugin nodes accept MIDI on
//     an implicit MIDI input port (when accepts_midi=1) and produce
//     MIDI on an implicit MIDI output port (when produces_midi=1).
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
    MH_NODE_PLUGIN          = 0,
    MH_NODE_INPUT           = 1,
    MH_NODE_OUTPUT          = 2,
    MH_NODE_MIX             = 3,
    MH_NODE_MIDI_INPUT      = 4,
    MH_NODE_MIDI_OUTPUT     = 5,
    MH_NODE_PICK_CHANNEL    = 6,
    MH_NODE_MERGE_CHANNELS  = 7,
    MH_NODE_MIDI_PROCESSOR  = 8,
    MH_NODE_MIDI_MERGE      = 9,
} MH_NodeKind;

typedef enum MH_MidiOp {
    MH_MIDI_OP_FILTER          = 0,
    MH_MIDI_OP_TRANSPOSE       = 1,
    MH_MIDI_OP_VELOCITY_CURVE  = 2,
} MH_MidiOp;

// Configuration for an MH_NODE_MIDI_PROCESSOR. The `op` field selects
// which fields below apply; the rest are ignored.
//
// FILTER: pass events through unchanged when:
//   - For Note On/Off (status nibble 0x80/0x90): channel bit in
//     `channel_mask` is set AND data1 (note) is in
//     [min_note, max_note]. Other channel-voice messages (CC,
//     program change, pitch bend, aftertouch) only check channel.
//     System messages (status >= 0xF0) pass through unchanged.
// TRANSPOSE: add `transpose_semitones` to data1 for Note On/Off
//   only. Out-of-range results (<0 or >127) drop the event.
// VELOCITY_CURVE: for Note On with non-zero velocity, remap
//   data2 := round(pow(data2/127, velocity_gamma) * 127), clamped
//   to [1, 127]. Note On with velocity=0 (a Note Off in disguise)
//   passes through unchanged.
typedef struct MH_MidiProcessorParams {
    MH_MidiOp op;
    // FILTER:
    int   min_note;             // 0..127 inclusive (default 0)
    int   max_note;             // 0..127 inclusive (default 127)
    int   channel_mask;         // bit i = pass channel i (default 0xFFFF)
    // TRANSPOSE:
    int   transpose_semitones;  // signed, e.g. -12..+12
    // VELOCITY_CURVE:
    float velocity_gamma;       // 1.0 = identity, <1 = compress, >1 = expand
} MH_MidiProcessorParams;

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

// Add a pick-channel node: extracts a single channel from an
// `in_channels`-channel input and outputs it as a 1-channel signal.
// channel_index must be in [0, in_channels). Used to feed mono-only
// plugins (or per-channel processing chains) from a stereo source
// without an upstream mono-reducer plugin.
//
// Topology:
//   - One input port (port 0): in_channels channels.
//   - One output port (port 0): 1 channel.
MH_NodeId mh_graph_v2_add_pick_channel(MH_GraphV2* g,
                                       int in_channels, int channel_index,
                                       char* err_buf, size_t err_buf_size);

// Add a merge-channels node: interleaves `out_channels` separate
// 1-channel inputs into one `out_channels`-channel output. The dst
// port (0..out_channels-1) selects which output channel that input
// feeds. Distinct from `mix`, which sums all inputs into one signal.
//
// Topology:
//   - out_channels input ports (port i: 1 channel, becomes channel i
//     of the output).
//   - One output port (port 0): out_channels channels.
MH_NodeId mh_graph_v2_add_merge_channels(MH_GraphV2* g, int out_channels,
                                         char* err_buf, size_t err_buf_size);

// Add a MIDI processor node: applies params.op (filter / transpose /
// velocity_curve) to events flowing from its single MIDI input port
// (port 0) to its single MIDI output port. Defaults for unused
// fields can be left zero -- the op selects which fields matter.
MH_NodeId mh_graph_v2_add_midi_processor(MH_GraphV2* g,
                                         MH_MidiProcessorParams params,
                                         char* err_buf, size_t err_buf_size);

// Replace the params on a MIDI processor node. The op can change
// post-add but the node kind stays MH_NODE_MIDI_PROCESSOR. The audio
// thread reads params each block; updating from another thread
// while render_block is in flight is undefined (callers should
// serialize, e.g. via LiveEngine's start/stop or message-thread
// cadence).
int mh_graph_v2_set_midi_processor_params(MH_GraphV2* g, MH_NodeId node,
                                          MH_MidiProcessorParams params);

// Add a MIDI merge node: concatenates events from `num_inputs`
// separate MIDI input ports (0..num_inputs-1) into one output
// stream, sorted by sample_offset (stable across ports). The
// canonical fan-in primitive for MIDI; pair with
// mh_graph_v2_connect_midi_port to wire each source.
MH_NodeId mh_graph_v2_add_midi_merge(MH_GraphV2* g, int num_inputs,
                                     char* err_buf, size_t err_buf_size);

// Add a MIDI input node: produces a per-block MIDI event stream
// supplied by the caller via mh_graph_v2_set_midi_input_events.
// Has no audio I/O. Cannot be a destination of any edge.
MH_NodeId mh_graph_v2_add_midi_input(MH_GraphV2* g,
                                     char* err_buf, size_t err_buf_size);

// Add a MIDI output node: consumes a MIDI stream from upstream and
// stores it for retrieval via mh_graph_v2_get_midi_output_events.
// Has no audio I/O. Has one MIDI input "port" (port 0).
MH_NodeId mh_graph_v2_add_midi_output(MH_GraphV2* g,
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

// Connect a MIDI edge: src.midi_out -> dst.midi_in[dst_port=0].
// Convenience wrapper around mh_graph_v2_connect_midi_port; the
// destination port is always 0 for non-MIDI_MERGE consumers.
//
// Valid src kinds:
//   - MH_NODE_MIDI_INPUT
//   - MH_NODE_MIDI_PROCESSOR
//   - MH_NODE_MIDI_MERGE
//   - MH_NODE_PLUGIN with produces_midi=1
// Valid dst kinds:
//   - MH_NODE_PLUGIN with accepts_midi=1
//   - MH_NODE_MIDI_OUTPUT
//   - MH_NODE_MIDI_PROCESSOR
//   - MH_NODE_MIDI_MERGE  (use mh_graph_v2_connect_midi_port to pick a
//     specific input port; this function uses port 0)
//
// One MIDI edge per (dst node, dst port). Fan-out from a source is
// allowed. MIDI edges contribute to topological ordering the same
// way audio edges do.
int mh_graph_v2_connect_midi(MH_GraphV2* g,
                             MH_NodeId src, MH_NodeId dst,
                             char* err_buf, size_t err_buf_size);

// Connect a MIDI edge to a specific dst_port. Required for
// MH_NODE_MIDI_MERGE; other dst kinds accept only dst_port == 0.
int mh_graph_v2_connect_midi_port(MH_GraphV2* g,
                                  MH_NodeId src, MH_NodeId dst,
                                  int dst_port,
                                  char* err_buf, size_t err_buf_size);

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
// If the plugin has an incoming MIDI edge, the edge takes precedence
// and events staged via this function are ignored for that block.
// This API remains for callers that drive a plugin directly without
// a MIDI graph topology (e.g. legacy live-engine integrations).
//
// Must NOT be called concurrently with render_block on the same
// graph. LiveEngine serializes both on its audio thread.
//
// Returns 1 on success, 0 if node is not a plugin or is out of range.
int mh_graph_v2_set_node_midi(MH_GraphV2* g, MH_NodeId node,
                              const MH_MidiEvent* events,
                              int num_events);

// Stage MIDI events on a MIDI_INPUT node for the next render_block
// call. Borrowed pointer; the caller must keep it alive until
// render_block returns. Cleared automatically afterward.
//
// Returns 1 on success, 0 if node is not a MIDI_INPUT or out of range.
int mh_graph_v2_set_midi_input_events(MH_GraphV2* g, MH_NodeId node,
                                      const MH_MidiEvent* events,
                                      int num_events);

// After render_block, drain the events that flowed into a MIDI_OUTPUT
// node. Writes up to `capacity` events into `out_buf`; on return,
// *num_events_out is the total event count produced this block (may
// exceed capacity, in which case events were truncated).
//
// Must be called after render_block returns and before the next
// render_block call. Pass out_buf=NULL with capacity=0 to query the
// count only.
//
// Returns 1 on success, 0 if node is not a MIDI_OUTPUT or out of
// range.
int mh_graph_v2_get_midi_output_events(MH_GraphV2* g, MH_NodeId node,
                                       MH_MidiEvent* out_buf,
                                       int capacity,
                                       int* num_events_out);

// Introspection.
int mh_graph_v2_num_nodes(MH_GraphV2* g);
int mh_graph_v2_num_input_nodes(MH_GraphV2* g);
int mh_graph_v2_num_output_nodes(MH_GraphV2* g);
int mh_graph_v2_is_compiled(MH_GraphV2* g);

#ifdef __cplusplus
}
#endif
