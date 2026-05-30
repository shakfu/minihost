// minihost_graph_v2.cpp
// General-DAG graph executor. See minihost_graph_v2.h for design.

#include "minihost_graph_v2.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

struct Edge {
    MH_NodeId src_node;
    int       src_port;        // always 0 in v1
    MH_NodeId dst_node;
    int       dst_port;
    int       channels;        // set at connect time
};

struct MidiEdge {
    MH_NodeId src_node;
    MH_NodeId dst_node;
    int       dst_port;   // 0 except for MH_NODE_MIDI_MERGE
};

// Per-plugin midi-out capture buffer (events).
constexpr int kMidiBufCapacity = 1024;

struct Node {
    MH_NodeKind kind;

    // For plugin nodes only.
    MH_Plugin*  plugin = nullptr;

    // Channel counts. Output is the node's single output port; input
    // counts are per dst_port for mix nodes (all equal), or a single
    // value for plugin/output. Input nodes have no inputs.
    int         input_channels  = 0;   // per port; mix: same on every port
    int         num_input_ports = 0;
    int         output_channels = 0;

    // Mix-only: per-input gains. size == num_input_ports.
    std::vector<float> mix_gains;

    // Resolved at compile: for each input port, the buffer source.
    // For plugin/output: input_sources.size() == 1.
    // For mix:           input_sources.size() == num_input_ports.
    // Each entry references either the caller's input buffer (for an
    // upstream input node, which has no pool buffer) or a pool entry.
    struct InputRef {
        bool        from_caller;   // true => upstream is an input node
        bool        is_silent;     // true => unwired plugin port; feed silence
        int         caller_index;  // index into input_buffers[]
        int         pool_index;    // index into pool_storage_, when !from_caller
    };
    std::vector<InputRef> input_sources;

    // Resolved at compile: the buffer this node writes to.
    //  - input  : not used at render time (we read straight from caller)
    //  - plugin/mix : index into pool_storage_
    //  - output : caller's output buffer index (caller_index)
    bool out_to_caller   = false;
    int  out_caller_index = 0;
    int  out_pool_index   = -1;

    // For input/output nodes, the position within the graph's
    // input_nodes_ / output_nodes_ list (set at add_input/add_output).
    int io_index = -1;

    // For MH_NODE_PICK_CHANNEL only: which channel of the input to
    // forward to the (single) output channel.
    int pick_channel_index = 0;

    // MIDI capabilities (cached from MH_Info for plugins).
    bool accepts_midi  = false;
    bool produces_midi = false;

    // MIDI routing: at most one incoming MIDI edge per (node, port).
    // For nodes with only a single MIDI input port (the common case:
    // plugin, MIDI_OUTPUT, MIDI_PROCESSOR) midi_srcs has size 1.
    // For MH_NODE_MIDI_MERGE it has size num_midi_input_ports. An
    // entry of -1 means that port is unconnected (compile catches
    // missing connections via per-port validation).
    int                    num_midi_input_ports = 0;
    std::vector<MH_NodeId> midi_srcs;
    bool                   has_outgoing_midi_edge = false;

    // For MH_NODE_MIDI_PROCESSOR only.
    MH_MidiProcessorParams midi_processor_params{};

    // MIDI buffers:
    //  - midi_out_buf: events produced by this node this block
    //    (MIDI_INPUT: written by set_midi_input_events; plugin: filled
    //    by mh_process_midi_io; MIDI_OUTPUT: copy of upstream events,
    //    drained by get_midi_output_events).
    //  - midi_out_count: number of valid events in midi_out_buf.
    //  - midi_out_truncated_count: full count even when truncated.
    std::vector<MH_MidiEvent> midi_out_buf;
    int                       midi_out_count = 0;
    int                       midi_out_truncated_count = 0;

    // For MIDI_INPUT only: borrowed pointer / count staged by caller
    // before render_block; cleared after each render_block.
    const MH_MidiEvent* staged_midi_events = nullptr;
    int                 staged_midi_count  = 0;
};

struct EdgeKey {
    MH_NodeId dst_node;
    int       dst_port;
    bool operator==(const EdgeKey& o) const
    { return dst_node == o.dst_node && dst_port == o.dst_port; }
};

} // namespace

struct MH_GraphV2 {
    int    max_block_size = 0;
    double sample_rate    = 0.0;

    std::vector<Node>      nodes;
    std::vector<Edge>      edges;
    std::vector<MidiEdge>  midi_edges;
    std::vector<MH_NodeId> input_nodes_;   // node ids in add order
    std::vector<MH_NodeId> output_nodes_;

    // Filled at compile.
    bool                    compiled = false;
    std::vector<MH_NodeId>  schedule;

    // Single zero-filled buffer sized max_block_size, used to feed
    // silence into plugin nodes whose audio input port is unwired
    // (instruments / synths typically don't need a wired input but
    // their JUCE descriptor still reports one).
    std::vector<float>                silence_buf;

    // Per-pool-entry: channels * max_block_size floats (contiguous).
    // Pointer table for handing planar views to mh_process.
    std::vector<std::vector<float>>   pool_storage;
    std::vector<std::vector<float*>>  pool_ptrs;        // [pool][channel] -> frame_ptr

    // Pending MIDI for the next render_block call, indexed by node id.
    // Caller-owned pointers; cleared after each render_block.
    struct PendingMidi { const MH_MidiEvent* events; int count; };
    std::vector<PendingMidi> pending_midi;  // sized at compile, one per node

    // Pending automation for the next render_block call, indexed by
    // node id. Same lifetime contract as pending_midi.
    struct PendingAuto { const MH_ParamChange* changes; int count; };
    std::vector<PendingAuto> pending_auto;
};

namespace {

void setErr(char* buf, size_t n, const char* msg)
{
    if (buf == nullptr || n == 0) return;
    std::snprintf(buf, n, "%s", msg);
}

void setErrf(char* buf, size_t n, const char* fmt, ...)
{
    if (buf == nullptr || n == 0) return;
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, n, fmt, args);
    va_end(args);
}

