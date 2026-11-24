/* minihost_test_clap.c
 *
 * Deterministic CLAP fixture: the CLAP counterpart of TestPluginProcessor.
 * Same parameter contract, so the same exact-value assertions apply:
 *
 *   0  gain       0..1, default 1, continuous
 *   1  latency    0..4096 samples, stepped; delays the output by exactly
 *                 that many samples and reports it through clap.latency
 *   2  scmix      0..1, default 0; output += scmix * sidechain (effect only)
 *   3  seen tempo read-only, 0..1000: the transport tempo of the last block,
 *                 0 without transport. Reported through output events.
 *   4  seen flags read-only, stepped 0..255: clap_event_transport.flags of
 *                 the last block.
 *
 * Built twice (see ../CMakeLists.txt):
 *   MinihostTestFx    - stereo in + stereo sidechain in, stereo out. Note
 *                       ports speak MIDI only.
 *   MinihostTestSynth - stereo out, monophonic sine (MINIHOST_TEST_SYNTH=1).
 *                       Note ports prefer the CLAP dialect, so the host's
 *                       MIDI <-> CLAP note conversion is exercised.
 *
 * Both echo every note/MIDI event they receive to their note output, at the
 * same sample offset.
 *
 * Stalls on request, for the host's hang handling (tests/test_clap_stalls.py):
 *   CC 119, value v   request_restart, then sleep v*10 ms inside process()
 *   CC 118, value v   request_callback; on_main_thread then sleeps v*10 ms
 *   MINIHOST_TEST_CLAP_HANG_INIT="<substring>:<ms>"
 *                     clap_entry.init() sleeps <ms> when its path contains
 *                     <substring> State is the three values behind a magic header; any
 * other blob is rejected, so a host that ignores load() failure is caught.
 */

#include <clap/clap.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD) ms); }
#else
#include <time.h>
static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long) (ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) != 0) {}
}
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef MINIHOST_TEST_SYNTH
#define MINIHOST_TEST_SYNTH 0
#endif

#define MAX_LATENCY 4096
#define RING (MAX_LATENCY + 1)
#define NUM_CH 2
#define VOICE_PEAK 0.25f

enum { P_GAIN = 0, P_LATENCY = 1, P_SCMIX = 2, P_SEEN_TEMPO = 3, P_SEEN_FLAGS = 4, NUM_PARAMS = 5 };

static const char STATE_MAGIC[4] = { 'M', 'H', 'T', 'C' };

typedef struct {
    char magic[4];
    double gain;
    int32_t latency;
    double scmix;
} saved_state;

typedef struct {
    clap_plugin_t plugin;
    const clap_host_t* host;
    const clap_host_latency_t* host_latency;

    double gain, scmix;
    int latency;          /* parameter value */
    int applied_latency;  /* what the delay line currently implements */

    double sample_rate;
    float* ring[NUM_CH];
    int write_pos;

    /* sine voice */
    double phase, phase_delta;
    float level;
    int active_note;

    /* transport as last reported to the host */
    double seen_tempo;
    uint32_t seen_flags;

    int main_thread_sleep_ms;   /* consumed by on_main_thread */
} fixture;

static fixture* self(const clap_plugin_t* p) { return (fixture*) p->plugin_data; }

/* ---- DSP ---------------------------------------------------------------- */

static void reset_dsp(fixture* f)
{
    for (int ch = 0; ch < NUM_CH; ++ch)
        if (f->ring[ch]) memset(f->ring[ch], 0, sizeof(float) * RING);
    f->write_pos = 0;
    f->phase = 0.0;
    f->phase_delta = 0.0;
    f->level = 0.0f;
    f->active_note = -1;
}

static void set_latency(fixture* f, int samples)
{
    if (samples < 0) samples = 0;
    if (samples > MAX_LATENCY) samples = MAX_LATENCY;
    f->latency = samples;
}

/* Called at the top of each block: a new latency takes effect at once, with
 * a cleared line, as the VST3 fixture does. The host learns of it through
 * request_restart. */
static void apply_latency_change(fixture* f)
{
    if (f->latency == f->applied_latency) return;
    f->applied_latency = f->latency;
    for (int ch = 0; ch < NUM_CH; ++ch)
        memset(f->ring[ch], 0, sizeof(float) * RING);
    f->write_pos = 0;
    f->host->request_restart(f->host);
}

