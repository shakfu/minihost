// main.c - CLI frontend for minihost (pure C implementation)
// Provides command-line access to plugin hosting features

#include "minihost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Helper functions
// ============================================================================

static void print_usage(const char* prog) {
    printf("minihost - Audio plugin hosting CLI (C version)\n\n");
    printf("Usage: %s [OPTIONS] COMMAND [ARGS]\n\n", prog);
    printf("Global Options:\n");
    printf("  -r, --rate RATE      Sample rate in Hz (default: 48000)\n");
    printf("  -b, --block SIZE     Block size in samples (default: 512)\n");
    printf("  -h, --help           Show this help message\n\n");
    printf("Commands:\n");
    printf("  probe PLUGIN         Get plugin metadata without loading\n");
    printf("  scan DIRECTORY       Scan directory for plugins\n");
    printf("  info PLUGIN          Show detailed plugin information\n");
    printf("  params PLUGIN        List plugin parameters\n");
    printf("  get-param PLUGIN N   Get parameter N value\n");
    printf("  set-param PLUGIN N V Set parameter N to value V (0.0-1.0)\n");
    printf("  presets PLUGIN       List factory presets\n");
    printf("  load-preset PLUGIN N Load factory preset N\n");
    printf("  save-state PLUGIN F  Save plugin state to file F\n");
    printf("  load-state PLUGIN F  Load plugin state from file F\n");
    printf("  process PLUGIN I O   Process raw audio file I to O\n\n");
    printf("Options for specific commands:\n");
    printf("  -j, --json           Output as JSON (probe, scan, params)\n");
    printf("  -s, --state FILE     State file (set-param, load-preset, process)\n");
    printf("  -d, --double         Use double precision (process)\n");
    printf("  -p, --params         Show params after loading (load-state)\n");
}

static int str_eq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
}

static int str_starts(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static float** alloc_channels(int ch, int n) {
    float** p = (float**)calloc((size_t)ch, sizeof(float*));
    if (!p) return NULL;
    for (int i = 0; i < ch; ++i) {
        p[i] = (float*)calloc((size_t)n, sizeof(float));
        if (!p[i]) {
            for (int j = 0; j < i; ++j) free(p[j]);
            free(p);
            return NULL;
        }
    }
    return p;
}

static void free_channels(float** p, int ch) {
    if (!p) return;
    for (int i = 0; i < ch; ++i) free(p[i]);
    free(p);
}

static double** alloc_channels_double(int ch, int n) {
    double** p = (double**)calloc((size_t)ch, sizeof(double*));
    if (!p) return NULL;
    for (int i = 0; i < ch; ++i) {
        p[i] = (double*)calloc((size_t)n, sizeof(double));
        if (!p[i]) {
            for (int j = 0; j < i; ++j) free(p[j]);
            free(p);
            return NULL;
        }
    }
    return p;
}

static void free_channels_double(double** p, int ch) {
    if (!p) return;
    for (int i = 0; i < ch; ++i) free(p[i]);
    free(p);
}

static int min_int(int a, int b) { return a < b ? a : b; }
static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }

// ============================================================================
// Command: probe
// ============================================================================

static int cmd_probe(const char* plugin_path, int json_output) {
    MH_PluginDesc desc;
    char err[1024] = {0};

    if (!mh_probe(plugin_path, &desc, err, sizeof(err))) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    if (json_output) {
        printf("{\n");
        printf("  \"name\": \"%s\",\n", desc.name);
        printf("  \"vendor\": \"%s\",\n", desc.vendor);
        printf("  \"version\": \"%s\",\n", desc.version);
        printf("  \"format\": \"%s\",\n", desc.format);
        printf("  \"unique_id\": \"%s\",\n", desc.unique_id);
        printf("  \"accepts_midi\": %s,\n", desc.accepts_midi ? "true" : "false");
        printf("  \"produces_midi\": %s,\n", desc.produces_midi ? "true" : "false");
        printf("  \"num_inputs\": %d,\n", desc.num_inputs);
        printf("  \"num_outputs\": %d\n", desc.num_outputs);
        printf("}\n");
    } else {
        printf("Name:      %s\n", desc.name);
        printf("Vendor:    %s\n", desc.vendor);
        printf("Version:   %s\n", desc.version);
        printf("Format:    %s\n", desc.format);
        printf("ID:        %s\n", desc.unique_id);
        printf("MIDI In:   %s\n", desc.accepts_midi ? "yes" : "no");
        printf("MIDI Out:  %s\n", desc.produces_midi ? "yes" : "no");
        printf("Inputs:    %d\n", desc.num_inputs);
        printf("Outputs:   %d\n", desc.num_outputs);
    }

    return 0;
}

