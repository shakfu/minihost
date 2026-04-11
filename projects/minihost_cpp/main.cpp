// main.cpp - CLI frontend for minihost
// Provides command-line access to plugin hosting features

#include "minihost.h"
#include "minihost_audio.h"
#include "minihost_audiofile.h"
#include "minihost_vstpreset.h"
#include "MidiFile.h"
#include "MidiEvent.h"
#include "include/CLI11.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <iomanip>
#include <iostream>

// ============================================================================
// Helper functions
// ============================================================================

static void print_error(const char* msg) {
    std::fprintf(stderr, "Error: %s\n", msg);
}

static void print_plugin_desc(const MH_PluginDesc& desc, bool verbose = false) {
    std::printf("Name:      %s\n", desc.name);
    std::printf("Vendor:    %s\n", desc.vendor);
    std::printf("Version:   %s\n", desc.version);
    std::printf("Format:    %s\n", desc.format);
    std::printf("ID:        %s\n", desc.unique_id);
    if (desc.path[0] != '\0') {
        std::printf("Path:      %s\n", desc.path);
    }
    if (verbose) {
        std::printf("MIDI In:   %s\n", desc.accepts_midi ? "yes" : "no");
        std::printf("MIDI Out:  %s\n", desc.produces_midi ? "yes" : "no");
        std::printf("Inputs:    %d\n", desc.num_inputs);
        std::printf("Outputs:   %d\n", desc.num_outputs);
    }
}

static void print_param_info(int index, const MH_ParamInfo& info, float current_value) {
    std::printf("  [%3d] %-30s = %.4f", index, info.name, current_value);
    if (info.label[0] != '\0') {
        std::printf(" %s", info.label);
    }
    std::printf(" (%s)\n", info.current_value_str);
}

static void print_bus_info(int index, bool is_input, const MH_BusInfo& info) {
    std::printf("  [%d] %-20s  %d ch  %s%s\n",
                index,
                info.name,
                info.num_channels,
                info.is_main ? "[main]" : "",
                info.is_enabled ? "" : " (disabled)");
}

// Detect audio file by extension
static bool is_audio_file(const std::string& path) {
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    std::string ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wav" || ext == ".flac" || ext == ".mp3" || ext == ".ogg";
}

// Parse "Name:value" or "index:value" parameter specification
// Returns true on success, sets out_index and out_value
static bool parse_param_spec(MH_Plugin* p, const std::string& spec,
                             int& out_index, float& out_value) {
    auto colon_pos = spec.find(':');
    if (colon_pos == std::string::npos) return false;

    std::string name_part = spec.substr(0, colon_pos);
    std::string value_part = spec.substr(colon_pos + 1);

    try {
        out_value = std::stof(value_part);
    } catch (...) {
        return false;
    }

    // Try as numeric index first
    bool is_numeric = !name_part.empty();
    for (char c : name_part) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            is_numeric = false;
            break;
        }
    }

    if (is_numeric) {
        out_index = std::stoi(name_part);
        return out_index >= 0 && out_index < mh_get_num_params(p);
    }

    // Try as parameter name (case-insensitive substring match)
    int num_params = mh_get_num_params(p);
    for (int i = 0; i < num_params; i++) {
        MH_ParamInfo info;
        if (mh_get_param_info(p, i, &info)) {
            if (name_part == info.name) {
                out_index = i;
                return true;
            }
        }
    }

    return false;
}

// Interleaved float32 buffer helper
struct AudioBuffer {
    std::vector<float> interleaved;  // interleaved samples
    int channels = 0;
    int frames = 0;
    int sample_rate = 0;

    // Deinterleave to per-channel pointers
    void deinterleave(std::vector<std::vector<float>>& ch_data) const {
        ch_data.resize(channels);
        for (int c = 0; c < channels; c++) {
            ch_data[c].resize(frames);
            for (int f = 0; f < frames; f++) {
                ch_data[c][f] = interleaved[f * channels + c];
            }
        }
    }

    // Interleave from per-channel data
    static void interleave_from(const std::vector<float*>& ch_ptrs,
                                int ch, int n_frames,
                                std::vector<float>& out) {
        out.resize(ch * n_frames);
        for (int f = 0; f < n_frames; f++) {
            for (int c = 0; c < ch; c++) {
                out[f * ch + c] = ch_ptrs[c][f];
            }
        }
    }
};

// Read an audio file into an AudioBuffer
static bool read_audio_file(const std::string& path, AudioBuffer& buf) {
    char err[1024] = {0};
    MH_AudioData* data = mh_audio_read(path.c_str(), err, sizeof(err));
    if (!data) {
        print_error(err);
        return false;
    }
    buf.channels = static_cast<int>(data->channels);
    buf.frames = static_cast<int>(data->frames);
    buf.sample_rate = static_cast<int>(data->sample_rate);
    buf.interleaved.assign(data->data, data->data + (size_t)buf.channels * buf.frames);
    mh_audio_data_free(data);
    return true;
}

// MIDI event with absolute sample position (for sorting/dispatching)
struct SampleMidiEvent {
    int sample_pos;
    unsigned char status;
    unsigned char data1;
    unsigned char data2;
};

// Load a MIDI file and convert events to sample-positioned MH_MidiEvents
static bool load_midi_file(const std::string& path, double sample_rate,
                           std::vector<SampleMidiEvent>& events,
                           int& total_samples) {
    smf::MidiFile midifile;
    if (!midifile.read(path)) {
        std::fprintf(stderr, "Error: Failed to read MIDI file: %s\n", path.c_str());
        return false;
    }

    midifile.doTimeAnalysis();
    midifile.joinTracks();
    midifile.makeAbsoluteTicks();

    events.clear();
    total_samples = 0;

    int num_events = midifile[0].getEventCount();
    for (int i = 0; i < num_events; i++) {
        smf::MidiEvent& ev = midifile[0][i];

        // Skip meta events
        if (ev.isMetaMessage()) continue;

        double seconds = midifile.getTimeInSeconds(0, i);
        int sample_pos = static_cast<int>(seconds * sample_rate);

        SampleMidiEvent sev;
        sev.sample_pos = sample_pos;
        sev.status = ev[0];
        sev.data1 = ev.getSize() > 1 ? ev[1] : 0;
        sev.data2 = ev.getSize() > 2 ? ev[2] : 0;
        events.push_back(sev);

        if (sample_pos > total_samples) {
            total_samples = sample_pos;
        }
    }

    std::sort(events.begin(), events.end(),
              [](const SampleMidiEvent& a, const SampleMidiEvent& b) {
                  return a.sample_pos < b.sample_pos;
              });

    return true;
}