static void note_on(fixture* f, int key, double velocity)
{
    f->active_note = key;
    f->phase_delta = 2.0 * M_PI * (440.0 * pow(2.0, (key - 69) / 12.0)) / f->sample_rate;
    f->phase = 0.0;
    f->level = VOICE_PEAK * (float) velocity;
}

static void note_off(fixture* f, int key)
{
    if (key != f->active_note) return;
    f->active_note = -1;
    f->level = 0.0f;
}

/* Render frames [a, b) of the block. */
static void render(fixture* f, const clap_process_t* proc, uint32_t a, uint32_t b)
{
    float** out = proc->audio_outputs[0].data32;
    const float g = (float) f->gain;

#if MINIHOST_TEST_SYNTH
    for (uint32_t i = a; i < b; ++i)
    {
        const float s = f->level * (float) sin(f->phase);
        f->phase += f->phase_delta;
        for (int ch = 0; ch < NUM_CH; ++ch)
            out[ch][i] = s;
    }
#else
    float** in = proc->audio_inputs[0].data32;
    float** sc = proc->audio_inputs_count > 1 ? proc->audio_inputs[1].data32 : NULL;
    const float mix = (float) f->scmix;
    for (int ch = 0; ch < NUM_CH; ++ch)
        for (uint32_t i = a; i < b; ++i)
        {
            float x = in[ch][i];
            if (mix != 0.0f && sc != NULL) x += mix * sc[ch][i];
            out[ch][i] = x;
        }
#endif

    if (g != 1.0f)
        for (int ch = 0; ch < NUM_CH; ++ch)
            for (uint32_t i = a; i < b; ++i)
                out[ch][i] *= g;
}

static void delay_block(fixture* f, const clap_process_t* proc, uint32_t n)
{
    const int lat = f->applied_latency;
    if (lat <= 0) return;
    float** out = proc->audio_outputs[0].data32;
    int w = 0;
    for (int ch = 0; ch < NUM_CH; ++ch)
    {
        float* mem = f->ring[ch];
        w = f->write_pos;
        for (uint32_t i = 0; i < n; ++i)
        {
            const int r = (w + RING - lat) % RING;
            const float y = mem[r];
            mem[w] = out[ch][i];
            out[ch][i] = y;
            w = (w + 1) % RING;
        }
    }
    f->write_pos = w;
}

/* ---- events ------------------------------------------------------------- */

static void handle_param(fixture* f, const clap_event_param_value_t* ev)
{
    switch (ev->param_id)
    {
        case P_GAIN:    f->gain = ev->value; break;
        case P_LATENCY: set_latency(f, (int) lround(ev->value)); break;
        case P_SCMIX:   f->scmix = ev->value; break;
        default: break;
    }
}

static void handle_event(fixture* f, const clap_event_header_t* h, const clap_output_events_t* out)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
    switch (h->type)
    {
        case CLAP_EVENT_PARAM_VALUE:
            handle_param(f, (const clap_event_param_value_t*) h);
            return;
        case CLAP_EVENT_NOTE_ON:
        {
            const clap_event_note_t* ev = (const clap_event_note_t*) h;
            note_on(f, ev->key, ev->velocity);
            break;
        }
        case CLAP_EVENT_NOTE_OFF:
        {
            const clap_event_note_t* ev = (const clap_event_note_t*) h;
            note_off(f, ev->key);
            break;
        }
        case CLAP_EVENT_MIDI:
        {
            const clap_event_midi_t* ev = (const clap_event_midi_t*) h;
            const uint8_t kind = ev->data[0] & 0xF0;
            if (kind == 0x90 && ev->data[2] > 0) note_on(f, ev->data[1], ev->data[2] / 127.0);
            else if (kind == 0x80 || kind == 0x90) note_off(f, ev->data[1]);
            else if (kind == 0xB0 && ev->data[1] == 119)
            {
                f->host->request_restart(f->host);
                sleep_ms(ev->data[2] * 10);
            }
            else if (kind == 0xB0 && ev->data[1] == 118)
            {
                f->main_thread_sleep_ms = ev->data[2] * 10;
                f->host->request_callback(f->host);
            }
            break;
        }
        case CLAP_EVENT_MIDI_SYSEX:
            break;
        default:
            return;
    }
    if (out != NULL) out->try_push(out, h);
}