// ============================================================================
// Command: scan
// ============================================================================

typedef struct {
    int json;
    int count;
    int first;
} ScanContext;

static void scan_callback(const MH_PluginDesc* desc, void* user_data) {
    ScanContext* ctx = (ScanContext*)user_data;

    if (ctx->json) {
        if (!ctx->first) printf(",\n");
        ctx->first = 0;
        printf("  {\n");
        printf("    \"name\": \"%s\",\n", desc->name);
        printf("    \"vendor\": \"%s\",\n", desc->vendor);
        printf("    \"format\": \"%s\",\n", desc->format);
        printf("    \"path\": \"%s\"\n", desc->path);
        printf("  }");
    } else {
        printf("[%d] %s (%s) - %s\n",
               ctx->count + 1,
               desc->name,
               desc->format,
               desc->path);
    }
    ctx->count++;
}

static int cmd_scan(const char* directory, int json_output) {
    ScanContext ctx = {json_output, 0, 1};

    if (json_output) {
        printf("[\n");
    }

    int result = mh_scan_directory(directory, scan_callback, &ctx);

    if (json_output) {
        if (ctx.count > 0) printf("\n");
        printf("]\n");
    }

    if (result < 0) {
        fprintf(stderr, "Error: Failed to scan directory\n");
        return 1;
    }

    if (!json_output) {
        printf("\nFound %d plugin(s)\n", ctx.count);
    }

    return 0;
}

// ============================================================================
// Command: info
// ============================================================================

