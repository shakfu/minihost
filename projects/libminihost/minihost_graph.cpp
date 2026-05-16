// minihost_graph.cpp
// Parallel plugin routing implementation.

#include "minihost_graph.h"
#include "minihost_chain.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

struct MH_PluginGraph
{
    int in_channels;
    int out_channels;
    int max_block_size;
    double sample_rate;

    std::vector<MH_PluginChain*> branches;
    std::vector<float> gains;

    // Per-branch scratch output buffer. Each branch writes here, then
    // the graph sums into the user's output. Pre-allocated at create
    // time so process is allocation-free.
    std::vector<std::vector<float>> branch_storage;
    std::vector<std::vector<float*>> branch_ptrs;
};

static void setErr(char* buf, size_t n, const char* msg)
{
    if (buf == nullptr || n == 0) return;
    std::snprintf(buf, n, "%s", msg);
}

MH_PluginGraph* mh_graph_create(int num_in_channels,
                                 int num_out_channels,
                                 int max_block_size,
                                 double sample_rate,
                                 char* err_buf,
                                 size_t err_buf_size)
{
    if (num_in_channels <= 0 || num_out_channels <= 0)
    {
        setErr(err_buf, err_buf_size,
               "Graph channel counts must be positive");
        return nullptr;
    }
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

    auto* graph = new MH_PluginGraph();
    graph->in_channels = num_in_channels;
    graph->out_channels = num_out_channels;
    graph->max_block_size = max_block_size;
    graph->sample_rate = sample_rate;
    return graph;
}

void mh_graph_close(MH_PluginGraph* graph)
{
    if (graph == nullptr) return;
    // Branches are owned by the caller -- do not close them.
    delete graph;
}

int mh_graph_add_branch(MH_PluginGraph* graph,
                         MH_PluginChain* chain,
                         float gain,
                         char* err_buf,
                         size_t err_buf_size)
{
    if (graph == nullptr) return -1;
    if (chain == nullptr)
    {
        setErr(err_buf, err_buf_size, "chain is NULL");
        return -1;
    }

    int br_in = mh_chain_get_num_input_channels(chain);
    int br_out = mh_chain_get_num_output_channels(chain);
    if (br_in != graph->in_channels)
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Branch input channels (%d) do not match graph "
                      "input channels (%d)",
                      br_in, graph->in_channels);
        setErr(err_buf, err_buf_size, msg);
        return -1;
    }
    if (br_out != graph->out_channels)
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Branch output channels (%d) do not match graph "
                      "output channels (%d)",
                      br_out, graph->out_channels);
        setErr(err_buf, err_buf_size, msg);
        return -1;
    }

    double br_sr = mh_chain_get_sample_rate(chain);
    if (std::fabs(br_sr - graph->sample_rate) > 0.1)
    {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Branch sample rate (%.1f) does not match graph "
                      "sample rate (%.1f)",
                      br_sr, graph->sample_rate);
        setErr(err_buf, err_buf_size, msg);
        return -1;
    }

    graph->branches.push_back(chain);
    graph->gains.push_back(gain);

    // Allocate scratch output buffer for this branch.
    graph->branch_storage.emplace_back(
        graph->out_channels * graph->max_block_size, 0.0f);
    auto& storage = graph->branch_storage.back();
    graph->branch_ptrs.emplace_back();
    auto& ptrs = graph->branch_ptrs.back();
    ptrs.resize(graph->out_channels);
    for (int c = 0; c < graph->out_channels; ++c)
        ptrs[c] = storage.data() + c * graph->max_block_size;

    return static_cast<int>(graph->branches.size()) - 1;
}

int mh_graph_set_branch_gain(MH_PluginGraph* graph, int branch_index, float gain)
{
    if (graph == nullptr) return 0;
    if (branch_index < 0 ||
        branch_index >= static_cast<int>(graph->branches.size()))
        return 0;
    graph->gains[branch_index] = gain;
    return 1;
}

float mh_graph_get_branch_gain(MH_PluginGraph* graph, int branch_index)
{
    if (graph == nullptr) return std::numeric_limits<float>::quiet_NaN();
    if (branch_index < 0 ||
        branch_index >= static_cast<int>(graph->branches.size()))
        return std::numeric_limits<float>::quiet_NaN();
    return graph->gains[branch_index];
}

int mh_graph_get_num_branches(MH_PluginGraph* graph)
{
    if (graph == nullptr) return 0;
    return static_cast<int>(graph->branches.size());
}

int mh_graph_process(MH_PluginGraph* graph,
                      const float* const* inputs,
                      float* const* outputs,
                      int nframes)
{
    if (graph == nullptr) return 0;
    if (nframes <= 0 || nframes > graph->max_block_size) return 0;
    if (outputs == nullptr) return 0;

    int n_branches = static_cast<int>(graph->branches.size());
    int out_ch = graph->out_channels;

    // Zero the user output first so the summed result starts clean.
    for (int c = 0; c < out_ch; ++c)
    {
        if (outputs[c])
            std::memset(outputs[c], 0, sizeof(float) * nframes);
    }

    if (n_branches == 0) return 1;

    for (int b = 0; b < n_branches; ++b)
    {
        float gain = graph->gains[b];
        if (gain == 0.0f)
            continue;  // muted branch -- skip processing entirely

        float* const* branch_out = graph->branch_ptrs[b].data();
        int r = mh_chain_process(graph->branches[b], inputs,
                                  branch_out, nframes);
        if (!r) return 0;

        // Sum branch_out * gain into outputs.
        for (int c = 0; c < out_ch; ++c)
        {
            if (!outputs[c]) continue;
            const float* src = branch_out[c];
            float* dst = outputs[c];
            for (int n = 0; n < nframes; ++n)
                dst[n] += gain * src[n];
        }
    }

    return 1;
}

int mh_graph_get_num_input_channels(MH_PluginGraph* graph)
{
    return graph ? graph->in_channels : 0;
}

int mh_graph_get_num_output_channels(MH_PluginGraph* graph)
{
    return graph ? graph->out_channels : 0;
}

double mh_graph_get_sample_rate(MH_PluginGraph* graph)
{
    return graph ? graph->sample_rate : 0.0;
}

int mh_graph_get_max_block_size(MH_PluginGraph* graph)
{
    return graph ? graph->max_block_size : 0;
}

int mh_graph_get_latency_samples(MH_PluginGraph* graph)
{
    if (graph == nullptr) return 0;
    int max_latency = 0;
    for (auto* chain : graph->branches)
    {
        int l = mh_chain_get_latency_samples(chain);
        if (l > max_latency) max_latency = l;
    }
    return max_latency;
}

double mh_graph_get_tail_seconds(MH_PluginGraph* graph)
{
    if (graph == nullptr) return 0.0;
    double max_tail = 0.0;
    for (auto* chain : graph->branches)
    {
        double t = mh_chain_get_tail_seconds(chain);
        if (t > max_tail) max_tail = t;
    }
    return max_tail;
}
