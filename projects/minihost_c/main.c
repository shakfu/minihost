// main.c - CLI frontend for minihost (pure C implementation)
// Provides command-line access to plugin hosting features

#include "minihost.h"
#include "minihost_audio.h"
#include "minihost_chain.h"
#include "minihost_graph.h"
#include "minihost_audiofile.h"
#include "minihost_midi.h"
#include "minihost_vstpreset.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef _MSC_VER
#define strcasecmp _stricmp
/* MSVC spells the re-entrant strtok differently. Same signature, same
 * semantics; without this the C compiler assumes an int-returning
 * strtok_r and the link fails on an unresolved symbol. */
#define strtok_r strtok_s
#endif

static volatile sig_atomic_t g_running = 1;

static void sigint_handler(int sig) {
    (void)sig;
    g_running = 0;
}

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
    printf("  scan [DIRECTORY]        Scan for plugins (default: this platform's locations)\n");
    printf("  info PLUGIN             Show detailed plugin information\n");
    printf("  params PLUGIN           List plugin parameters\n");
    printf("  get-param PLUGIN N      Get parameter N value\n");
    printf("  set-param PLUGIN N V    Set parameter N to value V (0.0-1.0)\n");
    printf("  presets PLUGIN          List factory presets, or save state as .vstpreset\n");
    printf("  devices                 List audio playback/capture devices\n");
    printf("  midi                    List MIDI ports or monitor input\n");
    printf("  play PLUGIN             Play plugin with real-time audio\n");
    printf("  load-preset PLUGIN N    Load factory preset N\n");
    printf("  save-state PLUGIN F     Save plugin state to file F\n");
    printf("  load-state PLUGIN F     Load plugin state from file F\n");
    printf("  process PLUGIN          Process audio through plugin\n");
    printf("  chain P1 P2 [...]       Process through plugins in series\n");
    printf("  bus B1 B2 [...]         Split input across parallel branches, summed\n");
    printf("  morph PLUGIN            Interpolate between two parameter snapshots\n");
    printf("  resample INPUT OUTPUT   Resample audio file\n\n");
    printf("Options for specific commands:\n");
    printf("  -j, --json              Output as JSON (probe, scan, params, info)\n");
    printf("  -s, --state FILE        State file (set-param, load-preset, process)\n");
    printf("  -d, --double            Use double precision (process)\n");
    printf("  -p, --params            Show params after loading (load-state)\n");
    printf("  -V, --verbose           Show extended param info (params)\n");
    printf("  --probe                 Lightweight metadata-only mode (info)\n");
    printf("  --format au|vst3        Pick a format when a name matches both\n");
    printf("  --fuzzy                 Let a plugin name match part of a name\n");
    printf("  --in-process            Scan without a child process per plugin (scan)\n\n");
    printf("Process command options:\n");
    printf("  -i, --input FILE        Input audio file (WAV, FLAC, MP3)\n");
    printf("  -o, --output FILE       Output audio file (WAV, FLAC)\n");
    printf("  -m, --midi FILE         MIDI file to play through the plugin\n");
    printf("  --sidechain FILE        Sidechain input audio file\n");
    printf("  --preset N              Load factory preset N\n");
    printf("  --param NAME:VALUE      Set parameter (repeatable)\n");
    printf("  --non-realtime          Enable non-realtime mode\n");
    printf("  --bpm BPM              Set transport BPM\n");
    printf("  --bit-depth N           Output bit depth (16, 24, 32)\n");
    printf("  --tail SECONDS          Extra tail time for reverb/delay (process)\n\n");
    printf("Chain command options:\n");
    printf("  (accepts the process options above, plus)\n");
    printf("  --mix INDEX:VALUE       Dry/wet mix for one plugin, 0.0-1.0 (repeatable)\n");
    printf("Bus command options:\n");
    printf("  (accepts the process options above, plus)\n");
    printf("  --gain INDEX:VALUE      Gain for one branch (repeatable)\n");
    printf("  Each argument is one branch; commas chain plugins within it:\n");
    printf("    bus \"chorder.component,synth.vst3\" synth.vst3 -m song.mid -o out.wav\n");
    printf("  MIDI effects must precede the instrument they drive\n\n");
    printf("Morph command options:\n");
    printf("  --a-program N           Snapshot A from factory program N\n");
    printf("  --b-program N           Snapshot B from factory program N\n");
    printf("  --a-state FILE          Snapshot A from a saved state file\n");
    printf("  --b-state FILE          Snapshot B from a saved state file\n");
    printf("  -t, --blend T           Blend amount 0..1 (default 0.5)\n");
    printf("  --apply                 Apply the morphed snapshot to the plugin\n");
    printf("  --save FILE             Apply and save morphed state to FILE\n\n");
    printf("MIDI command options:\n");
    printf("  --monitor               Monitor MIDI input (Ctrl-C to stop)\n");
    printf("  --port N                MIDI port index\n");
    printf("  --virtual NAME          Create virtual MIDI port\n\n");
    printf("Play command options:\n");
    printf("  --port N                MIDI input port index\n");
    printf("  --virtual NAME          Create virtual MIDI input port\n");
    printf("  --capture               Enable audio capture (duplex mode)\n");
    printf("  --playback-device N     Playback device index\n");
    printf("  --capture-device N      Capture device index\n\n");
    printf("Resample command options:\n");
    printf("  --rate N                Target sample rate in Hz\n\n");
    printf("Presets command options:\n");
    printf("  --save FILE             Write current state as .vstpreset to FILE\n");
    printf("  --program N             Select factory program N before saving\n");
    printf("  -s, --state FILE        Load raw state blob before saving\n");
    printf("  --load-vstpreset FILE   Load .vstpreset before saving (preserves class_id)\n");
    printf("  -y, --overwrite         Overwrite --save output if it exists\n");
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

static void print_midi_msg(const unsigned char* data, size_t len) {
    static const char* note_names[] = {
        "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
    };

    if (len == 0) return;

    unsigned char status = data[0];
    unsigned char hi = status & 0xF0;
    unsigned char ch = (status & 0x0F) + 1;

    switch (hi) {
    case 0x90:
        if (len >= 3) {
            int note = data[1];
            printf("NoteOn  ch=%u  %s%d  vel=%u\n",
                   ch, note_names[note % 12], note / 12 - 1, data[2]);
        }
        break;
    case 0x80:
        if (len >= 3) {
            int note = data[1];
            printf("NoteOff ch=%u  %s%d  vel=%u\n",
                   ch, note_names[note % 12], note / 12 - 1, data[2]);
        }
        break;
    case 0xB0:
        if (len >= 3)
            printf("CC      ch=%u  cc=%u  val=%u\n", ch, data[1], data[2]);
        break;
    case 0xE0:
        if (len >= 3) {
            int bend = (int)data[1] | ((int)data[2] << 7);
            printf("Bend    ch=%u  val=%d\n", ch, bend - 8192);
        }
        break;
    case 0xC0:
        if (len >= 2)
            printf("PgmChg  ch=%u  pgm=%u\n", ch, data[1]);
        break;
    case 0xD0:
        if (len >= 2)
            printf("ChPress ch=%u  val=%u\n", ch, data[1]);
        break;
    default:
        printf("MIDI   ");
        for (size_t i = 0; i < len; i++)
            printf(" %02X", data[i]);
        printf("\n");
        break;
    }
}

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

