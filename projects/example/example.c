// example_c.c
#include "minihost.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static float** alloc_channels(int ch, int n)
{
    float** p = (float**)calloc((size_t)ch, sizeof(float*));
    for (int i = 0; i < ch; ++i)
        p[i] = (float*)calloc((size_t)n, sizeof(float));
    return p;
}

static void free_channels(float** p, int ch)
{
    if (!p) return;
    for (int i = 0; i < ch; ++i) free(p[i]);
    free(p);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: %s /path/to/plugin.vst3 or .component\n", argv[0]);
        return 1;
    }

    const double sr = 48000.0;
    const int bs = 512;
    const int inCh = 2, outCh = 2;

    char err[1024] = {0};
    MH_Plugin* plug = mh_open(argv[1], sr, bs, inCh, outCh, err, sizeof(err));
    if (!plug) {
        printf("Failed to open plugin: %s\n", err);
        return 1;
    }

    MH_Info info;
    mh_get_info(plug, &info);
    printf("Params: %d, InCh: %d, OutCh: %d, Latency: %d samples\n",
           info.num_params, info.num_input_ch, info.num_output_ch, info.latency_samples);

    // Latency compensation info
    int latency = mh_get_latency_samples(plug);
    if (latency > 0)
        printf("Latency: %d samples (%.2f ms at %.0f Hz)\n",
               latency, latency * 1000.0 / sr, sr);

    // Query tail time (for reverbs/delays)
    double tail = mh_get_tail_seconds(plug);
    if (tail > 0.0)
        printf("Tail: %.2f seconds\n", tail);

    // Test bypass (if supported)
    printf("Bypass supported: %s\n", mh_set_bypass(plug, 1) ? "yes" : "no");
    mh_set_bypass(plug, 0);  // Disable bypass for processing

    // Print first 5 parameters (or fewer if plugin has less)
    int maxParams = info.num_params < 5 ? info.num_params : 5;
    for (int i = 0; i < maxParams; ++i) {
        MH_ParamInfo pinfo;
        if (mh_get_param_info(plug, i, &pinfo)) {
            printf("  Param %d: \"%s\" = %s %s (default: %.2f, steps: %d%s%s)\n",
                   i, pinfo.name, pinfo.current_value_str, pinfo.label, pinfo.default_value,
                   pinfo.num_steps,
                   pinfo.is_boolean ? ", bool" : "",
                   pinfo.is_automatable ? "" : ", no-auto");
        }
    }

    float** in  = alloc_channels(inCh, bs);
    float** out = alloc_channels(outCh, bs);

    // Put a simple impulse in L channel
    in[0][0] = 1.0f;

    // Example: set param 0 to 0.5 if exists
    if (mh_get_num_params(plug) > 0)
        mh_set_param(plug, 0, 0.5f);

    // Example: send a MIDI note-on (middle C, velocity 100)
    MH_MidiEvent midi_in[2];
    midi_in[0].sample_offset = 0;
    midi_in[0].status = 0x90;  // Note on, channel 1
    midi_in[0].data1 = 60;     // Middle C
    midi_in[0].data2 = 100;    // Velocity

    // Note off at sample 256
    midi_in[1].sample_offset = 256;
    midi_in[1].status = 0x80;  // Note off, channel 1
    midi_in[1].data1 = 60;
    midi_in[1].data2 = 0;

    // Set transport info (for tempo-synced plugins)
    MH_TransportInfo transport = {0};
    transport.bpm = 120.0;
    transport.time_sig_numerator = 4;
    transport.time_sig_denominator = 4;
    transport.position_samples = 0;
    transport.position_beats = 0.0;
    transport.is_playing = 1;
    mh_set_transport(plug, &transport);

    // Example: sample-accurate parameter automation
    // Ramp param 0 from 0.0 to 1.0 across the block
    MH_ParamChange param_changes[4];
    int num_changes = 0;
    if (mh_get_num_params(plug) > 0) {
        param_changes[0].sample_offset = 0;
        param_changes[0].param_index = 0;
        param_changes[0].value = 0.0f;

        param_changes[1].sample_offset = bs / 4;
        param_changes[1].param_index = 0;
        param_changes[1].value = 0.33f;

        param_changes[2].sample_offset = bs / 2;
        param_changes[2].param_index = 0;
        param_changes[2].value = 0.66f;

        param_changes[3].sample_offset = bs * 3 / 4;
        param_changes[3].param_index = 0;
        param_changes[3].value = 1.0f;

        num_changes = 4;
    }

    // Process with MIDI I/O and sample-accurate automation
    MH_MidiEvent midi_out[64];
    int num_midi_out = 0;

    if (!mh_process_auto(plug, (const float* const*)in, out, bs,
                         midi_in, 2, midi_out, 64, &num_midi_out,
                         param_changes, num_changes))
        printf("process failed\n");

    printf("Out[0][0]=%f\n", out[0][0]);
    if (num_midi_out > 0)
        printf("Plugin generated %d MIDI output event(s)\n", num_midi_out);

    // Example: save and restore state
    int state_size = mh_get_state_size(plug);
    if (state_size > 0) {
        printf("State size: %d bytes\n", state_size);
        void* state = malloc((size_t)state_size);
        if (state && mh_get_state(plug, state, state_size)) {
            printf("State saved successfully\n");
            // Could save to file here, then restore later:
            if (mh_set_state(plug, state, state_size))
                printf("State restored successfully\n");
        }
        free(state);
    }

    free_channels(in, inCh);
    free_channels(out, outCh);
    mh_close(plug);
    return 0;
}

