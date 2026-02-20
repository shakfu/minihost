// minihost_chain.cpp
// Plugin chaining implementation

#include "minihost_chain.h"
#include "minihost.h"

#include <vector>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>

struct MH_PluginChain
{
    std::vector<MH_Plugin*> plugins;

    // Pre-allocated intermediate buffers (n-1 for n plugins)
    // Each intermediate buffer has storage for all channels
    std::vector<std::vector<float>> intermediate_storage;
    std::vector<std::vector<float*>> intermediate_ptrs;

    // Channel counts at each stage
    std::vector<int> stage_channels;

    int max_block_size;
    double sample_rate;
    int num_input_channels;   // First plugin's input channels
    int num_output_channels;  // Last plugin's output channels
};

static void setErr(char* buf, size_t n, const char* msg)
{
    if (buf == nullptr || n == 0) return;
    std::snprintf(buf, n, "%s", msg);
}

MH_PluginChain* mh_chain_create(MH_Plugin** plugins, int num_plugins,
                                 char* err_buf, size_t err_buf_size)
{
    if (plugins == nullptr || num_plugins <= 0)
    {
        setErr(err_buf, err_buf_size, "Plugin array is empty or null");
        return nullptr;
    }

    // Validate all plugins are non-null
    for (int i = 0; i < num_plugins; ++i)
    {
        if (plugins[i] == nullptr)
        {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "Plugin at index %d is null", i);
            setErr(err_buf, err_buf_size, msg);
            return nullptr;
        }
    }

    // Check all plugins have the same sample rate
    double sample_rate = mh_get_sample_rate(plugins[0]);
    for (int i = 1; i < num_plugins; ++i)
    {
        double rate = mh_get_sample_rate(plugins[i]);
        if (std::fabs(rate - sample_rate) > 0.1)  // Allow tiny floating point differences
        {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                         "Sample rate mismatch: plugin 0 has %.0f Hz, plugin %d has %.0f Hz",
                         sample_rate, i, rate);
            setErr(err_buf, err_buf_size, msg);
            return nullptr;
        }
    }

    // Get info for each plugin to determine channel counts
    std::vector<MH_Info> infos(num_plugins);
    for (int i = 0; i < num_plugins; ++i)
    {
        if (!mh_get_info(plugins[i], &infos[i]))
        {
            char msg[256];
            std::snprintf(msg, sizeof(msg), "Failed to get info for plugin %d", i);
            setErr(err_buf, err_buf_size, msg);
            return nullptr;
        }
    }

    // Determine intermediate buffer sizes
    // Between plugin i and i+1, we need max(plugin[i].out_ch, plugin[i+1].in_ch)
    // to handle both cases where we need to truncate or zero-pad
    std::vector<int> inter_channels;
    for (int i = 0; i < num_plugins - 1; ++i)
    {
        int out_ch = infos[i].num_output_ch;
        int in_ch = infos[i + 1].num_input_ch;
        inter_channels.push_back(std::max(out_ch, in_ch));
    }

    // Create the chain
    auto* chain = new MH_PluginChain();
    chain->plugins.assign(plugins, plugins + num_plugins);
    chain->sample_rate = sample_rate;
    chain->num_input_channels = infos[0].num_input_ch;
    chain->num_output_channels = infos[num_plugins - 1].num_output_ch;

    // Determine max block size (minimum across all plugins to be safe)
    // For now, we don't have a way to query this from plugins, so we use a reasonable default
    // In practice, the caller should ensure all plugins were opened with compatible block sizes
    chain->max_block_size = 8192;  // Large enough for most use cases

    // Store stage channel counts (outputs of each plugin)
    for (int i = 0; i < num_plugins; ++i)
    {
        chain->stage_channels.push_back(infos[i].num_output_ch);
    }

    // Allocate intermediate buffers (n-1 buffers for n plugins)
    chain->intermediate_storage.resize(num_plugins - 1);
    chain->intermediate_ptrs.resize(num_plugins - 1);

    for (int i = 0; i < num_plugins - 1; ++i)
    {
        int channels = inter_channels[i];
        int buffer_size = channels * chain->max_block_size;

        chain->intermediate_storage[i].resize(buffer_size, 0.0f);
        chain->intermediate_ptrs[i].resize(channels);

        // Set up channel pointers
        for (int ch = 0; ch < channels; ++ch)
        {
            chain->intermediate_ptrs[i][ch] =
                chain->intermediate_storage[i].data() + ch * chain->max_block_size;
        }
    }

    return chain;
}

void mh_chain_close(MH_PluginChain* chain)
{
    if (chain == nullptr) return;
    // Note: We do NOT close the individual plugins - caller owns them
    delete chain;
}