// Save the plugin's full state blob to a file. Returns 1 on success, 0 on failure.
static int save_state_file(MH_Plugin* p, const char* path) {
    int size = mh_get_state_size(p);
    if (size <= 0) return 0;
    void* data = malloc((size_t)size);
    if (!data) return 0;
    if (!mh_get_state(p, data, size)) { free(data); return 0; }
    FILE* f = fopen(path, "wb");
    if (!f) { free(data); return 0; }
    size_t wrote = fwrite(data, 1, (size_t)size, f);
    fclose(f);
    free(data);
    return wrote == (size_t)size;
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

/* Resolve a plugin argument that may be a path or a cached name.
 *
 * A path wins whenever it exists, so nothing that worked before changes
 * meaning. Otherwise the argument is looked up in the shared scan cache
 * (the same file the Python CLI writes), case-insensitively. Returns a
 * pointer to `buf` on success and NULL after reporting the failure.
 */
static const char* g_plugin_format = NULL;   /* --format au|vst3 */
static int g_plugin_fuzzy = 0;               /* --fuzzy: allow substring names */
static int g_scan_in_process = 0;            /* --in-process: scan without a child */

static const char* resolve_plugin_arg(const char* arg, char* buf, size_t buf_size) {
    if (arg == NULL || arg[0] == '\0') return NULL;

    FILE* probe = fopen(arg, "rb");
    if (probe) { fclose(probe); return arg; }          /* plain file */
    if (strchr(arg, '/') != NULL) return arg;          /* looks like a path: let it fail loudly */
#ifdef _WIN32
    if (strchr(arg, '\\') != NULL) return arg;
#endif
    /* Bundles are directories: opendir-free existence check via mh_probe is
     * expensive, so rely on the path-shaped test above plus the cache. */
    int matches = mh_plugin_cache_lookup(arg, g_plugin_format, g_plugin_fuzzy,
                                         buf, buf_size);
    if (matches == 1) return buf;

    if (matches == 0) {
        char cache_path[1024] = {0};
        mh_plugin_cache_path(cache_path, sizeof(cache_path));
        fprintf(stderr,
                "Error: no plugin named '%s' in the scan cache (%s)\n"
                "       run 'scan' first, or pass a path%s\n",
                arg, cache_path,
                g_plugin_fuzzy ? "" : ", or --fuzzy to match part of a name");
        return NULL;
    }

    fprintf(stderr, "Error: '%s' matches %d plugins:\n", arg, matches);
    for (int i = 0; i < matches; i++) {
        char one[1024] = {0};
        if (mh_plugin_cache_match(arg, g_plugin_format, g_plugin_fuzzy, i,
                                  one, sizeof(one)))
            fprintf(stderr, "       %s\n", one);
    }
    fprintf(stderr, "       name it more precisely, pass a path, "
                    "or pick a format with --format\n");
    return NULL;
}

/* Scan for plugins and refresh the shared cache.
 *
 * `directory` may be NULL, in which case the canonical plugin locations
 * for this platform are scanned -- the common case, and what makes name
 * resolution usable without the user knowing where plugins live.
 */
static int cmd_scan(const char* directory, int json_output, int in_process) {
    ScanContext ctx = {json_output, 0, 1};

    if (!json_output) {
        if (directory) {
            fprintf(stderr, "Scanning %s\n", directory);
        } else {
            char dir[1024] = {0};
            fprintf(stderr, "Scanning the default plugin locations:\n");
            for (int i = 0; mh_get_default_plugin_dir(i, dir, sizeof(dir)); i++)
                fprintf(stderr, "  %s\n", dir);
        }
    }

    if (json_output) {
        printf("[\n");
    }

    char scan_err[1024] = {0};
    const char* dirs[1] = { directory };
    const char* const* dir_arg = directory ? dirs : NULL;
    const int num_dirs = directory ? 1 : 0;
    int result;
    if (in_process) {
        result = mh_plugin_cache_scan(dir_arg, num_dirs, 0, scan_callback, &ctx,
                                      scan_err, sizeof(scan_err));
    } else {
        /* Each plugin is probed in a child process, so one that hangs or
           crashes on load costs that entry and not the scan. */
        result = mh_plugin_cache_scan_supervised(dir_arg, num_dirs, 0,
                                                 NULL, 0, 0,
                                                 scan_callback, &ctx,
                                                 scan_err, sizeof(scan_err));
    }
    if (result < 0 && scan_err[0] != '\0')
        fprintf(stderr, "Error: %s\n", scan_err);

    if (json_output) {
        if (ctx.count > 0) printf("\n");
        printf("]\n");
    }

    if (result < 0) {
        fprintf(stderr, "Error: Failed to scan\n");
        return 1;
    }

    if (!json_output) {
        char cache_path[1024] = {0};
        mh_plugin_cache_path(cache_path, sizeof(cache_path));
        printf("\nFound %d newly probed plugin(s); %d in the cache\n",
               ctx.count, result);
        printf("Cache: %s\n", cache_path);
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
// Command: devices
// ============================================================================

static int cmd_devices(int json_output) {
    int playback_count = mh_audio_enumerate_playback_devices(NULL, 0);
    int capture_count = mh_audio_enumerate_capture_devices(NULL, 0);
    if (playback_count < 0) playback_count = 0;
    if (capture_count < 0) capture_count = 0;

    MH_AudioDeviceInfo* playback = NULL;
    MH_AudioDeviceInfo* capture = NULL;
    if (playback_count > 0) {
        playback = (MH_AudioDeviceInfo*)calloc(
            (size_t)playback_count, sizeof(MH_AudioDeviceInfo));
        if (!playback) {
            fprintf(stderr, "Error: Out of memory\n");
            return 1;
        }
        mh_audio_enumerate_playback_devices(playback, playback_count);
    }
    if (capture_count > 0) {
        capture = (MH_AudioDeviceInfo*)calloc(
            (size_t)capture_count, sizeof(MH_AudioDeviceInfo));
        if (!capture) {
            free(playback);
            fprintf(stderr, "Error: Out of memory\n");
            return 1;
        }
        mh_audio_enumerate_capture_devices(capture, capture_count);
    }

    if (json_output) {
        printf("{\n");
        printf("  \"playback\": [");
        for (int i = 0; i < playback_count; i++) {
            printf("%s\n    {\"index\": %d, \"name\": \"%s\", \"is_default\": %s}",
                   i == 0 ? "" : ",", i, playback[i].name,
                   playback[i].is_default ? "true" : "false");
        }
        printf("%s],\n", playback_count > 0 ? "\n  " : "");
        printf("  \"capture\": [");
        for (int i = 0; i < capture_count; i++) {
            printf("%s\n    {\"index\": %d, \"name\": \"%s\", \"is_default\": %s}",
                   i == 0 ? "" : ",", i, capture[i].name,
                   capture[i].is_default ? "true" : "false");
        }
        printf("%s]\n", capture_count > 0 ? "\n  " : "");
        printf("}\n");
    } else {
        printf("Audio Playback (Output) Devices:\n");
        if (playback_count == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < playback_count; i++) {
                printf("  [%d] %s%s\n", i, playback[i].name,
                       playback[i].is_default ? " (default)" : "");
            }
        }
        printf("\nAudio Capture (Input) Devices:\n");
        if (capture_count == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < capture_count; i++) {
                printf("  [%d] %s%s\n", i, capture[i].name,
                       capture[i].is_default ? " (default)" : "");
            }
        }
    }

    free(playback);
    free(capture);
    return 0;
}

// ============================================================================
// Command: presets
// ============================================================================

// List-mode presets (shared by the original behaviour and the default listing).
static int cmd_presets_list(MH_Plugin* p, int json_output) {
    int num_programs = mh_get_num_programs(p);
    int current = mh_get_program(p);

    if (json_output) {
        printf("{\n  \"count\": %d,\n  \"presets\": [", num_programs);
        for (int i = 0; i < num_programs; i++) {
            char name[256] = {0};
            mh_get_program_name(p, i, name, sizeof(name));
            printf("%s\n    {\"index\": %d, \"name\": \"%s\", \"is_current\": %s}",
                   i == 0 ? "" : ",", i, name,
                   i == current ? "true" : "false");
        }
        printf("%s]\n}\n", num_programs > 0 ? "\n  " : "");
        return 0;
    }

    if (num_programs == 0) {
        printf("(no factory presets)\n");
        return 0;
    }
    printf("Factory Presets (%d):\n", num_programs);
    for (int i = 0; i < num_programs; i++) {
        char name[256] = {0};
        mh_get_program_name(p, i, name, sizeof(name));
        printf("  [%3d] %s%s\n", i, name, (i == current) ? " *" : "");
    }
    return 0;
}

// Load a raw state blob from file into the plugin.
static int load_state_from_file(MH_Plugin* p, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open state file '%s'\n", path);
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long size = ftell(f);
    if (size < 0) { fclose(f); return 0; }
    rewind(f);
    void* data = malloc((size_t)size);
    if (!data) { fclose(f); fprintf(stderr, "Error: Out of memory\n"); return 0; }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data); fclose(f);
        fprintf(stderr, "Error: Failed to read state file\n");
        return 0;
    }
    fclose(f);
    int ok = mh_set_state(p, data, (int)size);
    free(data);
    if (!ok) {
        fprintf(stderr, "Error: Failed to apply state\n");
        return 0;
    }
    return 1;
}

// Returns 1 if a file exists at path.
static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

