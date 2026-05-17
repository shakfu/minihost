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
    std::vector<MH_NodeId> input_nodes_;   // node ids in add order
    std::vector<MH_NodeId> output_nodes_;

    // Filled at compile.
    bool                    compiled = false;
    std::vector<MH_NodeId>  schedule;

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
    n.num_input_ports = 1;
    n.output_channels = info.num_output_ch;
    n.input_sources.resize(1);
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
    //    Plugin/output: port 0; mix: every port.
    for (int i = 0; i < N; ++i)
    {
        const auto& n = g->nodes[(size_t) i];
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
    }

    // 2. Kahn topological sort. Indegree = number of incoming edges.
    std::vector<int> indegree((size_t) N, 0);
    std::vector<std::vector<MH_NodeId>> succ((size_t) N);
    for (const auto& e : g->edges)
    {
        indegree[(size_t) e.dst_node]++;
        succ[(size_t) e.src_node].push_back(e.dst_node);
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
    g->pool_storage.assign((size_t) N, {});
    g->pool_ptrs.assign((size_t) N, {});
    for (int i = 0; i < N; ++i)
    {
        const auto& n = g->nodes[(size_t) i];
        if (n.kind == MH_NODE_PLUGIN || n.kind == MH_NODE_MIX)
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
            const auto& e = g->edges[(size_t) eidx];
            const auto& src = g->nodes[(size_t) e.src_node];
            Node::InputRef ref{};
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
        else if (n.kind == MH_NODE_PLUGIN || n.kind == MH_NODE_MIX)
        {
            n.out_to_caller    = false;
            n.out_caller_index = -1;
            n.out_pool_index   = i;
        }
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
            const auto& ref = n.input_sources[0];
            const int in_ch  = n.input_channels;
            const int out_ch = n.output_channels;

            // Build planar pointer arrays for mh_process. The pool
            // owns a vector<float*> we can reuse for outputs, but
            // inputs may come from either a caller buffer or a pool
            // buffer. Stack-alloc a small ptr table -- in_ch is
            // bounded by plugin channel count, typically 2-16.
            const float* in_ptrs[64];
            const int kMaxCh = 64;
            if (in_ch > kMaxCh) return 0;
            if (ref.from_caller)
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

            float** out_ptrs_raw = g->pool_ptrs[(size_t) id].data();
            (void) out_ch;
            const auto& midi = g->pending_midi[(size_t) id];
            const auto& autm = g->pending_auto[(size_t) id];
            int r;
            if (autm.count > 0)
                r = mh_process_auto(
                        n.plugin, in_ptrs, out_ptrs_raw, nframes,
                        midi.events, midi.count,
                        /*midi_out=*/nullptr,
                        /*midi_out_capacity=*/0,
                        /*num_midi_out=*/nullptr,
                        autm.changes, autm.count);
            else if (midi.count > 0)
                r = mh_process_midi(n.plugin, in_ptrs, out_ptrs_raw,
                                    nframes, midi.events, midi.count);
            else
                r = mh_process(n.plugin, in_ptrs, out_ptrs_raw, nframes);
            if (!r) return 0;
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
        }
    }
    // Clear pending MIDI / automation -- callers must re-stage
    // every block.
    for (auto& pm : g->pending_midi) { pm.events = nullptr; pm.count = 0; }
    for (auto& pa : g->pending_auto) { pa.changes = nullptr; pa.count = 0; }
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