// ============================================================================
// Command: probe
// ============================================================================

int cmd_probe(const std::string& plugin_path, bool json_output) {
    MH_PluginDesc desc;
    char err[1024] = {0};

    if (!mh_probe(plugin_path.c_str(), &desc, err, sizeof(err))) {
        print_error(err);
        return 1;
    }

    if (json_output) {
        std::printf("{\n");
        std::printf("  \"name\": \"%s\",\n", desc.name);
        std::printf("  \"vendor\": \"%s\",\n", desc.vendor);
        std::printf("  \"version\": \"%s\",\n", desc.version);
        std::printf("  \"format\": \"%s\",\n", desc.format);
        std::printf("  \"unique_id\": \"%s\",\n", desc.unique_id);
        std::printf("  \"accepts_midi\": %s,\n", desc.accepts_midi ? "true" : "false");
        std::printf("  \"produces_midi\": %s,\n", desc.produces_midi ? "true" : "false");
        std::printf("  \"num_inputs\": %d,\n", desc.num_inputs);
        std::printf("  \"num_outputs\": %d\n", desc.num_outputs);
        std::printf("}\n");
    } else {
        print_plugin_desc(desc, true);
    }

    return 0;
}

// ============================================================================
// Command: scan
// ============================================================================

struct ScanContext {
    bool json;
    int count;
    bool first;
};

static void scan_callback(const MH_PluginDesc* desc, void* user_data) {
    auto* ctx = static_cast<ScanContext*>(user_data);

    if (ctx->json) {
        if (!ctx->first) std::printf(",\n");
        ctx->first = false;
        std::printf("  {\n");
        std::printf("    \"name\": \"%s\",\n", desc->name);
        std::printf("    \"vendor\": \"%s\",\n", desc->vendor);
        std::printf("    \"format\": \"%s\",\n", desc->format);
        std::printf("    \"path\": \"%s\"\n", desc->path);
        std::printf("  }");
    } else {
        std::printf("[%d] %s (%s) - %s\n",
                    ctx->count + 1,
                    desc->name,
                    desc->format,
                    desc->path);
    }
    ctx->count++;
}

int cmd_scan(const std::string& directory, bool json_output) {
    ScanContext ctx{json_output, 0, true};

    if (json_output) {
        std::printf("[\n");
    }

    int result = mh_scan_directory(directory.c_str(), scan_callback, &ctx);

    if (json_output) {
        if (ctx.count > 0) std::printf("\n");
        std::printf("]\n");
    }

    if (result < 0) {
        print_error("Failed to scan directory");
        return 1;
    }

    if (!json_output) {
        std::printf("\nFound %d plugin(s)\n", ctx.count);
    }

    return 0;
}

// ============================================================================
// Command: info
// ============================================================================