static int cmd_presets(const char* plugin_path, double sample_rate, int block_size,
                       int json_output, const char* save_file, int program_index,
                       const char* state_file_input, const char* load_vstpreset_file,
                       int overwrite) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    // Class ID we'll write if --save is used. Prefer the source vstpreset's
    // class_id (when one is loaded) over the probed unique_id fallback.
    char class_id_buf[MH_VSTPRESET_CLASS_ID_LEN + 1] = {0};
    int have_class_id = 0;

    // Apply input state, if any.
    if (state_file_input && state_file_input[0]) {
        if (!load_state_from_file(p, state_file_input)) {
            mh_close(p);
            return 1;
        }
    }

    if (load_vstpreset_file && load_vstpreset_file[0]) {
        MH_VstPreset preset;
        char perr[512] = {0};
        if (!mh_vstpreset_read(load_vstpreset_file, &preset, perr, sizeof(perr))) {
            fprintf(stderr, "Error loading .vstpreset '%s': %s\n",
                    load_vstpreset_file, perr);
            mh_close(p);
            return 1;
        }
        if (!preset.component_state || preset.component_size == 0) {
            fprintf(stderr, "Error: preset '%s' has no component state\n",
                    load_vstpreset_file);
            mh_vstpreset_free(&preset);
            mh_close(p);
            return 1;
        }
        if (!mh_set_state(p, preset.component_state, preset.component_size)) {
            fprintf(stderr, "Error: Failed to apply preset state\n");
            mh_vstpreset_free(&preset);
            mh_close(p);
            return 1;
        }
        // Preserve the source preset's class_id for save.
        snprintf(class_id_buf, sizeof(class_id_buf), "%s", preset.class_id);
        have_class_id = 1;
        mh_vstpreset_free(&preset);
    }

    if (program_index >= 0) {
        int num_programs = mh_get_num_programs(p);
        if (num_programs == 0) {
            fprintf(stderr, "Error: plugin has no factory presets\n");
            mh_close(p);
            return 1;
        }
        if (program_index >= num_programs) {
            fprintf(stderr, "Error: program %d out of range (0-%d)\n",
                    program_index, num_programs - 1);
            mh_close(p);
            return 1;
        }
        if (!mh_set_program(p, program_index)) {
            fprintf(stderr, "Error: Failed to select program %d\n", program_index);
            mh_close(p);
            return 1;
        }
    }

    // Save mode
    if (save_file && save_file[0]) {
        if (!overwrite && file_exists(save_file)) {
            fprintf(stderr,
                    "Error: Output file '%s' already exists. Use -y/--overwrite to overwrite.\n",
                    save_file);
            mh_close(p);
            return 1;
        }

        if (!have_class_id) {
            // Auto-detect from the plugin bundle's moduleinfo.json. There is
            // no silent fallback -- if this fails we error out rather than
            // write a .vstpreset with a bogus class_id.
            char cid_err[512] = {0};
            if (!mh_vstpreset_read_class_id_from_bundle(
                    plugin_path, class_id_buf, cid_err, sizeof(cid_err))) {
                fprintf(stderr,
                        "Error: cannot determine VST3 class_id for '%s': %s\n"
                        "Use --load-vstpreset to inherit a class_id from an "
                        "existing .vstpreset file.\n",
                        plugin_path, cid_err);
                mh_close(p);
                return 1;
            }
        }

        int state_size = mh_get_state_size(p);
        if (state_size <= 0) {
            fprintf(stderr, "Error: Plugin has no state to save\n");
            mh_close(p);
            return 1;
        }
        void* state = malloc((size_t)state_size);
        if (!state) {
            fprintf(stderr, "Error: Out of memory\n");
            mh_close(p);
            return 1;
        }
        if (!mh_get_state(p, state, state_size)) {
            fprintf(stderr, "Error: Failed to read plugin state\n");
            free(state);
            mh_close(p);
            return 1;
        }

        char werr[512] = {0};
        int ok = mh_vstpreset_write(save_file, class_id_buf,
                                    state, state_size,
                                    NULL, 0,
                                    werr, sizeof(werr));
        free(state);
        if (!ok) {
            fprintf(stderr, "Error writing '%s': %s\n", save_file, werr);
            mh_close(p);
            return 1;
        }
        printf("Wrote %s\n", save_file);
        mh_close(p);
        return 0;
    }

    // Listing mode
    int ret = cmd_presets_list(p, json_output);
    mh_close(p);
    return ret;
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
#define MAX_BLOCK_MIDI_EVENTS 256