/* ---- plugin ------------------------------------------------------------- */

static bool fx_init(const clap_plugin_t* p)
{
    fixture* f = self(p);
    f->host_latency = (const clap_host_latency_t*) f->host->get_extension(f->host, CLAP_EXT_LATENCY);
    return true;
}

static void fx_destroy(const clap_plugin_t* p)
{
    fixture* f = self(p);
    for (int ch = 0; ch < NUM_CH; ++ch) free(f->ring[ch]);
    free(f);
}

static bool fx_activate(const clap_plugin_t* p, double sr, uint32_t minf, uint32_t maxf)
{
    (void) minf; (void) maxf;
    fixture* f = self(p);
    f->sample_rate = sr;
    for (int ch = 0; ch < NUM_CH; ++ch)
    {
        if (!f->ring[ch]) f->ring[ch] = (float*) calloc(RING, sizeof(float));
        if (!f->ring[ch]) return false;
    }
    reset_dsp(f);
    f->applied_latency = f->latency;
    return true;
}

static void fx_deactivate(const clap_plugin_t* p) { (void) p; }
static bool fx_start(const clap_plugin_t* p) { (void) p; return true; }
static void fx_stop(const clap_plugin_t* p) { (void) p; }
static void fx_reset(const clap_plugin_t* p) { reset_dsp(self(p)); }

static void report(const clap_output_events_t* out, clap_id id, double value)
{
    clap_event_param_value_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.header.size = sizeof(ev);
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.param_id = id;
    ev.note_id = -1;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.value = value;
    out->try_push(out, &ev.header);
}

static void report_transport(fixture* f, const clap_process_t* proc)
{
    const double tempo = proc->transport ? proc->transport->tempo : 0.0;
    const uint32_t flags = proc->transport ? proc->transport->flags : 0;
    if (tempo != f->seen_tempo)
    {
        f->seen_tempo = tempo;
        report(proc->out_events, P_SEEN_TEMPO, tempo);
    }
    if (flags != f->seen_flags)
    {
        f->seen_flags = flags;
        report(proc->out_events, P_SEEN_FLAGS, (double) flags);
    }
}

static clap_process_status fx_process(const clap_plugin_t* p, const clap_process_t* proc)
{
    fixture* f = self(p);
    report_transport(f, proc);
    const uint32_t n = proc->frames_count;
    const uint32_t count = proc->in_events->size(proc->in_events);

    uint32_t pos = 0;
    for (uint32_t e = 0; e < count; ++e)
    {
        const clap_event_header_t* h = proc->in_events->get(proc->in_events, e);
        uint32_t at = h->time < n ? h->time : n;
        if (at > pos)
        {
            render(f, proc, pos, at);
            pos = at;
        }
        handle_event(f, h, proc->out_events);
    }
    if (pos < n) render(f, proc, pos, n);

    apply_latency_change(f);
    delay_block(f, proc, n);
    return CLAP_PROCESS_CONTINUE;
}

static void fx_on_main_thread(const clap_plugin_t* p)
{
    fixture* f = self(p);
    const int ms = f->main_thread_sleep_ms;
    f->main_thread_sleep_ms = 0;
    if (ms > 0) sleep_ms(ms);
}

/* ---- extensions --------------------------------------------------------- */

static uint32_t ap_count(const clap_plugin_t* p, bool is_input)
{
    (void) p;
#if MINIHOST_TEST_SYNTH
    return is_input ? 0 : 1;
#else
    return is_input ? 2 : 1;
#endif
}

static bool ap_get(const clap_plugin_t* p, uint32_t index, bool is_input, clap_audio_port_info_t* info)
{
    if (index >= ap_count(p, is_input)) return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->channel_count = NUM_CH;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    if (index == 0)
    {
        info->flags = CLAP_AUDIO_PORT_IS_MAIN;
        strcpy(info->name, is_input ? "Input" : "Output");
    }
    else
    {
        strcpy(info->name, "Sidechain");
    }
    return true;
}

static const clap_plugin_audio_ports_t audio_ports = { ap_count, ap_get };

static uint32_t np_count(const clap_plugin_t* p, bool is_input) { (void) p; (void) is_input; return 1; }