bool inRange(MH_NodeId id, int sz) { return id >= 0 && id < sz; }

// Find the edge whose (dst_node, dst_port) matches key. Returns -1
// if none. Linear scan; v1 graphs are small.
int findEdge(const std::vector<Edge>& edges, MH_NodeId dst, int port)
{
    for (size_t i = 0; i < edges.size(); ++i)
        if (edges[i].dst_node == dst && edges[i].dst_port == port)
            return (int) i;
    return -1;
}

} // namespace

extern "C" MH_GraphV2* mh_graph_v2_create(int max_block_size,
                                          double sample_rate,
                                          char* err_buf, size_t err_buf_size)
{
    if (max_block_size <= 0)
    {
        setErr(err_buf, err_buf_size, "max_block_size must be positive");
        return nullptr;
    }
    if (!(sample_rate > 0.0))
    {
        setErr(err_buf, err_buf_size, "sample_rate must be positive");
        return nullptr;
    }
    auto* g = new MH_GraphV2();
    g->max_block_size = max_block_size;
    g->sample_rate    = sample_rate;
    return g;
}

extern "C" void mh_graph_v2_close(MH_GraphV2* g)
{
    if (g == nullptr) return;
    // Plugin nodes are caller-owned.
    delete g;
}

extern "C" MH_NodeId mh_graph_v2_add_plugin(MH_GraphV2* g, MH_Plugin* p,
                                            char* err_buf, size_t err_buf_size)
{
    if (g == nullptr || p == nullptr)
    {
        setErr(err_buf, err_buf_size, "null graph or plugin");
        return -1;
    }
    if (g->compiled)
    {
        setErr(err_buf, err_buf_size,
               "graph already compiled; add_plugin not permitted");
        return -1;
    }
    MH_Info info{};
    if (!mh_get_info(p, &info))
    {
        setErr(err_buf, err_buf_size, "mh_get_info failed on plugin");
        return -1;
    }
    Node n;
    n.kind            = MH_NODE_PLUGIN;
    n.plugin          = p;
    n.input_channels  = info.num_input_ch;
    // Instruments report num_input_ch == 0 (synths driven by MIDI
    // alone); they don't expose an audio input port and compile
    // doesn't require one to be connected. Effects (num_input_ch > 0)
    // have exactly one audio input port.
    n.num_input_ports = info.num_input_ch > 0 ? 1 : 0;
    n.output_channels = info.num_output_ch;
    n.accepts_midi    = info.accepts_midi != 0;
    n.produces_midi   = info.produces_midi != 0;
    n.input_sources.resize((size_t) n.num_input_ports);
    if (n.accepts_midi)
    {
        n.num_midi_input_ports = 1;
        n.midi_srcs.assign(1, -1);
    }
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" MH_NodeId mh_graph_v2_add_pick_channel(MH_GraphV2* g,
                                                  int in_channels,
                                                  int channel_index,
                                                  char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    if (in_channels <= 0)
    {
        setErr(err_buf, err_buf_size,
               "pick_channel in_channels must be positive");
        return -1;
    }
    if (channel_index < 0 || channel_index >= in_channels)
    {
        setErrf(err_buf, err_buf_size,
                "pick_channel channel_index %d out of range [0, %d)",
                channel_index, in_channels);
        return -1;
    }
    Node n;
    n.kind               = MH_NODE_PICK_CHANNEL;
    n.input_channels     = in_channels;
    n.num_input_ports    = 1;
    n.output_channels    = 1;
    n.pick_channel_index = channel_index;
    n.input_sources.resize(1);
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" MH_NodeId mh_graph_v2_add_merge_channels(MH_GraphV2* g,
                                                    int out_channels,
                                                    char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    if (out_channels <= 0)
    {
        setErr(err_buf, err_buf_size,
               "merge_channels out_channels must be positive");
        return -1;
    }
    Node n;
    n.kind            = MH_NODE_MERGE_CHANNELS;
    // Each input port is a single channel; we have one port per
    // output channel.
    n.input_channels  = 1;
    n.num_input_ports = out_channels;
    n.output_channels = out_channels;
    n.input_sources.resize((size_t) out_channels);
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" MH_NodeId mh_graph_v2_add_midi_input(MH_GraphV2* g,
                                                char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    Node n;
    n.kind            = MH_NODE_MIDI_INPUT;
    n.input_channels  = 0;
    n.num_input_ports = 0;
    n.output_channels = 0;
    n.produces_midi   = true;
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" MH_NodeId mh_graph_v2_add_midi_output(MH_GraphV2* g,
                                                 char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    Node n;
    n.kind                = MH_NODE_MIDI_OUTPUT;
    n.input_channels      = 0;
    n.num_input_ports     = 0;
    n.output_channels     = 0;
    n.accepts_midi        = true;
    n.num_midi_input_ports = 1;
    n.midi_srcs.assign(1, -1);
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" MH_NodeId mh_graph_v2_add_input(MH_GraphV2* g, int channels,
                                           char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    if (channels <= 0)
    {
        setErr(err_buf, err_buf_size, "input channels must be positive");
        return -1;
    }
    Node n;
    n.kind            = MH_NODE_INPUT;
    n.input_channels  = 0;
    n.num_input_ports = 0;
    n.output_channels = channels;
    n.io_index        = (int) g->input_nodes_.size();
    g->nodes.push_back(std::move(n));
    MH_NodeId id = (MH_NodeId)(g->nodes.size() - 1);
    g->input_nodes_.push_back(id);
    return id;
}