static int cmd_process(const char* plugin_path,
                       const char* input_file,
                       const char* output_file,
                       const char* sidechain_file,
                       const char* midi_file,
                       double sample_rate,
                       int block_size,
                       const char* state_file,
                       int preset_index,
                       const char** param_specs,
                       int num_param_specs,
                       int use_double,
                       int non_realtime,
                       double bpm,
                       int bit_depth,
                       double tail_seconds) {
    char err[1024] = {0};

    int has_audio_input = (input_file && input_file[0] != '\0');
    int has_sidechain = (sidechain_file && sidechain_file[0] != '\0');
    int has_midi = (midi_file && midi_file[0] != '\0');

    if (!has_audio_input && !has_midi) {
        fprintf(stderr, "Error: an input file (-i) or a MIDI file (-m) is required\n");
        return 1;
    }

    /* --- Read MIDI input ---
     * Events arrive with absolute sample offsets; the process loop
     * rebases them per block. An instrument needs no audio input, so a
     * MIDI-only render sizes its (silent) input from the MIDI duration.
     */
    MH_MidiEvent* midi_events = NULL;
    int num_midi_events = 0;
    double midi_duration = 0.0;
    if (has_midi) {
        char midi_err[1024] = {0};
        if (!mh_midi_file_load(midi_file, sample_rate, &midi_events,
                               &num_midi_events, &midi_duration,
                               midi_err, sizeof(midi_err))) {
            fprintf(stderr, "Error: %s: %s\n", midi_file, midi_err);
            return 1;
        }
        if (num_midi_events == 0)
            fprintf(stderr, "Warning: %s holds no playable MIDI events\n", midi_file);
    }

    // --- Read audio input ---
    MH_AudioData* audio_data = NULL;
    float* raw_data = NULL;
    int in_ch = 2;
    int in_frames = 0;
    int input_is_audio = has_audio_input && is_audio_file(input_file);

    if (!has_audio_input) {
        /* MIDI-only render: the instrument is fed silence for as long as
         * the MIDI lasts. The tail is added by the caller's --tail. */
        in_ch = 2;
        in_frames = (int)(midi_duration * sample_rate);
        if (in_frames <= 0) in_frames = (int)sample_rate;   /* a second of nothing */
    } else if (input_is_audio) {
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
            mh_midi_file_free(midi_events);
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
        mh_midi_file_free(midi_events);
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
            mh_midi_file_free(midi_events);
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
            mh_midi_file_free(midi_events);
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
    int tail_frames = 0;
    if (tail_seconds > 0)
        tail_frames = (int)(tail_seconds * sample_rate);
    int total_samples = in_frames + tail_frames;
    int output_total = total_samples + latency;

    // --- Print summary ---
    fprintf(stderr, "Plugin: %s\n", plugin_path);
    fprintf(stderr, "  Sample rate: %.0f Hz\n", sample_rate);
    fprintf(stderr, "  Block size:  %d\n", block_size);
    fprintf(stderr, "  Latency:     %d samples\n", latency);
    fprintf(stderr, "  Input:       %d ch, %d samples\n", in_ch, in_frames);
    if (tail_frames > 0) {
        fprintf(stderr, "  Tail:        %d samples (%.2fs)\n", tail_frames, tail_seconds);
    }
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
        mh_midi_file_free(midi_events);
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

    // MIDI is consumed by the loop below via midi_cursor; the array is
    // freed once the loop is done (see after the process loop).
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
    int midi_cursor = 0;      /* next unconsumed event in midi_events */

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

        /* Slice this block's MIDI out of the absolute-offset array and
         * rebase each event to the block. The array is time-ordered, so
         * a cursor walks it once across the whole render. */
        MH_MidiEvent block_midi[MAX_BLOCK_MIDI_EVENTS];
        int num_block_midi = 0;
        while (midi_cursor < num_midi_events
               && midi_events[midi_cursor].sample_offset < end) {
            if (num_block_midi < MAX_BLOCK_MIDI_EVENTS) {
                block_midi[num_block_midi] = midi_events[midi_cursor];
                block_midi[num_block_midi].sample_offset -= start;
                if (block_midi[num_block_midi].sample_offset < 0)
                    block_midi[num_block_midi].sample_offset = 0;
                num_block_midi++;
            }
            midi_cursor++;
        }

        if (has_sidechain && sc_channels) {
            const float* sc_ptrs[32];
            for (int c = 0; c < sc_ch && c < 32; c++)
                sc_ptrs[c] = sc_channels[c] + start;
            mh_process_sidechain(p, in_ptrs, out_ptrs, sc_ptrs, bsize);
        } else if (has_midi || has_param_automation) {
            mh_process_auto(p,
                            in_ptrs, out_ptrs, bsize,
                            num_block_midi > 0 ? block_midi : NULL,
                            num_block_midi,
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

    mh_midi_file_free(midi_events);
    midi_events = NULL;

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
// Command: midi
// ============================================================================

static void midi_monitor_callback(const unsigned char* data, size_t len, void* user_data) {
    (void)user_data;
    print_midi_msg(data, len);
    fflush(stdout);
}

static int cmd_midi(int json_output, int monitor, int midi_port,
                    const char* virtual_midi_name) {
    if (monitor) {
        char err[1024] = {0};
        MH_MidiIn* midi_in = NULL;

        if (virtual_midi_name && virtual_midi_name[0] != '\0') {
            midi_in = mh_midi_in_open_virtual(virtual_midi_name,
                                               midi_monitor_callback, NULL,
                                               err, sizeof(err));
            if (!midi_in) {
                fprintf(stderr, "Error: %s\n", err);
                return 1;
            }
            fprintf(stderr, "Monitoring virtual MIDI port '%s' (Ctrl-C to stop)\n",
                    virtual_midi_name);
        } else {
            if (midi_port < 0) midi_port = 0;
            midi_in = mh_midi_in_open(midi_port,
                                       midi_monitor_callback, NULL,
                                       err, sizeof(err));
            if (!midi_in) {
                fprintf(stderr, "Error: %s\n", err);
                return 1;
            }
            char name[256] = {0};
            mh_midi_get_input_name(midi_port, name, sizeof(name));
            fprintf(stderr, "Monitoring MIDI port [%d] %s (Ctrl-C to stop)\n",
                    midi_port, name);
        }

        signal(SIGINT, sigint_handler);
        g_running = 1;
        while (g_running) {
#ifdef _WIN32
            Sleep(100);
#else
            usleep(100000);
#endif
        }
        fprintf(stderr, "\nStopped.\n");
        mh_midi_in_close(midi_in);
        return 0;
    }

    // List MIDI ports
    int num_inputs = mh_midi_get_num_inputs();
    int num_outputs = mh_midi_get_num_outputs();
    if (num_inputs < 0) num_inputs = 0;
    if (num_outputs < 0) num_outputs = 0;

    if (json_output) {
        printf("{\n");
        printf("  \"inputs\": [");
        for (int i = 0; i < num_inputs; i++) {
            char name[256] = {0};
            mh_midi_get_input_name(i, name, sizeof(name));
            printf("%s\n    {\"index\": %d, \"name\": \"%s\"}",
                   i == 0 ? "" : ",", i, name);
        }
        printf("%s],\n", num_inputs > 0 ? "\n  " : "");
        printf("  \"outputs\": [");
        for (int i = 0; i < num_outputs; i++) {
            char name[256] = {0};
            mh_midi_get_output_name(i, name, sizeof(name));
            printf("%s\n    {\"index\": %d, \"name\": \"%s\"}",
                   i == 0 ? "" : ",", i, name);
        }
        printf("%s]\n", num_outputs > 0 ? "\n  " : "");
        printf("}\n");
    } else {
        printf("MIDI Input Ports:\n");
        if (num_inputs == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < num_inputs; i++) {
                char name[256] = {0};
                mh_midi_get_input_name(i, name, sizeof(name));
                printf("  [%d] %s\n", i, name);
            }
        }
        printf("\nMIDI Output Ports:\n");
        if (num_outputs == 0) {
            printf("  (none)\n");
        } else {
            for (int i = 0; i < num_outputs; i++) {
                char name[256] = {0};
                mh_midi_get_output_name(i, name, sizeof(name));
                printf("  [%d] %s\n", i, name);
            }
        }
    }

    return 0;
}

// ============================================================================
// Command: play
// ============================================================================

static int cmd_play(const char* plugin_path,
                    double sample_rate,
                    int block_size,
                    const char* state_file,
                    int preset_index,
                    const char** param_specs,
                    int num_param_specs,
                    int midi_port,
                    const char* virtual_midi_name,
                    int capture,
                    int playback_device,
                    int capture_device) {
    char err[1024] = {0};

    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    // Load state
    if (state_file && state_file[0] != '\0') {
        if (load_state_file(p, state_file)) {
            fprintf(stderr, "Loaded state from %s\n", state_file);
        } else {
            fprintf(stderr, "Warning: Failed to load state from %s\n", state_file);
        }
    }

    // Load preset
    if (preset_index >= 0) {
        int num_programs = mh_get_num_programs(p);
        if (preset_index >= num_programs) {
            fprintf(stderr, "Error: Preset index %d out of range (0-%d)\n",
                    preset_index, num_programs - 1);
            mh_close(p);
            return 1;
        }
        mh_set_program(p, preset_index);
        char name[256] = {0};
        mh_get_program_name(p, preset_index, name, sizeof(name));
        fprintf(stderr, "Loaded preset [%d]: %s\n", preset_index, name);
    }

    // Apply param overrides
    for (int i = 0; i < num_param_specs && i < MAX_PARAM_SPECS; i++) {
        int idx;
        float val;
        if (!parse_param_spec(p, param_specs[i], &idx, &val)) {
            fprintf(stderr, "Error: Invalid parameter spec '%s'\n", param_specs[i]);
            mh_close(p);
            return 1;
        }
        mh_set_param(p, idx, val);
    }

    // Build audio config
    MH_AudioConfig config;
    memset(&config, 0, sizeof(config));
    config.sample_rate = sample_rate;
    config.buffer_frames = block_size;
    config.midi_input_port = midi_port;
    config.midi_output_port = -1;
    config.capture = capture;
    config.playback_device_index = playback_device;
    config.capture_device_index = capture_device;

    MH_AudioDevice* dev = mh_audio_open(p, &config, err, sizeof(err));
    if (!dev) {
        fprintf(stderr, "Error: %s\n", err);
        mh_close(p);
        return 1;
    }

    // Create virtual MIDI port if requested
    if (virtual_midi_name && virtual_midi_name[0] != '\0') {
        if (!mh_audio_create_virtual_midi_input(dev, virtual_midi_name)) {
            fprintf(stderr, "Warning: Failed to create virtual MIDI port '%s'\n",
                    virtual_midi_name);
        } else {
            fprintf(stderr, "Virtual MIDI input: %s\n", virtual_midi_name);
        }
    }

    if (!mh_audio_start(dev)) {
        fprintf(stderr, "Error: Failed to start audio\n");
        mh_audio_close(dev);
        mh_close(p);
        return 1;
    }

    fprintf(stderr, "Playing (Ctrl-C to stop)\n");
    fprintf(stderr, "  Sample rate: %.0f Hz\n", mh_audio_get_sample_rate(dev));
    fprintf(stderr, "  Buffer:      %d frames\n", mh_audio_get_buffer_frames(dev));
    fprintf(stderr, "  Channels:    %d\n", mh_audio_get_channels(dev));

    signal(SIGINT, sigint_handler);
    g_running = 1;
    while (g_running) {
#ifdef _WIN32
        Sleep(100);
#else
        usleep(100000);
#endif
    }

    fprintf(stderr, "\nStopping...\n");
    mh_audio_stop(dev);
    mh_audio_close(dev);
    mh_close(p);
    return 0;
}

// ============================================================================
// Command: resample
// ============================================================================

static int cmd_resample(const char* input_file, const char* output_file,
                        int target_rate, int bit_depth) {
    char err[1024] = {0};

    MH_AudioData* audio = mh_audio_read(input_file, err, sizeof(err));
    if (!audio) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    if (target_rate <= 0) {
        fprintf(stderr, "Error: Target sample rate required (--rate N)\n");
        mh_audio_data_free(audio);
        return 1;
    }

    if ((unsigned)target_rate == audio->sample_rate) {
        fprintf(stderr, "Error: Input already at %d Hz\n", target_rate);
        mh_audio_data_free(audio);
        return 1;
    }

    MH_AudioData* resampled = mh_audio_resample(audio->data,
                                                 audio->channels,
                                                 audio->frames,
                                                 audio->sample_rate,
                                                 (unsigned)target_rate,
                                                 err, sizeof(err));
    if (!resampled) {
        fprintf(stderr, "Error: %s\n", err);
        mh_audio_data_free(audio);
        return 1;
    }

    if (bit_depth <= 0) bit_depth = 24;
    if (!mh_audio_write(output_file, resampled->data,
                        resampled->channels, resampled->frames,
                        resampled->sample_rate, bit_depth,
                        err, sizeof(err))) {
        fprintf(stderr, "Error: %s\n", err);
        mh_audio_data_free(resampled);
        mh_audio_data_free(audio);
        return 1;
    }

    fprintf(stderr, "Resampled %u Hz -> %u Hz\n", audio->sample_rate, resampled->sample_rate);
    fprintf(stderr, "  Input:  %u frames, %u ch\n", audio->frames, audio->channels);
    fprintf(stderr, "  Output: %u frames -> %s\n", resampled->frames, output_file);

    mh_audio_data_free(resampled);
    mh_audio_data_free(audio);
    return 0;
}

// ============================================================================
// Command: chain
// ============================================================================

#define MAX_CHAIN_PLUGINS 32

/* Process audio and/or MIDI through several plugins in series.
 *
 * MIDI enters the first plugin that accepts it and is carried onward by
 * any plugin that produces MIDI, so a MIDI effect ahead of an
 * instrument drives it (see mh_chain_process_midi_io). That ordering
 * rule is why the plugin list is taken in signal order.
 */
static int cmd_chain(const char** plugin_paths, int num_plugins,
                     const char* input_file, const char* output_file,
                     const char* midi_file, double sample_rate,
                     int block_size, int non_realtime, double bpm,
                     int bit_depth, double tail_seconds,
                     const char** mix_specs, int num_mix_specs) {
    char err[1024] = {0};

    if (num_plugins < 1) {
        fprintf(stderr, "Error: chain needs at least one plugin\n");
        return 1;
    }
    if (num_plugins > MAX_CHAIN_PLUGINS) {
        fprintf(stderr, "Error: chain is limited to %d plugins\n", MAX_CHAIN_PLUGINS);
        return 1;
    }

    int has_audio_input = (input_file && input_file[0] != '\0');
    int has_midi = (midi_file && midi_file[0] != '\0');
    if (!has_audio_input && !has_midi) {
        fprintf(stderr, "Error: an input file (-i) or a MIDI file (-m) is required\n");
        return 1;
    }
    if (!output_file || output_file[0] == '\0') {
        fprintf(stderr, "Error: an output file (-o) is required\n");
        return 1;
    }

    /* --- Input audio (optional: an instrument chain needs none) --- */
    MH_AudioData* audio_data = NULL;
    int file_ch = 2;
    int in_frames = 0;
    if (has_audio_input) {
        audio_data = mh_audio_read(input_file, err, sizeof(err));
        if (!audio_data) {
            fprintf(stderr, "Error: %s\n", err);
            return 1;
        }
        file_ch = (int)audio_data->channels;
        in_frames = (int)audio_data->frames;
        sample_rate = (double)audio_data->sample_rate;
    }

    /* --- Input MIDI (optional) --- */
    MH_MidiEvent* midi_events = NULL;
    int num_midi_events = 0;
    double midi_duration = 0.0;
    if (has_midi) {
        char midi_err[1024] = {0};
        if (!mh_midi_file_load(midi_file, sample_rate, &midi_events,
                               &num_midi_events, &midi_duration,
                               midi_err, sizeof(midi_err))) {
            fprintf(stderr, "Error: %s: %s\n", midi_file, midi_err);
            if (audio_data) mh_audio_data_free(audio_data);
            return 1;
        }
        if (!has_audio_input) {
            in_frames = (int)(midi_duration * sample_rate);
            if (in_frames <= 0) in_frames = (int)sample_rate;
        }
    }

    /* --- Open every plugin, then bind them into a chain ---
     *
     * One session across the whole chain: mh_open builds and registers a
     * JUCE plugin-format manager per call, which is wasted work once you
     * are loading more than one plugin. The session builds it once.
     */
    MH_Plugin* plugins[MAX_CHAIN_PLUGINS];
    int opened = 0;
    MH_Session* session = mh_session_create(err, sizeof(err));
    for (int i = 0; i < num_plugins; i++) {
        plugins[i] = session
            ? mh_session_open(session, plugin_paths[i], sample_rate, block_size,
                              2, 2, 0, err, sizeof(err))
            : mh_open(plugin_paths[i], sample_rate, block_size,
                      2, 2, err, sizeof(err));
        if (!plugins[i]) {
            fprintf(stderr, "Error: %s: %s\n", plugin_paths[i], err);
            for (int j = 0; j < opened; j++) mh_close(plugins[j]);
            if (session) mh_session_close(session);
            if (audio_data) mh_audio_data_free(audio_data);
            mh_midi_file_free(midi_events);
            return 1;
        }
        opened++;
        if (non_realtime) mh_set_non_realtime(plugins[i], 1);
        if (bpm > 0.0) {
            MH_TransportInfo tr;
            memset(&tr, 0, sizeof(tr));
            tr.bpm = bpm;
            tr.time_sig_numerator = 4;
            tr.time_sig_denominator = 4;
            tr.is_playing = 1;
            mh_set_transport(plugins[i], &tr);
        }
    }

    /* Plugins outlive the session that loaded them. */
    if (session) mh_session_close(session);
    session = NULL;

    MH_PluginChain* chain = mh_chain_create(plugins, num_plugins, err, sizeof(err));
    if (!chain) {
        fprintf(stderr, "Error: %s\n", err);
        for (int i = 0; i < opened; i++) mh_close(plugins[i]);
        if (audio_data) mh_audio_data_free(audio_data);
        mh_midi_file_free(midi_events);
        return 1;
    }

    /* --- Per-plugin dry/wet mix: --mix INDEX:VALUE --- */
    for (int i = 0; i < num_mix_specs; i++) {
        int idx = 0;
        float value = 1.0f;
        if (sscanf(mix_specs[i], "%d:%f", &idx, &value) != 2) {
            fprintf(stderr, "Error: --mix wants INDEX:VALUE, got '%s'\n", mix_specs[i]);
            mh_chain_close(chain);
            for (int j = 0; j < opened; j++) mh_close(plugins[j]);
            if (audio_data) mh_audio_data_free(audio_data);
            mh_midi_file_free(midi_events);
            return 1;
        }
        if (!mh_chain_set_mix(chain, idx, value)) {
            fprintf(stderr, "Error: could not set mix %s (index out of range, "
                            "or the plugin's input and output channel counts differ)\n",
                    mix_specs[i]);
            mh_chain_close(chain);
            for (int j = 0; j < opened; j++) mh_close(plugins[j]);
            if (audio_data) mh_audio_data_free(audio_data);
            mh_midi_file_free(midi_events);
            return 1;
        }
    }

    int in_ch = mh_chain_get_num_input_channels(chain);
    int out_ch = mh_chain_get_num_output_channels(chain);
    if (in_ch < 1) in_ch = 1;         /* instruments read nothing; keep one silent channel */
    if (out_ch < 1) out_ch = 2;
    int latency = mh_chain_get_latency_samples(chain);
    int tail_frames = tail_seconds > 0 ? (int)(tail_seconds * sample_rate) : 0;
    int total_samples = in_frames + tail_frames;
    int output_total = total_samples + latency;

    fprintf(stderr, "Chain of %d plugin(s) @ %.0f Hz\n", num_plugins, sample_rate);
    for (int i = 0; i < num_plugins; i++) {
        MH_Info info;
        memset(&info, 0, sizeof(info));
        mh_get_info(plugins[i], &info);
        const char* slash = strrchr(plugin_paths[i], '/');
        fprintf(stderr, "  [%d] %-28s %din/%dout  midi in:%s out:%s  latency %d\n",
                i, slash ? slash + 1 : plugin_paths[i],
                info.num_input_ch, info.num_output_ch,
                info.accepts_midi ? "yes" : "no",
                info.produces_midi ? "yes" : "no",
                mh_get_latency_samples(plugins[i]));
    }
    fprintf(stderr, "  Chain I/O:   %d in / %d out, latency %d samples\n",
            in_ch, out_ch, latency);
    if (has_midi)
        fprintf(stderr, "  MIDI events: %d (%.2fs)\n", num_midi_events, midi_duration);
    fprintf(stderr, "  Output:      %s\n", output_file);

    /* --- Buffers --- */
    float** in_channels = alloc_channels(in_ch, output_total);
    float** out_channels = alloc_channels(out_ch, output_total);
    if (!in_channels || !out_channels) {
        fprintf(stderr, "Error: Out of memory\n");
        free_channels(in_channels, in_ch);
        free_channels(out_channels, out_ch);
        mh_chain_close(chain);
        for (int i = 0; i < opened; i++) mh_close(plugins[i]);
        if (audio_data) mh_audio_data_free(audio_data);
        mh_midi_file_free(midi_events);
        return 1;
    }
    if (audio_data) {
        for (int f = 0; f < in_frames; f++)
            for (int c = 0; c < in_ch; c++)
                in_channels[c][f] = (c < file_ch)
                    ? audio_data->data[(size_t)f * (size_t)file_ch + (size_t)c]
                    : 0.0f;
    }

    /* --- Process --- */
    int midi_cursor = 0;
    for (int start = 0; start < output_total; start += block_size) {
        int end = start + block_size;
        if (end > output_total) end = output_total;
        int bsize = end - start;

        const float* in_ptrs[32];
        float* out_ptrs[32];
        for (int c = 0; c < in_ch && c < 32; c++) in_ptrs[c] = in_channels[c] + start;
        for (int c = 0; c < out_ch && c < 32; c++) out_ptrs[c] = out_channels[c] + start;

        MH_MidiEvent block_midi[MAX_BLOCK_MIDI_EVENTS];
        int num_block_midi = 0;
        while (midi_cursor < num_midi_events
               && midi_events[midi_cursor].sample_offset < end) {
            if (num_block_midi < MAX_BLOCK_MIDI_EVENTS) {
                block_midi[num_block_midi] = midi_events[midi_cursor];
                block_midi[num_block_midi].sample_offset -= start;
                if (block_midi[num_block_midi].sample_offset < 0)
                    block_midi[num_block_midi].sample_offset = 0;
                num_block_midi++;
            }
            midi_cursor++;
        }

        mh_chain_process_midi_io(chain, in_ptrs, out_ptrs, bsize,
                                 num_block_midi > 0 ? block_midi : NULL,
                                 num_block_midi,
                                 NULL, 0, NULL);
    }

    mh_midi_file_free(midi_events);
    midi_events = NULL;

    /* --- Latency compensation and write --- */
    int write_offset = latency;
    int write_frames = total_samples;
    if (write_offset + write_frames > output_total)
        write_frames = output_total - write_offset;
    if (write_frames < 0) write_frames = 0;

    int rc = 0;
    float* interleaved = (float*)malloc((size_t)out_ch * (size_t)write_frames * sizeof(float));
    if (!interleaved) {
        fprintf(stderr, "Error: Out of memory\n");
        rc = 1;
    } else {
        for (int f = 0; f < write_frames; f++)
            for (int c = 0; c < out_ch; c++)
                interleaved[(size_t)f * (size_t)out_ch + (size_t)c] =
                    out_channels[c][write_offset + f];
        if (bit_depth <= 0) bit_depth = 24;
        if (!mh_audio_write(output_file, interleaved, (unsigned)out_ch,
                            (unsigned)write_frames, (unsigned)sample_rate,
                            bit_depth, err, sizeof(err))) {
            fprintf(stderr, "Error: %s\n", err);
            rc = 1;
        } else {
            fprintf(stderr, "Wrote %d samples (%.2fs) to %s\n", write_frames,
                    (double)write_frames / sample_rate, output_file);
        }
        free(interleaved);
    }

    free_channels(in_channels, in_ch);
    free_channels(out_channels, out_ch);
    mh_chain_close(chain);
    for (int i = 0; i < opened; i++) mh_close(plugins[i]);
    if (audio_data) mh_audio_data_free(audio_data);
    return rc;
}

// ============================================================================
// Command: bus
// ============================================================================

#define MAX_BUS_BRANCHES 16

/* Split one input (audio and/or MIDI) across parallel branches and sum
 * their audio -- the layering shape: one MIDI part driving several
 * instruments at once.
 *
 * Each positional argument is one branch. Commas inside an argument
 * chain plugins in series within that branch, so
 *
 *     bus "chorder.component,synth.vst3" "synth.vst3"
 *
 * layers a chorded synth against a plain one. A bus of instruments
 * carries no audio input: plugins driven by MIDI alone expose no audio
 * input bus, which is why the bus is created with a zero-width input in
 * that case.
 */
static int cmd_bus(const char** branch_specs, int num_branches,
                   const char* input_file, const char* output_file,
                   const char* midi_file, double sample_rate,
                   int block_size, int non_realtime, double bpm,
                   int bit_depth, double tail_seconds,
                   const char** gain_specs, int num_gain_specs) {
    char err[1024] = {0};

    if (num_branches < 1) {
        fprintf(stderr, "Error: bus needs at least one branch\n");
        return 1;
    }
    if (num_branches > MAX_BUS_BRANCHES) {
        fprintf(stderr, "Error: bus is limited to %d branches\n", MAX_BUS_BRANCHES);
        return 1;
    }

    int has_audio_input = (input_file && input_file[0] != '\0');
    int has_midi = (midi_file && midi_file[0] != '\0');
    if (!has_audio_input && !has_midi) {
        fprintf(stderr, "Error: an input file (-i) or a MIDI file (-m) is required\n");
        return 1;
    }
    if (!output_file || output_file[0] == '\0') {
        fprintf(stderr, "Error: an output file (-o) is required\n");
        return 1;
    }

    /* --- Inputs --- */
    MH_AudioData* audio_data = NULL;
    int file_ch = 2;
    int in_frames = 0;
    if (has_audio_input) {
        audio_data = mh_audio_read(input_file, err, sizeof(err));
        if (!audio_data) {
            fprintf(stderr, "Error: %s\n", err);
            return 1;
        }
        file_ch = (int)audio_data->channels;
        in_frames = (int)audio_data->frames;
        sample_rate = (double)audio_data->sample_rate;
    }

    MH_MidiEvent* midi_events = NULL;
    int num_midi_events = 0;
    double midi_duration = 0.0;
    if (has_midi) {
        char midi_err[1024] = {0};
        if (!mh_midi_file_load(midi_file, sample_rate, &midi_events,
                               &num_midi_events, &midi_duration,
                               midi_err, sizeof(midi_err))) {
            fprintf(stderr, "Error: %s: %s\n", midi_file, midi_err);
            if (audio_data) mh_audio_data_free(audio_data);
            return 1;
        }
        if (!has_audio_input) {
            in_frames = (int)(midi_duration * sample_rate);
            if (in_frames <= 0) in_frames = (int)sample_rate;
        }
    }

    /* --- Open each branch: split on commas, open in series, chain --- */
    MH_Plugin* plugins[MAX_BUS_BRANCHES * MAX_CHAIN_PLUGINS];
    int num_plugins = 0;
    MH_PluginChain* chains[MAX_BUS_BRANCHES];
    int num_chains = 0;
    MH_PluginBus* bus = NULL;
    int rc = 1;

    /* One session for every plugin across every branch: mh_open would
     * build a JUCE format manager per call. */
    MH_Session* session = mh_session_create(err, sizeof(err));

    for (int b = 0; b < num_branches; b++) {
        MH_Plugin* branch_plugins[MAX_CHAIN_PLUGINS];
        int branch_count = 0;

        char spec[4096];
        snprintf(spec, sizeof(spec), "%s", branch_specs[b]);
        char* save = NULL;
        for (char* tok = strtok_r(spec, ",", &save);
             tok != NULL && branch_count < MAX_CHAIN_PLUGINS;
             tok = strtok_r(NULL, ",", &save)) {
            while (*tok == ' ') tok++;
            /* Each element may be a cached plugin name rather than a path. */
            char resolved[1024] = {0};
            const char* got = resolve_plugin_arg(tok, resolved, sizeof(resolved));
            if (!got) goto cleanup;
            tok = (char*) got;
            MH_Plugin* p = session
                ? mh_session_open(session, tok, sample_rate, block_size,
                                  2, 2, 0, err, sizeof(err))
                : mh_open(tok, sample_rate, block_size, 2, 2, err, sizeof(err));
            if (!p) {
                fprintf(stderr, "Error: %s: %s\n", tok, err);
                goto cleanup;
            }
            if (non_realtime) mh_set_non_realtime(p, 1);
            if (bpm > 0.0) {
                MH_TransportInfo tr;
                memset(&tr, 0, sizeof(tr));
                tr.bpm = bpm;
                tr.time_sig_numerator = 4;
                tr.time_sig_denominator = 4;
                tr.is_playing = 1;
                mh_set_transport(p, &tr);
            }
            plugins[num_plugins++] = p;
            branch_plugins[branch_count++] = p;
        }
        if (branch_count == 0) {
            fprintf(stderr, "Error: branch %d names no plugin\n", b);
            goto cleanup;
        }

        MH_PluginChain* chain = mh_chain_create(branch_plugins, branch_count,
                                                err, sizeof(err));
        if (!chain) {
            fprintf(stderr, "Error: branch %d: %s\n", b, err);
            goto cleanup;
        }
        chains[num_chains++] = chain;
    }

    /* Bus width: the widest branch input (zero for an instrument bus),
     * and the branch output width, which every branch must share. */
    int bus_in = 0;
    int bus_out = mh_chain_get_num_output_channels(chains[0]);
    for (int i = 0; i < num_chains; i++) {
        int ci = mh_chain_get_num_input_channels(chains[i]);
        if (ci > bus_in) bus_in = ci;
        int co = mh_chain_get_num_output_channels(chains[i]);
        if (co != bus_out) {
            fprintf(stderr, "Error: branch %d outputs %d channels, branch 0 outputs %d "
                            "-- a bus sums branches, so their output widths must match\n",
                    i, co, bus_out);
            goto cleanup;
        }
    }
    if (bus_out < 1) bus_out = 2;

    bus = mh_bus_create(bus_in, bus_out, block_size, sample_rate, err, sizeof(err));
    if (!bus) {
        fprintf(stderr, "Error: %s\n", err);
        goto cleanup;
    }
    for (int i = 0; i < num_chains; i++) {
        if (mh_bus_add_branch(bus, chains[i], 1.0f, err, sizeof(err)) < 0) {
            fprintf(stderr, "Error: branch %d: %s\n", i, err);
            goto cleanup;
        }
    }

    /* --- Per-branch gain: --gain INDEX:VALUE --- */
    for (int i = 0; i < num_gain_specs; i++) {
        int idx = 0;
        float value = 1.0f;
        if (sscanf(gain_specs[i], "%d:%f", &idx, &value) != 2) {
            fprintf(stderr, "Error: --gain wants INDEX:VALUE, got '%s'\n", gain_specs[i]);
            goto cleanup;
        }
        if (!mh_bus_set_branch_gain(bus, idx, value)) {
            fprintf(stderr, "Error: branch index %d out of range\n", idx);
            goto cleanup;
        }
    }

    int latency = mh_bus_get_latency_samples(bus);
    int tail_frames = tail_seconds > 0 ? (int)(tail_seconds * sample_rate) : 0;
    int total_samples = in_frames + tail_frames;
    int output_total = total_samples + latency;

    fprintf(stderr, "Bus of %d branch(es) @ %.0f Hz\n", num_chains, sample_rate);
    for (int i = 0; i < num_chains; i++)
        fprintf(stderr, "  [%d] %-44s %din/%dout\n", i, branch_specs[i],
                mh_chain_get_num_input_channels(chains[i]),
                mh_chain_get_num_output_channels(chains[i]));
    fprintf(stderr, "  Bus I/O:     %d in / %d out, latency %d samples\n",
            bus_in, bus_out, latency);
    if (has_midi)
        fprintf(stderr, "  MIDI events: %d (%.2fs) fanned to every branch\n",
                num_midi_events, midi_duration);
    fprintf(stderr, "  Output:      %s\n", output_file);

    /* --- Buffers --- */
    int in_alloc = bus_in > 0 ? bus_in : 1;
    float** in_channels = alloc_channels(in_alloc, output_total);
    float** out_channels = alloc_channels(bus_out, output_total);
    if (!in_channels || !out_channels) {
        fprintf(stderr, "Error: Out of memory\n");
        free_channels(in_channels, in_alloc);
        free_channels(out_channels, bus_out);
        goto cleanup;
    }
    if (audio_data) {
        for (int f = 0; f < in_frames; f++)
            for (int c = 0; c < bus_in; c++)
                in_channels[c][f] = (c < file_ch)
                    ? audio_data->data[(size_t)f * (size_t)file_ch + (size_t)c]
                    : 0.0f;
    }

    /* --- Process --- */
    int midi_cursor = 0;
    for (int start = 0; start < output_total; start += block_size) {
        int end = start + block_size;
        if (end > output_total) end = output_total;
        int bsize = end - start;

        const float* in_ptrs[32];
        float* out_ptrs[32];
        for (int c = 0; c < bus_in && c < 32; c++) in_ptrs[c] = in_channels[c] + start;
        for (int c = 0; c < bus_out && c < 32; c++) out_ptrs[c] = out_channels[c] + start;

        MH_MidiEvent block_midi[MAX_BLOCK_MIDI_EVENTS];
        int num_block_midi = 0;
        while (midi_cursor < num_midi_events
               && midi_events[midi_cursor].sample_offset < end) {
            if (num_block_midi < MAX_BLOCK_MIDI_EVENTS) {
                block_midi[num_block_midi] = midi_events[midi_cursor];
                block_midi[num_block_midi].sample_offset -= start;
                if (block_midi[num_block_midi].sample_offset < 0)
                    block_midi[num_block_midi].sample_offset = 0;
                num_block_midi++;
            }
            midi_cursor++;
        }

        mh_bus_process_midi_io(bus, bus_in > 0 ? in_ptrs : NULL, out_ptrs, bsize,
                               num_block_midi > 0 ? block_midi : NULL,
                               num_block_midi,
                               NULL, 0, NULL, NULL);
    }

    /* --- Latency compensation and write --- */
    int write_offset = latency;
    int write_frames = total_samples;
    if (write_offset + write_frames > output_total)
        write_frames = output_total - write_offset;
    if (write_frames < 0) write_frames = 0;

    rc = 0;
    float* interleaved = (float*)malloc((size_t)bus_out * (size_t)write_frames * sizeof(float));
    if (!interleaved) {
        fprintf(stderr, "Error: Out of memory\n");
        rc = 1;
    } else {
        for (int f = 0; f < write_frames; f++)
            for (int c = 0; c < bus_out; c++)
                interleaved[(size_t)f * (size_t)bus_out + (size_t)c] =
                    out_channels[c][write_offset + f];
        if (bit_depth <= 0) bit_depth = 24;
        if (!mh_audio_write(output_file, interleaved, (unsigned)bus_out,
                            (unsigned)write_frames, (unsigned)sample_rate,
                            bit_depth, err, sizeof(err))) {
            fprintf(stderr, "Error: %s\n", err);
            rc = 1;
        } else {
            fprintf(stderr, "Wrote %d samples (%.2fs) to %s\n", write_frames,
                    (double)write_frames / sample_rate, output_file);
        }
        free(interleaved);
    }

    free_channels(in_channels, in_alloc);
    free_channels(out_channels, bus_out);

cleanup:
    if (session) mh_session_close(session);   /* plugins outlive it */
    if (bus) mh_bus_close(bus);
    for (int i = 0; i < num_chains; i++) mh_chain_close(chains[i]);
    for (int i = 0; i < num_plugins; i++) mh_close(plugins[i]);
    if (audio_data) mh_audio_data_free(audio_data);
    mh_midi_file_free(midi_events);
    return rc;
}

// ============================================================================
// Command: morph
// ============================================================================

// Resolve a snapshot source onto the plugin, then capture its normalized
// parameter values into `out`. `program` >= 0 selects a factory program;
// otherwise `state` (if non-NULL) loads a state blob; otherwise the plugin's
// current values are captured. Returns the param count, or -1 on error.
static int morph_capture_source(MH_Plugin* p, int program, const char* state,
                                float* out, int cap, const char* label) {
    if (state && state[0] != '\0') {
        if (!load_state_file(p, state)) {
            fprintf(stderr, "Error: failed to load snapshot %s state from %s\n", label, state);
            return -1;
        }
    } else if (program >= 0) {
        int np = mh_get_num_programs(p);
        if (program >= np) {
            fprintf(stderr, "Error: snapshot %s program %d out of range (plugin has %d)\n",
                    label, program, np);
            return -1;
        }
        mh_set_program(p, program);
    }
    int n = mh_morph_capture(p, out, cap);
    if (n < 0) {
        fprintf(stderr, "Error: failed to capture snapshot %s\n", label);
        return -1;
    }
    return n;
}

static int cmd_morph(const char* plugin_path, double sample_rate, int block_size,
                     int a_program, int b_program,
                     const char* a_state, const char* b_state,
                     double blend, int apply, const char* save_file,
                     int json_output) {
    char err[1024] = {0};
    MH_Plugin* p = mh_open(plugin_path, sample_rate, block_size, 2, 2, err, sizeof(err));
    if (!p) {
        fprintf(stderr, "Error: %s\n", err);
        return 1;
    }

    int n = mh_get_num_params(p);
    if (n <= 0) {
        fprintf(stderr, "Error: plugin has no parameters to morph\n");
        mh_close(p);
        return 1;
    }

    // Default sources: factory programs 0 and 1 when nothing is specified.
    int have_sources = (a_program >= 0 || b_program >= 0 || a_state || b_state);
    if (!have_sources) {
        int np = mh_get_num_programs(p);
        if (np >= 2) {
            a_program = 0;
            b_program = 1;
        } else {
            fprintf(stderr,
                    "Error: no snapshot sources given and plugin has < 2 factory programs.\n"
                    "       Pass --a-program/--b-program or --a-state/--b-state.\n");
            mh_close(p);
            return 1;
        }
    }

    float* a = (float*)malloc(sizeof(float) * (size_t)n);
    float* b = (float*)malloc(sizeof(float) * (size_t)n);
    float* m = (float*)malloc(sizeof(float) * (size_t)n);
    if (!a || !b || !m) {
        fprintf(stderr, "Error: out of memory\n");
        free(a); free(b); free(m);
        mh_close(p);
        return 1;
    }

    if (morph_capture_source(p, a_program, a_state, a, n, "A") < 0 ||
        morph_capture_source(p, b_program, b_state, b, n, "B") < 0) {
        free(a); free(b); free(m);
        mh_close(p);
        return 1;
    }

    if (!mh_morph_lerp(a, b, m, n, (float)blend)) {
        fprintf(stderr, "Error: morph interpolation failed\n");
        free(a); free(b); free(m);
        mh_close(p);
        return 1;
    }

    // Report the A/B/blend snapshot table.
    if (json_output) {
        printf("{\n  \"blend\": %.6f,\n  \"num_params\": %d,\n  \"params\": [\n", blend, n);
        for (int i = 0; i < n; i++) {
            printf("    {\"index\": %d, \"a\": %.6f, \"b\": %.6f, \"blend\": %.6f}%s\n",
                   i, a[i], b[i], m[i], (i + 1 < n) ? "," : "");
        }
        printf("  ]\n}\n");
    } else {
        fprintf(stderr, "Morph between A and B at t=%.3f (%d params)\n", blend, n);
        printf("%-4s %-28s %9s %9s %9s\n", "idx", "name", "A", "B", "blend");
        for (int i = 0; i < n; i++) {
            MH_ParamInfo pi;
            char name[MH_PARAM_NAME_LEN] = {0};
            memset(&pi, 0, sizeof(pi));
            if (mh_get_param_info(p, i, &pi))
                snprintf(name, sizeof(name), "%s", pi.name);
            printf("%-4d %-28s %9.4f %9.4f %9.4f\n", i, name, a[i], b[i], m[i]);
        }
    }

    // Apply and optionally persist the morphed snapshot.
    if (apply || (save_file && save_file[0] != '\0')) {
        if (!mh_morph_apply(p, m, n)) {
            fprintf(stderr, "Error: failed to apply morphed snapshot\n");
            free(a); free(b); free(m);
            mh_close(p);
            return 1;
        }
        fprintf(stderr, "Applied morphed snapshot to plugin.\n");
        if (save_file && save_file[0] != '\0') {
            if (save_state_file(p, save_file))
                fprintf(stderr, "Saved morphed state to %s\n", save_file);
            else
                fprintf(stderr, "Warning: failed to save state to %s\n", save_file);
        }
    }

    free(a); free(b); free(m);
    mh_close(p);
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    // This binary is its own scan worker: a supervised scan re-runs it once
    // per plugin with --mh-probe-one. That has to be answered before anything
    // else happens here, and in particular before the message thread starts,
    // since the worker's whole job is to probe one plugin and die.
    if (mh_plugin_scan_worker_main(argc, argv)) {
        return 0;
    }

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Cleanly stop the dedicated JUCE plugin thread at process exit. Without
    // this, any command that loads a plugin leaves the message thread's
    // std::thread joinable at static teardown, which calls std::terminate
    // (SIGABRT on exit). We bring the thread up now and register the shutdown
    // with atexit: because the thread is constructed *before* this atexit
    // registration, C++ teardown ordering runs our shutdown handler before the
    // thread object's own destructor. Both calls are idempotent, and no-ops
    // when the message thread is disabled (MINIHOST_MESSAGE_THREAD=0).
    mh_message_thread_init();
    atexit(mh_message_thread_shutdown);

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
    // midi / play / resample options
    int midi_port = -1;
    const char* virtual_midi_name = NULL;
    int monitor_flag = 0;
    int capture_flag = 0;
    int playback_device = -1;
    int capture_device = -1;
    int resample_rate = 0;
    double tail_seconds = 0.0;
    const char* midi_input_file = NULL;
    // presets subcommand
    const char* save_file = NULL;
    int program_index = -1;
    const char* load_vstpreset_file = NULL;
    int overwrite = 0;
    // morph subcommand
    int morph_a_program = -1;
    int morph_b_program = -1;
    const char* morph_a_state = NULL;
    const char* morph_b_state = NULL;
    double morph_blend = 0.5;
    int morph_apply = 0;

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
        } else if (str_eq(opt, "--format")) {
            /* Accepted before the command as well as after it, so
             * `minihost_c --format vst3 probe NAME` works like the
             * subcommand-style parsers users expect. */
            if (cmd_index + 1 >= argc) {
                fprintf(stderr, "Error: --format needs au or vst3\n");
                return 1;
            }
            g_plugin_format = argv[++cmd_index];
        } else if (str_eq(opt, "--fuzzy")) {
            g_plugin_fuzzy = 1;
        } else if (str_eq(opt, "--in-process")) {
            g_scan_in_process = 1;
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
    const char* mix_specs[MAX_PARAM_SPECS];
    int num_mix_specs = 0;
    const char* gain_specs[MAX_PARAM_SPECS];
    int num_gain_specs = 0;
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
        } else if (str_eq(args[i], "--save") && i + 1 < remaining) {
            save_file = args[++i];
        } else if (str_eq(args[i], "--program") && i + 1 < remaining) {
            program_index = atoi(args[++i]);
        } else if (str_eq(args[i], "--load-vstpreset") && i + 1 < remaining) {
            load_vstpreset_file = args[++i];
        } else if (str_eq(args[i], "-y") || str_eq(args[i], "--overwrite")) {
            overwrite = 1;
        } else if (str_eq(args[i], "--monitor")) {
            monitor_flag = 1;
        } else if (str_eq(args[i], "--port") && i + 1 < remaining) {
            midi_port = atoi(args[++i]);
        } else if (str_eq(args[i], "--virtual") && i + 1 < remaining) {
            virtual_midi_name = args[++i];
        } else if (str_eq(args[i], "--capture")) {
            capture_flag = 1;
        } else if (str_eq(args[i], "--playback-device") && i + 1 < remaining) {
            playback_device = atoi(args[++i]);
        } else if (str_eq(args[i], "--capture-device") && i + 1 < remaining) {
            capture_device = atoi(args[++i]);
        } else if (str_eq(args[i], "--rate") && i + 1 < remaining) {
            resample_rate = atoi(args[++i]);
        } else if (str_eq(args[i], "--tail") && i + 1 < remaining) {
            tail_seconds = atof(args[++i]);
        } else if ((str_eq(args[i], "-m") || str_eq(args[i], "--midi")) && i + 1 < remaining) {
            midi_input_file = args[++i];
        } else if (str_eq(args[i], "--a-program") && i + 1 < remaining) {
            morph_a_program = atoi(args[++i]);
        } else if (str_eq(args[i], "--b-program") && i + 1 < remaining) {
            morph_b_program = atoi(args[++i]);
        } else if (str_eq(args[i], "--a-state") && i + 1 < remaining) {
            morph_a_state = args[++i];
        } else if (str_eq(args[i], "--b-state") && i + 1 < remaining) {
            morph_b_state = args[++i];
        } else if ((str_eq(args[i], "-t") || str_eq(args[i], "--blend")) && i + 1 < remaining) {
            morph_blend = atof(args[++i]);
        } else if (str_eq(args[i], "--apply")) {
            morph_apply = 1;
        } else if (str_eq(args[i], "--mix") && i + 1 < remaining) {
            if (num_mix_specs < MAX_PARAM_SPECS) mix_specs[num_mix_specs++] = args[++i];
            else i++;
        } else if (str_eq(args[i], "--format") && i + 1 < remaining) {
            g_plugin_format = args[++i];
        } else if (str_eq(args[i], "--fuzzy")) {
            g_plugin_fuzzy = 1;
        } else if (str_eq(args[i], "--in-process")) {
            g_scan_in_process = 1;
        } else if (str_eq(args[i], "--gain") && i + 1 < remaining) {
            if (num_gain_specs < MAX_PARAM_SPECS) gain_specs[num_gain_specs++] = args[++i];
            else i++;
        } else {
            // Positional argument
            if (num_pos_args < 16) {
                pos_args[num_pos_args++] = i;
            }
        }
    }

    /* Commands whose first positional is a plugin accept a cached name as
     * well as a path; resolve it once here rather than in each handler. */
    char resolved_plugin[1024] = {0};
    static const char* kPluginCommands[] = {
        "probe", "info", "params", "get-param", "set-param", "presets",
        "load-preset", "save-state", "load-state", "process", "morph", "play"
    };
    for (size_t i = 0; i < sizeof(kPluginCommands) / sizeof(kPluginCommands[0]); i++) {
        if (str_eq(cmd, kPluginCommands[i]) && num_pos_args >= 1) {
            const char* got = resolve_plugin_arg(args[pos_args[0]], resolved_plugin,
                                                 sizeof(resolved_plugin));
            if (!got) return 1;
            args[pos_args[0]] = (char*) got;
            break;
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
        /* No directory: scan this platform's canonical plugin locations. */
        return cmd_scan(num_pos_args >= 1 ? args[pos_args[0]] : NULL, json_output,
                        g_scan_in_process);
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
            fprintf(stderr, "Usage: %s presets PLUGIN [--save FILE [--program N | --state FILE | --load-vstpreset FILE] [-y]]\n", argv[0]);
            return 1;
        }
        return cmd_presets(args[pos_args[0]], sample_rate, block_size,
                           json_output, save_file, program_index,
                           state_file, load_vstpreset_file, overwrite);
    }
    else if (str_eq(cmd, "devices")) {
        return cmd_devices(json_output);
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
    else if (str_eq(cmd, "midi")) {
        return cmd_midi(json_output, monitor_flag, midi_port, virtual_midi_name);
    }
    else if (str_eq(cmd, "play")) {
        if (num_pos_args < 1) {
            fprintf(stderr, "Usage: %s play PLUGIN [options]\n", argv[0]);
            return 1;
        }
        return cmd_play(args[pos_args[0]], sample_rate, block_size,
                        state_file, preset_index, param_specs, num_param_specs,
                        midi_port, virtual_midi_name,
                        capture_flag, playback_device, capture_device);
    }
    else if (str_eq(cmd, "resample")) {
        const char* rs_input = NULL;
        const char* rs_output = NULL;
        if (input_file && output_file) {
            rs_input = input_file;
            rs_output = output_file;
        } else if (num_pos_args >= 2) {
            rs_input = args[pos_args[0]];
            rs_output = args[pos_args[1]];
        }
        if (!rs_input || !rs_output) {
            fprintf(stderr, "Usage: %s resample INPUT OUTPUT --rate N\n", argv[0]);
            return 1;
        }
        return cmd_resample(rs_input, rs_output, resample_rate, bit_depth);
    }
    else if (str_eq(cmd, "chain")) {
        /* Every positional argument is a plugin, in signal order. */
        const char* chain_plugins[MAX_CHAIN_PLUGINS];
        static char chain_resolved[MAX_CHAIN_PLUGINS][1024];
        int chain_count = 0;
        for (int i = 0; i < num_pos_args && chain_count < MAX_CHAIN_PLUGINS; i++) {
            const char* got = resolve_plugin_arg(args[pos_args[i]],
                                                 chain_resolved[chain_count],
                                                 sizeof(chain_resolved[chain_count]));
            if (!got) return 1;
            chain_plugins[chain_count++] = got;
        }
        if (chain_count < 1) {
            fprintf(stderr, "Usage: %s chain PLUGIN [PLUGIN...] "
                            "[-i INPUT] [-m MIDI] -o OUTPUT\n", argv[0]);
            return 1;
        }
        return cmd_chain(chain_plugins, chain_count, input_file, output_file,
                         midi_input_file, sample_rate, block_size,
                         non_realtime, bpm, bit_depth, tail_seconds,
                         mix_specs, num_mix_specs);
    }
    else if (str_eq(cmd, "bus")) {
        const char* branches[MAX_BUS_BRANCHES];
        int branch_count = 0;
        for (int i = 0; i < num_pos_args && branch_count < MAX_BUS_BRANCHES; i++)
            branches[branch_count++] = args[pos_args[i]];
        if (branch_count < 1) {
            fprintf(stderr, "Usage: %s bus BRANCH [BRANCH...] "
                            "[-i INPUT] [-m MIDI] -o OUTPUT\n"
                            "  a BRANCH is one plugin, or several comma-separated "
                            "plugins in series\n", argv[0]);
            return 1;
        }
        return cmd_bus(branches, branch_count, input_file, output_file,
                       midi_input_file, sample_rate, block_size,
                       non_realtime, bpm, bit_depth, tail_seconds,
                       gain_specs, num_gain_specs);
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

        if (!output_file || (!input_file && !midi_input_file)) {
            fprintf(stderr,
                    "Error: an output file (-o) and either an input file (-i) "
                    "or a MIDI file (-m) are required\n");
            return 1;
        }

        return cmd_process(plugin, input_file, output_file, sidechain_file,
                           midi_input_file,
                           sample_rate, block_size, state_file,
                           preset_index, param_specs, num_param_specs,
                           use_double, non_realtime, bpm, bit_depth,
                           tail_seconds);
    }
    else if (str_eq(cmd, "morph")) {
        if (num_pos_args < 1) {
            fprintf(stderr,
                    "Usage: %s morph PLUGIN [--a-program N | --a-state FILE]\n"
                    "                      [--b-program N | --b-state FILE]\n"
                    "                      [-t BLEND] [--apply] [--save FILE] [-j]\n",
                    argv[0]);
            return 1;
        }
        return cmd_morph(args[pos_args[0]], sample_rate, block_size,
                         morph_a_program, morph_b_program,
                         morph_a_state, morph_b_state,
                         morph_blend, morph_apply, save_file, json_output);
    }
    else {
        fprintf(stderr, "Error: Unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}
