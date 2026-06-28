// minihost_graph.cpp
// Parallel plugin routing implementation.

#include "minihost_graph.h"
#include "minihost_chain.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

struct MH_PluginBus
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

MH_PluginBus* mh_bus_create(int num_in_channels,
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

    auto* graph = new MH_PluginBus();
    graph->in_channels = num_in_channels;
    graph->out_channels = num_out_channels;
    graph->max_block_size = max_block_size;
    graph->sample_rate = sample_rate;
    return graph;
}

void mh_bus_close(MH_PluginBus* graph)
{
    if (graph == nullptr) return;
    // Branches are owned by the caller -- do not close them.
    delete graph;
}

int mh_bus_add_branch(MH_PluginBus* graph,
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

int mh_bus_set_branch_gain(MH_PluginBus* graph, int branch_index, float gain)
{
    if (graph == nullptr) return 0;
    if (branch_index < 0 ||
        branch_index >= static_cast<int>(graph->branches.size()))
        return 0;
    graph->gains[branch_index] = gain;
    return 1;
}

float mh_bus_get_branch_gain(MH_PluginBus* graph, int branch_index)
{
    if (graph == nullptr) return std::numeric_limits<float>::quiet_NaN();
    if (branch_index < 0 ||
        branch_index >= static_cast<int>(graph->branches.size()))
        return std::numeric_limits<float>::quiet_NaN();
    return graph->gains[branch_index];
}

int mh_bus_get_num_branches(MH_PluginBus* graph)
{
    if (graph == nullptr) return 0;
    return static_cast<int>(graph->branches.size());
}

// Shared fan-out-and-sum core for the audio-only and MIDI variants.
// When midi_in is non-NULL (and num_midi_in > 0) each branch is driven
// via mh_chain_process_midi_io so the same MIDI reaches every branch.
// When midi_out is non-NULL (capacity > 0) each branch's MIDI output is
// appended and then stably merge-sorted by sample_offset; otherwise
// branch MIDI output is discarded. The audio fan-out-and-sum is
// identical in every case.
static int graph_process_impl(MH_PluginBus* graph,
                              const float* const* inputs,
                              float* const* outputs,
                              int nframes,
                              const MH_MidiEvent* midi_in,
                              int num_midi_in,
                              MH_MidiEvent* midi_out,
                              int midi_out_capacity,
                              int* num_midi_out,
                              int* midi_out_overflow)
{
    // Default the MIDI-out reports up front so every early return is sane.
    if (num_midi_out) *num_midi_out = 0;
    if (midi_out_overflow) *midi_out_overflow = 0;

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

    const bool have_midi = (midi_in != nullptr && num_midi_in > 0);
    const bool collect_midi = (midi_out != nullptr && midi_out_capacity > 0);

    // Branch MIDI is appended directly into the caller's midi_out buffer
    // (no internal allocation), then stably sorted once at the end.
    int total_midi_out = 0;

    for (int b = 0; b < n_branches; ++b)
    {
        float gain = graph->gains[b];
        if (gain == 0.0f)
            continue;  // muted branch -- skip processing entirely

        float* const* branch_out = graph->branch_ptrs[b].data();
        int r;
        if (have_midi || collect_midi)
        {
            // Fan the same MIDI to every branch. When collecting, append
            // this branch's MIDI output into the remaining tail of the
            // caller's buffer; mh_chain_process_midi_io caps writes at the
            // capacity we hand it, so the buffer never overflows.
            MH_MidiEvent* branch_midi_out =
                collect_midi ? (midi_out + total_midi_out) : nullptr;
            int branch_cap =
                collect_midi ? (midi_out_capacity - total_midi_out) : 0;
            int branch_count = 0;
            r = mh_chain_process_midi_io(graph->branches[b], inputs,
                                         branch_out, nframes,
                                         midi_in, num_midi_in,
                                         branch_midi_out, branch_cap,
                                         collect_midi ? &branch_count : nullptr);
            if (collect_midi)
            {
                total_midi_out += branch_count;
                // The chain filled all the room we gave it: this branch (or
                // a later one) may have produced more than fit.
                if (branch_count == branch_cap &&
                    total_midi_out == midi_out_capacity &&
                    midi_out_overflow)
                {
                    *midi_out_overflow = 1;
                }
            }
        }
        else
        {
            r = mh_chain_process(graph->branches[b], inputs,
                                 branch_out, nframes);
        }
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

    if (collect_midi && total_midi_out > 1)
    {
        // Stable insertion sort by sample_offset. Allocation-free and
        // stable (uses '>' so equal-offset events keep branch order),
        // which matters for note-off/note-on pairs at the same frame.
        // Event counts per block are small, so O(n^2) is acceptable on
        // the audio thread.
        for (int i = 1; i < total_midi_out; ++i)
        {
            MH_MidiEvent key = midi_out[i];
            int j = i - 1;
            while (j >= 0 && midi_out[j].sample_offset > key.sample_offset)
            {
                midi_out[j + 1] = midi_out[j];
                --j;
            }
            midi_out[j + 1] = key;
        }
    }

    if (num_midi_out) *num_midi_out = total_midi_out;
    return 1;
}

int mh_bus_process(MH_PluginBus* graph,
                      const float* const* inputs,
                      float* const* outputs,
                      int nframes)
{
    return graph_process_impl(graph, inputs, outputs, nframes,
                              nullptr, 0, nullptr, 0, nullptr, nullptr);
}

int mh_bus_process_midi(MH_PluginBus* graph,
                          const float* const* inputs,
                          float* const* outputs,
                          int nframes,
                          const MH_MidiEvent* midi_in,
                          int num_midi_in)
{
    return graph_process_impl(graph, inputs, outputs, nframes,
                              midi_in, num_midi_in,
                              nullptr, 0, nullptr, nullptr);
}

int mh_bus_process_midi_io(MH_PluginBus* graph,
                           const float* const* inputs,
                           float* const* outputs,
                           int nframes,
                           const MH_MidiEvent* midi_in,
                           int num_midi_in,
                           MH_MidiEvent* midi_out,
                           int midi_out_capacity,
                           int* num_midi_out,
                           int* midi_out_overflow)
{
    return graph_process_impl(graph, inputs, outputs, nframes,
                              midi_in, num_midi_in,
                              midi_out, midi_out_capacity,
                              num_midi_out, midi_out_overflow);
}

int mh_bus_get_num_input_channels(MH_PluginBus* graph)
{
    return graph ? graph->in_channels : 0;
}

int mh_bus_get_num_output_channels(MH_PluginBus* graph)
{
    return graph ? graph->out_channels : 0;
}

double mh_bus_get_sample_rate(MH_PluginBus* graph)
{
    return graph ? graph->sample_rate : 0.0;
}

int mh_bus_get_max_block_size(MH_PluginBus* graph)
{
    return graph ? graph->max_block_size : 0;
}

int mh_bus_get_latency_samples(MH_PluginBus* graph)
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

double mh_bus_get_tail_seconds(MH_PluginBus* graph)
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