extern "C" MH_NodeId mh_graph_v2_add_output(MH_GraphV2* g, int channels,
                                            char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    if (channels <= 0)
    {
        setErr(err_buf, err_buf_size, "output channels must be positive");
        return -1;
    }
    Node n;
    n.kind            = MH_NODE_OUTPUT;
    n.input_channels  = channels;
    n.num_input_ports = 1;
    n.output_channels = 0;
    n.io_index        = (int) g->output_nodes_.size();
    n.input_sources.resize(1);
    g->nodes.push_back(std::move(n));
    MH_NodeId id = (MH_NodeId)(g->nodes.size() - 1);
    g->output_nodes_.push_back(id);
    return id;
}

extern "C" MH_NodeId mh_graph_v2_add_mix(MH_GraphV2* g,
                                         int num_inputs, int channels,
                                         char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    if (num_inputs <= 0 || channels <= 0)
    {
        setErr(err_buf, err_buf_size,
               "mix num_inputs and channels must be positive");
        return -1;
    }
    Node n;
    n.kind            = MH_NODE_MIX;
    n.input_channels  = channels;
    n.num_input_ports = num_inputs;
    n.output_channels = channels;
    n.mix_gains.assign((size_t) num_inputs, 1.0f);
    n.input_sources.resize((size_t) num_inputs);
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" MH_NodeId mh_graph_v2_add_midi_processor(
    MH_GraphV2* g, MH_MidiProcessorParams params,
    char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    if (params.op != MH_MIDI_OP_FILTER
        && params.op != MH_MIDI_OP_TRANSPOSE
        && params.op != MH_MIDI_OP_VELOCITY_CURVE)
    {
        setErr(err_buf, err_buf_size, "invalid MH_MidiOp");
        return -1;
    }
    Node n;
    n.kind                  = MH_NODE_MIDI_PROCESSOR;
    n.accepts_midi          = true;
    n.produces_midi         = true;
    n.num_midi_input_ports  = 1;
    n.midi_srcs.assign(1, -1);
    n.midi_processor_params = params;
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" int mh_graph_v2_set_midi_processor_params(
    MH_GraphV2* g, MH_NodeId node, MH_MidiProcessorParams params)
{
    if (g == nullptr) return 0;
    if (!inRange(node, (int) g->nodes.size())) return 0;
    auto& n = g->nodes[(size_t) node];
    if (n.kind != MH_NODE_MIDI_PROCESSOR) return 0;
    n.midi_processor_params = params;
    return 1;
}

extern "C" MH_NodeId mh_graph_v2_add_midi_merge(
    MH_GraphV2* g, int num_inputs,
    char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return -1; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return -1; }
    if (num_inputs <= 0)
    {
        setErr(err_buf, err_buf_size,
               "midi_merge num_inputs must be positive");
        return -1;
    }
    Node n;
    n.kind                 = MH_NODE_MIDI_MERGE;
    n.accepts_midi         = true;
    n.produces_midi        = true;
    n.num_midi_input_ports = num_inputs;
    n.midi_srcs.assign((size_t) num_inputs, -1);
    g->nodes.push_back(std::move(n));
    return (MH_NodeId)(g->nodes.size() - 1);
}

extern "C" int mh_graph_v2_connect(MH_GraphV2* g,
                                   MH_NodeId src, int src_port,
                                   MH_NodeId dst, int dst_port,
                                   char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return 0; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return 0; }
    const int N = (int) g->nodes.size();
    if (!inRange(src, N) || !inRange(dst, N))
    {
        setErr(err_buf, err_buf_size, "node id out of range");
        return 0;
    }
    if (src_port != 0)
    {
        setErr(err_buf, err_buf_size,
               "src_port must be 0 (only one output port in v1)");
        return 0;
    }
    if (src == dst)
    {
        setErr(err_buf, err_buf_size, "self-edges not allowed");
        return 0;
    }
    auto& dn = g->nodes[(size_t) dst];
    auto& sn = g->nodes[(size_t) src];
    if (dn.kind == MH_NODE_INPUT)
    {
        setErr(err_buf, err_buf_size,
               "cannot connect into an input node");
        return 0;
    }
    auto is_midi_kind = [](MH_NodeKind k) {
        return k == MH_NODE_MIDI_INPUT
            || k == MH_NODE_MIDI_OUTPUT
            || k == MH_NODE_MIDI_PROCESSOR
            || k == MH_NODE_MIDI_MERGE;
    };
    if (is_midi_kind(sn.kind) || is_midi_kind(dn.kind))
    {
        setErr(err_buf, err_buf_size,
               "MIDI nodes cannot participate in audio edges; "
               "use mh_graph_v2_connect_midi");
        return 0;
    }
    if (dst_port < 0 || dst_port >= dn.num_input_ports)
    {
        setErrf(err_buf, err_buf_size,
                "dst_port %d out of range [0, %d)", dst_port,
                dn.num_input_ports);
        return 0;
    }
    if (dn.input_channels != sn.output_channels)
    {
        setErrf(err_buf, err_buf_size,
                "channel mismatch on edge: src %d ch, dst %d ch",
                sn.output_channels, dn.input_channels);
        return 0;
    }

    // Overwrite any existing edge to (dst, dst_port).
    int existing = findEdge(g->edges, dst, dst_port);
    Edge e{ src, src_port, dst, dst_port, sn.output_channels };
    if (existing >= 0) g->edges[(size_t) existing] = e;
    else               g->edges.push_back(e);
    return 1;
}

extern "C" int mh_graph_v2_set_mix_gain(MH_GraphV2* g, MH_NodeId mix_node,
                                        int input_index, float gain)
{
    if (g == nullptr) return 0;
    if (!inRange(mix_node, (int) g->nodes.size())) return 0;
    auto& n = g->nodes[(size_t) mix_node];
    if (n.kind != MH_NODE_MIX) return 0;
    if (input_index < 0 || input_index >= (int) n.mix_gains.size()) return 0;
    n.mix_gains[(size_t) input_index] = gain;
    return 1;
}

