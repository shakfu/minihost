// main.c - CLI frontend for minihost (pure C implementation)
// Provides command-line access to plugin hosting features

#include "minihost.h"
#include "minihost_audiofile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
    printf("  probe PLUGIN            Get plugin metadata without loading\n");
    printf("  scan DIRECTORY          Scan directory for plugins\n");
    printf("  info PLUGIN             Show detailed plugin information\n");
    printf("  params PLUGIN           List plugin parameters\n");
    printf("  get-param PLUGIN N      Get parameter N value\n");
    printf("  set-param PLUGIN N V    Set parameter N to value V (0.0-1.0)\n");
    printf("  presets PLUGIN          List factory presets\n");
    printf("  load-preset PLUGIN N    Load factory preset N\n");
    printf("  save-state PLUGIN F     Save plugin state to file F\n");
    printf("  load-state PLUGIN F     Load plugin state from file F\n");
    printf("  process PLUGIN          Process audio through plugin\n\n");
    printf("Options for specific commands:\n");
    printf("  -j, --json              Output as JSON (probe, scan, params, info)\n");
    printf("  -s, --state FILE        State file (set-param, load-preset, process)\n");
    printf("  -d, --double            Use double precision (process)\n");
    printf("  -p, --params            Show params after loading (load-state)\n");
    printf("  -V, --verbose           Show extended param info (params)\n");
    printf("  --probe                 Lightweight metadata-only mode (info)\n\n");
    printf("Process command options:\n");
    printf("  -i, --input FILE        Input audio file (WAV, FLAC, MP3)\n");
    printf("  -o, --output FILE       Output audio file (WAV, FLAC)\n");
    printf("  --sidechain FILE        Sidechain input audio file\n");
    printf("  --preset N              Load factory preset N\n");
    printf("  --param NAME:VALUE      Set parameter (repeatable)\n");
    printf("  --non-realtime          Enable non-realtime mode\n");
    printf("  --bpm BPM              Set transport BPM\n");
    printf("  --bit-depth N           Output bit depth (16, 24, 32)\n");
}

static int str_eq(const char* a, const char* b) {
    return strcmp(a, b) == 0;
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

// Detect audio file by extension
static int is_audio_file(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return 0;
    // Case-insensitive compare
    if (strcasecmp(dot, ".wav") == 0) return 1;
    if (strcasecmp(dot, ".flac") == 0) return 1;
    if (strcasecmp(dot, ".mp3") == 0) return 1;
    if (strcasecmp(dot, ".ogg") == 0) return 1;
    return 0;
}

// Parse "Name:value" or "index:value" parameter specification
static int parse_param_spec(MH_Plugin* p, const char* spec, int* out_index, float* out_value) {
    const char* colon = strchr(spec, ':');
    if (!colon) return 0;

    // Extract name part
    size_t name_len = (size_t)(colon - spec);
    char name[256] = {0};
    if (name_len >= sizeof(name)) return 0;
    memcpy(name, spec, name_len);
    name[name_len] = '\0';

    // Parse value
    char* end_ptr;
    *out_value = strtof(colon + 1, &end_ptr);
    if (end_ptr == colon + 1) return 0;

    // Try as numeric index
    int is_numeric = 1;
    for (size_t i = 0; i < name_len; i++) {
        if (!isdigit((unsigned char)name[i])) {
            is_numeric = 0;
            break;
        }
    }
    if (is_numeric && name_len > 0) {
        *out_index = atoi(name);
        return *out_index >= 0 && *out_index < mh_get_num_params(p);
    }

    // Try as parameter name
    int num_params = mh_get_num_params(p);
    for (int i = 0; i < num_params; i++) {
        MH_ParamInfo info;
        if (mh_get_param_info(p, i, &info)) {
            if (strcmp(name, info.name) == 0) {
                *out_index = i;
                return 1;
            }
        }
    }

    return 0;
}

// Load state from file into plugin
static int load_state_file(MH_Plugin* p, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    void* data = malloc((size_t)size);
    if (!data) { fclose(f); return 0; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data); fclose(f); return 0;
    }
    fclose(f);
    int ok = mh_set_state(p, data, (int)size);
    free(data);
    return ok;
}

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