int mh_chain_process(MH_PluginChain* chain,
                     const float* const* inputs,
                     float* const* outputs,
                     int nframes)
{
    return mh_chain_process_midi_io(chain, inputs, outputs, nframes,
                                    nullptr, 0, nullptr, 0, nullptr);
}

int mh_chain_process_midi_io(MH_PluginChain* chain,
                             const float* const* inputs,
                             float* const* outputs,
                             int nframes,
                             const MH_MidiEvent* midi_in,
                             int num_midi_in,
                             MH_MidiEvent* midi_out,
                             int midi_out_capacity,
                             int* num_midi_out)
{
    if (chain == nullptr) return 0;
    if (nframes <= 0 || nframes > chain->max_block_size) return 0;

    int num_plugins = static_cast<int>(chain->plugins.size());
    if (num_plugins == 0) return 0;

    // Initialize midi output count
    if (num_midi_out)
        *num_midi_out = 0;

    // Special case: single plugin
    if (num_plugins == 1)
    {
        return mh_process_midi_io(chain->plugins[0],
                                  inputs, outputs, nframes,
                                  midi_in, num_midi_in,
                                  midi_out, midi_out_capacity, num_midi_out);
    }

    // Multi-plugin chain processing

    // Process first plugin with MIDI -> intermediate[0]
    float* const* first_output = chain->intermediate_ptrs[0].data();
    int result = mh_process_midi_io(chain->plugins[0],
                                    inputs, first_output, nframes,
                                    midi_in, num_midi_in,
                                    midi_out, midi_out_capacity, num_midi_out);
    if (!result) return 0;

    // Process middle plugins (no MIDI): intermediate[i-1] -> intermediate[i]
    for (int i = 1; i < num_plugins - 1; ++i)
    {
        const float* const* in_ptrs =
            const_cast<const float* const*>(chain->intermediate_ptrs[i - 1].data());
        float* const* out_ptrs = chain->intermediate_ptrs[i].data();

        // Get channel info for this stage
        MH_Info prev_info, curr_info;
        mh_get_info(chain->plugins[i - 1], &prev_info);
        mh_get_info(chain->plugins[i], &curr_info);

        // Handle channel mismatch: zero extra input channels if needed
        int prev_out_ch = prev_info.num_output_ch;
        int curr_in_ch = curr_info.num_input_ch;

        if (curr_in_ch > prev_out_ch)
        {
            // Zero-pad extra input channels
            for (int ch = prev_out_ch; ch < curr_in_ch; ++ch)
            {
                std::memset(chain->intermediate_ptrs[i - 1][ch], 0,
                           sizeof(float) * nframes);
            }
        }

        result = mh_process(chain->plugins[i], in_ptrs, out_ptrs, nframes);
        if (!result) return 0;
    }

    // Process last plugin: intermediate[n-2] -> outputs
    const float* const* last_input =
        const_cast<const float* const*>(chain->intermediate_ptrs[num_plugins - 2].data());

    // Handle channel mismatch for last plugin
    MH_Info prev_info, last_info;
    mh_get_info(chain->plugins[num_plugins - 2], &prev_info);
    mh_get_info(chain->plugins[num_plugins - 1], &last_info);

    int prev_out_ch = prev_info.num_output_ch;
    int last_in_ch = last_info.num_input_ch;

    if (last_in_ch > prev_out_ch)
    {
        // Zero-pad extra input channels
        for (int ch = prev_out_ch; ch < last_in_ch; ++ch)
        {
            std::memset(chain->intermediate_ptrs[num_plugins - 2][ch], 0,
                       sizeof(float) * nframes);
        }
    }

    result = mh_process(chain->plugins[num_plugins - 1], last_input, outputs, nframes);
    return result;
}

int mh_chain_get_latency_samples(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0;

    int total_latency = 0;
    for (auto* plugin : chain->plugins)
    {
        total_latency += mh_get_latency_samples(plugin);
    }
    return total_latency;
}

int mh_chain_get_num_plugins(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0;
    return static_cast<int>(chain->plugins.size());
}

MH_Plugin* mh_chain_get_plugin(MH_PluginChain* chain, int index)
{
    if (chain == nullptr) return nullptr;
    if (index < 0 || index >= static_cast<int>(chain->plugins.size())) return nullptr;
    return chain->plugins[index];
}

int mh_chain_get_num_input_channels(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0;
    return chain->num_input_channels;
}

int mh_chain_get_num_output_channels(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0;
    return chain->num_output_channels;
}

double mh_chain_get_sample_rate(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0.0;
    return chain->sample_rate;
}

int mh_chain_get_max_block_size(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0;
    return chain->max_block_size;
}