extern "C" int mh_graph_v2_connect_midi_port(MH_GraphV2* g,
                                             MH_NodeId src, MH_NodeId dst,
                                             int dst_port,
                                             char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return 0; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "graph already compiled"); return 0; }
    const int N = (int) g->nodes.size();
    if (!inRange(src, N) || !inRange(dst, N))
    {
        setErr(err_buf, err_buf_size, "node id out of range");
        return 0;
    }
    if (src == dst)
    {
        setErr(err_buf, err_buf_size, "self-edges not allowed");
        return 0;
    }
    auto& sn = g->nodes[(size_t) src];
    auto& dn = g->nodes[(size_t) dst];

    // Validate src can produce MIDI.
    bool src_ok = false;
    if (sn.kind == MH_NODE_MIDI_INPUT
        || sn.kind == MH_NODE_MIDI_PROCESSOR
        || sn.kind == MH_NODE_MIDI_MERGE) src_ok = true;
    else if (sn.kind == MH_NODE_PLUGIN && sn.produces_midi) src_ok = true;
    if (!src_ok)
    {
        setErr(err_buf, err_buf_size,
               "src does not produce MIDI (must be MIDI_INPUT, "
               "MIDI_PROCESSOR, MIDI_MERGE, or plugin with "
               "produces_midi=1)");
        return 0;
    }

    // Validate dst can accept MIDI.
    bool dst_ok = false;
    if (dn.kind == MH_NODE_MIDI_OUTPUT
        || dn.kind == MH_NODE_MIDI_PROCESSOR
        || dn.kind == MH_NODE_MIDI_MERGE) dst_ok = true;
    else if (dn.kind == MH_NODE_PLUGIN && dn.accepts_midi) dst_ok = true;
    if (!dst_ok)
    {
        setErr(err_buf, err_buf_size,
               "dst does not accept MIDI (must be MIDI_OUTPUT, "
               "MIDI_PROCESSOR, MIDI_MERGE, or plugin with "
               "accepts_midi=1)");
        return 0;
    }

    if (dst_port < 0 || dst_port >= dn.num_midi_input_ports)
    {
        setErrf(err_buf, err_buf_size,
                "dst_port %d out of range [0, %d) for this dst kind",
                dst_port, dn.num_midi_input_ports);
        return 0;
    }

    // One MIDI edge per (dst, dst_port). Overwrite if one already
    // exists.
    int existing = -1;
    for (size_t i = 0; i < g->midi_edges.size(); ++i)
        if (g->midi_edges[i].dst_node == dst
            && g->midi_edges[i].dst_port == dst_port) { existing = (int) i; break; }
    MidiEdge e{ src, dst, dst_port };
    if (existing >= 0) g->midi_edges[(size_t) existing] = e;
    else               g->midi_edges.push_back(e);
    return 1;
}

extern "C" int mh_graph_v2_connect_midi(MH_GraphV2* g,
                                        MH_NodeId src, MH_NodeId dst,
                                        char* err_buf, size_t err_buf_size)
{
    return mh_graph_v2_connect_midi_port(g, src, dst, 0, err_buf, err_buf_size);
}