static int cmd_info(const char* plugin_path, double sample_rate, int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    MH_PluginDesc desc;
    if (mh_probe(plugin_path, &desc, err, sizeof(err))) {
        printf("Name:      %s\n", desc.name);
        printf("Vendor:    %s\n", desc.vendor);
        printf("Version:   %s\n", desc.version);
        printf("Format:    %s\n", desc.format);
        printf("ID:        %s\n", desc.unique_id);
        printf("MIDI In:   %s\n", desc.accepts_midi ? "yes" : "no");
        printf("MIDI Out:  %s\n", desc.produces_midi ? "yes" : "no");
        printf("Inputs:    %d\n", desc.num_inputs);
        printf("Outputs:   %d\n", desc.num_outputs);
    }

    MH_Info info;
    mh_get_info(p, &info);

    printf("\nRuntime Info:\n");
    printf("  Sample Rate:    %.0f Hz\n", mh_get_sample_rate(p));
    printf("  Parameters:     %d\n", info.num_params);
    printf("  Input Ch:       %d\n", info.num_input_ch);
    printf("  Output Ch:      %d\n", info.num_output_ch);
    printf("  Latency:        %d samples\n", info.latency_samples);
    printf("  Tail:           %.3f s\n", mh_get_tail_seconds(p));
    printf("  Double Prec:    %s\n", mh_supports_double(p) ? "yes" : "no");

    // Bus info
    int num_in_buses = mh_get_num_buses(p, 1);
    int num_out_buses = mh_get_num_buses(p, 0);

    if (num_in_buses > 0) {
        printf("\nInput Buses:\n");
        for (int i = 0; i < num_in_buses; i++) {
            MH_BusInfo bus;
            if (mh_get_bus_info(p, 1, i, &bus)) {
                printf("  [%d] %-20s  %d ch  %s%s\n",
                       i, bus.name, bus.num_channels,
                       bus.is_main ? "[main]" : "",
                       bus.is_enabled ? "" : " (disabled)");
            }
        }
    }

    if (num_out_buses > 0) {
        printf("\nOutput Buses:\n");
        for (int i = 0; i < num_out_buses; i++) {
            MH_BusInfo bus;
            if (mh_get_bus_info(p, 0, i, &bus)) {
                printf("  [%d] %-20s  %d ch  %s%s\n",
                       i, bus.name, bus.num_channels,
                       bus.is_main ? "[main]" : "",
                       bus.is_enabled ? "" : " (disabled)");
            }
        }
    }

    // Factory presets
    int num_programs = mh_get_num_programs(p);
    if (num_programs > 0) {
        printf("\nFactory Presets: %d\n", num_programs);
        int current = mh_get_program(p);
        int show_count = min_int(num_programs, 10);
        for (int i = 0; i < show_count; i++) {
            char name[256] = {0};
            mh_get_program_name(p, i, name, sizeof(name));
            printf("  [%d] %s%s\n", i, name, (i == current) ? " (current)" : "");
        }
        if (num_programs > 10) {
            printf("  ... and %d more\n", num_programs - 10);
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: params
// ============================================================================

static int cmd_params(const char* plugin_path, double sample_rate, int block_size, int json_output) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    int num_params = mh_get_num_params(p);

    if (json_output) {
        printf("[\n");
        for (int i = 0; i < num_params; i++) {
            MH_ParamInfo info;
            if (mh_get_param_info(p, i, &info)) {
                float value = mh_get_param(p, i);
                if (i > 0) printf(",\n");
                printf("  {\n");
                printf("    \"index\": %d,\n", i);
                printf("    \"name\": \"%s\",\n", info.name);
                printf("    \"label\": \"%s\",\n", info.label);
                printf("    \"value\": %.6f,\n", value);
                printf("    \"value_str\": \"%s\",\n", info.current_value_str);
                printf("    \"default\": %.6f,\n", info.default_value);
                printf("    \"automatable\": %s,\n", info.is_automatable ? "true" : "false");
                printf("    \"boolean\": %s,\n", info.is_boolean ? "true" : "false");
                printf("    \"steps\": %d\n", info.num_steps);
                printf("  }");
            }
        }
        printf("\n]\n");
    } else {
        printf("Parameters (%d):\n", num_params);
        for (int i = 0; i < num_params; i++) {
            MH_ParamInfo info;
            if (mh_get_param_info(p, i, &info)) {
                float value = mh_get_param(p, i);
                printf("  [%3d] %-30s = %.4f", i, info.name, value);
                if (info.label[0] != '\0') {
                    printf(" %s", info.label);
                }
                printf(" (%s)\n", info.current_value_str);
            }
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: get-param
// ============================================================================

static int cmd_get_param(const char* plugin_path, int param_index,
                         double sample_rate, int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    int num_params = mh_get_num_params(p);
    if (param_index < 0 || param_index >= num_params) {
        fprintf(stderr, "Error: Parameter index %d out of range (0-%d)\n",
                param_index, num_params - 1);
        mh_close(p);
        return 1;
    }

    MH_ParamInfo info;
    float value = mh_get_param(p, param_index);

    if (mh_get_param_info(p, param_index, &info)) {
        printf("%s = %.6f (%s)\n", info.name, value, info.current_value_str);
    } else {
        printf("%.6f\n", value);
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: set-param
// ============================================================================

static int cmd_set_param(const char* plugin_path, int param_index, float param_value,
                         double sample_rate, int block_size, const char* state_file) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    // Load state if provided
    if (state_file && state_file[0] != '\0') {
        FILE* f = fopen(state_file, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            void* data = malloc((size_t)size);
            if (data) {
                if (fread(data, 1, (size_t)size, f) == (size_t)size) {
                    mh_set_state(p, data, (int)size);
                }
                free(data);
            }
            fclose(f);
        }
    }

    int num_params = mh_get_num_params(p);
    if (param_index < 0 || param_index >= num_params) {
        fprintf(stderr, "Error: Parameter index %d out of range (0-%d)\n",
                param_index, num_params - 1);
        mh_close(p);
        return 1;
    }

    if (!mh_set_param(p, param_index, param_value)) {
        fprintf(stderr, "Error: Failed to set parameter\n");
        mh_close(p);
        return 1;
    }

    // Show result
    MH_ParamInfo info;
    float new_value = mh_get_param(p, param_index);
    if (mh_get_param_info(p, param_index, &info)) {
        printf("%s = %.6f (%s)\n", info.name, new_value, info.current_value_str);
    }

    // Save state if file was provided
    if (state_file && state_file[0] != '\0') {
        int size = mh_get_state_size(p);
        if (size > 0) {
            void* data = malloc((size_t)size);
            if (data && mh_get_state(p, data, size)) {
                FILE* f = fopen(state_file, "wb");
                if (f) {
                    fwrite(data, 1, (size_t)size, f);
                    fclose(f);
                    printf("State saved to %s\n", state_file);
                }
            }
            free(data);
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: presets
// ============================================================================

static int cmd_presets(const char* plugin_path, double sample_rate, int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    int num_programs = mh_get_num_programs(p);
    int current = mh_get_program(p);

    printf("Factory Presets (%d):\n", num_programs);
    for (int i = 0; i < num_programs; i++) {
        char name[256] = {0};
        mh_get_program_name(p, i, name, sizeof(name));
        printf("  [%3d] %s%s\n", i, name, (i == current) ? " *" : "");
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: load-preset
// ============================================================================

static int cmd_load_preset(const char* plugin_path, int preset_index,
                           double sample_rate, int block_size, const char* state_file) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    int num_programs = mh_get_num_programs(p);
    if (preset_index < 0 || preset_index >= num_programs) {
        fprintf(stderr, "Error: Preset index %d out of range (0-%d)\n",
                preset_index, num_programs - 1);
        mh_close(p);
        return 1;
    }

    if (!mh_set_program(p, preset_index)) {
        fprintf(stderr, "Error: Failed to load preset\n");
        mh_close(p);
        return 1;
    }

    char name[256] = {0};
    mh_get_program_name(p, preset_index, name, sizeof(name));
    printf("Loaded preset [%d]: %s\n", preset_index, name);

    // Save state if file was provided
    if (state_file && state_file[0] != '\0') {
        int size = mh_get_state_size(p);
        if (size > 0) {
            void* data = malloc((size_t)size);
            if (data && mh_get_state(p, data, size)) {
                FILE* f = fopen(state_file, "wb");
                if (f) {
                    fwrite(data, 1, (size_t)size, f);
                    fclose(f);
                    printf("State saved to %s\n", state_file);
                }
            }
            free(data);
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: save-state
// ============================================================================

static int cmd_save_state(const char* plugin_path, const char* state_file,
                          double sample_rate, int block_size) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    int size = mh_get_state_size(p);
    if (size <= 0) {
        fprintf(stderr, "Error: Plugin has no state to save\n");
        mh_close(p);
        return 1;
    }

    void* data = malloc((size_t)size);
    if (!data) {
        fprintf(stderr, "Error: Out of memory\n");
        mh_close(p);
        return 1;
    }

    if (!mh_get_state(p, data, size)) {
        fprintf(stderr, "Error: Failed to get plugin state\n");
        free(data);
        mh_close(p);
        return 1;
    }

    FILE* f = fopen(state_file, "wb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s for writing\n", state_file);
        free(data);
        mh_close(p);
        return 1;
    }

    fwrite(data, 1, (size_t)size, f);
    fclose(f);
    free(data);

    printf("State saved to %s (%d bytes)\n", state_file, size);

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: load-state
// ============================================================================

static int cmd_load_state(const char* plugin_path, const char* state_file,
                          double sample_rate, int block_size, int show_params) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    FILE* f = fopen(state_file, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open %s for reading\n", state_file);
        mh_close(p);
        return 1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    void* data = malloc((size_t)size);
    if (!data) {
        fprintf(stderr, "Error: Out of memory\n");
        fclose(f);
        mh_close(p);
        return 1;
    }

    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fprintf(stderr, "Error: Failed to read state file\n");
        free(data);
        fclose(f);
        mh_close(p);
        return 1;
    }
    fclose(f);

    if (!mh_set_state(p, data, (int)size)) {
        fprintf(stderr, "Error: Failed to restore plugin state\n");
        free(data);
        mh_close(p);
        return 1;
    }
    free(data);

    printf("State loaded from %s (%ld bytes)\n", state_file, size);

    if (show_params) {
        int num_params = mh_get_num_params(p);
        printf("\nParameters after loading:\n");
        for (int i = 0; i < num_params; i++) {
            MH_ParamInfo info;
            if (mh_get_param_info(p, i, &info)) {
                float value = mh_get_param(p, i);
                printf("  [%3d] %-30s = %.4f", i, info.name, value);
                if (info.label[0] != '\0') {
                    printf(" %s", info.label);
                }
                printf(" (%s)\n", info.current_value_str);
            }
        }
    }

    mh_close(p);
    return 0;
}

// ============================================================================
// Command: process
// ============================================================================

static int cmd_process(const char* plugin_path, const char* input_file,
                       const char* output_file, double sample_rate, int block_size,
                       const char* state_file, int use_double) {
    char err[1024] = {0};

    // Open plugin
    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    // Load state if provided
    if (state_file && state_file[0] != '\0') {
        FILE* sf = fopen(state_file, "rb");
        if (sf) {
            fseek(sf, 0, SEEK_END);
            long ssize = ftell(sf);
            fseek(sf, 0, SEEK_SET);
            void* sdata = malloc((size_t)ssize);
            if (sdata) {
                if (fread(sdata, 1, (size_t)ssize, sf) == (size_t)ssize) {
                    if (mh_set_state(p, sdata, (int)ssize)) {
                        fprintf(stderr, "Loaded state from %s\n", state_file);
                    }
                }
                free(sdata);
            }
            fclose(sf);
        }
    }

    MH_Info info;
    mh_get_info(p, &info);

    int in_ch = info.num_input_ch > 0 ? info.num_input_ch : 2;
    int out_ch = info.num_output_ch > 0 ? info.num_output_ch : 2;

    // Open input file
    FILE* fin = fopen(input_file, "rb");
    if (!fin) {
        fprintf(stderr, "Error: Cannot open input file %s\n", input_file);
        mh_close(p);
        return 1;
    }

    // Get file size
    fseek(fin, 0, SEEK_END);
    long file_size = ftell(fin);
    fseek(fin, 0, SEEK_SET);

    size_t total_frames = (size_t)file_size / (sizeof(float) * (size_t)in_ch);

    // Open output file
    FILE* fout = fopen(output_file, "wb");
    if (!fout) {
        fprintf(stderr, "Error: Cannot open output file %s\n", output_file);
        fclose(fin);
        mh_close(p);
        return 1;
    }

    // Allocate buffers
    float* in_interleaved = (float*)malloc((size_t)block_size * (size_t)in_ch * sizeof(float));
    float* out_interleaved = (float*)malloc((size_t)block_size * (size_t)out_ch * sizeof(float));
    float** in_channels = alloc_channels(in_ch, block_size);
    float** out_channels = alloc_channels(out_ch, block_size);

    if (!in_interleaved || !out_interleaved || !in_channels || !out_channels) {
        fprintf(stderr, "Error: Out of memory\n");
        free(in_interleaved);
        free(out_interleaved);
        free_channels(in_channels, in_ch);
        free_channels(out_channels, out_ch);
        fclose(fin);
        fclose(fout);
        mh_close(p);
        return 1;
    }

    // Double precision buffers (allocated only if needed)
    double** in_d = NULL;
    double** out_d = NULL;
    int supports_double = mh_supports_double(p);

    if (use_double && supports_double) {
        in_d = alloc_channels_double(in_ch, block_size);
        out_d = alloc_channels_double(out_ch, block_size);
    }

    size_t frames_processed = 0;

    while (frames_processed < total_frames) {
        int frames_to_process = (int)min_size((size_t)block_size, total_frames - frames_processed);

        // Read interleaved input
        size_t read_count = fread(in_interleaved, sizeof(float),
                                   (size_t)frames_to_process * (size_t)in_ch, fin);
        if (read_count < (size_t)frames_to_process * (size_t)in_ch) {
            // Pad with zeros
            memset(in_interleaved + read_count, 0,
                   ((size_t)frames_to_process * (size_t)in_ch - read_count) * sizeof(float));
        }

        // Deinterleave
        for (int f = 0; f < frames_to_process; f++) {
            for (int ch = 0; ch < in_ch; ch++) {
                in_channels[ch][f] = in_interleaved[f * in_ch + ch];
            }
        }

        // Clear output
        for (int ch = 0; ch < out_ch; ch++) {
            memset(out_channels[ch], 0, (size_t)block_size * sizeof(float));
        }

        // Process
        if (use_double && supports_double && in_d && out_d) {
            // Convert to double
            for (int ch = 0; ch < in_ch; ch++) {
                for (int f = 0; f < frames_to_process; f++) {
                    in_d[ch][f] = (double)in_channels[ch][f];
                }
            }
            for (int ch = 0; ch < out_ch; ch++) {
                memset(out_d[ch], 0, (size_t)block_size * sizeof(double));
            }

            mh_process_double(p, (const double* const*)in_d, out_d, frames_to_process);

            // Convert back to float
            for (int ch = 0; ch < out_ch; ch++) {
                for (int f = 0; f < frames_to_process; f++) {
                    out_channels[ch][f] = (float)out_d[ch][f];
                }
            }
        } else {
            mh_process(p, (const float* const*)in_channels, out_channels, frames_to_process);
        }

        // Interleave output
        for (int f = 0; f < frames_to_process; f++) {
            for (int ch = 0; ch < out_ch; ch++) {
                out_interleaved[f * out_ch + ch] = out_channels[ch][f];
            }
        }

        // Write output
        fwrite(out_interleaved, sizeof(float), (size_t)frames_to_process * (size_t)out_ch, fout);

        frames_processed += (size_t)frames_to_process;
    }

    fprintf(stderr, "Processed %zu frames (%d in, %d out) @ %.0f Hz\n",
            frames_processed, in_ch, out_ch, sample_rate);

    // Cleanup
    free(in_interleaved);
    free(out_interleaved);
    free_channels(in_channels, in_ch);
    free_channels(out_channels, out_ch);
    if (in_d) free_channels_double(in_d, in_ch);
    if (out_d) free_channels_double(out_d, out_ch);
    fclose(fin);
    fclose(fout);
    mh_close(p);
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Default options
    double sample_rate = 48000.0;
    int block_size = 512;
    int json_output = 0;
    int use_double = 0;
    int show_params = 0;
    const char* state_file = NULL;

    // Parse global options and find command
    int cmd_index = 1;
    while (cmd_index < argc && argv[cmd_index][0] == '-') {
        const char* opt = argv[cmd_index];

        if (str_eq(opt, "-h") || str_eq(opt, "--help")) {
            print_usage(argv[0]);
            return 0;
        } else if (str_eq(opt, "-r") || str_eq(opt, "--rate")) {
            if (cmd_index + 1 >= argc) {
                fprintf(stderr, "Error: -r requires a value\n");
                return 1;
            }
            sample_rate = atof(argv[++cmd_index]);
        } else if (str_eq(opt, "-b") || str_eq(opt, "--block")) {
            if (cmd_index + 1 >= argc) {
                fprintf(stderr, "Error: -b requires a value\n");
                return 1;
            }
            block_size = atoi(argv[++cmd_index]);
        } else if (str_eq(opt, "-j") || str_eq(opt, "--json")) {
            json_output = 1;
        } else if (str_eq(opt, "-d") || str_eq(opt, "--double")) {
            use_double = 1;
        } else if (str_eq(opt, "-p") || str_eq(opt, "--params")) {
            show_params = 1;
        } else if (str_eq(opt, "-s") || str_eq(opt, "--state")) {
            if (cmd_index + 1 >= argc) {
                fprintf(stderr, "Error: -s requires a file path\n");
                return 1;
            }
            state_file = argv[++cmd_index];
        } else {
            fprintf(stderr, "Error: Unknown option %s\n", opt);
            return 1;
        }
        cmd_index++;
    }

    if (cmd_index >= argc) {
        fprintf(stderr, "Error: No command specified\n");
        print_usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[cmd_index];
    int remaining = argc - cmd_index - 1;
    char** args = argv + cmd_index + 1;

    // Also check for options after the command
    for (int i = 0; i < remaining; i++) {
        if (str_eq(args[i], "-j") || str_eq(args[i], "--json")) {
            json_output = 1;
            // Shift remaining args
            for (int j = i; j < remaining - 1; j++) args[j] = args[j + 1];
            remaining--;
            i--;
        } else if (str_eq(args[i], "-d") || str_eq(args[i], "--double")) {
            use_double = 1;
            for (int j = i; j < remaining - 1; j++) args[j] = args[j + 1];
            remaining--;
            i--;
        } else if (str_eq(args[i], "-p") || str_eq(args[i], "--params")) {
            show_params = 1;
            for (int j = i; j < remaining - 1; j++) args[j] = args[j + 1];
            remaining--;
            i--;
        } else if ((str_eq(args[i], "-s") || str_eq(args[i], "--state")) && i + 1 < remaining) {
            state_file = args[i + 1];
            for (int j = i; j < remaining - 2; j++) args[j] = args[j + 2];
            remaining -= 2;
            i--;
        }
    }

    // Dispatch to command handlers
    if (str_eq(cmd, "probe")) {
        if (remaining < 1) {
            fprintf(stderr, "Usage: %s probe PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_probe(args[0], json_output);
    }
    else if (str_eq(cmd, "scan")) {
        if (remaining < 1) {
            fprintf(stderr, "Usage: %s scan DIRECTORY\n", argv[0]);
            return 1;
        }
        return cmd_scan(args[0], json_output);
    }
    else if (str_eq(cmd, "info")) {
        if (remaining < 1) {
            fprintf(stderr, "Usage: %s info PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_info(args[0], sample_rate, block_size);
    }
    else if (str_eq(cmd, "params")) {
        if (remaining < 1) {
            fprintf(stderr, "Usage: %s params PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_params(args[0], sample_rate, block_size, json_output);
    }
    else if (str_eq(cmd, "get-param")) {
        if (remaining < 2) {
            fprintf(stderr, "Usage: %s get-param PLUGIN INDEX\n", argv[0]);
            return 1;
        }
        return cmd_get_param(args[0], atoi(args[1]), sample_rate, block_size);
    }
    else if (str_eq(cmd, "set-param")) {
        if (remaining < 3) {
            fprintf(stderr, "Usage: %s set-param PLUGIN INDEX VALUE\n", argv[0]);
            return 1;
        }
        return cmd_set_param(args[0], atoi(args[1]), (float)atof(args[2]),
                             sample_rate, block_size, state_file);
    }
    else if (str_eq(cmd, "presets")) {
        if (remaining < 1) {
            fprintf(stderr, "Usage: %s presets PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_presets(args[0], sample_rate, block_size);
    }
    else if (str_eq(cmd, "load-preset")) {
        if (remaining < 2) {
            fprintf(stderr, "Usage: %s load-preset PLUGIN INDEX\n", argv[0]);
            return 1;
        }
        return cmd_load_preset(args[0], atoi(args[1]), sample_rate, block_size, state_file);
    }
    else if (str_eq(cmd, "save-state")) {
        if (remaining < 2) {
            fprintf(stderr, "Usage: %s save-state PLUGIN FILE\n", argv[0]);
            return 1;
        }
        return cmd_save_state(args[0], args[1], sample_rate, block_size);
    }
    else if (str_eq(cmd, "load-state")) {
        if (remaining < 2) {
            fprintf(stderr, "Usage: %s load-state PLUGIN FILE\n", argv[0]);
            return 1;
        }
        return cmd_load_state(args[0], args[1], sample_rate, block_size, show_params);
    }
    else if (str_eq(cmd, "process")) {
        if (remaining < 3) {
            fprintf(stderr, "Usage: %s process PLUGIN INPUT OUTPUT\n", argv[0]);
            return 1;
        }
        return cmd_process(args[0], args[1], args[2], sample_rate, block_size,
                           state_file, use_double);
    }
    else {
        fprintf(stderr, "Error: Unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}
