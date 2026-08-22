// main.cpp - CLI frontend for minihost
// Provides command-line access to plugin hosting features

#include "minihost.h"
#include "minihost_audio.h"
#include "minihost_chain.h"
#include "minihost_graph.h"
#include "minihost_audiofile.h"
#include "minihost_midi.h"
#include "minihost_vstpreset.h"
#include "minihost_version.h"
#include <filesystem>
#include <sstream>
#include "MidiFile.h"
#include "MidiEvent.h"
#include "include/CLI11.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <fstream>
#include <iomanip>
#include <iostream>

volatile sig_atomic_t g_running = 1;

void sigint_handler(int) { g_running = 0; }

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

// Scan for plugins and refresh the shared cache. An empty `directory`
// scans this platform's canonical plugin locations, which is what makes
// name resolution usable without the user knowing where plugins live.
int cmd_scan(const std::string& directory, bool json_output, bool in_process) {
    ScanContext ctx{json_output, 0, true};

    if (!json_output) {
        if (!directory.empty()) {
            std::fprintf(stderr, "Scanning %s\n", directory.c_str());
        } else {
            char dir[1024] = {0};
            std::fprintf(stderr, "Scanning the default plugin locations:\n");
            for (int i = 0; mh_get_default_plugin_dir(i, dir, sizeof(dir)); i++)
                std::fprintf(stderr, "  %s\n", dir);
        }
    }

    if (json_output) {
        std::printf("[\n");
    }

    char scan_err[1024] = {0};
    const char* dirs[1] = { directory.c_str() };
    const char* const* dir_arg = directory.empty() ? nullptr : dirs;
    const int num_dirs = directory.empty() ? 0 : 1;
    int result;
    if (in_process) {
        result = mh_plugin_cache_scan(dir_arg, num_dirs, 0, scan_callback, &ctx,
                                      scan_err, sizeof(scan_err));
    } else {
        // Each plugin is probed in a child process, so one that hangs or
        // crashes on load costs that entry rather than the whole scan.
        result = mh_plugin_cache_scan_supervised(dir_arg, num_dirs, 0,
                                                 nullptr, 0, 0,
                                                 scan_callback, &ctx,
                                                 scan_err, sizeof(scan_err));
    }
    if (result < 0 && scan_err[0] != '\0') print_error(scan_err);

    if (json_output) {
        if (ctx.count > 0) std::printf("\n");
        std::printf("]\n");
    }

    if (result < 0) {
        print_error("Failed to scan");
        return 1;
    }

    if (!json_output) {
        char cache_path[1024] = {0};
        mh_plugin_cache_path(cache_path, sizeof(cache_path));
        std::printf("\nFound %d newly probed plugin(s); %d in the cache\n",
                    ctx.count, result);
        std::printf("Cache: %s\n", cache_path);
    }

    return 0;
}

// Resolve a plugin argument that may be a path or a cached name.
//
// A path wins whenever it exists, so nothing that worked before changes
// meaning. Otherwise the argument is looked up in the shared scan cache
// (the same file the Python CLI writes), case-insensitively. Returns the
// resolved path, or an empty string after reporting the failure.
static std::string g_plugin_format;      // --format au|vst3
static bool g_plugin_fuzzy = false;      // --fuzzy: allow substring names