extern "C" int mh_graph_v2_compile(MH_GraphV2* g,
                                   char* err_buf, size_t err_buf_size)
{
    if (g == nullptr) { setErr(err_buf, err_buf_size, "null graph"); return 0; }
    if (g->compiled)  { setErr(err_buf, err_buf_size,
                               "already compiled"); return 0; }
    if (g->output_nodes_.empty())
    {
        setErr(err_buf, err_buf_size, "graph has no output nodes");
        return 0;
    }

    const int N = (int) g->nodes.size();

    // 1. Validate every required input port is connected.
    //    Mix / output: every port required.
    //    Plugin: input ports tolerate being unwired and are fed
    //    silence at render time. Instruments often expose a stereo
    //    audio input bus (JUCE convention) even though they're
    //    MIDI-driven; requiring users to wire silence in by hand
    //    is hostile UX.
    for (int i = 0; i < N; ++i)
    {
        const auto& n = g->nodes[(size_t) i];
        if (n.kind == MH_NODE_PLUGIN) continue;
        for (int port = 0; port < n.num_input_ports; ++port)
        {
            if (findEdge(g->edges, i, port) < 0)
            {
                setErrf(err_buf, err_buf_size,
                        "node %d input port %d is unconnected",
                        i, port);
                return 0;
            }
        }
        // MIDI sinks must have their required input ports connected.
        // MIDI_OUTPUT and MIDI_PROCESSOR: port 0 required.
        // MIDI_MERGE: every port 0..num_midi_input_ports-1 required.
        // Plugins with accepts_midi: optional (legacy
        // set_node_midi staging covers them when unconnected).
        if (n.kind == MH_NODE_MIDI_OUTPUT
            || n.kind == MH_NODE_MIDI_PROCESSOR
            || n.kind == MH_NODE_MIDI_MERGE)
        {
            for (int p = 0; p < n.num_midi_input_ports; ++p)
            {
                bool found = false;
                for (const auto& me : g->midi_edges)
                    if (me.dst_node == i && me.dst_port == p)
                    { found = true; break; }
                if (!found)
                {
                    setErrf(err_buf, err_buf_size,
                            "node %d MIDI input port %d has no incoming MIDI edge",
                            i, p);
                    return 0;
                }
            }
        }
    }

    // 2. Kahn topological sort. Indegree counts both audio and MIDI
    //    incoming edges so MIDI dependencies are respected.
    std::vector<int> indegree((size_t) N, 0);
    std::vector<std::vector<MH_NodeId>> succ((size_t) N);
    for (const auto& e : g->edges)
    {
        indegree[(size_t) e.dst_node]++;
        succ[(size_t) e.src_node].push_back(e.dst_node);
    }
    for (const auto& me : g->midi_edges)
    {
        indegree[(size_t) me.dst_node]++;
        succ[(size_t) me.src_node].push_back(me.dst_node);
    }
    std::vector<MH_NodeId> order;
    order.reserve((size_t) N);
    std::vector<MH_NodeId> ready;
    for (int i = 0; i < N; ++i)
        if (indegree[(size_t) i] == 0) ready.push_back(i);
    while (!ready.empty())
    {
        MH_NodeId u = ready.back();
        ready.pop_back();
        order.push_back(u);
        for (auto v : succ[(size_t) u])
            if (--indegree[(size_t) v] == 0) ready.push_back(v);
    }
    if ((int) order.size() != N)
    {
        setErr(err_buf, err_buf_size, "graph contains a cycle");
        return 0;
    }

    // 3. Allocate pool buffers for every non-input, non-output node.
    //    Input nodes alias caller buffers; output nodes write into
    //    caller buffers. We index pool entries on the node id directly
    //    for simplicity (some entries unused -- not a hot path).
    g->silence_buf.assign((size_t) g->max_block_size, 0.0f);
    g->pool_storage.assign((size_t) N, {});
    g->pool_ptrs.assign((size_t) N, {});
    for (int i = 0; i < N; ++i)
    {
        const auto& n = g->nodes[(size_t) i];
        if (n.kind == MH_NODE_PLUGIN
            || n.kind == MH_NODE_MIX
            || n.kind == MH_NODE_PICK_CHANNEL
            || n.kind == MH_NODE_MERGE_CHANNELS)
        {
            const int ch = n.output_channels;
            const int F  = g->max_block_size;
            g->pool_storage[(size_t) i].assign(
                (size_t) ch * (size_t) F, 0.0f);
            g->pool_ptrs[(size_t) i].resize((size_t) ch);
            for (int c = 0; c < ch; ++c)
                g->pool_ptrs[(size_t) i][(size_t) c]
                    = g->pool_storage[(size_t) i].data() + (size_t) c * F;
        }
    }

    // 4. Resolve every node's input_sources + output target.
    for (int i = 0; i < N; ++i)
    {
        auto& n = g->nodes[(size_t) i];
        for (int port = 0; port < n.num_input_ports; ++port)
        {
            int eidx = findEdge(g->edges, i, port);
            Node::InputRef ref{};
            if (eidx < 0)
            {
                // Only plugin ports are allowed to reach this point
                // unwired (step 1 rejected the others); mark them
                // silent so render feeds zeroes.
                ref.is_silent    = true;
                ref.from_caller  = false;
                ref.caller_index = -1;
                ref.pool_index   = -1;
                n.input_sources[(size_t) port] = ref;
                continue;
            }
            const auto& e = g->edges[(size_t) eidx];
            const auto& src = g->nodes[(size_t) e.src_node];
            if (src.kind == MH_NODE_INPUT)
            {
                ref.from_caller  = true;
                ref.caller_index = src.io_index;
                ref.pool_index   = -1;
            }
            else
            {
                ref.from_caller  = false;
                ref.caller_index = -1;
                ref.pool_index   = e.src_node;
            }
            n.input_sources[(size_t) port] = ref;
        }
        if (n.kind == MH_NODE_OUTPUT)
        {
            n.out_to_caller    = true;
            n.out_caller_index = n.io_index;
            n.out_pool_index   = -1;
        }
        else if (n.kind == MH_NODE_PLUGIN
                 || n.kind == MH_NODE_MIX
                 || n.kind == MH_NODE_PICK_CHANNEL
                 || n.kind == MH_NODE_MERGE_CHANNELS)
        {
            n.out_to_caller    = false;
            n.out_caller_index = -1;
            n.out_pool_index   = i;
        }
    }

    // 5. Resolve MIDI routing: per dst node, cache the upstream src.
    //    Mark sources that have any outgoing MIDI edge so we know
    //    whether to allocate a midi_out capture buffer for them.
    for (const auto& me : g->midi_edges)
    {
        auto& dn = g->nodes[(size_t) me.dst_node];
        dn.midi_srcs[(size_t) me.dst_port] = me.src_node;
        g->nodes[(size_t) me.src_node].has_outgoing_midi_edge = true;
    }

    // 6. Allocate per-node MIDI output buffers:
    //    - MIDI_INPUT  : always (caller stages into staged_midi_events;
    //                    midi_out_buf unused, but allocated for safety)
    //    - PLUGIN with produces_midi=1 and has_outgoing_midi_edge=true:
    //                    capture MIDI from mh_process_midi_io
    //    - MIDI_OUTPUT : holds events copied from upstream this block,
    //                    drained by get_midi_output_events.
    for (int i = 0; i < N; ++i)
    {
        auto& n = g->nodes[(size_t) i];
        bool needs_buf = false;
        if (n.kind == MH_NODE_MIDI_OUTPUT) needs_buf = true;
        else if (n.kind == MH_NODE_PLUGIN
                 && n.produces_midi
                 && n.has_outgoing_midi_edge) needs_buf = true;
        else if ((n.kind == MH_NODE_MIDI_PROCESSOR
                  || n.kind == MH_NODE_MIDI_MERGE)
                 && n.has_outgoing_midi_edge) needs_buf = true;
        if (needs_buf)
            n.midi_out_buf.assign((size_t) kMidiBufCapacity, MH_MidiEvent{});
    }

    g->schedule = std::move(order);
    g->pending_midi.assign((size_t) N, MH_GraphV2::PendingMidi{nullptr, 0});
    g->pending_auto.assign((size_t) N, MH_GraphV2::PendingAuto{nullptr, 0});
    g->compiled = true;
    return 1;
}

extern "C" int mh_graph_v2_set_node_midi(MH_GraphV2* g, MH_NodeId node,
                                         const MH_MidiEvent* events,
                                         int num_events)
{
    if (g == nullptr) return 0;
    if (!g->compiled) return 0;
    if (!inRange(node, (int) g->nodes.size())) return 0;
    if (g->nodes[(size_t) node].kind != MH_NODE_PLUGIN) return 0;
    g->pending_midi[(size_t) node].events = events;
    g->pending_midi[(size_t) node].count  = events ? num_events : 0;
    return 1;
}