static int cmd_info(const char* plugin_path, double sample_rate, int block_size,
                    int probe_only, int json_output) {
    char err[1024] = {0};

    // Probe-only mode: lightweight metadata without full load
    if (probe_only) {
        return cmd_probe(plugin_path, json_output);
    }

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    MH_PluginDesc desc;
    int have_desc = mh_probe(plugin_path, &desc, err, sizeof(err));

    MH_Info info;
    mh_get_info(p, &info);

    if (json_output) {
        printf("{\n");
        if (have_desc) {
            printf("  \"name\": \"%s\",\n", desc.name);
            printf("  \"vendor\": \"%s\",\n", desc.vendor);
            printf("  \"version\": \"%s\",\n", desc.version);
            printf("  \"format\": \"%s\",\n", desc.format);
            printf("  \"unique_id\": \"%s\",\n", desc.unique_id);
            printf("  \"accepts_midi\": %s,\n", desc.accepts_midi ? "true" : "false");
            printf("  \"produces_midi\": %s,\n", desc.produces_midi ? "true" : "false");
            printf("  \"num_inputs\": %d,\n", desc.num_inputs);
            printf("  \"num_outputs\": %d,\n", desc.num_outputs);
        }
        printf("  \"sample_rate\": %.0f,\n", mh_get_sample_rate(p));
        printf("  \"num_params\": %d,\n", info.num_params);
        printf("  \"num_input_channels\": %d,\n", info.num_input_ch);
        printf("  \"num_output_channels\": %d,\n", info.num_output_ch);
        printf("  \"latency_samples\": %d,\n", info.latency_samples);
        printf("  \"tail_seconds\": %.3f,\n", mh_get_tail_seconds(p));
        printf("  \"supports_double\": %s,\n", mh_supports_double(p) ? "true" : "false");
        printf("  \"num_programs\": %d\n", mh_get_num_programs(p));
        printf("}\n");
        mh_close(p);
        return 0;
    }

    if (have_desc) {
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

static int cmd_params(const char* plugin_path, double sample_rate, int block_size,
                      int json_output, int verbose) {
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
    } else if (verbose) {
        printf("Parameters (%d):\n", num_params);
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

                printf("  [%3d] %s\n", i, info.name);
                printf("         Value:   %.4f", value);
                if (info.label[0] != '\0') printf(" %s", info.label);
                printf(" (%s)\n", info.current_value_str);
                printf("         Range:   %s .. %s\n", min_text, max_text);
                printf("         Default: %.4f (%s)\n", info.default_value, default_text);

                // Flags
                int has_flags = 0;
                if (info.is_automatable || info.num_steps > 0) {
                    printf("         Flags:   ");
                    if (info.is_automatable) {
                        printf("automatable");
                        has_flags = 1;
                    }
                    if (info.num_steps > 0) {
                        if (has_flags) printf(", ");
                        printf("discrete, %d steps", info.num_steps);
                    }
                    printf("\n");
                }
            }
        }
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
        load_state_file(p, state_file);
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

// Max param overrides supported via CLI
#define MAX_PARAM_SPECS 64

static int cmd_process(const char* plugin_path,
                       const char* input_file,
                       const char* output_file,
                       const char* sidechain_file,
                       double sample_rate,
                       int block_size,
                       const char* state_file,
                       int preset_index,
                       const char** param_specs,
                       int num_param_specs,
                       int use_double,
                       int non_realtime,
                       double bpm,
                       int bit_depth) {
    char err[1024] = {0};

    int has_audio_input = (input_file && input_file[0] != '\0');
    int has_sidechain = (sidechain_file && sidechain_file[0] != '\0');

    if (!has_audio_input) {
        fprintf(stderr, "Error: Input file is required\n");
        return 1;
    }

    // --- Read audio input ---
    MH_AudioData* audio_data = NULL;
    float* raw_data = NULL;
    int in_ch = 2;
    int in_frames = 0;
    int input_is_audio = is_audio_file(input_file);

    if (input_is_audio) {
        audio_data = mh_audio_read(input_file, err, sizeof(err));
        if (!audio_data) {
            fprintf(stderr, "Error: %s\n", err);
            return 1;
        }
        in_ch = (int)audio_data->channels;
        in_frames = (int)audio_data->frames;
        sample_rate = (double)audio_data->sample_rate;
    } else {
        // Legacy raw float32 fallback
        FILE* fin = fopen(input_file, "rb");
        if (!fin) {
            fprintf(stderr, "Error: Cannot open input file %s\n", input_file);
            return 1;
        }
        fseek(fin, 0, SEEK_END);
        long file_size = ftell(fin);
        fseek(fin, 0, SEEK_SET);
        in_frames = (int)((size_t)file_size / (sizeof(float) * (size_t)in_ch));
        raw_data = (float*)malloc((size_t)file_size);
        if (!raw_data) {
            fprintf(stderr, "Error: Out of memory\n");
            fclose(fin);
            return 1;
        }
        fread(raw_data, 1, (size_t)file_size, fin);
        fclose(fin);
    }

    // --- Read sidechain input ---
    MH_AudioData* sc_data = NULL;
    int sc_ch = 0;
    if (has_sidechain) {
        sc_data = mh_audio_read(sidechain_file, err, sizeof(err));
        if (!sc_data) {
            fprintf(stderr, "Error: %s\n", err);
            if (audio_data) mh_audio_data_free(audio_data);
            free(raw_data);
            return 1;
        }
        sc_ch = (int)sc_data->channels;
    }

    // --- Open plugin ---
    MH_Plugin* p = NULL;
    if (sc_ch > 0) {
        p = mh_open_ex(plugin_path, sample_rate, block_size,
                       in_ch, 2, sc_ch, err, sizeof(err));
    } else {
        p = mh_open(plugin_path, sample_rate, block_size, in_ch, 2, err, sizeof(err));
    }
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        if (audio_data) mh_audio_data_free(audio_data);
        if (sc_data) mh_audio_data_free(sc_data);
        free(raw_data);
        return 1;
    }

    // --- Load state ---
    if (state_file && state_file[0] != '\0') {
        if (load_state_file(p, state_file)) {
            fprintf(stderr, "Loaded state from %s\n", state_file);
        } else {
            fprintf(stderr, "Warning: Failed to load state from %s\n", state_file);
        }
    }

    // --- Load preset ---
    if (preset_index >= 0) {
        int num_programs = mh_get_num_programs(p);
        if (preset_index >= num_programs) {
            fprintf(stderr, "Error: Preset index %d out of range (0-%d)\n",
                    preset_index, num_programs - 1);
            mh_close(p);
            if (audio_data) mh_audio_data_free(audio_data);
            if (sc_data) mh_audio_data_free(sc_data);
            free(raw_data);
            return 1;
        }
        mh_set_program(p, preset_index);
        char name[256] = {0};
        mh_get_program_name(p, preset_index, name, sizeof(name));
        fprintf(stderr, "Loaded preset [%d]: %s\n", preset_index, name);
    }

    // --- Apply static param overrides ---
    MH_ParamChange param_changes[MAX_PARAM_SPECS];
    int num_changes = 0;
    for (int i = 0; i < num_param_specs && i < MAX_PARAM_SPECS; i++) {
        int idx;
        float val;
        if (!parse_param_spec(p, param_specs[i], &idx, &val)) {
            fprintf(stderr, "Error: Invalid parameter spec '%s'\n", param_specs[i]);
            mh_close(p);
            if (audio_data) mh_audio_data_free(audio_data);
            if (sc_data) mh_audio_data_free(sc_data);
            free(raw_data);
            return 1;
        }
        mh_set_param(p, idx, val);
        param_changes[num_changes].sample_offset = 0;
        param_changes[num_changes].param_index = idx;
        param_changes[num_changes].value = val;
        num_changes++;
    }

    // --- Non-realtime mode ---
    if (non_realtime) {
        mh_set_non_realtime(p, 1);
    }

    // --- Transport ---
    if (bpm > 0) {
        MH_TransportInfo transport;
        memset(&transport, 0, sizeof(transport));
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
    int total_samples = in_frames;
    int output_total = total_samples + latency;

    // --- Print summary ---
    fprintf(stderr, "Plugin: %s\n", plugin_path);
    fprintf(stderr, "  Sample rate: %.0f Hz\n", sample_rate);
    fprintf(stderr, "  Block size:  %d\n", block_size);
    fprintf(stderr, "  Latency:     %d samples\n", latency);
    fprintf(stderr, "  Input:       %d ch, %d samples\n", in_ch, in_frames);
    if (has_sidechain) {
        fprintf(stderr, "  Sidechain:   %d ch\n", sc_ch);
    }
    if (num_changes > 0) {
        fprintf(stderr, "  Params:      %d override(s)\n", num_changes);
    }
    fprintf(stderr, "  Output:      %d ch -> %s\n", out_ch, output_file);

    // --- Deinterleave audio input ---
    float** in_channels = alloc_channels(in_ch, output_total);
    float** out_channels = alloc_channels(out_ch, output_total);
    float** sc_channels = has_sidechain ? alloc_channels(sc_ch, output_total) : NULL;

    if (!in_channels || !out_channels || (has_sidechain && !sc_channels)) {
        fprintf(stderr, "Error: Out of memory\n");
        free_channels(in_channels, in_ch);
        free_channels(out_channels, out_ch);
        if (sc_channels) free_channels(sc_channels, sc_ch);
        mh_close(p);
        if (audio_data) mh_audio_data_free(audio_data);
        if (sc_data) mh_audio_data_free(sc_data);
        free(raw_data);
        return 1;
    }

    // Fill input channels
    if (audio_data) {
        for (int f = 0; f < in_frames; f++) {
            for (int c = 0; c < in_ch; c++) {
                in_channels[c][f] = audio_data->data[f * in_ch + c];
            }
        }
    } else if (raw_data) {
        for (int f = 0; f < in_frames; f++) {
            for (int c = 0; c < in_ch; c++) {
                in_channels[c][f] = raw_data[f * in_ch + c];
            }
        }
    }

    if (sc_data && sc_channels) {
        int sc_frames = min_int((int)sc_data->frames, output_total);
        for (int f = 0; f < sc_frames; f++) {
            for (int c = 0; c < sc_ch; c++) {
                sc_channels[c][f] = sc_data->data[f * sc_ch + c];
            }
        }
    }

    // Free raw audio data (now deinterleaved)
    if (audio_data) mh_audio_data_free(audio_data);
    if (sc_data) mh_audio_data_free(sc_data);
    free(raw_data);
    audio_data = NULL;
    sc_data = NULL;
    raw_data = NULL;

    // --- Double precision buffers ---
    double** in_d = NULL;
    double** out_d = NULL;
    int supports_double = mh_supports_double(p);
    if (use_double && supports_double && !has_sidechain && num_changes == 0) {
        in_d = alloc_channels_double(in_ch, block_size);
        out_d = alloc_channels_double(out_ch, block_size);
    }

    // --- Process loop ---
    int has_param_automation = (num_changes > 0);

    for (int start = 0; start < output_total; start += block_size) {
        int end = start + block_size;
        if (end > output_total) end = output_total;
        int bsize = end - start;

        const float* in_ptrs[32];
        float* out_ptrs[32];
        for (int c = 0; c < in_ch && c < 32; c++)
            in_ptrs[c] = in_channels[c] + start;
        for (int c = 0; c < out_ch && c < 32; c++)
            out_ptrs[c] = out_channels[c] + start;

        if (has_sidechain && sc_channels) {
            const float* sc_ptrs[32];
            for (int c = 0; c < sc_ch && c < 32; c++)
                sc_ptrs[c] = sc_channels[c] + start;
            mh_process_sidechain(p, in_ptrs, out_ptrs, sc_ptrs, bsize);
        } else if (has_param_automation) {
            mh_process_auto(p,
                            in_ptrs, out_ptrs, bsize,
                            NULL, 0,
                            NULL, 0, NULL,
                            (start == 0) ? param_changes : NULL,
                            (start == 0) ? num_changes : 0);
        } else if (use_double && supports_double && in_d && out_d) {
            const double* in_d_ptrs[32];
            double* out_d_ptrs[32];
            for (int c = 0; c < in_ch && c < 32; c++) {
                for (int f = 0; f < bsize; f++)
                    in_d[c][f] = (double)in_ptrs[c][f];
                in_d_ptrs[c] = in_d[c];
            }
            for (int c = 0; c < out_ch && c < 32; c++) {
                memset(out_d[c], 0, (size_t)bsize * sizeof(double));
                out_d_ptrs[c] = out_d[c];
            }
            mh_process_double(p, in_d_ptrs, out_d_ptrs, bsize);
            for (int c = 0; c < out_ch && c < 32; c++) {
                for (int f = 0; f < bsize; f++)
                    out_ptrs[c][f] = (float)out_d[c][f];
            }
        } else {
            mh_process(p, in_ptrs, out_ptrs, bsize);
        }
    }

    // --- Latency compensation ---
    int write_offset = latency;
    int write_frames = total_samples;
    if (write_offset + write_frames > output_total) {
        write_frames = output_total - write_offset;
    }

    // --- Write output ---
    if (is_audio_file(output_file)) {
        // Interleave for audio file write
        float* out_interleaved = (float*)malloc((size_t)out_ch * (size_t)write_frames * sizeof(float));
        if (!out_interleaved) {
            fprintf(stderr, "Error: Out of memory\n");
            free_channels(in_channels, in_ch);
            free_channels(out_channels, out_ch);
            if (sc_channels) free_channels(sc_channels, sc_ch);
            if (in_d) free_channels_double(in_d, in_ch);
            if (out_d) free_channels_double(out_d, out_ch);
            mh_close(p);
            return 1;
        }
        for (int f = 0; f < write_frames; f++) {
            for (int c = 0; c < out_ch; c++) {
                out_interleaved[f * out_ch + c] = out_channels[c][write_offset + f];
            }
        }
        if (bit_depth <= 0) bit_depth = 24;
        if (!mh_audio_write(output_file, out_interleaved,
                            (unsigned)out_ch, (unsigned)write_frames,
                            (unsigned)sample_rate, bit_depth,
                            err, sizeof(err))) {
            fprintf(stderr, "Error: %s\n", err);
            free(out_interleaved);
            free_channels(in_channels, in_ch);
            free_channels(out_channels, out_ch);
            if (sc_channels) free_channels(sc_channels, sc_ch);
            if (in_d) free_channels_double(in_d, in_ch);
            if (out_d) free_channels_double(out_d, out_ch);
            mh_close(p);
            return 1;
        }
        free(out_interleaved);
    } else {
        // Raw float32 output
        FILE* fout = fopen(output_file, "wb");
        if (!fout) {
            fprintf(stderr, "Error: Cannot open output file %s\n", output_file);
            free_channels(in_channels, in_ch);
            free_channels(out_channels, out_ch);
            if (sc_channels) free_channels(sc_channels, sc_ch);
            if (in_d) free_channels_double(in_d, in_ch);
            if (out_d) free_channels_double(out_d, out_ch);
            mh_close(p);
            return 1;
        }
        float* out_buf = (float*)malloc((size_t)out_ch * sizeof(float));
        for (int f = 0; f < write_frames; f++) {
            for (int c = 0; c < out_ch; c++) {
                out_buf[c] = out_channels[c][write_offset + f];
            }
            fwrite(out_buf, sizeof(float), (size_t)out_ch, fout);
        }
        free(out_buf);
        fclose(fout);
    }

    double duration = (double)write_frames / sample_rate;
    fprintf(stderr, "Wrote %d samples (%.2fs) to %s\n", write_frames, duration, output_file);

    // Cleanup
    free_channels(in_channels, in_ch);
    free_channels(out_channels, out_ch);
    if (sc_channels) free_channels(sc_channels, sc_ch);
    if (in_d) free_channels_double(in_d, in_ch);
    if (out_d) free_channels_double(out_d, out_ch);
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
    int verbose = 0;
    int probe_only = 0;
    int non_realtime = 0;
    double bpm = 0.0;
    int bit_depth = 0;
    int preset_index = -1;
    const char* state_file = NULL;
    const char* input_file = NULL;
    const char* output_file = NULL;
    const char* sidechain_file = NULL;
    const char* param_specs[MAX_PARAM_SPECS];
    int num_param_specs = 0;

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
        } else if (str_eq(opt, "-V") || str_eq(opt, "--verbose")) {
            verbose = 1;
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

    // Parse options that can appear after the command
    int pos_args[16];
    int num_pos_args = 0;

    for (int i = 0; i < remaining; i++) {
        if (str_eq(args[i], "-j") || str_eq(args[i], "--json")) {
            json_output = 1;
        } else if (str_eq(args[i], "-d") || str_eq(args[i], "--double")) {
            use_double = 1;
        } else if (str_eq(args[i], "-p") || str_eq(args[i], "--params")) {
            show_params = 1;
        } else if (str_eq(args[i], "-V") || str_eq(args[i], "--verbose")) {
            verbose = 1;
        } else if (str_eq(args[i], "--probe")) {
            probe_only = 1;
        } else if (str_eq(args[i], "--non-realtime")) {
            non_realtime = 1;
        } else if ((str_eq(args[i], "-s") || str_eq(args[i], "--state")) && i + 1 < remaining) {
            state_file = args[++i];
        } else if ((str_eq(args[i], "-i") || str_eq(args[i], "--input")) && i + 1 < remaining) {
            input_file = args[++i];
        } else if ((str_eq(args[i], "-o") || str_eq(args[i], "--output")) && i + 1 < remaining) {
            output_file = args[++i];
        } else if (str_eq(args[i], "--sidechain") && i + 1 < remaining) {
            sidechain_file = args[++i];
        } else if (str_eq(args[i], "--preset") && i + 1 < remaining) {
            preset_index = atoi(args[++i]);
        } else if (str_eq(args[i], "--param") && i + 1 < remaining) {
            if (num_param_specs < MAX_PARAM_SPECS) {
                param_specs[num_param_specs++] = args[++i];
            } else {
                i++;
            }
        } else if (str_eq(args[i], "--bpm") && i + 1 < remaining) {
            bpm = atof(args[++i]);
        } else if (str_eq(args[i], "--bit-depth") && i + 1 < remaining) {
            bit_depth = atoi(args[++i]);
        } else {
            // Positional argument
            if (num_pos_args < 16) {
                pos_args[num_pos_args++] = i;
            }
        }
    }

    // Dispatch to command handlers
    if (str_eq(cmd, "probe")) {
        if (num_pos_args < 1) {
            fprintf(stderr, "Usage: %s probe PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_probe(args[pos_args[0]], json_output);
    }
    else if (str_eq(cmd, "scan")) {
        if (num_pos_args < 1) {
            fprintf(stderr, "Usage: %s scan DIRECTORY\n", argv[0]);
            return 1;
        }
        return cmd_scan(args[pos_args[0]], json_output);
    }
    else if (str_eq(cmd, "info")) {
        if (num_pos_args < 1) {
            fprintf(stderr, "Usage: %s info PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_info(args[pos_args[0]], sample_rate, block_size, probe_only, json_output);
    }
    else if (str_eq(cmd, "params")) {
        if (num_pos_args < 1) {
            fprintf(stderr, "Usage: %s params PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_params(args[pos_args[0]], sample_rate, block_size, json_output, verbose);
    }
    else if (str_eq(cmd, "get-param")) {
        if (num_pos_args < 2) {
            fprintf(stderr, "Usage: %s get-param PLUGIN INDEX\n", argv[0]);
            return 1;
        }
        return cmd_get_param(args[pos_args[0]], atoi(args[pos_args[1]]), sample_rate, block_size);
    }
    else if (str_eq(cmd, "set-param")) {
        if (num_pos_args < 3) {
            fprintf(stderr, "Usage: %s set-param PLUGIN INDEX VALUE\n", argv[0]);
            return 1;
        }
        return cmd_set_param(args[pos_args[0]], atoi(args[pos_args[1]]),
                             (float)atof(args[pos_args[2]]),
                             sample_rate, block_size, state_file);
    }
    else if (str_eq(cmd, "presets")) {
        if (num_pos_args < 1) {
            fprintf(stderr, "Usage: %s presets PLUGIN\n", argv[0]);
            return 1;
        }
        return cmd_presets(args[pos_args[0]], sample_rate, block_size);
    }
    else if (str_eq(cmd, "load-preset")) {
        if (num_pos_args < 2) {
            fprintf(stderr, "Usage: %s load-preset PLUGIN INDEX\n", argv[0]);
            return 1;
        }
        return cmd_load_preset(args[pos_args[0]], atoi(args[pos_args[1]]),
                               sample_rate, block_size, state_file);
    }
    else if (str_eq(cmd, "save-state")) {
        if (num_pos_args < 2) {
            fprintf(stderr, "Usage: %s save-state PLUGIN FILE\n", argv[0]);
            return 1;
        }
        return cmd_save_state(args[pos_args[0]], args[pos_args[1]], sample_rate, block_size);
    }
    else if (str_eq(cmd, "load-state")) {
        if (num_pos_args < 2) {
            fprintf(stderr, "Usage: %s load-state PLUGIN FILE\n", argv[0]);
            return 1;
        }
        return cmd_load_state(args[pos_args[0]], args[pos_args[1]],
                              sample_rate, block_size, show_params);
    }
    else if (str_eq(cmd, "process")) {
        if (num_pos_args < 1) {
            fprintf(stderr, "Usage: %s process PLUGIN -i INPUT -o OUTPUT [options]\n", argv[0]);
            return 1;
        }
        const char* plugin = args[pos_args[0]];

        // Support legacy positional: process PLUGIN INPUT OUTPUT
        if (!input_file && num_pos_args >= 2) {
            input_file = args[pos_args[1]];
        }
        if (!output_file && num_pos_args >= 3) {
            output_file = args[pos_args[2]];
        }

        if (!input_file || !output_file) {
            fprintf(stderr, "Error: Both input (-i) and output (-o) files are required\n");
            return 1;
        }

        return cmd_process(plugin, input_file, output_file, sidechain_file,
                           sample_rate, block_size, state_file,
                           preset_index, param_specs, num_param_specs,
                           use_double, non_realtime, bpm, bit_depth);
    }
    else {
        fprintf(stderr, "Error: Unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}