static bool np_get(const clap_plugin_t* p, uint32_t index, bool is_input, clap_note_port_info_t* info)
{
    (void) p;
    if (index != 0) return false;
    memset(info, 0, sizeof(*info));
    info->id = 0;
#if MINIHOST_TEST_SYNTH
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
#else
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
#endif
    strcpy(info->name, is_input ? "MIDI In" : "MIDI Out");
    return true;
}

static const clap_plugin_note_ports_t note_ports = { np_count, np_get };

static uint32_t pr_count(const clap_plugin_t* p) { (void) p; return NUM_PARAMS; }

static bool pr_info(const clap_plugin_t* p, uint32_t index, clap_param_info_t* info)
{
    (void) p;
    if (index >= NUM_PARAMS) return false;
    memset(info, 0, sizeof(*info));
    info->id = index;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    switch (index)
    {
        case P_GAIN:
            strcpy(info->name, "Gain");
            info->min_value = 0.0; info->max_value = 1.0; info->default_value = 1.0;
            break;
        case P_LATENCY:
            strcpy(info->name, "Latency");
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->min_value = 0.0; info->max_value = MAX_LATENCY; info->default_value = 0.0;
            break;
        case P_SCMIX:
            strcpy(info->name, "Sidechain Mix");
            info->min_value = 0.0; info->max_value = 1.0; info->default_value = 0.0;
            break;
        case P_SEEN_TEMPO:
            strcpy(info->name, "Seen Tempo");
            info->flags = CLAP_PARAM_IS_READONLY;
            info->min_value = 0.0; info->max_value = 1000.0; info->default_value = 0.0;
            break;
        default:
            strcpy(info->name, "Seen Transport Flags");
            info->flags = CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
            info->min_value = 0.0; info->max_value = 255.0; info->default_value = 0.0;
            break;
    }
    return true;
}

static bool pr_value(const clap_plugin_t* p, clap_id id, double* out)
{
    fixture* f = self(p);
    switch (id)
    {
        case P_GAIN:    *out = f->gain; return true;
        case P_LATENCY: *out = f->latency; return true;
        case P_SCMIX:   *out = f->scmix; return true;
        case P_SEEN_TEMPO: *out = f->seen_tempo; return true;
        case P_SEEN_FLAGS: *out = (double) f->seen_flags; return true;
        default: return false;
    }
}

static bool pr_to_text(const clap_plugin_t* p, clap_id id, double v, char* buf, uint32_t cap)
{
    (void) p;
    if (id >= NUM_PARAMS || cap == 0) return false;
    if (id == P_LATENCY) snprintf(buf, cap, "%d smp", (int) lround(v));
    else snprintf(buf, cap, "%.2f", v);
    return true;
}

static bool pr_from_text(const clap_plugin_t* p, clap_id id, const char* text, double* out)
{
    (void) p;
    if (id >= NUM_PARAMS || text == NULL) return false;
    char* end = NULL;
    const double v = strtod(text, &end);
    if (end == text) return false;
    *out = v;
    return true;
}

static void pr_flush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
{
    (void) out;
    fixture* f = self(p);
    const uint32_t count = in->size(in);
    for (uint32_t e = 0; e < count; ++e)
    {
        const clap_event_header_t* h = in->get(in, e);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
            handle_param(f, (const clap_event_param_value_t*) h);
    }
}

static const clap_plugin_params_t params = {
    pr_count, pr_info, pr_value, pr_to_text, pr_from_text, pr_flush
};

static uint32_t lat_get(const clap_plugin_t* p) { return (uint32_t) self(p)->applied_latency; }

static const clap_plugin_latency_t latency = { lat_get };

static bool st_save(const clap_plugin_t* p, const clap_ostream_t* os)
{
    fixture* f = self(p);
    saved_state s;
    memset(&s, 0, sizeof(s));
    memcpy(s.magic, STATE_MAGIC, 4);
    s.gain = f->gain;
    s.latency = f->latency;
    s.scmix = f->scmix;
    const char* src = (const char*) &s;
    uint64_t left = sizeof(s);
    while (left > 0)
    {
        const int64_t n = os->write(os, src, left);
        if (n <= 0) return false;
        src += n;
        left -= (uint64_t) n;
    }
    return true;
}