extern "C" int mh_graph_v2_set_midi_input_events(MH_GraphV2* g, MH_NodeId node,
                                                 const MH_MidiEvent* events,
                                                 int num_events)
{
    if (g == nullptr) return 0;
    if (!g->compiled) return 0;
    if (!inRange(node, (int) g->nodes.size())) return 0;
    auto& n = g->nodes[(size_t) node];
    if (n.kind != MH_NODE_MIDI_INPUT) return 0;
    n.staged_midi_events = events;
    n.staged_midi_count  = events ? num_events : 0;
    return 1;
}

extern "C" int mh_graph_v2_get_midi_output_events(MH_GraphV2* g, MH_NodeId node,
                                                  MH_MidiEvent* out_buf,
                                                  int capacity,
                                                  int* num_events_out)
{
    if (g == nullptr) return 0;
    if (!g->compiled) return 0;
    if (!inRange(node, (int) g->nodes.size())) return 0;
    auto& n = g->nodes[(size_t) node];
    if (n.kind != MH_NODE_MIDI_OUTPUT) return 0;
    const int total = n.midi_out_truncated_count;
    if (num_events_out) *num_events_out = total;
    if (out_buf != nullptr && capacity > 0)
    {
        const int to_copy = total < capacity ? total : capacity;
        for (int i = 0; i < to_copy; ++i)
            out_buf[i] = n.midi_out_buf[(size_t) i];
    }
    return 1;
}

extern "C" int mh_graph_v2_set_node_automation(MH_GraphV2* g, MH_NodeId node,
                                               const MH_ParamChange* changes,
                                               int num_changes)
{
    if (g == nullptr) return 0;
    if (!g->compiled) return 0;
    if (!inRange(node, (int) g->nodes.size())) return 0;
    if (g->nodes[(size_t) node].kind != MH_NODE_PLUGIN) return 0;
    g->pending_auto[(size_t) node].changes = changes;
    g->pending_auto[(size_t) node].count
        = changes ? num_changes : 0;
    return 1;
}

