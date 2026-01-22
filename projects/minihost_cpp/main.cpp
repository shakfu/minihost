// main.cpp - CLI frontend for minihost
// Provides command-line access to plugin hosting features

#include "minihost.h"
#include "include/CLI11.hpp"

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
             int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    MH_Info info;
    mh_get_info(p, &info);

    // Get plugin metadata via probe
    MH_PluginDesc desc;
    if (mh_probe(plugin_path.c_str(), &desc, err, sizeof(err))) {
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
               bool json_output) {
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
// Command: presets
// ============================================================================

int cmd_presets(const std::string& plugin_path,
                double sample_rate,
                int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path.c_str(), sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        print_error(err);
        return 1;
    }

    int num_programs = mh_get_num_programs(p);
    int current = mh_get_program(p);

    std::printf("Factory Presets (%d):\n", num_programs);
    for (int i = 0; i < num_programs; i++) {
        char name[256] = {0};
        mh_get_program_name(p, i, name, sizeof(name));
        std::printf("  [%3d] %s%s\n", i, name, (i == current) ? " *" : "");
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
                double sample_rate,
                int block_size,
                const std::string& state_file,
                bool use_double) {
    char err[1024] = {0};

    // Open plugin
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
            } else {
                std::fprintf(stderr, "Loaded state from %s\n", state_file.c_str());
            }
        }
    }

    MH_Info info;
    mh_get_info(p, &info);

    int in_ch = info.num_input_ch > 0 ? info.num_input_ch : 2;
    int out_ch = info.num_output_ch > 0 ? info.num_output_ch : 2;

    // Open input file (raw float32 interleaved)
    std::ifstream ifs(input_file, std::ios::binary);
    if (!ifs) {
        std::fprintf(stderr, "Error: Cannot open input file %s\n", input_file.c_str());
        mh_close(p);
        return 1;
    }

    // Get file size
    ifs.seekg(0, std::ios::end);
    size_t file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    // Calculate number of frames (assuming interleaved float32)
    size_t total_frames = file_size / (sizeof(float) * in_ch);

    // Open output file
    std::ofstream ofs(output_file, std::ios::binary);
    if (!ofs) {
        std::fprintf(stderr, "Error: Cannot open output file %s\n", output_file.c_str());
        mh_close(p);
        return 1;
    }

    // Allocate buffers
    std::vector<float> in_interleaved(block_size * in_ch);
    std::vector<float> out_interleaved(block_size * out_ch);
    std::vector<std::vector<float>> in_channels(in_ch, std::vector<float>(block_size));
    std::vector<std::vector<float>> out_channels(out_ch, std::vector<float>(block_size));
    std::vector<const float*> in_ptrs(in_ch);
    std::vector<float*> out_ptrs(out_ch);

    for (int ch = 0; ch < in_ch; ch++) in_ptrs[ch] = in_channels[ch].data();
    for (int ch = 0; ch < out_ch; ch++) out_ptrs[ch] = out_channels[ch].data();

    size_t frames_processed = 0;

    while (frames_processed < total_frames) {
        int frames_to_process = std::min((size_t)block_size, total_frames - frames_processed);

        // Read interleaved input
        ifs.read(reinterpret_cast<char*>(in_interleaved.data()),
                 frames_to_process * in_ch * sizeof(float));

        // Deinterleave
        for (int f = 0; f < frames_to_process; f++) {
            for (int ch = 0; ch < in_ch; ch++) {
                in_channels[ch][f] = in_interleaved[f * in_ch + ch];
            }
        }

        // Clear output
        for (int ch = 0; ch < out_ch; ch++) {
            std::fill(out_channels[ch].begin(), out_channels[ch].end(), 0.0f);
        }

        // Process
        if (use_double && mh_supports_double(p)) {
            // Convert to double, process, convert back
            std::vector<std::vector<double>> in_d(in_ch, std::vector<double>(block_size));
            std::vector<std::vector<double>> out_d(out_ch, std::vector<double>(block_size));
            std::vector<const double*> in_d_ptrs(in_ch);
            std::vector<double*> out_d_ptrs(out_ch);

            for (int ch = 0; ch < in_ch; ch++) {
                for (int f = 0; f < frames_to_process; f++) {
                    in_d[ch][f] = in_channels[ch][f];
                }
                in_d_ptrs[ch] = in_d[ch].data();
            }
            for (int ch = 0; ch < out_ch; ch++) {
                out_d_ptrs[ch] = out_d[ch].data();
            }

            mh_process_double(p, in_d_ptrs.data(), out_d_ptrs.data(), frames_to_process);

            for (int ch = 0; ch < out_ch; ch++) {
                for (int f = 0; f < frames_to_process; f++) {
                    out_channels[ch][f] = static_cast<float>(out_d[ch][f]);
                }
            }
        } else {
            mh_process(p, in_ptrs.data(), out_ptrs.data(), frames_to_process);
        }

        // Interleave output
        for (int f = 0; f < frames_to_process; f++) {
            for (int ch = 0; ch < out_ch; ch++) {
                out_interleaved[f * out_ch + ch] = out_channels[ch][f];
            }
        }

        // Write output
        ofs.write(reinterpret_cast<char*>(out_interleaved.data()),
                  frames_to_process * out_ch * sizeof(float));

        frames_processed += frames_to_process;
    }

    std::fprintf(stderr, "Processed %zu frames (%d in, %d out) @ %.0f Hz\n",
                 frames_processed, in_ch, out_ch, sample_rate);

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

    info_cmd->add_option("plugin", info_path, "Path to plugin")
        ->required();

    info_cmd->callback([&]() {
        std::exit(cmd_info(info_path, sample_rate, block_size));
    });

    // ========================================================================
    // Subcommand: params
    // ========================================================================
    auto* params_cmd = app.add_subcommand("params", "List plugin parameters");
    std::string params_path;
    bool params_json = false;

    params_cmd->add_option("plugin", params_path, "Path to plugin")
        ->required();
    params_cmd->add_flag("-j,--json", params_json, "Output as JSON");

    params_cmd->callback([&]() {
        std::exit(cmd_params(params_path, sample_rate, block_size, params_json));
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
    auto* presets_cmd = app.add_subcommand("presets", "List factory presets");
    std::string presets_path;

    presets_cmd->add_option("plugin", presets_path, "Path to plugin")
        ->required();

    presets_cmd->callback([&]() {
        std::exit(cmd_presets(presets_path, sample_rate, block_size));
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
    auto* process_cmd = app.add_subcommand("process", "Process raw audio file");
    std::string process_plugin;
    std::string process_input;
    std::string process_output;
    std::string process_state;
    bool process_double = false;

    process_cmd->add_option("plugin", process_plugin, "Path to plugin")
        ->required();
    process_cmd->add_option("input", process_input, "Input file (raw float32 interleaved)")
        ->required();
    process_cmd->add_option("output", process_output, "Output file (raw float32 interleaved)")
        ->required();
    process_cmd->add_option("-s,--state", process_state, "State file to load");
    process_cmd->add_flag("-d,--double", process_double, "Use double precision if supported");

    process_cmd->callback([&]() {
        std::exit(cmd_process(process_plugin, process_input, process_output,
                              sample_rate, block_size, process_state, process_double));
    });

    // Parse and run
    CLI11_PARSE(app, argc, argv);

    return 0;
}