std::string resolve_plugin_arg(const std::string& arg) {
    if (arg.empty()) return {};
    if (std::filesystem::exists(arg)) return arg;   // bundles are directories
    if (arg.find('/') != std::string::npos) return arg;   // a path: let it fail loudly
#ifdef _WIN32
    if (arg.find('\\') != std::string::npos) return arg;
#endif

    char resolved[1024] = {0};
    const char* fmt = g_plugin_format.empty() ? nullptr : g_plugin_format.c_str();
    const int matches = mh_plugin_cache_lookup(arg.c_str(), fmt, g_plugin_fuzzy ? 1 : 0,
                                               resolved, sizeof(resolved));
    if (matches == 1) return resolved;

    if (matches == 0) {
        char cache_path[1024] = {0};
        mh_plugin_cache_path(cache_path, sizeof(cache_path));
        std::fprintf(stderr,
                     "Error: no plugin named '%s' in the scan cache (%s)\n"
                     "       run 'scan' first, or pass a path%s\n",
                     arg.c_str(), cache_path,
                     g_plugin_fuzzy ? "" : ", or --fuzzy to match part of a name");
        return {};
    }

    std::fprintf(stderr, "Error: '%s' matches %d plugins:\n", arg.c_str(), matches);
    for (int i = 0; i < matches; i++) {
        char one[1024] = {0};
        if (mh_plugin_cache_match(arg.c_str(), fmt, g_plugin_fuzzy ? 1 : 0, i,
                                  one, sizeof(one)))
            std::fprintf(stderr, "       %s\n", one);
    }
    std::fprintf(stderr, "       name it more precisely, pass a path, "
                         "or pick a format with --format\n");
    return {};
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

// Save the plugin's full state blob to a file. Returns 1 on success, 0 on failure.
int save_state_to_file_cpp(MH_Plugin* p, const std::string& path) {
    int size = mh_get_state_size(p);
    if (size <= 0) return 0;
    std::vector<char> data(static_cast<size_t>(size));
    if (!mh_get_state(p, data.data(), size)) return 0;
    std::ofstream f(path, std::ios::binary);
    if (!f) return 0;
    f.write(data.data(), size);
    return static_cast<bool>(f) ? 1 : 0;
}

// Resolve a snapshot source onto the plugin, then capture its normalized
// parameter values into `out`. `program` >= 0 selects a factory program;
// otherwise a non-empty `state` loads a state blob; otherwise the plugin's
// current values are captured. Returns the param count, or -1 on error.
int morph_capture_source_cpp(MH_Plugin* p, int program, const std::string& state,
                             std::vector<float>& out, const char* label) {
    if (!state.empty()) {
        if (!load_state_from_file_cpp(p, state)) {
            std::fprintf(stderr, "Error: failed to load snapshot %s state from %s\n",
                         label, state.c_str());
            return -1;
        }
    } else if (program >= 0) {
        int np = mh_get_num_programs(p);
        if (program >= np) {
            std::fprintf(stderr, "Error: snapshot %s program %d out of range (plugin has %d)\n",
                         label, program, np);
            return -1;
        }
        mh_set_program(p, program);
    }
    int n = mh_morph_capture(p, out.data(), static_cast<int>(out.size()));
    if (n < 0) {
        std::fprintf(stderr, "Error: failed to capture snapshot %s\n", label);
        return -1;
    }
    return n;
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
            // Auto-detect from the plugin bundle's moduleinfo.json. There is
            // no silent fallback -- if this fails we error out rather than
            // write a .vstpreset with a bogus class_id.
            char cid_buf[MH_VSTPRESET_CLASS_ID_LEN + 1] = {0};
            char cid_err[512] = {0};
            if (!mh_vstpreset_read_class_id_from_bundle(
                    plugin_path.c_str(), cid_buf, cid_err, sizeof(cid_err))) {
                std::fprintf(stderr,
                             "Error: cannot determine VST3 class_id for '%s': %s\n"
                             "Use --load-vstpreset to inherit a class_id from an "
                             "existing .vstpreset file.\n",
                             plugin_path.c_str(), cid_err);
                mh_close(p);
                return 1;
            }
            class_id = cid_buf;
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
// Command: morph
// ============================================================================

int cmd_morph(const std::string& plugin_path, double sample_rate, int block_size,
              int a_program, int b_program,
              const std::string& a_state, const std::string& b_state,
              double blend, bool apply, const std::string& save_file,
              bool json_output) {
    char err[1024] = {0};
    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    int n = mh_get_num_params(p);
    if (n <= 0) {
        std::fprintf(stderr, "Error: plugin has no parameters to morph\n");
        mh_close(p);
        return 1;
    }

    // Default sources: factory programs 0 and 1 when nothing is specified.
    bool have_sources = (a_program >= 0 || b_program >= 0 ||
                         !a_state.empty() || !b_state.empty());
    if (!have_sources) {
        int np = mh_get_num_programs(p);
        if (np >= 2) {
            a_program = 0;
            b_program = 1;
        } else {
            std::fprintf(stderr,
                "Error: no snapshot sources given and plugin has < 2 factory programs.\n"
                "       Pass --a-program/--b-program or --a-state/--b-state.\n");
            mh_close(p);
            return 1;
        }
    }

    std::vector<float> a(static_cast<size_t>(n));
    std::vector<float> b(static_cast<size_t>(n));
    std::vector<float> m(static_cast<size_t>(n));

    if (morph_capture_source_cpp(p, a_program, a_state, a, "A") < 0 ||
        morph_capture_source_cpp(p, b_program, b_state, b, "B") < 0) {
        mh_close(p);
        return 1;
    }

    if (!mh_morph_lerp(a.data(), b.data(), m.data(), n, static_cast<float>(blend))) {
        std::fprintf(stderr, "Error: morph interpolation failed\n");
        mh_close(p);
        return 1;
    }

    // Report the A/B/blend snapshot table.
    if (json_output) {
        std::printf("{\n  \"blend\": %.6f,\n  \"num_params\": %d,\n  \"params\": [\n", blend, n);
        for (int i = 0; i < n; i++) {
            std::printf("    {\"index\": %d, \"a\": %.6f, \"b\": %.6f, \"blend\": %.6f}%s\n",
                        i, a[i], b[i], m[i], (i + 1 < n) ? "," : "");
        }
        std::printf("  ]\n}\n");
    } else {
        std::fprintf(stderr, "Morph between A and B at t=%.3f (%d params)\n", blend, n);
        std::printf("%-4s %-28s %9s %9s %9s\n", "idx", "name", "A", "B", "blend");
        for (int i = 0; i < n; i++) {
            MH_ParamInfo pi;
            std::memset(&pi, 0, sizeof(pi));
            char name[MH_PARAM_NAME_LEN] = {0};
            if (mh_get_param_info(p, i, &pi))
                std::snprintf(name, sizeof(name), "%s", pi.name);
            std::printf("%-4d %-28s %9.4f %9.4f %9.4f\n", i, name, a[i], b[i], m[i]);
        }
    }

    // Apply and optionally persist the morphed snapshot.
    if (apply || !save_file.empty()) {
        if (!mh_morph_apply(p, m.data(), n)) {
            std::fprintf(stderr, "Error: failed to apply morphed snapshot\n");
            mh_close(p);
            return 1;
        }
        std::fprintf(stderr, "Applied morphed snapshot to plugin.\n");
        if (!save_file.empty()) {
            if (save_state_to_file_cpp(p, save_file))
                std::fprintf(stderr, "Saved morphed state to %s\n", save_file.c_str());
            else
                std::fprintf(stderr, "Warning: failed to save state to %s\n", save_file.c_str());
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
// Command: chain
// ============================================================================

// Process audio and/or MIDI through several plugins in series.
//
// MIDI enters the first plugin that accepts it and is carried onward by
// any plugin that produces MIDI, so a MIDI effect placed ahead of an
// instrument drives it. That is why the plugin list is taken in signal
// order and why MIDI effects have to come first.
int cmd_chain(const std::vector<std::string>& plugin_paths,
              const std::string& input_file,
              const std::string& output_file,
              const std::string& midi_file,
              double sample_rate,
              int block_size,
              bool non_realtime,
              double bpm,
              int bit_depth,
              double tail_seconds,
              const std::vector<std::string>& mix_specs) {
    char err[1024] = {0};

    if (plugin_paths.empty()) {
        std::fprintf(stderr, "Error: chain needs at least one plugin\n");
        return 1;
    }
    const bool has_audio_input = !input_file.empty();
    const bool has_midi = !midi_file.empty();
    if (!has_audio_input && !has_midi) {
        std::fprintf(stderr, "Error: an input file (-i) or a MIDI file (-m) is required\n");
        return 1;
    }
    if (output_file.empty()) {
        std::fprintf(stderr, "Error: an output file (-o) is required\n");
        return 1;
    }

    // --- Input audio (optional: an instrument chain needs none) ---
    MH_AudioData* audio_data = nullptr;
    int file_ch = 2;
    int in_frames = 0;
    if (has_audio_input) {
        audio_data = mh_audio_read(input_file.c_str(), err, sizeof(err));
        if (!audio_data) {
            std::fprintf(stderr, "Error: %s\n", err);
            return 1;
        }
        file_ch = static_cast<int>(audio_data->channels);
        in_frames = static_cast<int>(audio_data->frames);
        sample_rate = static_cast<double>(audio_data->sample_rate);
    }
    struct AudioGuard {
        MH_AudioData* d;
        ~AudioGuard() { if (d) mh_audio_data_free(d); }
    } audio_guard{audio_data};

    // --- Input MIDI (optional) ---
    MH_MidiEvent* midi_events = nullptr;
    int num_midi_events = 0;
    double midi_duration = 0.0;
    if (has_midi) {
        char midi_err[1024] = {0};
        if (!mh_midi_file_load(midi_file.c_str(), sample_rate, &midi_events,
                               &num_midi_events, &midi_duration,
                               midi_err, sizeof(midi_err))) {
            std::fprintf(stderr, "Error: %s: %s\n", midi_file.c_str(), midi_err);
            return 1;
        }
        if (!has_audio_input) {
            in_frames = static_cast<int>(midi_duration * sample_rate);
            if (in_frames <= 0) in_frames = static_cast<int>(sample_rate);
        }
    }
    struct MidiGuard {
        MH_MidiEvent* e;
        ~MidiGuard() { mh_midi_file_free(e); }
    } midi_guard{midi_events};

    // --- Open every plugin, then bind them into a chain ---
    std::vector<MH_Plugin*> plugins;
    struct PluginGuard {
        std::vector<MH_Plugin*>& v;
        ~PluginGuard() { for (auto* p : v) mh_close(p); }
    } plugin_guard{plugins};

    // One session across the chain: mh_open builds and registers a JUCE
    // plugin-format manager per call, which is wasted once you are
    // loading more than one plugin.
    MH_Session* session = mh_session_create(err, sizeof(err));
    struct SessionGuard {
        MH_Session* s;
        ~SessionGuard() { if (s) mh_session_close(s); }   // plugins outlive it
    } session_guard{session};

    for (const auto& path : plugin_paths) {
        MH_Plugin* p = session
            ? mh_session_open(session, path.c_str(), sample_rate, block_size,
                              2, 2, 0, err, sizeof(err))
            : mh_open(path.c_str(), sample_rate, block_size, 2, 2,
                      err, sizeof(err));
        if (!p) {
            std::fprintf(stderr, "Error: %s: %s\n", path.c_str(), err);
            return 1;
        }
        plugins.push_back(p);
        if (non_realtime) mh_set_non_realtime(p, 1);
        if (bpm > 0.0) {
            MH_TransportInfo tr{};
            tr.bpm = bpm;
            tr.time_sig_numerator = 4;
            tr.time_sig_denominator = 4;
            tr.is_playing = 1;
            mh_set_transport(p, &tr);
        }
    }

    MH_PluginChain* chain = mh_chain_create(plugins.data(),
                                            static_cast<int>(plugins.size()),
                                            err, sizeof(err));
    if (!chain) {
        std::fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    struct ChainGuard {
        MH_PluginChain* c;
        ~ChainGuard() { mh_chain_close(c); }
    } chain_guard{chain};

    // --- Per-plugin dry/wet mix: --mix INDEX:VALUE ---
    for (const auto& spec : mix_specs) {
        int idx = 0;
        float value = 1.0f;
        if (std::sscanf(spec.c_str(), "%d:%f", &idx, &value) != 2) {
            std::fprintf(stderr, "Error: --mix wants INDEX:VALUE, got '%s'\n", spec.c_str());
            return 1;
        }
        if (!mh_chain_set_mix(chain, idx, value)) {
            std::fprintf(stderr,
                         "Error: could not set mix %s (index out of range, or the "
                         "plugin's input and output channel counts differ)\n",
                         spec.c_str());
            return 1;
        }
    }

    int in_ch = mh_chain_get_num_input_channels(chain);
    int out_ch = mh_chain_get_num_output_channels(chain);
    if (in_ch < 1) in_ch = 1;      // instruments read nothing; keep one silent channel
    if (out_ch < 1) out_ch = 2;
    const int latency = mh_chain_get_latency_samples(chain);
    const int tail_frames = tail_seconds > 0 ? static_cast<int>(tail_seconds * sample_rate) : 0;
    const int total_samples = in_frames + tail_frames;
    const int output_total = total_samples + latency;

    std::fprintf(stderr, "Chain of %zu plugin(s) @ %.0f Hz\n", plugins.size(), sample_rate);
    for (size_t i = 0; i < plugins.size(); i++) {
        MH_Info info{};
        mh_get_info(plugins[i], &info);
        const auto slash = plugin_paths[i].find_last_of('/');
        const std::string name = slash == std::string::npos
            ? plugin_paths[i] : plugin_paths[i].substr(slash + 1);
        std::fprintf(stderr, "  [%zu] %-28s %din/%dout  midi in:%s out:%s  latency %d\n",
                     i, name.c_str(), info.num_input_ch, info.num_output_ch,
                     info.accepts_midi ? "yes" : "no",
                     info.produces_midi ? "yes" : "no",
                     mh_get_latency_samples(plugins[i]));
    }
    std::fprintf(stderr, "  Chain I/O:   %d in / %d out, latency %d samples\n",
                 in_ch, out_ch, latency);
    if (has_midi)
        std::fprintf(stderr, "  MIDI events: %d (%.2fs)\n", num_midi_events, midi_duration);
    std::fprintf(stderr, "  Output:      %s\n", output_file.c_str());

    // --- Buffers ---
    std::vector<std::vector<float>> in_storage(in_ch, std::vector<float>(output_total, 0.0f));
    std::vector<std::vector<float>> out_storage(out_ch, std::vector<float>(output_total, 0.0f));
    if (audio_data) {
        for (int f = 0; f < in_frames; f++)
            for (int c = 0; c < in_ch; c++)
                in_storage[c][f] = (c < file_ch)
                    ? audio_data->data[static_cast<size_t>(f) * file_ch + c]
                    : 0.0f;
    }

    // --- Process ---
    int midi_cursor = 0;
    for (int start = 0; start < output_total; start += block_size) {
        int end = std::min(start + block_size, output_total);
        const int bsize = end - start;

        std::vector<const float*> in_ptrs(in_ch);
        std::vector<float*> out_ptrs(out_ch);
        for (int c = 0; c < in_ch; c++) in_ptrs[c] = in_storage[c].data() + start;
        for (int c = 0; c < out_ch; c++) out_ptrs[c] = out_storage[c].data() + start;

        // Slice this block's MIDI out of the absolute-offset array and
        // rebase each event to the block.
        std::vector<MH_MidiEvent> block_midi;
        while (midi_cursor < num_midi_events
               && midi_events[midi_cursor].sample_offset < end) {
            MH_MidiEvent ev = midi_events[midi_cursor];
            ev.sample_offset = std::max(0, ev.sample_offset - start);
            block_midi.push_back(ev);
            midi_cursor++;
        }

        mh_chain_process_midi_io(chain, in_ptrs.data(), out_ptrs.data(), bsize,
                                 block_midi.empty() ? nullptr : block_midi.data(),
                                 static_cast<int>(block_midi.size()),
                                 nullptr, 0, nullptr);
    }

    // --- Latency compensation and write ---
    int write_offset = latency;
    int write_frames = total_samples;
    if (write_offset + write_frames > output_total)
        write_frames = output_total - write_offset;
    if (write_frames < 0) write_frames = 0;

    std::vector<float> interleaved(static_cast<size_t>(out_ch) * write_frames);
    for (int f = 0; f < write_frames; f++)
        for (int c = 0; c < out_ch; c++)
            interleaved[static_cast<size_t>(f) * out_ch + c] = out_storage[c][write_offset + f];

    if (bit_depth <= 0) bit_depth = 24;
    if (!mh_audio_write(output_file.c_str(), interleaved.data(),
                        static_cast<unsigned>(out_ch),
                        static_cast<unsigned>(write_frames),
                        static_cast<unsigned>(sample_rate), bit_depth,
                        err, sizeof(err))) {
        std::fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    std::fprintf(stderr, "Wrote %d samples (%.2fs) to %s\n", write_frames,
                 static_cast<double>(write_frames) / sample_rate, output_file.c_str());
    return 0;
}

// ============================================================================
// Command: bus
// ============================================================================

// Split one input across parallel branches and sum their audio -- the
// layering shape: one MIDI part driving several instruments at once.
// Each entry in `branch_specs` is one branch; commas inside an entry
// chain plugins in series within that branch.
int cmd_bus(const std::vector<std::string>& branch_specs,
            const std::string& input_file,
            const std::string& output_file,
            const std::string& midi_file,
            double sample_rate,
            int block_size,
            bool non_realtime,
            double bpm,
            int bit_depth,
            double tail_seconds,
            const std::vector<std::string>& gain_specs) {
    char err[1024] = {0};

    if (branch_specs.empty()) {
        std::fprintf(stderr, "Error: bus needs at least one branch\n");
        return 1;
    }
    const bool has_audio_input = !input_file.empty();
    const bool has_midi = !midi_file.empty();
    if (!has_audio_input && !has_midi) {
        std::fprintf(stderr, "Error: an input file (-i) or a MIDI file (-m) is required\n");
        return 1;
    }
    if (output_file.empty()) {
        std::fprintf(stderr, "Error: an output file (-o) is required\n");
        return 1;
    }

    // --- Inputs ---
    MH_AudioData* audio_data = nullptr;
    int file_ch = 2;
    int in_frames = 0;
    if (has_audio_input) {
        audio_data = mh_audio_read(input_file.c_str(), err, sizeof(err));
        if (!audio_data) {
            std::fprintf(stderr, "Error: %s\n", err);
            return 1;
        }
        file_ch = static_cast<int>(audio_data->channels);
        in_frames = static_cast<int>(audio_data->frames);
        sample_rate = static_cast<double>(audio_data->sample_rate);
    }
    struct AudioGuard { MH_AudioData* d; ~AudioGuard() { if (d) mh_audio_data_free(d); } }
        audio_guard{audio_data};

    MH_MidiEvent* midi_events = nullptr;
    int num_midi_events = 0;
    double midi_duration = 0.0;
    if (has_midi) {
        char midi_err[1024] = {0};
        if (!mh_midi_file_load(midi_file.c_str(), sample_rate, &midi_events,
                               &num_midi_events, &midi_duration,
                               midi_err, sizeof(midi_err))) {
            std::fprintf(stderr, "Error: %s: %s\n", midi_file.c_str(), midi_err);
            return 1;
        }
        if (!has_audio_input) {
            in_frames = static_cast<int>(midi_duration * sample_rate);
            if (in_frames <= 0) in_frames = static_cast<int>(sample_rate);
        }
    }
    struct MidiGuard { MH_MidiEvent* e; ~MidiGuard() { mh_midi_file_free(e); } }
        midi_guard{midi_events};

    // --- Open each branch (comma-separated plugins run in series) ---
    std::vector<MH_Plugin*> plugins;
    std::vector<MH_PluginChain*> chains;
    struct ChainGuard {
        std::vector<MH_PluginChain*>& c;
        std::vector<MH_Plugin*>& p;
        ~ChainGuard() {
            for (auto* ch : c) mh_chain_close(ch);
            for (auto* pl : p) mh_close(pl);
        }
    } guard{chains, plugins};

    // One session for every plugin across every branch.
    MH_Session* session = mh_session_create(err, sizeof(err));
    struct SessionGuard {
        MH_Session* s;
        ~SessionGuard() { if (s) mh_session_close(s); }
    } session_guard{session};

    for (size_t b = 0; b < branch_specs.size(); b++) {
        std::vector<MH_Plugin*> branch;
        std::stringstream ss(branch_specs[b]);
        std::string path;
        while (std::getline(ss, path, ',')) {
            while (!path.empty() && path.front() == ' ') path.erase(path.begin());
            if (path.empty()) continue;
            MH_Plugin* p = session
                ? mh_session_open(session, path.c_str(), sample_rate, block_size,
                                  2, 2, 0, err, sizeof(err))
                : mh_open(path.c_str(), sample_rate, block_size, 2, 2,
                          err, sizeof(err));
            if (!p) {
                std::fprintf(stderr, "Error: %s: %s\n", path.c_str(), err);
                return 1;
            }
            if (non_realtime) mh_set_non_realtime(p, 1);
            if (bpm > 0.0) {
                MH_TransportInfo tr{};
                tr.bpm = bpm;
                tr.time_sig_numerator = 4;
                tr.time_sig_denominator = 4;
                tr.is_playing = 1;
                mh_set_transport(p, &tr);
            }
            plugins.push_back(p);
            branch.push_back(p);
        }
        if (branch.empty()) {
            std::fprintf(stderr, "Error: branch %zu names no plugin\n", b);
            return 1;
        }
        MH_PluginChain* chain = mh_chain_create(branch.data(),
                                                static_cast<int>(branch.size()),
                                                err, sizeof(err));
        if (!chain) {
            std::fprintf(stderr, "Error: branch %zu: %s\n", b, err);
            return 1;
        }
        chains.push_back(chain);
    }

    // Bus width: the widest branch input (zero for an instrument bus),
    // and the branch output width, which every branch must share.
    int bus_in = 0;
    int bus_out = mh_chain_get_num_output_channels(chains[0]);
    for (size_t i = 0; i < chains.size(); i++) {
        bus_in = std::max(bus_in, mh_chain_get_num_input_channels(chains[i]));
        const int co = mh_chain_get_num_output_channels(chains[i]);
        if (co != bus_out) {
            std::fprintf(stderr,
                         "Error: branch %zu outputs %d channels, branch 0 outputs %d "
                         "-- a bus sums branches, so their output widths must match\n",
                         i, co, bus_out);
            return 1;
        }
    }
    if (bus_out < 1) bus_out = 2;

    MH_PluginBus* bus = mh_bus_create(bus_in, bus_out, block_size, sample_rate,
                                      err, sizeof(err));
    if (!bus) {
        std::fprintf(stderr, "Error: %s\n", err);
        return 1;
    }
    struct BusGuard { MH_PluginBus* b; ~BusGuard() { mh_bus_close(b); } } bus_guard{bus};

    for (size_t i = 0; i < chains.size(); i++) {
        if (mh_bus_add_branch(bus, chains[i], 1.0f, err, sizeof(err)) < 0) {
            std::fprintf(stderr, "Error: branch %zu: %s\n", i, err);
            return 1;
        }
    }

    // --- Per-branch gain: --gain INDEX:VALUE ---
    for (const auto& spec : gain_specs) {
        int idx = 0;
        float value = 1.0f;
        if (std::sscanf(spec.c_str(), "%d:%f", &idx, &value) != 2) {
            std::fprintf(stderr, "Error: --gain wants INDEX:VALUE, got '%s'\n", spec.c_str());
            return 1;
        }
        if (!mh_bus_set_branch_gain(bus, idx, value)) {
            std::fprintf(stderr, "Error: branch index %d out of range\n", idx);
            return 1;
        }
    }

    const int latency = mh_bus_get_latency_samples(bus);
    const int tail_frames = tail_seconds > 0 ? static_cast<int>(tail_seconds * sample_rate) : 0;
    const int total_samples = in_frames + tail_frames;
    const int output_total = total_samples + latency;

    std::fprintf(stderr, "Bus of %zu branch(es) @ %.0f Hz\n", chains.size(), sample_rate);
    for (size_t i = 0; i < chains.size(); i++)
        std::fprintf(stderr, "  [%zu] %-44s %din/%dout\n", i, branch_specs[i].c_str(),
                     mh_chain_get_num_input_channels(chains[i]),
                     mh_chain_get_num_output_channels(chains[i]));
    std::fprintf(stderr, "  Bus I/O:     %d in / %d out, latency %d samples\n",
                 bus_in, bus_out, latency);
    if (has_midi)
        std::fprintf(stderr, "  MIDI events: %d (%.2fs) fanned to every branch\n",
                     num_midi_events, midi_duration);
    std::fprintf(stderr, "  Output:      %s\n", output_file.c_str());

    // --- Buffers ---
    const int in_alloc = bus_in > 0 ? bus_in : 1;
    std::vector<std::vector<float>> in_storage(in_alloc, std::vector<float>(output_total, 0.0f));
    std::vector<std::vector<float>> out_storage(bus_out, std::vector<float>(output_total, 0.0f));
    if (audio_data) {
        for (int f = 0; f < in_frames; f++)
            for (int c = 0; c < bus_in; c++)
                in_storage[c][f] = (c < file_ch)
                    ? audio_data->data[static_cast<size_t>(f) * file_ch + c]
                    : 0.0f;
    }

    // --- Process ---
    int midi_cursor = 0;
    for (int start = 0; start < output_total; start += block_size) {
        const int end = std::min(start + block_size, output_total);
        const int bsize = end - start;

        std::vector<const float*> in_ptrs(in_alloc);
        std::vector<float*> out_ptrs(bus_out);
        for (int c = 0; c < in_alloc; c++) in_ptrs[c] = in_storage[c].data() + start;
        for (int c = 0; c < bus_out; c++) out_ptrs[c] = out_storage[c].data() + start;

        std::vector<MH_MidiEvent> block_midi;
        while (midi_cursor < num_midi_events
               && midi_events[midi_cursor].sample_offset < end) {
            MH_MidiEvent ev = midi_events[midi_cursor];
            ev.sample_offset = std::max(0, ev.sample_offset - start);
            block_midi.push_back(ev);
            midi_cursor++;
        }

        mh_bus_process_midi_io(bus, bus_in > 0 ? in_ptrs.data() : nullptr,
                               out_ptrs.data(), bsize,
                               block_midi.empty() ? nullptr : block_midi.data(),
                               static_cast<int>(block_midi.size()),
                               nullptr, 0, nullptr, nullptr);
    }

    // --- Latency compensation and write ---
    int write_offset = latency;
    int write_frames = total_samples;
    if (write_offset + write_frames > output_total)
        write_frames = output_total - write_offset;
    if (write_frames < 0) write_frames = 0;

    std::vector<float> interleaved(static_cast<size_t>(bus_out) * write_frames);
    for (int f = 0; f < write_frames; f++)
        for (int c = 0; c < bus_out; c++)
            interleaved[static_cast<size_t>(f) * bus_out + c] = out_storage[c][write_offset + f];

    if (bit_depth <= 0) bit_depth = 24;
    if (!mh_audio_write(output_file.c_str(), interleaved.data(),
                        static_cast<unsigned>(bus_out),
                        static_cast<unsigned>(write_frames),
                        static_cast<unsigned>(sample_rate), bit_depth,
                        err, sizeof(err))) {
        std::fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    std::fprintf(stderr, "Wrote %d samples (%.2fs) to %s\n", write_frames,
                 static_cast<double>(write_frames) / sample_rate, output_file.c_str());
    return 0;
}

// ============================================================================
// Helper: print MIDI message
// ============================================================================

static void print_midi_msg(const unsigned char* data, size_t len) {
    static const char* note_names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };

    if (len == 0) return;

    unsigned char status = data[0];
    unsigned char type = status & 0xF0;
    unsigned char ch = (status & 0x0F) + 1;

    switch (type) {
    case 0x90:
        if (len >= 3 && data[2] > 0) {
            int note = data[1];
            int oct = (note / 12) - 1;
            std::printf("NoteOn   ch=%2d  %s%d (%3d)  vel=%3d\n",
                        ch, note_names[note % 12], oct, note, data[2]);
        } else if (len >= 3) {
            int note = data[1];
            int oct = (note / 12) - 1;
            std::printf("NoteOff  ch=%2d  %s%d (%3d)\n",
                        ch, note_names[note % 12], oct, note);
        }
        break;
    case 0x80:
        if (len >= 3) {
            int note = data[1];
            int oct = (note / 12) - 1;
            std::printf("NoteOff  ch=%2d  %s%d (%3d)  vel=%3d\n",
                        ch, note_names[note % 12], oct, note, data[2]);
        }
        break;
    case 0xB0:
        if (len >= 3) {
            std::printf("CC       ch=%2d  cc=%3d  val=%3d\n", ch, data[1], data[2]);
        }
        break;
    case 0xE0:
        if (len >= 3) {
            int bend = (data[2] << 7) | data[1];
            std::printf("PitchBend ch=%2d  val=%5d\n", ch, bend);
        }
        break;
    case 0xC0:
        if (len >= 2) {
            std::printf("PgmChg   ch=%2d  pgm=%3d\n", ch, data[1]);
        }
        break;
    case 0xD0:
        if (len >= 2) {
            std::printf("ChPress  ch=%2d  val=%3d\n", ch, data[1]);
        }
        break;
    default:
        std::printf("MIDI    ");
        for (size_t i = 0; i < len; i++) {
            std::printf(" %02X", data[i]);
        }
        std::printf("\n");
        break;
    }
}

// ============================================================================
// Command: midi
// ============================================================================

int cmd_midi(int port_index, const std::string& virtual_name,
             bool monitor, bool json_output) {
    if (!monitor) {
        // List mode
        int num_in = mh_midi_get_num_inputs();
        int num_out = mh_midi_get_num_outputs();

        if (json_output) {
            std::printf("{\n");
            std::printf("  \"inputs\": [");
            for (int i = 0; i < num_in; i++) {
                char name[256] = {0};
                mh_midi_get_input_name(i, name, sizeof(name));
                std::printf("%s\n    {\"index\": %d, \"name\": \"%s\"}",
                            i == 0 ? "" : ",", i, name);
            }
            std::printf("%s],\n", num_in > 0 ? "\n  " : "");
            std::printf("  \"outputs\": [");
            for (int i = 0; i < num_out; i++) {
                char name[256] = {0};
                mh_midi_get_output_name(i, name, sizeof(name));
                std::printf("%s\n    {\"index\": %d, \"name\": \"%s\"}",
                            i == 0 ? "" : ",", i, name);
            }
            std::printf("%s]\n", num_out > 0 ? "\n  " : "");
            std::printf("}\n");
        } else {
            std::printf("MIDI Input Ports:\n");
            if (num_in == 0) {
                std::printf("  (none)\n");
            } else {
                for (int i = 0; i < num_in; i++) {
                    char name[256] = {0};
                    mh_midi_get_input_name(i, name, sizeof(name));
                    std::printf("  [%d] %s\n", i, name);
                }
            }
            std::printf("\nMIDI Output Ports:\n");
            if (num_out == 0) {
                std::printf("  (none)\n");
            } else {
                for (int i = 0; i < num_out; i++) {
                    char name[256] = {0};
                    mh_midi_get_output_name(i, name, sizeof(name));
                    std::printf("  [%d] %s\n", i, name);
                }
            }
        }
        return 0;
    }

    // Monitor mode
    auto midi_callback = [](const unsigned char* data, size_t len, void* /*user_data*/) {
        print_midi_msg(data, len);
    };

    char err[1024] = {0};
    MH_MidiIn* midi_in = nullptr;

    if (!virtual_name.empty()) {
        midi_in = mh_midi_in_open_virtual(virtual_name.c_str(), midi_callback, nullptr,
                                          err, sizeof(err));
        if (!midi_in) {
            print_error(err);
            return 1;
        }
        std::fprintf(stderr, "Monitoring virtual MIDI port: %s\n", virtual_name.c_str());
    } else {
        if (port_index < 0) {
            port_index = 0;
        }
        midi_in = mh_midi_in_open(port_index, midi_callback, nullptr, err, sizeof(err));
        if (!midi_in) {
            print_error(err);
            return 1;
        }
        char name[256] = {0};
        mh_midi_get_input_name(port_index, name, sizeof(name));
        std::fprintf(stderr, "Monitoring MIDI port [%d]: %s\n", port_index, name);
    }

    std::fprintf(stderr, "Press Ctrl+C to stop.\n");

    g_running = 1;
    std::signal(SIGINT, sigint_handler);

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    mh_midi_in_close(midi_in);
    std::fprintf(stderr, "\nStopped.\n");
    return 0;
}

// ============================================================================
// Command: play
// ============================================================================

int cmd_play(const std::string& plugin_path,
             double sample_rate, int block_size,
             int midi_port, const std::string& virtual_midi,
             const std::string& virtual_midi_out,
             int playback_device, int capture_device, bool capture,
             const std::string& state_file, int preset_index,
             const std::vector<std::string>& param_specs) {
    char err[1024] = {0};

    // Open plugin
    int in_ch = capture ? 2 : 2;
    int out_ch = 2;
    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size,
                           in_ch, out_ch, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    // Load state
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

    // Load preset
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

    // Apply parameter overrides
    for (const auto& spec : param_specs) {
        int idx;
        float val;
        if (!parse_param_spec(p, spec, idx, val)) {
            std::fprintf(stderr, "Error: Invalid parameter spec '%s'\n", spec.c_str());
            mh_close(p);
            return 1;
        }
        mh_set_param(p, idx, val);
    }

    // Build audio config
    MH_AudioConfig config = {};
    config.sample_rate = sample_rate;
    config.buffer_frames = block_size;
    config.output_channels = 0;
    config.midi_input_port = virtual_midi.empty() ? midi_port : -1;
    config.midi_output_port = -1;
    config.capture = capture ? 1 : 0;
    config.playback_device_index = playback_device;
    config.capture_device_index = capture_device;

    // Open audio device
    MH_AudioDevice* dev = mh_audio_open(p, &config, err, sizeof(err));
    if (!dev) {
        print_error(err);
        mh_close(p);
        return 1;
    }

    // Create virtual MIDI ports
    if (!virtual_midi.empty()) {
        if (!mh_audio_create_virtual_midi_input(dev, virtual_midi.c_str())) {
            std::fprintf(stderr, "Warning: Failed to create virtual MIDI input '%s'\n",
                         virtual_midi.c_str());
        } else {
            std::fprintf(stderr, "Virtual MIDI input: %s\n", virtual_midi.c_str());
        }
    }

    if (!virtual_midi_out.empty()) {
        if (!mh_audio_create_virtual_midi_output(dev, virtual_midi_out.c_str())) {
            std::fprintf(stderr, "Warning: Failed to create virtual MIDI output '%s'\n",
                         virtual_midi_out.c_str());
        } else {
            std::fprintf(stderr, "Virtual MIDI output: %s\n", virtual_midi_out.c_str());
        }
    }

    // Start audio
    if (!mh_audio_start(dev)) {
        print_error("Failed to start audio");
        mh_audio_close(dev);
        mh_close(p);
        return 1;
    }

    // Install signal handler
    g_running = 1;
    std::signal(SIGINT, sigint_handler);

    std::fprintf(stderr, "Playing: %s\n", plugin_path.c_str());
    std::fprintf(stderr, "  Sample rate: %.0f Hz\n", mh_audio_get_sample_rate(dev));
    std::fprintf(stderr, "  Buffer:      %d frames\n", mh_audio_get_buffer_frames(dev));
    std::fprintf(stderr, "  Channels:    %d\n", mh_audio_get_channels(dev));
    std::fprintf(stderr, "Press Ctrl+C to stop.\n");

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Cleanup
    std::fprintf(stderr, "\nStopping...\n");
    mh_audio_stop(dev);
    mh_audio_close(dev);
    mh_close(p);
    return 0;
}

// ============================================================================
// Command: resample
// ============================================================================

int cmd_resample(const std::string& input_path, const std::string& output_path,
                 unsigned int target_rate, int bit_depth, bool overwrite) {
    // Check if output exists
    if (!overwrite) {
        std::ifstream test(output_path);
        if (test.good()) {
            std::fprintf(stderr, "Error: Output file '%s' already exists (use -y to overwrite)\n",
                         output_path.c_str());
            return 1;
        }
    }

    // Read input
    char err[1024] = {0};
    MH_AudioData* input = mh_audio_read(input_path.c_str(), err, sizeof(err));
    if (!input) {
        print_error(err);
        return 1;
    }

    if (input->sample_rate == target_rate) {
        std::fprintf(stderr, "Input already at %u Hz, writing without resampling\n", target_rate);
        if (!mh_audio_write(output_path.c_str(), input->data,
                            input->channels, input->frames,
                            input->sample_rate, bit_depth, err, sizeof(err))) {
            print_error(err);
            mh_audio_data_free(input);
            return 1;
        }
        std::printf("%s -> %s (%u Hz, %u ch, %u frames, %d-bit)\n",
                    input_path.c_str(), output_path.c_str(),
                    input->sample_rate, input->channels, input->frames, bit_depth);
        mh_audio_data_free(input);
        return 0;
    }

    // Resample
    MH_AudioData* resampled = mh_audio_resample(
        input->data, input->channels, input->frames,
        input->sample_rate, target_rate, err, sizeof(err));
    if (!resampled) {
        print_error(err);
        mh_audio_data_free(input);
        return 1;
    }

    // Write output
    if (!mh_audio_write(output_path.c_str(), resampled->data,
                        resampled->channels, resampled->frames,
                        resampled->sample_rate, bit_depth, err, sizeof(err))) {
        print_error(err);
        mh_audio_data_free(resampled);
        mh_audio_data_free(input);
        return 1;
    }

    std::printf("%s -> %s (%u Hz -> %u Hz, %u ch, %u -> %u frames, %d-bit)\n",
                input_path.c_str(), output_path.c_str(),
                input->sample_rate, resampled->sample_rate,
                resampled->channels, input->frames, resampled->frames, bit_depth);

    mh_audio_data_free(resampled);
    mh_audio_data_free(input);
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    // This binary is its own scan worker: a supervised scan re-runs it once
    // per plugin with --mh-probe-one. Answer that before anything else --
    // before the message thread, before CLI11 sees an argument it has no
    // subcommand for -- since the worker exists only to probe one plugin.
    if (mh_plugin_scan_worker_main(argc, argv)) {
        return 0;
    }

    // Cleanly stop the dedicated JUCE plugin thread at process exit. Without
    // this, any command that loads a plugin leaves the message thread's
    // std::thread joinable at static teardown, which calls std::terminate
    // (SIGABRT on exit). We bring the thread up now and register the shutdown
    // with std::atexit: because the thread is constructed *before* this atexit
    // registration, C++ teardown ordering runs our shutdown handler before the
    // thread object's own destructor -- and it also fires on the std::exit()
    // calls in the subcommand callbacks. Both calls are idempotent, and no-ops
    // when the message thread is disabled (MINIHOST_MESSAGE_THREAD=0).
    mh_message_thread_init();
    std::atexit(mh_message_thread_shutdown);

    CLI::App app{"minihost - Audio plugin hosting CLI"};
    app.require_subcommand(1);

    // Release version (from pyproject.toml via minihost_version.h) plus the C
    // ABI version of the linked library -- two independent axes, both worth
    // knowing when triaging a bug report. Long form only, matching minihost_c
    // where the short -V is already --verbose.
    app.set_version_flag("--version",
                         std::string("minihost ") + MINIHOST_VERSION + "\n"
                         + "libminihost ABI " + mh_api_version_string());

    // Global options
    double sample_rate = 48000.0;
    int block_size = 512;

    app.add_option("-r,--rate", sample_rate, "Sample rate (Hz)")
        ->default_val(48000.0);
    app.add_option("-b,--block", block_size, "Block size (samples)")
        ->default_val(512);
    // Plugin-name resolution. Both are inherited by the subcommands so they
    // can be given before or after the command name.
    app.add_option("--format", g_plugin_format,
                   "Pick a format when a plugin name matches both (au|vst3)");
    app.add_flag("--fuzzy", g_plugin_fuzzy,
                 "Let a plugin name match part of a name");
    app.get_option("--format")->configurable(false);

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
        probe_path = resolve_plugin_arg(probe_path);
        if (probe_path.empty()) std::exit(1);
        std::exit(cmd_probe(probe_path, probe_json));
    });

    // ========================================================================
    // Subcommand: scan
    // ========================================================================
    auto* scan_cmd = app.add_subcommand("scan", "Scan directory for plugins");
    std::string scan_dir;
    bool scan_json = false;

    scan_cmd->add_option("directory", scan_dir,
                         "Directory to scan (default: this platform's plugin locations)");
    scan_cmd->add_flag("-j,--json", scan_json, "Output as JSON");
    bool scan_in_process = false;
    scan_cmd->add_flag("--in-process", scan_in_process,
                       "Probe in this process instead of one child per plugin");

    scan_cmd->callback([&]() {
        std::exit(cmd_scan(scan_dir, scan_json, scan_in_process));
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
        info_path = resolve_plugin_arg(info_path);
        if (info_path.empty()) std::exit(1);
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
        params_path = resolve_plugin_arg(params_path);
        if (params_path.empty()) std::exit(1);
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
        get_param_path = resolve_plugin_arg(get_param_path);
        if (get_param_path.empty()) std::exit(1);
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
        set_param_path = resolve_plugin_arg(set_param_path);
        if (set_param_path.empty()) std::exit(1);
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
        presets_path = resolve_plugin_arg(presets_path);
        if (presets_path.empty()) std::exit(1);
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
        load_preset_path = resolve_plugin_arg(load_preset_path);
        if (load_preset_path.empty()) std::exit(1);
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
        save_state_plugin = resolve_plugin_arg(save_state_plugin);
        if (save_state_plugin.empty()) std::exit(1);
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
        load_state_plugin = resolve_plugin_arg(load_state_plugin);
        if (load_state_plugin.empty()) std::exit(1);
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
        process_plugin = resolve_plugin_arg(process_plugin);
        if (process_plugin.empty()) std::exit(1);
        std::exit(cmd_process(process_plugin, process_input, process_output,
                              process_sidechain, process_midi,
                              sample_rate, block_size, process_state,
                              process_preset, process_params,
                              process_double, process_nrt, process_bpm,
                              process_bit_depth, process_tail));
    });

    // ========================================================================
    // Subcommand: morph
    // ========================================================================
    // chain: N plugins in series, audio and/or MIDI in, one file out
    std::vector<std::string> chain_plugins;
    std::string chain_input, chain_output, chain_midi;
    std::vector<std::string> chain_mix;
    double chain_bpm = 0.0;
    double chain_tail = 2.0;
    int chain_bit_depth = 24;
    bool chain_nrt = false;
    auto* chain_cmd = app.add_subcommand(
        "chain", "Process audio and/or MIDI through plugins in series");
    chain_cmd->add_option("plugins", chain_plugins,
                          "Plugin paths in signal order (MIDI effects first)")
        ->required();
    chain_cmd->add_option("-i,--input", chain_input, "Input audio file");
    chain_cmd->add_option("-m,--midi-input", chain_midi, "Input MIDI file");
    chain_cmd->add_option("-o,--output", chain_output, "Output audio file")->required();
    chain_cmd->add_option("--mix", chain_mix,
                          "Dry/wet mix for one plugin: \"INDEX:VALUE\" (repeatable)");
    chain_cmd->add_flag("--non-realtime", chain_nrt, "Enable non-realtime processing mode");
    chain_cmd->add_option("--bpm", chain_bpm, "Set transport BPM");
    chain_cmd->add_option("--bit-depth", chain_bit_depth, "Output bit depth (16, 24, or 32)");
    chain_cmd->add_option("-t,--tail", chain_tail, "Tail length in seconds (default: 2.0)");

    chain_cmd->callback([&]() {
        for (auto& path : chain_plugins) {
            path = resolve_plugin_arg(path);
            if (path.empty()) std::exit(1);
        }
        std::exit(cmd_chain(chain_plugins, chain_input, chain_output, chain_midi,
                            sample_rate, block_size, chain_nrt, chain_bpm,
                            chain_bit_depth, chain_tail, chain_mix));
    });

    // bus: parallel branches summed; commas chain plugins within a branch
    std::vector<std::string> bus_branches;
    std::string bus_input, bus_output, bus_midi;
    std::vector<std::string> bus_gain;
    double bus_bpm = 0.0;
    double bus_tail = 2.0;
    int bus_bit_depth = 24;
    bool bus_nrt = false;
    auto* bus_cmd = app.add_subcommand(
        "bus", "Split input across parallel branches and sum them");
    bus_cmd->add_option("branches", bus_branches,
                        "One branch per argument; commas chain plugins in series")
        ->required();
    bus_cmd->add_option("-i,--input", bus_input, "Input audio file");
    bus_cmd->add_option("-m,--midi-input", bus_midi, "Input MIDI file");
    bus_cmd->add_option("-o,--output", bus_output, "Output audio file")->required();
    bus_cmd->add_option("--gain", bus_gain, "Gain for one branch: \"INDEX:VALUE\" (repeatable)");
    bus_cmd->add_flag("--non-realtime", bus_nrt, "Enable non-realtime processing mode");
    bus_cmd->add_option("--bpm", bus_bpm, "Set transport BPM");
    bus_cmd->add_option("--bit-depth", bus_bit_depth, "Output bit depth (16, 24, or 32)");
    bus_cmd->add_option("-t,--tail", bus_tail, "Tail length in seconds (default: 2.0)");

    bus_cmd->callback([&]() {
        // A branch is a comma-separated list, so resolve each element and
        // rebuild the branch string.
        for (auto& branch : bus_branches) {
            std::stringstream parts(branch);
            std::string one, rebuilt;
            while (std::getline(parts, one, ',')) {
                while (!one.empty() && one.front() == ' ') one.erase(one.begin());
                if (one.empty()) continue;
                const std::string got = resolve_plugin_arg(one);
                if (got.empty()) std::exit(1);
                if (!rebuilt.empty()) rebuilt += ',';
                rebuilt += got;
            }
            branch = rebuilt;
        }
        std::exit(cmd_bus(bus_branches, bus_input, bus_output, bus_midi,
                          sample_rate, block_size, bus_nrt, bus_bpm,
                          bus_bit_depth, bus_tail, bus_gain));
    });

    auto* morph_cmd = app.add_subcommand(
        "morph", "Interpolate between two parameter snapshots (A/B morph)");
    std::string morph_plugin;
    int morph_a_program = -1;
    int morph_b_program = -1;
    std::string morph_a_state;
    std::string morph_b_state;
    double morph_blend = 0.5;
    bool morph_apply = false;
    std::string morph_save;
    bool morph_json = false;

    morph_cmd->add_option("plugin", morph_plugin, "Path to plugin")
        ->required();
    morph_cmd->add_option("--a-program", morph_a_program, "Snapshot A from factory program N");
    morph_cmd->add_option("--b-program", morph_b_program, "Snapshot B from factory program N");
    morph_cmd->add_option("--a-state", morph_a_state, "Snapshot A from a saved state file");
    morph_cmd->add_option("--b-state", morph_b_state, "Snapshot B from a saved state file");
    morph_cmd->add_option("-t,--blend", morph_blend, "Blend amount 0..1 (default 0.5)")
        ->default_val(0.5);
    morph_cmd->add_flag("--apply", morph_apply, "Apply the morphed snapshot to the plugin");
    morph_cmd->add_option("--save", morph_save, "Apply and save morphed state to file");
    morph_cmd->add_flag("-j,--json", morph_json, "Output as JSON");

    morph_cmd->callback([&]() {
        morph_plugin = resolve_plugin_arg(morph_plugin);
        if (morph_plugin.empty()) std::exit(1);
        std::exit(cmd_morph(morph_plugin, sample_rate, block_size,
                            morph_a_program, morph_b_program,
                            morph_a_state, morph_b_state,
                            morph_blend, morph_apply, morph_save, morph_json));
    });

    // ========================================================================
    // Subcommand: midi
    // ========================================================================
    auto* midi_cmd = app.add_subcommand("midi", "List MIDI ports or monitor input");
    int midi_port = -1;
    std::string midi_virtual_name;
    bool midi_monitor = false;
    bool midi_json = false;

    midi_cmd->add_option("-m,--port", midi_port, "MIDI input port index")
        ->default_val(-1);
    midi_cmd->add_option("--virtual-midi", midi_virtual_name, "Create virtual MIDI input port");
    midi_cmd->add_flag("--monitor", midi_monitor, "Monitor MIDI input");
    midi_cmd->add_flag("-j,--json", midi_json, "Output as JSON");

    midi_cmd->callback([&]() {
        std::exit(cmd_midi(midi_port, midi_virtual_name, midi_monitor, midi_json));
    });

    // ========================================================================
    // Subcommand: play
    // ========================================================================
    auto* play_cmd = app.add_subcommand("play", "Real-time playback with plugin");
    std::string play_plugin;
    int play_midi = -1;
    std::string play_virtual_midi;
    std::string play_virtual_midi_out;
    bool play_capture = false;
    int play_playback_device = -1;
    int play_capture_device = -1;
    std::string play_state;
    int play_preset = -1;
    std::vector<std::string> play_params;

    play_cmd->add_option("plugin", play_plugin, "Path to plugin")
        ->required();
    play_cmd->add_option("-m,--midi", play_midi, "MIDI input port index")
        ->default_val(-1);
    play_cmd->add_option("--virtual-midi", play_virtual_midi, "Create virtual MIDI input port");
    play_cmd->add_option("--virtual-midi-out", play_virtual_midi_out, "Create virtual MIDI output port");
    play_cmd->add_flag("-i,--input", play_capture, "Enable audio capture (duplex mode)");
    play_cmd->add_option("--playback-device", play_playback_device, "Playback device index")
        ->default_val(-1);
    play_cmd->add_option("--capture-device", play_capture_device, "Capture device index")
        ->default_val(-1);
    play_cmd->add_option("-s,--state", play_state, "Load plugin state from file");
    play_cmd->add_option("-p,--preset", play_preset, "Load factory preset N")
        ->default_val(-1);
    play_cmd->add_option("--param", play_params, "Set parameter: \"Name:value\" (repeatable)");

    play_cmd->callback([&]() {
        play_plugin = resolve_plugin_arg(play_plugin);
        if (play_plugin.empty()) std::exit(1);
        std::exit(cmd_play(play_plugin, sample_rate, block_size,
                           play_midi, play_virtual_midi, play_virtual_midi_out,
                           play_playback_device, play_capture_device, play_capture,
                           play_state, play_preset, play_params));
    });

    // ========================================================================
    // Subcommand: resample
    // ========================================================================
    auto* resample_cmd = app.add_subcommand("resample", "Resample audio file");
    std::string resample_input;
    std::string resample_output;
    unsigned int resample_target_rate = 0;
    int resample_bit_depth = 24;
    bool resample_overwrite = false;

    // Both argument shapes are accepted so a command line is portable
    // between the two binaries: minihost_c takes `resample IN OUT --rate N`,
    // this one historically took `resample IN -o OUT -r N`, and a script
    // written for one failed against the other.
    std::string resample_output_pos;
    resample_cmd->add_option("input", resample_input, "Input audio file")
        ->required();
    // Named "output_path" rather than "output": CLI11 rejects a positional
    // whose name collides with the long form of an option (--output).
    resample_cmd->add_option("output_path", resample_output_pos,
                             "Output audio file (or use -o)");
    resample_cmd->add_option("-o,--output", resample_output, "Output audio file");
    resample_cmd->add_option("-r,--target-rate,--rate", resample_target_rate,
                             "Target sample rate (Hz)")
        ->required();
    resample_cmd->add_option("--bit-depth", resample_bit_depth, "Output bit depth (16, 24, or 32)")
        ->default_val(24)
        ->check(CLI::IsMember({16, 24, 32}));
    resample_cmd->add_flag("-y,--overwrite", resample_overwrite, "Overwrite output if it exists");

    resample_cmd->callback([&]() {
        if (resample_output.empty()) resample_output = resample_output_pos;
        if (resample_output.empty()) {
            std::fprintf(stderr,
                         "Error: an output file is required "
                         "(second positional argument, or -o)\n");
            std::exit(1);
        }
        std::exit(cmd_resample(resample_input, resample_output,
                               resample_target_rate, resample_bit_depth,
                               resample_overwrite));
    });

    // Parse and run
    CLI11_PARSE(app, argc, argv);

    return 0;
}