extern "C" int mh_graph_v2_render_block(MH_GraphV2* g,
                                        const float* const* const* input_buffers,
                                        int num_input_nodes,
                                        float* const* const* output_buffers,
                                        int num_output_nodes,
                                        int nframes)
{
    if (g == nullptr || !g->compiled) return 0;
    if (nframes <= 0 || nframes > g->max_block_size) return 0;
    if (num_input_nodes  != (int) g->input_nodes_.size())  return 0;
    if (num_output_nodes != (int) g->output_nodes_.size()) return 0;
    if (num_input_nodes  > 0 && input_buffers  == nullptr) return 0;
    if (num_output_nodes > 0 && output_buffers == nullptr) return 0;

    for (MH_NodeId id : g->schedule)
    {
        Node& n = g->nodes[(size_t) id];

        switch (n.kind)
        {
        case MH_NODE_INPUT:
            // No work: the buffer lives in the caller; downstream
            // nodes read straight from input_buffers[io_index].
            break;

        case MH_NODE_OUTPUT:
        {
            // Copy from upstream into caller's output buffer.
            const auto& ref = n.input_sources[0];
            float* const* dst = output_buffers[(size_t) n.io_index];
            const int ch = n.input_channels;
            if (ref.from_caller)
            {
                const float* const* src
                    = input_buffers[(size_t) ref.caller_index];
                for (int c = 0; c < ch; ++c)
                    std::memcpy(dst[c], src[c],
                                (size_t) nframes * sizeof(float));
            }
            else
            {
                const auto& src = g->pool_ptrs[(size_t) ref.pool_index];
                for (int c = 0; c < ch; ++c)
                    std::memcpy(dst[c], src[(size_t) c],
                                (size_t) nframes * sizeof(float));
            }
            break;
        }

        case MH_NODE_PLUGIN:
        {
            const int in_ch  = n.input_channels;
            const int out_ch = n.output_channels;

            // Build planar pointer arrays for mh_process. Instruments
            // (in_ch == 0, num_input_ports == 0) have no audio input
            // and skip the input-resolution step entirely; mh_process
            // accepts a null inputs pointer table in that case.
            const float* in_ptrs[64];
            const int kMaxCh = 64;
            if (in_ch > kMaxCh) return 0;
            if (n.num_input_ports > 0)
            {
                const auto& ref = n.input_sources[0];
                if (ref.is_silent)
                {
                    // Unwired: every channel reads from the shared
                    // zero-filled silence buffer.
                    for (int c = 0; c < in_ch; ++c)
                        in_ptrs[c] = g->silence_buf.data();
                }
                else if (ref.from_caller)
                {
                    const float* const* src
                        = input_buffers[(size_t) ref.caller_index];
                    for (int c = 0; c < in_ch; ++c) in_ptrs[c] = src[c];
                }
                else
                {
                    const auto& src = g->pool_ptrs[(size_t) ref.pool_index];
                    for (int c = 0; c < in_ch; ++c)
                        in_ptrs[c] = src[(size_t) c];
                }
            }

            float** out_ptrs_raw = g->pool_ptrs[(size_t) id].data();
            (void) out_ch;

            // Resolve MIDI input: incoming MIDI edge (port 0) wins;
            // otherwise fall back to events staged via set_node_midi
            // (legacy).
            const MH_MidiEvent* midi_in_evts = nullptr;
            int                 midi_in_n    = 0;
            const MH_NodeId midi_src0
                = (!n.midi_srcs.empty()) ? n.midi_srcs[0] : -1;
            if (midi_src0 >= 0)
            {
                const Node& s = g->nodes[(size_t) midi_src0];
                if (s.kind == MH_NODE_MIDI_INPUT)
                {
                    midi_in_evts = s.staged_midi_events;
                    midi_in_n    = s.staged_midi_count;
                }
                else
                {
                    // Plugin / processor / merge source: events live in
                    // s.midi_out_buf, populated earlier in topo.
                    midi_in_evts = s.midi_out_buf.empty()
                                       ? nullptr : s.midi_out_buf.data();
                    midi_in_n    = s.midi_out_count;
                }
            }
            else
            {
                const auto& pm = g->pending_midi[(size_t) id];
                midi_in_evts = pm.events;
                midi_in_n    = pm.count;
            }

            // Determine if we need to capture this plugin's MIDI
            // output (downstream consumer wired up).
            const bool capture_midi_out
                = n.produces_midi && n.has_outgoing_midi_edge;
            MH_MidiEvent* midi_out_ptr = nullptr;
            int           midi_out_cap = 0;
            int           midi_out_n   = 0;
            if (capture_midi_out)
            {
                midi_out_ptr = n.midi_out_buf.data();
                midi_out_cap = (int) n.midi_out_buf.size();
            }

            const auto& autm = g->pending_auto[(size_t) id];
            int r;
            if (autm.count > 0)
                r = mh_process_auto(
                        n.plugin, in_ptrs, out_ptrs_raw, nframes,
                        midi_in_evts, midi_in_n,
                        midi_out_ptr, midi_out_cap,
                        capture_midi_out ? &midi_out_n : nullptr,
                        autm.changes, autm.count);
            else if (capture_midi_out)
                r = mh_process_midi_io(n.plugin, in_ptrs, out_ptrs_raw,
                                       nframes,
                                       midi_in_evts, midi_in_n,
                                       midi_out_ptr, midi_out_cap,
                                       &midi_out_n);
            else if (midi_in_n > 0)
                r = mh_process_midi(n.plugin, in_ptrs, out_ptrs_raw,
                                    nframes, midi_in_evts, midi_in_n);
            else
                r = mh_process(n.plugin, in_ptrs, out_ptrs_raw, nframes);
            if (!r) return 0;
            if (capture_midi_out)
            {
                // mh_process_midi_io / mh_process_auto write up to
                // capacity; we store the (possibly truncated) count
                // and treat n.midi_out_buf[0..count) as live events.
                n.midi_out_count           = midi_out_n;
                n.midi_out_truncated_count = midi_out_n;
            }
            break;
        }

        case MH_NODE_MIDI_INPUT:
            // No work at render time. Events live in
            // n.staged_midi_events for downstream consumers.
            break;

        case MH_NODE_MIDI_OUTPUT:
        {
            // Copy from upstream MIDI source into our buffer for
            // caller retrieval via get_midi_output_events.
            n.midi_out_count           = 0;
            n.midi_out_truncated_count = 0;
            const MH_NodeId src_id
                = (!n.midi_srcs.empty()) ? n.midi_srcs[0] : -1;
            if (src_id < 0) break;
            const Node& s = g->nodes[(size_t) src_id];
            const MH_MidiEvent* src_evts = nullptr;
            int                 src_n    = 0;
            if (s.kind == MH_NODE_MIDI_INPUT)
            {
                src_evts = s.staged_midi_events;
                src_n    = s.staged_midi_count;
            }
            else
            {
                src_evts = s.midi_out_buf.empty()
                               ? nullptr : s.midi_out_buf.data();
                src_n    = s.midi_out_count;
            }
            n.midi_out_truncated_count = src_n;
            const int cap = (int) n.midi_out_buf.size();
            const int to_copy = src_n < cap ? src_n : cap;
            for (int i = 0; i < to_copy; ++i)
                n.midi_out_buf[(size_t) i] = src_evts[i];
            n.midi_out_count = to_copy;
            break;
        }

        case MH_NODE_MIDI_PROCESSOR:
        {
            n.midi_out_count           = 0;
            n.midi_out_truncated_count = 0;
            // Resolve upstream events (port 0).
            const MH_NodeId src_id
                = (!n.midi_srcs.empty()) ? n.midi_srcs[0] : -1;
            if (src_id < 0 || !n.has_outgoing_midi_edge) break;
            const Node& s = g->nodes[(size_t) src_id];
            const MH_MidiEvent* src_evts = nullptr;
            int src_n = 0;
            if (s.kind == MH_NODE_MIDI_INPUT)
            {
                src_evts = s.staged_midi_events;
                src_n    = s.staged_midi_count;
            }
            else
            {
                src_evts = s.midi_out_buf.empty()
                               ? nullptr : s.midi_out_buf.data();
                src_n    = s.midi_out_count;
            }

            const auto& p   = n.midi_processor_params;
            const int   cap = (int) n.midi_out_buf.size();
            int         w   = 0;
            auto push = [&](const MH_MidiEvent& e) {
                if (w < cap) n.midi_out_buf[(size_t) w] = e;
                ++w;
            };
            for (int i = 0; i < src_n; ++i)
            {
                MH_MidiEvent e = src_evts[i];
                const unsigned char status_hi = (unsigned char)(e.status & 0xF0);
                const int channel             = e.status & 0x0F;
                const bool is_note_on  = (status_hi == 0x90);
                const bool is_note_off = (status_hi == 0x80);
                const bool is_sys      = ((unsigned char) e.status >= 0xF0);

                switch (p.op)
                {
                case MH_MIDI_OP_FILTER:
                {
                    if (is_sys) { push(e); break; }
                    // Channel filter applies to all channel-voice messages.
                    if (((p.channel_mask >> channel) & 1) == 0) break;
                    // Note range filter only applies to note on/off.
                    if (is_note_on || is_note_off)
                    {
                        const int note = e.data1 & 0x7F;
                        if (note < p.min_note || note > p.max_note) break;
                    }
                    push(e);
                    break;
                }
                case MH_MIDI_OP_TRANSPOSE:
                {
                    if (is_note_on || is_note_off)
                    {
                        const int new_note
                            = (e.data1 & 0x7F) + p.transpose_semitones;
                        if (new_note < 0 || new_note > 127) break;
                        e.data1 = (unsigned char) new_note;
                    }
                    push(e);
                    break;
                }
                case MH_MIDI_OP_VELOCITY_CURVE:
                {
                    if (is_note_on && e.data2 > 0)
                    {
                        const float norm = (float) (e.data2 & 0x7F) / 127.0f;
                        const float curved = std::pow(norm,
                            p.velocity_gamma > 0.0f ? p.velocity_gamma : 1.0f);
                        int v = (int) std::lround(curved * 127.0f);
                        if (v < 1)   v = 1;
                        if (v > 127) v = 127;
                        e.data2 = (unsigned char) v;
                    }
                    push(e);
                    break;
                }
                }
            }
            n.midi_out_count           = (w < cap) ? w : cap;
            n.midi_out_truncated_count = w;
            break;
        }

        case MH_NODE_MIDI_MERGE:
        {
            n.midi_out_count           = 0;
            n.midi_out_truncated_count = 0;
            if (!n.has_outgoing_midi_edge) break;
            const int cap = (int) n.midi_out_buf.size();
            int w = 0;
            // Concatenate events from each input port, then stable
            // sort by sample_offset so downstream consumers see a
            // single ordered stream.
            for (int port = 0; port < n.num_midi_input_ports; ++port)
            {
                const MH_NodeId src_id = n.midi_srcs[(size_t) port];
                if (src_id < 0) continue;
                const Node& s = g->nodes[(size_t) src_id];
                const MH_MidiEvent* src_evts = nullptr;
                int src_n = 0;
                if (s.kind == MH_NODE_MIDI_INPUT)
                {
                    src_evts = s.staged_midi_events;
                    src_n    = s.staged_midi_count;
                }
                else
                {
                    src_evts = s.midi_out_buf.empty()
                                   ? nullptr : s.midi_out_buf.data();
                    src_n    = s.midi_out_count;
                }
                for (int i = 0; i < src_n; ++i)
                {
                    if (w < cap) n.midi_out_buf[(size_t) w] = src_evts[i];
                    ++w;
                }
            }
            const int kept = (w < cap) ? w : cap;
            // Stable sort by sample_offset (insertion sort -- typical
            // event counts per block are small and we want stability).
            for (int i = 1; i < kept; ++i)
            {
                MH_MidiEvent x = n.midi_out_buf[(size_t) i];
                int j = i - 1;
                while (j >= 0
                       && n.midi_out_buf[(size_t) j].sample_offset > x.sample_offset)
                {
                    n.midi_out_buf[(size_t) (j + 1)] = n.midi_out_buf[(size_t) j];
                    --j;
                }
                n.midi_out_buf[(size_t) (j + 1)] = x;
            }
            n.midi_out_count           = kept;
            n.midi_out_truncated_count = w;
            break;
        }

        case MH_NODE_MIX:
        {
            const int ch = n.output_channels;
            auto& outvec = g->pool_ptrs[(size_t) id];
            for (int c = 0; c < ch; ++c)
                std::memset(outvec[(size_t) c], 0,
                            (size_t) nframes * sizeof(float));
            for (int port = 0; port < n.num_input_ports; ++port)
            {
                const auto& ref  = n.input_sources[(size_t) port];
                const float gain = n.mix_gains[(size_t) port];
                for (int c = 0; c < ch; ++c)
                {
                    const float* s;
                    if (ref.from_caller)
                        s = input_buffers[(size_t) ref.caller_index][c];
                    else
                        s = g->pool_ptrs[(size_t) ref.pool_index][(size_t) c];
                    float* dst = outvec[(size_t) c];
                    for (int i = 0; i < nframes; ++i) dst[i] += s[i] * gain;
                }
            }
            break;
        }

        case MH_NODE_PICK_CHANNEL:
        {
            const auto& ref = n.input_sources[0];
            const float* src;
            if (ref.from_caller)
                src = input_buffers[(size_t) ref.caller_index]
                                   [n.pick_channel_index];
            else
                src = g->pool_ptrs[(size_t) ref.pool_index]
                                  [(size_t) n.pick_channel_index];
            float* dst = g->pool_ptrs[(size_t) id][0];
            std::memcpy(dst, src, (size_t) nframes * sizeof(float));
            break;
        }

        case MH_NODE_MERGE_CHANNELS:
        {
            auto& outvec = g->pool_ptrs[(size_t) id];
            for (int port = 0; port < n.num_input_ports; ++port)
            {
                const auto& ref = n.input_sources[(size_t) port];
                const float* src;
                if (ref.from_caller)
                    src = input_buffers[(size_t) ref.caller_index][0];
                else
                    src = g->pool_ptrs[(size_t) ref.pool_index][0];
                std::memcpy(outvec[(size_t) port], src,
                            (size_t) nframes * sizeof(float));
            }
            break;
        }
        }
    }
    // Clear pending MIDI / automation -- callers must re-stage
    // every block. MIDI_INPUT staging is also borrowed-pointer state
    // and must be re-staged each block. Note: MIDI_OUTPUT counts are
    // intentionally preserved until the caller drains them.
    for (auto& pm : g->pending_midi) { pm.events = nullptr; pm.count = 0; }
    for (auto& pa : g->pending_auto) { pa.changes = nullptr; pa.count = 0; }
    for (auto& node : g->nodes)
    {
        if (node.kind == MH_NODE_MIDI_INPUT)
        {
            node.staged_midi_events = nullptr;
            node.staged_midi_count  = 0;
        }
    }
    return 1;
}

extern "C" int mh_graph_v2_num_nodes(MH_GraphV2* g)
{ return g ? (int) g->nodes.size() : 0; }

extern "C" int mh_graph_v2_num_input_nodes(MH_GraphV2* g)
{ return g ? (int) g->input_nodes_.size() : 0; }

extern "C" int mh_graph_v2_num_output_nodes(MH_GraphV2* g)
{ return g ? (int) g->output_nodes_.size() : 0; }

extern "C" int mh_graph_v2_is_compiled(MH_GraphV2* g)
{ return (g && g->compiled) ? 1 : 0; }