int mh_chain_reset(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0;

    for (auto* plugin : chain->plugins)
    {
        if (!mh_reset(plugin))
            return 0;
    }
    return 1;
}

int mh_chain_set_non_realtime(MH_PluginChain* chain, int non_realtime)
{
    if (chain == nullptr) return 0;

    for (auto* plugin : chain->plugins)
    {
        if (!mh_set_non_realtime(plugin, non_realtime))
            return 0;
    }
    return 1;
}

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
                           int num_param_changes)
{
    if (chain == nullptr) return 0;
    if (nframes <= 0 || nframes > chain->max_block_size) return 0;

    // Initialize MIDI output count
    if (num_midi_out)
        *num_midi_out = 0;

    // Fast path: no param changes, delegate directly
    if (!param_changes || num_param_changes <= 0)
    {
        return mh_chain_process_midi_io(chain, inputs, outputs, nframes,
                                         midi_in, num_midi_in,
                                         midi_out, midi_out_capacity, num_midi_out);
    }

    int midi_out_idx = 0;
    int current_sample = 0;
    int midi_idx = 0;
    int param_idx = 0;

    int in_ch = chain->num_input_channels;
    int out_ch = chain->num_output_channels;

    while (current_sample < nframes)
    {
        // Find next chunk boundary (next unique sample_offset or end of buffer)
        int chunk_end = nframes;
        if (param_idx < num_param_changes)
        {
            int next_change = param_changes[param_idx].sample_offset;
            if (next_change > current_sample && next_change < chunk_end)
                chunk_end = next_change;
        }

        // Apply all parameter changes at or before current_sample
        while (param_idx < num_param_changes &&
               param_changes[param_idx].sample_offset <= current_sample)
        {
            const auto& pc = param_changes[param_idx];
            MH_Plugin* plugin = mh_chain_get_plugin(chain, pc.plugin_index);
            if (plugin)
            {
                mh_set_param(plugin, pc.param_index, pc.value);
            }
            ++param_idx;
        }

        int chunk_size = chunk_end - current_sample;
        if (chunk_size <= 0)
            break;

        // Prepare offset input/output pointers for this chunk
        std::vector<const float*> chunk_inputs(in_ch);
        std::vector<float*> chunk_outputs(out_ch);
        for (int ch = 0; ch < in_ch; ++ch)
            chunk_inputs[ch] = inputs ? inputs[ch] + current_sample : nullptr;
        for (int ch = 0; ch < out_ch; ++ch)
            chunk_outputs[ch] = outputs ? outputs[ch] + current_sample : nullptr;

        // Collect MIDI events for this chunk (adjust offsets to chunk-local)
        std::vector<MH_MidiEvent> chunk_midi;
        while (midi_idx < num_midi_in)
        {
            const auto& ev = midi_in[midi_idx];
            if (ev.sample_offset >= chunk_end)
                break;
            if (ev.sample_offset >= current_sample)
            {
                MH_MidiEvent local_ev = ev;
                local_ev.sample_offset = ev.sample_offset - current_sample;
                chunk_midi.push_back(local_ev);
            }
            ++midi_idx;
        }

        // Process chunk through the chain
        std::vector<MH_MidiEvent> chunk_midi_out(256);
        int chunk_num_midi_out = 0;

        int result = mh_chain_process_midi_io(
            chain,
            chunk_inputs.empty() ? inputs : chunk_inputs.data(),
            chunk_outputs.empty() ? outputs : chunk_outputs.data(),
            chunk_size,
            chunk_midi.empty() ? nullptr : chunk_midi.data(),
            static_cast<int>(chunk_midi.size()),
            midi_out ? chunk_midi_out.data() : nullptr,
            midi_out ? 256 : 0,
            midi_out ? &chunk_num_midi_out : nullptr);

        if (!result) return 0;

        // Collect MIDI output with globally-adjusted offsets
        if (midi_out && midi_out_capacity > 0)
        {
            for (int i = 0; i < chunk_num_midi_out; ++i)
            {
                if (midi_out_idx >= midi_out_capacity)
                    break;
                midi_out[midi_out_idx] = chunk_midi_out[i];
                midi_out[midi_out_idx].sample_offset += current_sample;
                ++midi_out_idx;
            }
        }

        current_sample = chunk_end;
    }

    if (num_midi_out)
        *num_midi_out = midi_out_idx;

    return 1;
}

double mh_chain_get_tail_seconds(MH_PluginChain* chain)
{
    if (chain == nullptr) return 0.0;

    double max_tail = 0.0;
    for (auto* plugin : chain->plugins)
    {
        double tail = mh_get_tail_seconds(plugin);
        if (tail > max_tail)
            max_tail = tail;
    }
    return max_tail;
}