int cmd_info(const std::string& plugin_path,
             double sample_rate,
             int block_size,
             bool probe_only,
             bool json_output) {
    char err[1024] = {0};

    // Probe-only mode: lightweight metadata without full load
    if (probe_only) {
        return cmd_probe(plugin_path, json_output);
    }

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    MH_Info info;
    mh_get_info(p, &info);

    // Get plugin metadata via probe
    MH_PluginDesc desc;
    bool have_desc = mh_probe(plugin_path.c_str(), &desc, err, sizeof(err));

    if (json_output) {
        std::printf("{\n");
        if (have_desc) {
            std::printf("  \"name\": \"%s\",\n", desc.name);
            std::printf("  \"vendor\": \"%s\",\n", desc.vendor);
            std::printf("  \"version\": \"%s\",\n", desc.version);
            std::printf("  \"format\": \"%s\",\n", desc.format);
            std::printf("  \"unique_id\": \"%s\",\n", desc.unique_id);
            std::printf("  \"accepts_midi\": %s,\n", desc.accepts_midi ? "true" : "false");
            std::printf("  \"produces_midi\": %s,\n", desc.produces_midi ? "true" : "false");
            std::printf("  \"num_inputs\": %d,\n", desc.num_inputs);
            std::printf("  \"num_outputs\": %d,\n", desc.num_outputs);
        }
        std::printf("  \"sample_rate\": %.0f,\n", mh_get_sample_rate(p));
        std::printf("  \"num_params\": %d,\n", info.num_params);
        std::printf("  \"num_input_channels\": %d,\n", info.num_input_ch);
        std::printf("  \"num_output_channels\": %d,\n", info.num_output_ch);
        std::printf("  \"latency_samples\": %d,\n", info.latency_samples);
        std::printf("  \"tail_seconds\": %.3f,\n", mh_get_tail_seconds(p));
        std::printf("  \"supports_double\": %s,\n", mh_supports_double(p) ? "true" : "false");
        std::printf("  \"num_programs\": %d\n", mh_get_num_programs(p));
        std::printf("}\n");
        mh_close(p);
        return 0;
    }

    if (have_desc) {
        print_plugin_desc(desc, true);
    }

    std::printf("\nRuntime Info:\n");
    std::printf("  Sample Rate:    %.0f Hz\n", mh_get_sample_rate(p));
    std::printf("  Parameters:     %d\n", info.num_params);
    std::printf("  Input Ch:       %d\n", info.num_input_ch);
    std::printf("  Output Ch:      %d\n", info.num_output_ch);
    std::printf("  Latency:        %d samples\n", info.latency_samples);
    std::printf("  Tail:           %.3f s\n", mh_get_tail_seconds(p));
    std::printf("  Double Prec:    %s\n", mh_supports_double(p) ? "yes" : "no");

    // Bus info
    int num_in_buses = mh_get_num_buses(p, 1);
    int num_out_buses = mh_get_num_buses(p, 0);

    if (num_in_buses > 0) {
        std::printf("\nInput Buses:\n");
        for (int i = 0; i < num_in_buses; i++) {
            MH_BusInfo bus;
            if (mh_get_bus_info(p, 1, i, &bus)) {
                print_bus_info(i, true, bus);
            }
        }
    }

    if (num_out_buses > 0) {
        std::printf("\nOutput Buses:\n");
        for (int i = 0; i < num_out_buses; i++) {
            MH_BusInfo bus;
            if (mh_get_bus_info(p, 0, i, &bus)) {
                print_bus_info(i, false, bus);
            }
        }
    }

    // Factory presets
    int num_programs = mh_get_num_programs(p);
    if (num_programs > 0) {
        std::printf("\nFactory Presets: %d\n", num_programs);
        int current = mh_get_program(p);
        for (int i = 0; i < std::min(num_programs, 10); i++) {
            char name[256] = {0};
            mh_get_program_name(p, i, name, sizeof(name));
            std::printf("  [%d] %s%s\n", i, name, (i == current) ? " (current)" : "");
        }
        if (num_programs > 10) {
            std::printf("  ... and %d more\n", num_programs - 10);
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: params
// ============================================================================

int cmd_params(const std::string& plugin_path,
               double sample_rate,
               int block_size,
               bool json_output,
               bool verbose) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    int num_params = mh_get_num_params(p);

    if (json_output) {
        std::printf("[\n");
        for (int i = 0; i < num_params; i++) {
            MH_ParamInfo info;
            if (mh_get_param_info(p, i, &info)) {
                float value = mh_get_param(p, i);
                if (i > 0) std::printf(",\n");
                std::printf("  {\n");
                std::printf("    \"index\": %d,\n", i);
                std::printf("    \"name\": \"%s\",\n", info.name);
                std::printf("    \"label\": \"%s\",\n", info.label);
                std::printf("    \"value\": %.6f,\n", value);
                std::printf("    \"value_str\": \"%s\",\n", info.current_value_str);
                std::printf("    \"default\": %.6f,\n", info.default_value);
                std::printf("    \"automatable\": %s,\n", info.is_automatable ? "true" : "false");
                std::printf("    \"boolean\": %s,\n", info.is_boolean ? "true" : "false");
                std::printf("    \"steps\": %d\n", info.num_steps);
                std::printf("  }");
            }
        }
        std::printf("\n]\n");
    } else if (verbose) {
        std::printf("Parameters (%d):\n", num_params);
        for (int i = 0; i < num_params; i++) {
            MH_ParamInfo info;
            if (mh_get_param_info(p, i, &info)) {
                float value = mh_get_param(p, i);

                char min_text[128] = {0};
                char max_text[128] = {0};
                char default_text[128] = {0};
                mh_param_to_text(p, i, 0.0f, min_text, sizeof(min_text));
                mh_param_to_text(p, i, 1.0f, max_text, sizeof(max_text));
                mh_param_to_text(p, i, info.default_value, default_text, sizeof(default_text));

                std::printf("  [%3d] %s\n", i, info.name);
                std::printf("         Value:   %.4f", value);
                if (info.label[0] != '\0') std::printf(" %s", info.label);
                std::printf(" (%s)\n", info.current_value_str);
                std::printf("         Range:   %s .. %s\n", min_text, max_text);
                std::printf("         Default: %.4f (%s)\n", info.default_value, default_text);

                // Flags
                std::string flags;
                if (info.is_automatable) {
                    if (!flags.empty()) flags += ", ";
                    flags += "automatable";
                }
                if (info.num_steps > 0) {
                    if (!flags.empty()) flags += ", ";
                    flags += "discrete, " + std::to_string(info.num_steps) + " steps";
                }
                if (!flags.empty()) {
                    std::printf("         Flags:   %s\n", flags.c_str());
                }
            }
        }
    } else {
        std::printf("Parameters (%d):\n", num_params);
        for (int i = 0; i < num_params; i++) {
            MH_ParamInfo info;
            if (mh_get_param_info(p, i, &info)) {
                float value = mh_get_param(p, i);
                print_param_info(i, info, value);
            }
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: get-param
// ============================================================================

int cmd_get_param(const std::string& plugin_path,
                  int param_index,
                  double sample_rate,
                  int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    int num_params = mh_get_num_params(p);
    if (param_index < 0 || param_index >= num_params) {
        std::fprintf(stderr, "Error: Parameter index %d out of range (0-%d)\n",
                     param_index, num_params - 1);
        mh_close(p);
        return 1;
    }

    MH_ParamInfo info;
    float value = mh_get_param(p, param_index);

    if (mh_get_param_info(p, param_index, &info)) {
        std::printf("%s = %.6f (%s)\n", info.name, value, info.current_value_str);
    } else {
        std::printf("%.6f\n", value);
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: set-param
// ============================================================================

int cmd_set_param(const std::string& plugin_path,
                  int param_index,
                  float param_value,
                  double sample_rate,
                  int block_size,
                  const std::string& state_file) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    // Load state if provided
    if (!state_file.empty()) {
        std::ifstream ifs(state_file, std::ios::binary);
        if (ifs) {
            std::vector<char> data((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
            if (!mh_set_state(p, data.data(), static_cast<int>(data.size()))) {
                std::fprintf(stderr, "Warning: Failed to load state from %s\n", state_file.c_str());
            }
        }
    }

    int num_params = mh_get_num_params(p);
    if (param_index < 0 || param_index >= num_params) {
        std::fprintf(stderr, "Error: Parameter index %d out of range (0-%d)\n",
                     param_index, num_params - 1);
        mh_close(p);
        return 1;
    }

    if (!mh_set_param(p, param_index, param_value)) {
        print_error("Failed to set parameter");
        mh_close(p);
        return 1;
    }

    // Show result
    MH_ParamInfo info;
    float new_value = mh_get_param(p, param_index);
    if (mh_get_param_info(p, param_index, &info)) {
        std::printf("%s = %.6f (%s)\n", info.name, new_value, info.current_value_str);
    }

    // Save state if file was provided
    if (!state_file.empty()) {
        int size = mh_get_state_size(p);
        if (size > 0) {
            std::vector<char> data(size);
            if (mh_get_state(p, data.data(), size)) {
                std::ofstream ofs(state_file, std::ios::binary);
                ofs.write(data.data(), size);
                std::printf("State saved to %s\n", state_file.c_str());
            }
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: devices
// ============================================================================

int cmd_devices(bool json_output) {
    int playback_count = mh_audio_enumerate_playback_devices(nullptr, 0);
    int capture_count = mh_audio_enumerate_capture_devices(nullptr, 0);
    if (playback_count < 0) playback_count = 0;
    if (capture_count < 0) capture_count = 0;

    std::vector<MH_AudioDeviceInfo> playback(playback_count);
    std::vector<MH_AudioDeviceInfo> capture(capture_count);
    if (playback_count > 0) {
        mh_audio_enumerate_playback_devices(playback.data(), playback_count);
    }
    if (capture_count > 0) {
        mh_audio_enumerate_capture_devices(capture.data(), capture_count);
    }

    if (json_output) {
        std::printf("{\n");
        std::printf("  \"playback\": [");
        for (int i = 0; i < playback_count; i++) {
            std::printf("%s\n    {\"index\": %d, \"name\": \"%s\", \"is_default\": %s}",
                        i == 0 ? "" : ",", i, playback[i].name,
                        playback[i].is_default ? "true" : "false");
        }
        std::printf("%s],\n", playback_count > 0 ? "\n  " : "");
        std::printf("  \"capture\": [");
        for (int i = 0; i < capture_count; i++) {
            std::printf("%s\n    {\"index\": %d, \"name\": \"%s\", \"is_default\": %s}",
                        i == 0 ? "" : ",", i, capture[i].name,
                        capture[i].is_default ? "true" : "false");
        }
        std::printf("%s]\n", capture_count > 0 ? "\n  " : "");
        std::printf("}\n");
    } else {
        std::printf("Audio Playback (Output) Devices:\n");
        if (playback_count == 0) {
            std::printf("  (none)\n");
        } else {
            for (int i = 0; i < playback_count; i++) {
                std::printf("  [%d] %s%s\n", i, playback[i].name,
                            playback[i].is_default ? " (default)" : "");
            }
        }
        std::printf("\nAudio Capture (Input) Devices:\n");
        if (capture_count == 0) {
            std::printf("  (none)\n");
        } else {
            for (int i = 0; i < capture_count; i++) {
                std::printf("  [%d] %s%s\n", i, capture[i].name,
                            capture[i].is_default ? " (default)" : "");
            }
        }
    }
    return 0;
}

// ============================================================================
// Command: presets
// ============================================================================

namespace {
bool file_exists_cpp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

int load_state_from_file_cpp(MH_Plugin* p, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::fprintf(stderr, "Error: Cannot open state file '%s'\n", path.c_str());
        return 0;
    }
    auto size = f.tellg();
    if (size <= 0) {
        std::fprintf(stderr, "Error: Empty state file '%s'\n", path.c_str());
        return 0;
    }
    f.seekg(0, std::ios::beg);
    std::vector<char> data(static_cast<size_t>(size));
    if (!f.read(data.data(), size)) {
        std::fprintf(stderr, "Error: Failed to read state file\n");
        return 0;
    }
    if (!mh_set_state(p, data.data(), static_cast<int>(size))) {
        std::fprintf(stderr, "Error: Failed to apply state\n");
        return 0;
    }
    return 1;
}
}  // namespace

int cmd_presets(const std::string& plugin_path,
                double sample_rate,
                int block_size,
                bool json_output,
                const std::string& save_file,
                int program_index,
                const std::string& state_file_input,
                const std::string& load_vstpreset_file,
                bool overwrite) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    std::string class_id;  // for save mode
    bool have_class_id = false;

    if (!state_file_input.empty()) {
        if (!load_state_from_file_cpp(p, state_file_input)) {
            mh_close(p);
            return 1;
        }
    }

    if (!load_vstpreset_file.empty()) {
        MH_VstPreset preset;
        char perr[512] = {0};
        if (!mh_vstpreset_read(load_vstpreset_file.c_str(), &preset, perr, sizeof(perr))) {
            std::fprintf(stderr, "Error loading .vstpreset '%s': %s\n",
                         load_vstpreset_file.c_str(), perr);
            mh_close(p);
            return 1;
        }
        if (!preset.component_state || preset.component_size == 0) {
            std::fprintf(stderr, "Error: preset '%s' has no component state\n",
                         load_vstpreset_file.c_str());
            mh_vstpreset_free(&preset);
            mh_close(p);
            return 1;
        }
        if (!mh_set_state(p, preset.component_state, preset.component_size)) {
            std::fprintf(stderr, "Error: Failed to apply preset state\n");
            mh_vstpreset_free(&preset);
            mh_close(p);
            return 1;
        }
        class_id = preset.class_id;
        have_class_id = true;
        mh_vstpreset_free(&preset);
    }

    if (program_index >= 0) {
        int num_programs = mh_get_num_programs(p);
        if (num_programs == 0) {
            std::fprintf(stderr, "Error: plugin has no factory presets\n");
            mh_close(p);
            return 1;
        }
        if (program_index >= num_programs) {
            std::fprintf(stderr, "Error: program %d out of range (0-%d)\n",
                         program_index, num_programs - 1);
            mh_close(p);
            return 1;
        }
        if (!mh_set_program(p, program_index)) {
            std::fprintf(stderr, "Error: Failed to select program %d\n", program_index);
            mh_close(p);
            return 1;
        }
    }

    // Save mode
    if (!save_file.empty()) {
        if (!overwrite && file_exists_cpp(save_file)) {
            std::fprintf(stderr,
                         "Error: Output file '%s' already exists. Use -y/--overwrite to overwrite.\n",
                         save_file.c_str());
            mh_close(p);
            return 1;
        }

        if (!have_class_id) {
            MH_PluginDesc desc;
            std::memset(&desc, 0, sizeof(desc));
            char probe_err[256] = {0};
            if (mh_probe(plugin_path.c_str(), &desc, probe_err, sizeof(probe_err))
                && desc.unique_id[0]) {
                class_id = desc.unique_id;
            } else {
                class_id = "minihost_unknown";
            }
        }

        int state_size = mh_get_state_size(p);
        if (state_size <= 0) {
            std::fprintf(stderr, "Error: Plugin has no state to save\n");
            mh_close(p);
            return 1;
        }
        std::vector<char> state(state_size);
        if (!mh_get_state(p, state.data(), state_size)) {
            std::fprintf(stderr, "Error: Failed to read plugin state\n");
            mh_close(p);
            return 1;
        }

        char werr[512] = {0};
        int ok = mh_vstpreset_write(save_file.c_str(), class_id.c_str(),
                                    state.data(), state_size,
                                    nullptr, 0,
                                    werr, sizeof(werr));
        if (!ok) {
            std::fprintf(stderr, "Error writing '%s': %s\n", save_file.c_str(), werr);
            mh_close(p);
            return 1;
        }
        std::printf("Wrote %s\n", save_file.c_str());
        mh_close(p);
        return 0;
    }

    // Listing mode
    int num_programs = mh_get_num_programs(p);
    int current = mh_get_program(p);

    if (json_output) {
        std::printf("{\n  \"count\": %d,\n  \"presets\": [", num_programs);
        for (int i = 0; i < num_programs; i++) {
            char name[256] = {0};
            mh_get_program_name(p, i, name, sizeof(name));
            std::printf("%s\n    {\"index\": %d, \"name\": \"%s\", \"is_current\": %s}",
                        i == 0 ? "" : ",", i, name,
                        i == current ? "true" : "false");
        }
        std::printf("%s]\n}\n", num_programs > 0 ? "\n  " : "");
    } else if (num_programs == 0) {
        std::printf("(no factory presets)\n");
    } else {
        std::printf("Factory Presets (%d):\n", num_programs);
        for (int i = 0; i < num_programs; i++) {
            char name[256] = {0};
            mh_get_program_name(p, i, name, sizeof(name));
            std::printf("  [%3d] %s%s\n", i, name, (i == current) ? " *" : "");
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: load-preset
// ============================================================================

int cmd_load_preset(const std::string& plugin_path,
                    int preset_index,
                    double sample_rate,
                    int block_size,
                    const std::string& state_file) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    int num_programs = mh_get_num_programs(p);
    if (preset_index < 0 || preset_index >= num_programs) {
        std::fprintf(stderr, "Error: Preset index %d out of range (0-%d)\n",
                     preset_index, num_programs - 1);
        mh_close(p);
        return 1;
    }

    if (!mh_set_program(p, preset_index)) {
        print_error("Failed to load preset");
        mh_close(p);
        return 1;
    }

    char name[256] = {0};
    mh_get_program_name(p, preset_index, name, sizeof(name));
    std::printf("Loaded preset [%d]: %s\n", preset_index, name);

    // Save state if file was provided
    if (!state_file.empty()) {
        int size = mh_get_state_size(p);
        if (size > 0) {
            std::vector<char> data(size);
            if (mh_get_state(p, data.data(), size)) {
                std::ofstream ofs(state_file, std::ios::binary);
                ofs.write(data.data(), size);
                std::printf("State saved to %s\n", state_file.c_str());
            }
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: save-state
// ============================================================================

int cmd_save_state(const std::string& plugin_path,
                   const std::string& state_file,
                   double sample_rate,
                   int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    int size = mh_get_state_size(p);
    if (size <= 0) {
        print_error("Plugin has no state to save");
        mh_close(p);
        return 1;
    }

    std::vector<char> data(size);
    if (!mh_get_state(p, data.data(), size)) {
        print_error("Failed to get plugin state");
        mh_close(p);
        return 1;
    }

    std::ofstream ofs(state_file, std::ios::binary);
    if (!ofs) {
        std::fprintf(stderr, "Error: Cannot open %s for writing\n", state_file.c_str());
        mh_close(p);
        return 1;
    }

    ofs.write(data.data(), size);
    std::printf("State saved to %s (%d bytes)\n", state_file.c_str(), size);

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: load-state
// ============================================================================

int cmd_load_state(const std::string& plugin_path,
                   const std::string& state_file,
                   double sample_rate,
                   int block_size,
                   bool show_params) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    std::ifstream ifs(state_file, std::ios::binary);
    if (!ifs) {
        std::fprintf(stderr, "Error: Cannot open %s for reading\n", state_file.c_str());
        mh_close(p);
        return 1;
    }

    std::vector<char> data((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

    if (!mh_set_state(p, data.data(), static_cast<int>(data.size()))) {
        print_error("Failed to restore plugin state");
        mh_close(p);
        return 1;
    }

    std::printf("State loaded from %s (%zu bytes)\n", state_file.c_str(), data.size());

    if (show_params) {
        int num_params = mh_get_num_params(p);
        std::printf("\nParameters after loading:\n");
        for (int i = 0; i < num_params; i++) {
            MH_ParamInfo info;
            if (mh_get_param_info(p, i, &info)) {
                float value = mh_get_param(p, i);
                print_param_info(i, info, value);
            }
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: process
// ============================================================================

int cmd_process(const std::string& plugin_path,
                const std::string& input_file,
                const std::string& output_file,
                const std::string& sidechain_file,
                const std::string& midi_file,
                double sample_rate,
                int block_size,
                const std::string& state_file,
                int preset_index,
                const std::vector<std::string>& param_specs,
                bool use_double,
                bool non_realtime,
                double bpm,
                int bit_depth,
                double tail_seconds) {
    char err[1024] = {0};

    bool has_audio_input = !input_file.empty();
    bool has_midi_input = !midi_file.empty();
    bool has_sidechain = !sidechain_file.empty();

    if (!has_audio_input && !has_midi_input) {
        print_error("At least one of input file or MIDI file is required");
        return 1;
    }

    // --- Read audio inputs ---
    AudioBuffer audio_in;
    if (has_audio_input) {
        if (is_audio_file(input_file)) {
            if (!read_audio_file(input_file, audio_in)) return 1;
            // Use input file's sample rate
            sample_rate = audio_in.sample_rate;
        } else {
            // Legacy raw float32 fallback
            std::ifstream ifs(input_file, std::ios::binary);
            if (!ifs) {
                std::fprintf(stderr, "Error: Cannot open input file %s\n", input_file.c_str());
                return 1;
            }
            ifs.seekg(0, std::ios::end);
            size_t file_size = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            // Assume stereo for raw files
            audio_in.channels = 2;
            audio_in.frames = static_cast<int>(file_size / (sizeof(float) * audio_in.channels));
            audio_in.sample_rate = static_cast<int>(sample_rate);
            audio_in.interleaved.resize(file_size / sizeof(float));
            ifs.read(reinterpret_cast<char*>(audio_in.interleaved.data()), file_size);
        }
    }

    AudioBuffer sidechain_in;
    if (has_sidechain) {
        if (!read_audio_file(sidechain_file, sidechain_in)) return 1;
    }

    // --- Load MIDI ---
    std::vector<SampleMidiEvent> midi_events;
    int midi_total_samples = 0;
    if (has_midi_input) {
        if (!load_midi_file(midi_file, sample_rate, midi_events, midi_total_samples)) {
            return 1;
        }
    }

    // --- Determine channel counts ---
    int in_ch = has_audio_input ? audio_in.channels : 2;
    int sidechain_ch = has_sidechain ? sidechain_in.channels : 0;

    // --- Open plugin ---
    MH_Plugin* p = nullptr;
    if (sidechain_ch > 0) {
        p = mh_open_ex(plugin_path.c_str(), sample_rate, block_size,
                       in_ch, 2, sidechain_ch, err, sizeof(err));
    } else {
        p = mh_open(plugin_path.c_str(), sample_rate, block_size, in_ch, 2, err, sizeof(err));
    }
    if (!p) {
        print_error(err);
        return 1;
    }

    // --- Load state ---
    if (!state_file.empty()) {
        std::ifstream ifs(state_file, std::ios::binary);
        if (ifs) {
            std::vector<char> data((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
            if (mh_set_state(p, data.data(), static_cast<int>(data.size()))) {
                std::fprintf(stderr, "Loaded state from %s\n", state_file.c_str());
            } else {
                std::fprintf(stderr, "Warning: Failed to load state from %s\n", state_file.c_str());
            }
        }
    }

    // --- Load preset ---
    if (preset_index >= 0) {
        int num_programs = mh_get_num_programs(p);
        if (preset_index >= num_programs) {
            std::fprintf(stderr, "Error: Preset index %d out of range (0-%d)\n",
                         preset_index, num_programs - 1);
            mh_close(p);
            return 1;
        }
        mh_set_program(p, preset_index);
        char name[256] = {0};
        mh_get_program_name(p, preset_index, name, sizeof(name));
        std::fprintf(stderr, "Loaded preset [%d]: %s\n", preset_index, name);
    }

    // --- Apply static parameter overrides ---
    std::vector<MH_ParamChange> param_changes;
    for (const auto& spec : param_specs) {
        int idx;
        float val;
        if (!parse_param_spec(p, spec, idx, val)) {
            std::fprintf(stderr, "Error: Invalid parameter spec '%s'\n", spec.c_str());
            mh_close(p);
            return 1;
        }
        // Apply as initial value and record for automation
        mh_set_param(p, idx, val);
        MH_ParamChange change;
        change.sample_offset = 0;
        change.param_index = idx;
        change.value = val;
        param_changes.push_back(change);
    }

    // --- Non-realtime mode ---
    if (non_realtime) {
        mh_set_non_realtime(p, 1);
    }

    // --- Transport ---
    if (bpm > 0) {
        MH_TransportInfo transport = {};
        transport.bpm = bpm;
        transport.time_sig_numerator = 4;
        transport.time_sig_denominator = 4;
        transport.is_playing = 1;
        mh_set_transport(p, &transport);
    }

    // --- Get plugin info ---
    MH_Info pinfo;
    mh_get_info(p, &pinfo);
    int out_ch = pinfo.num_output_ch > 0 ? pinfo.num_output_ch : 2;
    int latency = mh_get_latency_samples(p);

    // --- Calculate total processing length ---
    int total_samples = 0;
    if (has_audio_input) {
        total_samples = audio_in.frames;
    }
    if (has_midi_input) {
        int midi_end = midi_total_samples + static_cast<int>(tail_seconds * sample_rate);
        if (midi_end > total_samples) {
            total_samples = midi_end;
        }
    }

    if (total_samples == 0) {
        print_error("No audio or MIDI input data to process");
        mh_close(p);
        return 1;
    }

    int output_total = total_samples + latency;

    // --- Print summary ---
    std::fprintf(stderr, "Plugin: %s\n", plugin_path.c_str());
    std::fprintf(stderr, "  Sample rate: %.0f Hz\n", sample_rate);
    std::fprintf(stderr, "  Block size:  %d\n", block_size);
    std::fprintf(stderr, "  Latency:     %d samples\n", latency);
    if (has_audio_input) {
        std::fprintf(stderr, "  Input:       %d ch, %d samples\n", in_ch, audio_in.frames);
    }
    if (has_sidechain) {
        std::fprintf(stderr, "  Sidechain:   %d ch\n", sidechain_ch);
    }
    if (has_midi_input) {
        std::fprintf(stderr, "  MIDI events: %zu\n", midi_events.size());
    }
    if (!param_changes.empty()) {
        std::fprintf(stderr, "  Params:      %zu override(s)\n", param_changes.size());
    }
    std::fprintf(stderr, "  Output:      %d ch -> %s\n", out_ch, output_file.c_str());

    // --- Deinterleave audio inputs ---
    std::vector<std::vector<float>> in_channels;
    if (has_audio_input) {
        audio_in.deinterleave(in_channels);
        // Pad to output_total
        for (auto& ch : in_channels) {
            ch.resize(output_total, 0.0f);
        }
    } else {
        in_channels.resize(in_ch);
        for (auto& ch : in_channels) {
            ch.assign(output_total, 0.0f);
        }
    }

    std::vector<std::vector<float>> sc_channels;
    if (has_sidechain) {
        sidechain_in.deinterleave(sc_channels);
        for (auto& ch : sc_channels) {
            ch.resize(output_total, 0.0f);
        }
    }

    // --- Allocate output ---
    std::vector<std::vector<float>> out_channels(out_ch);
    for (auto& ch : out_channels) {
        ch.assign(output_total, 0.0f);
    }

    // --- Process loop ---
    size_t midi_idx = 0;
    bool has_param_automation = !param_changes.empty();

    for (int start = 0; start < output_total; start += block_size) {
        int end = std::min(start + block_size, output_total);
        int bsize = end - start;

        // Input pointers for this block
        std::vector<const float*> in_ptrs(in_ch);
        for (int c = 0; c < in_ch; c++) {
            in_ptrs[c] = in_channels[c].data() + start;
        }

        // Output pointers
        std::vector<float*> out_ptrs(out_ch);
        for (int c = 0; c < out_ch; c++) {
            out_ptrs[c] = out_channels[c].data() + start;
        }

        // Collect MIDI events for this block
        std::vector<MH_MidiEvent> block_midi;
        while (midi_idx < midi_events.size()) {
            const auto& ev = midi_events[midi_idx];
            if (ev.sample_pos >= end) break;
            MH_MidiEvent mev;
            mev.sample_offset = std::max(0, std::min(ev.sample_pos - start, bsize - 1));
            mev.status = ev.status;
            mev.data1 = ev.data1;
            mev.data2 = ev.data2;
            block_midi.push_back(mev);
            midi_idx++;
        }

        // Choose processing path
        if (has_sidechain) {
            std::vector<const float*> sc_ptrs(sidechain_ch);
            for (int c = 0; c < sidechain_ch; c++) {
                sc_ptrs[c] = sc_channels[c].data() + start;
            }
            mh_process_sidechain(p, in_ptrs.data(), out_ptrs.data(),
                                 sc_ptrs.data(), bsize);
        } else if (has_param_automation || !block_midi.empty()) {
            // Use process_auto for combined MIDI + param automation
            mh_process_auto(p,
                            in_ptrs.data(), out_ptrs.data(), bsize,
                            block_midi.empty() ? nullptr : block_midi.data(),
                            static_cast<int>(block_midi.size()),
                            nullptr, 0, nullptr,
                            // Only send param changes in first block
                            (start == 0 && has_param_automation) ? param_changes.data() : nullptr,
                            (start == 0 && has_param_automation) ? static_cast<int>(param_changes.size()) : 0);
        } else if (use_double && mh_supports_double(p)) {
            // Double precision path
            std::vector<std::vector<double>> in_d(in_ch, std::vector<double>(bsize));
            std::vector<std::vector<double>> out_d(out_ch, std::vector<double>(bsize));
            std::vector<const double*> in_d_ptrs(in_ch);
            std::vector<double*> out_d_ptrs(out_ch);
            for (int c = 0; c < in_ch; c++) {
                for (int f = 0; f < bsize; f++) in_d[c][f] = in_ptrs[c][f];
                in_d_ptrs[c] = in_d[c].data();
            }
            for (int c = 0; c < out_ch; c++) {
                out_d_ptrs[c] = out_d[c].data();
            }
            mh_process_double(p, in_d_ptrs.data(), out_d_ptrs.data(), bsize);
            for (int c = 0; c < out_ch; c++) {
                for (int f = 0; f < bsize; f++) out_ptrs[c][f] = static_cast<float>(out_d[c][f]);
            }
        } else {
            mh_process(p, in_ptrs.data(), out_ptrs.data(), bsize);
        }
    }

    // --- Latency compensation: trim leading latency samples ---
    int write_offset = latency;
    int write_frames = total_samples;
    if (write_offset + write_frames > output_total) {
        write_frames = output_total - write_offset;
    }

    // --- Write output ---
    if (is_audio_file(output_file)) {
        // Interleave output for audio file write
        std::vector<float> out_interleaved(static_cast<size_t>(out_ch) * write_frames);
        for (int f = 0; f < write_frames; f++) {
            for (int c = 0; c < out_ch; c++) {
                out_interleaved[f * out_ch + c] = out_channels[c][write_offset + f];
            }
        }

        if (bit_depth <= 0) bit_depth = 24;

        if (!mh_audio_write(output_file.c_str(), out_interleaved.data(),
                            static_cast<unsigned>(out_ch), static_cast<unsigned>(write_frames),
                            static_cast<unsigned>(sample_rate), bit_depth,
                            err, sizeof(err))) {
            print_error(err);
            mh_close(p);
            return 1;
        }
    } else {
        // Raw float32 output
        std::ofstream ofs(output_file, std::ios::binary);
        if (!ofs) {
            std::fprintf(stderr, "Error: Cannot open output file %s\n", output_file.c_str());
            mh_close(p);
            return 1;
        }
        std::vector<float> out_interleaved(static_cast<size_t>(out_ch) * write_frames);
        for (int f = 0; f < write_frames; f++) {
            for (int c = 0; c < out_ch; c++) {
                out_interleaved[f * out_ch + c] = out_channels[c][write_offset + f];
            }
        }
        ofs.write(reinterpret_cast<char*>(out_interleaved.data()),
                  static_cast<std::streamsize>(write_frames) * out_ch * sizeof(float));
    }

    double duration = static_cast<double>(write_frames) / sample_rate;
    std::fprintf(stderr, "Wrote %d samples (%.2fs) to %s\n",
                 write_frames, duration, output_file.c_str());

    mh_close(p);
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    CLI::App app{"minihost - Audio plugin hosting CLI"};
    app.require_subcommand(1);

    // Global options
    double sample_rate = 48000.0;
    int block_size = 512;

    app.add_option("-r,--rate", sample_rate, "Sample rate (Hz)")
        ->default_val(48000.0);
    app.add_option("-b,--block", block_size, "Block size (samples)")
        ->default_val(512);

    // ========================================================================
    // Subcommand: probe
    // ========================================================================
    auto* probe_cmd = app.add_subcommand("probe", "Get plugin metadata without loading");
    std::string probe_path;
    bool probe_json = false;

    probe_cmd->add_option("plugin", probe_path, "Path to plugin (.vst3 or .component)")
        ->required();
    probe_cmd->add_flag("-j,--json", probe_json, "Output as JSON");

    probe_cmd->callback([&]() {
        std::exit(cmd_probe(probe_path, probe_json));
    });

    // ========================================================================
    // Subcommand: scan
    // ========================================================================
    auto* scan_cmd = app.add_subcommand("scan", "Scan directory for plugins");
    std::string scan_dir;
    bool scan_json = false;

    scan_cmd->add_option("directory", scan_dir, "Directory to scan")
        ->required();
    scan_cmd->add_flag("-j,--json", scan_json, "Output as JSON");

    scan_cmd->callback([&]() {
        std::exit(cmd_scan(scan_dir, scan_json));
    });

    // ========================================================================
    // Subcommand: info
    // ========================================================================
    auto* info_cmd = app.add_subcommand("info", "Show detailed plugin information");
    std::string info_path;
    bool info_probe = false;
    bool info_json = false;

    info_cmd->add_option("plugin", info_path, "Path to plugin")
        ->required();
    info_cmd->add_flag("--probe", info_probe, "Lightweight mode: metadata only, no full load");
    info_cmd->add_flag("-j,--json", info_json, "Output as JSON");

    info_cmd->callback([&]() {
        std::exit(cmd_info(info_path, sample_rate, block_size, info_probe, info_json));
    });

    // ========================================================================
    // Subcommand: params
    // ========================================================================
    auto* params_cmd = app.add_subcommand("params", "List plugin parameters");
    std::string params_path;
    bool params_json = false;
    bool params_verbose = false;

    params_cmd->add_option("plugin", params_path, "Path to plugin")
        ->required();
    params_cmd->add_flag("-j,--json", params_json, "Output as JSON");
    params_cmd->add_flag("-V,--verbose", params_verbose, "Show extended info (ranges, defaults, flags)");

    params_cmd->callback([&]() {
        std::exit(cmd_params(params_path, sample_rate, block_size, params_json, params_verbose));
    });

    // ========================================================================
    // Subcommand: get-param
    // ========================================================================
    auto* get_param_cmd = app.add_subcommand("get-param", "Get parameter value");
    std::string get_param_path;
    int get_param_index = 0;

    get_param_cmd->add_option("plugin", get_param_path, "Path to plugin")
        ->required();
    get_param_cmd->add_option("index", get_param_index, "Parameter index")
        ->required();

    get_param_cmd->callback([&]() {
        std::exit(cmd_get_param(get_param_path, get_param_index, sample_rate, block_size));
    });

    // ========================================================================
    // Subcommand: set-param
    // ========================================================================
    auto* set_param_cmd = app.add_subcommand("set-param", "Set parameter value");
    std::string set_param_path;
    int set_param_index = 0;
    float set_param_value = 0.0f;
    std::string set_param_state;

    set_param_cmd->add_option("plugin", set_param_path, "Path to plugin")
        ->required();
    set_param_cmd->add_option("index", set_param_index, "Parameter index")
        ->required();
    set_param_cmd->add_option("value", set_param_value, "Parameter value (0.0-1.0)")
        ->required();
    set_param_cmd->add_option("-s,--state", set_param_state, "State file to load/save");

    set_param_cmd->callback([&]() {
        std::exit(cmd_set_param(set_param_path, set_param_index, set_param_value,
                                sample_rate, block_size, set_param_state));
    });

    // ========================================================================
    // Subcommand: presets
    // ========================================================================
    auto* presets_cmd = app.add_subcommand(
        "presets",
        "List factory presets, or save plugin state as .vstpreset");
    std::string presets_path;
    bool presets_json = false;
    std::string presets_save;
    int presets_program = -1;
    std::string presets_state;
    std::string presets_load_vstpreset;
    bool presets_overwrite = false;

    presets_cmd->add_option("plugin", presets_path, "Path to plugin")
        ->required();
    presets_cmd->add_flag("-j,--json", presets_json, "Output as JSON");
    presets_cmd->add_option("--save", presets_save,
                            "Write current state as .vstpreset to FILE");
    presets_cmd->add_option("--program", presets_program,
                            "Select factory program N before saving");
    presets_cmd->add_option("-s,--state", presets_state,
                            "Load raw state blob before saving");
    presets_cmd->add_option("--load-vstpreset", presets_load_vstpreset,
                            "Load .vstpreset before saving (preserves class_id)");
    presets_cmd->add_flag("-y,--overwrite", presets_overwrite,
                          "Overwrite --save output if it exists");

    presets_cmd->callback([&]() {
        std::exit(cmd_presets(presets_path, sample_rate, block_size,
                              presets_json, presets_save, presets_program,
                              presets_state, presets_load_vstpreset,
                              presets_overwrite));
    });

    // ========================================================================
    // Subcommand: devices
    // ========================================================================
    auto* devices_cmd = app.add_subcommand(
        "devices", "List audio playback/capture devices");
    bool devices_json = false;
    devices_cmd->add_flag("-j,--json", devices_json, "Output as JSON");
    devices_cmd->callback([&]() {
        std::exit(cmd_devices(devices_json));
    });

    // ========================================================================
    // Subcommand: load-preset
    // ========================================================================
    auto* load_preset_cmd = app.add_subcommand("load-preset", "Load factory preset");
    std::string load_preset_path;
    int load_preset_index = 0;
    std::string load_preset_state;

    load_preset_cmd->add_option("plugin", load_preset_path, "Path to plugin")
        ->required();
    load_preset_cmd->add_option("index", load_preset_index, "Preset index")
        ->required();
    load_preset_cmd->add_option("-s,--state", load_preset_state, "Save state to file");

    load_preset_cmd->callback([&]() {
        std::exit(cmd_load_preset(load_preset_path, load_preset_index,
                                  sample_rate, block_size, load_preset_state));
    });

    // ========================================================================
    // Subcommand: save-state
    // ========================================================================
    auto* save_state_cmd = app.add_subcommand("save-state", "Save plugin state to file");
    std::string save_state_plugin;
    std::string save_state_file;

    save_state_cmd->add_option("plugin", save_state_plugin, "Path to plugin")
        ->required();
    save_state_cmd->add_option("file", save_state_file, "Output state file")
        ->required();

    save_state_cmd->callback([&]() {
        std::exit(cmd_save_state(save_state_plugin, save_state_file, sample_rate, block_size));
    });

    // ========================================================================
    // Subcommand: load-state
    // ========================================================================
    auto* load_state_cmd = app.add_subcommand("load-state", "Load plugin state from file");
    std::string load_state_plugin;
    std::string load_state_file;
    bool load_state_params = false;

    load_state_cmd->add_option("plugin", load_state_plugin, "Path to plugin")
        ->required();
    load_state_cmd->add_option("file", load_state_file, "Input state file")
        ->required();
    load_state_cmd->add_flag("-p,--params", load_state_params, "Show parameters after loading");

    load_state_cmd->callback([&]() {
        std::exit(cmd_load_state(load_state_plugin, load_state_file,
                                 sample_rate, block_size, load_state_params));
    });

    // ========================================================================
    // Subcommand: process
    // ========================================================================
    auto* process_cmd = app.add_subcommand("process", "Process audio through plugin");
    std::string process_plugin;
    std::string process_input;
    std::string process_output;
    std::string process_sidechain;
    std::string process_midi;
    std::string process_state;
    int process_preset = -1;
    std::vector<std::string> process_params;
    bool process_double = false;
    bool process_nrt = false;
    double process_bpm = 0.0;
    int process_bit_depth = 0;
    double process_tail = 2.0;

    process_cmd->add_option("plugin", process_plugin, "Path to plugin")
        ->required();
    process_cmd->add_option("-i,--input", process_input, "Input audio file");
    process_cmd->add_option("-o,--output", process_output, "Output audio file")
        ->required();
    process_cmd->add_option("--sidechain", process_sidechain, "Sidechain input audio file");
    process_cmd->add_option("-m,--midi-input", process_midi, "Input MIDI file");
    process_cmd->add_option("-s,--state", process_state, "Load plugin state from file");
    process_cmd->add_option("-p,--preset", process_preset, "Load factory preset N");
    process_cmd->add_option("--param", process_params, "Set parameter: \"Name:value\" (repeatable)");
    process_cmd->add_flag("-d,--double", process_double, "Use double precision if supported");
    process_cmd->add_flag("--non-realtime", process_nrt, "Enable non-realtime processing mode");
    process_cmd->add_option("--bpm", process_bpm, "Set transport BPM");
    process_cmd->add_option("--bit-depth", process_bit_depth, "Output bit depth (16, 24, or 32)")
        ->check(CLI::IsMember({16, 24, 32}));
    process_cmd->add_option("-t,--tail", process_tail, "Tail length in seconds after MIDI ends (default: 2.0)")
        ->default_val(2.0);

    process_cmd->callback([&]() {
        std::exit(cmd_process(process_plugin, process_input, process_output,
                              process_sidechain, process_midi,
                              sample_rate, block_size, process_state,
                              process_preset, process_params,
                              process_double, process_nrt, process_bpm,
                              process_bit_depth, process_tail));
    });

    // Parse and run
    CLI11_PARSE(app, argc, argv);

    return 0;
}