static bool st_load(const clap_plugin_t* p, const clap_istream_t* is)
{
    fixture* f = self(p);
    saved_state s;
    char* dst = (char*) &s;
    uint64_t got = 0;
    while (got < sizeof(s))
    {
        const int64_t n = is->read(is, dst + got, sizeof(s) - got);
        if (n <= 0) break;
        got += (uint64_t) n;
    }
    char extra;
    if (got != sizeof(s) || is->read(is, &extra, 1) != 0) return false;
    if (memcmp(s.magic, STATE_MAGIC, 4) != 0) return false;
    f->gain = s.gain;
    set_latency(f, s.latency);
    f->scmix = s.scmix;
    return true;
}

static const clap_plugin_state_t state = { st_save, st_load };

static const void* fx_get_extension(const clap_plugin_t* p, const char* id)
{
    (void) p;
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audio_ports;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS))  return &note_ports;
    if (!strcmp(id, CLAP_EXT_PARAMS))      return &params;
    if (!strcmp(id, CLAP_EXT_LATENCY))     return &latency;
    if (!strcmp(id, CLAP_EXT_STATE))       return &state;
    return NULL;
}

/* ---- factory and entry -------------------------------------------------- */

#if MINIHOST_TEST_SYNTH
static const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, NULL };
#define PLUGIN_ID   "net.minihost.test-synth"
#define PLUGIN_NAME "MinihostTestSynth"
#else
static const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, NULL };
#define PLUGIN_ID   "net.minihost.test-fx"
#define PLUGIN_NAME "MinihostTestFx"
#endif

static const clap_plugin_descriptor_t descriptor = {
    CLAP_VERSION_INIT,
    PLUGIN_ID,
    PLUGIN_NAME,
    "minihost",
    "",
    "",
    "",
    "1.0.0",
    "minihost deterministic test fixture",
    features,
};

static const clap_plugin_t* create(const clap_host_t* host)
{
    fixture* f = (fixture*) calloc(1, sizeof(fixture));
    if (!f) return NULL;
    f->host = host;
    f->gain = 1.0;
    f->scmix = 0.0;
    f->latency = 0;
    f->applied_latency = 0;
    f->sample_rate = 44100.0;
    f->active_note = -1;
    f->plugin.desc = &descriptor;
    f->plugin.plugin_data = f;
    f->plugin.init = fx_init;
    f->plugin.destroy = fx_destroy;
    f->plugin.activate = fx_activate;
    f->plugin.deactivate = fx_deactivate;
    f->plugin.start_processing = fx_start;
    f->plugin.stop_processing = fx_stop;
    f->plugin.reset = fx_reset;
    f->plugin.process = fx_process;
    f->plugin.get_extension = fx_get_extension;
    f->plugin.on_main_thread = fx_on_main_thread;
    return &f->plugin;
}

static uint32_t factory_count(const clap_plugin_factory_t* fac) { (void) fac; return 1; }

static const clap_plugin_descriptor_t* factory_desc(const clap_plugin_factory_t* fac, uint32_t i)
{
    (void) fac;
    return i == 0 ? &descriptor : NULL;
}

static const clap_plugin_t* factory_create(const clap_plugin_factory_t* fac, const clap_host_t* host,
                                         const char* id)
{
    (void) fac;
    if (!clap_version_is_compatible(host->clap_version) || strcmp(id, PLUGIN_ID) != 0)
        return NULL;
    return create(host);
}

static const clap_plugin_factory_t factory = { factory_count, factory_desc, factory_create };

static bool entry_init(const char* path)
{
    const char* spec = getenv("MINIHOST_TEST_CLAP_HANG_INIT");
    const char* colon = spec ? strrchr(spec, ':') : NULL;
    if (colon != NULL && path != NULL)
    {
        char needle[256];
        size_t len = (size_t) (colon - spec);
        if (len >= sizeof(needle)) len = sizeof(needle) - 1;
        memcpy(needle, spec, len);
        needle[len] = '\0';
        if (len > 0 && strstr(path, needle) != NULL)
            sleep_ms(atoi(colon + 1));
    }
    return true;
}
static void entry_deinit(void) {}

static const void* entry_factory(const char* id)
{
    return strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, entry_init, entry_deinit, entry_factory
};
