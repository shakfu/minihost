// MIDI stage capacity and drop accounting in the plugin chain.
//
// A chain's inter-plugin MIDI buffers held 256 events, and anything past that
// was dropped with no count: a note-on at event 300 never reached the next
// plugin, and a bus reported no overflow. Real plugins cannot exercise the
// stage limit (JUCE's VST3 hosting hands a plugin at most 2048 events), so the
// chain is built here against a stub plugin that emits `factor` copies of each
// input event. Built under ASan, so a write past a stage buffer aborts.
// Built and run by `make native-tests`.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "minihost.h"
#include "minihost_chain.h"

// ---------------------------------------------------------------- stubs --

struct MH_Plugin {
    int factor = 1;   // output events per input event
    int dropped = 0;  // output events past capacity, last call
    int latency = 0;  // audio delay applied and reported, in frames
    double tail = 0.0;
    float last_param = -1.0f;  // last value set through mh_set_param_rt
    std::vector<float> hist = std::vector<float>(1 << 16, 0.0f);
    int pos = 0;
};

namespace {
constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
constexpr int kStage = 4096;  // kChainMidiStageCapacity
}

extern "C" {

int mh_get_info(MH_Plugin*, MH_Info* info) {
    std::memset(info, 0, sizeof(*info));
    info->num_input_ch = 1;
    info->num_output_ch = 1;
    info->accepts_midi = 1;
    info->produces_midi = 1;
    return 1;
}
double mh_get_sample_rate(MH_Plugin*) { return kRate; }
int mh_get_max_block_size(MH_Plugin*) { return kBlock; }
int mh_get_latency_samples(MH_Plugin* p) { return p->latency; }
int mh_get_latency_samples_rt(MH_Plugin* p) { return p->latency; }
double mh_get_tail_seconds(MH_Plugin* p) { return p->tail; }
int mh_reset(MH_Plugin*) { return 1; }
int mh_set_non_realtime(MH_Plugin*, int) { return 1; }
int mh_set_param_rt(MH_Plugin* p, int, float v) { p->last_param = v; return 1; }
int mh_get_midi_out_dropped(MH_Plugin* p) { return p ? p->dropped : 0; }

// Delays its one channel by `latency` frames.
int mh_process(MH_Plugin* p, const float* const* inputs, float* const* outputs,
               int nframes) {
    const int size = (int)p->hist.size();
    for (int n = 0; n < nframes; ++n) {
        p->hist[(p->pos + n) % size] = inputs ? inputs[0][n] : 0.0f;
        const float y = p->hist[(p->pos + n - p->latency + size) % size];
        if (outputs) outputs[0][n] = y;
    }
    p->pos = (p->pos + nframes) % size;
    return 1;
}

int mh_process_midi_io(MH_Plugin* p, const float* const* in, float* const* outputs,
                       int nframes, const MH_MidiEvent* midi_in, int num_midi_in,
                       MH_MidiEvent* midi_out, int midi_out_capacity,
                       int* num_midi_out) {
    mh_process(p, in, outputs, nframes);
    const int produced = num_midi_in * p->factor;
    int n = 0;
    if (midi_out) {
        for (; n < produced && n < midi_out_capacity; ++n)
            midi_out[n] = midi_in[n / p->factor];
    }
    if (num_midi_out) *num_midi_out = n;
    p->dropped = midi_out ? produced - n : 0;
    return 1;
}

} // extern "C"

// ---------------------------------------------------------------- tests --

namespace {

int failures = 0;

#define EXPECT(cond, ...)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            std::fprintf(stderr, "FAIL %s:%d: ", __func__, __LINE__);  \
            std::fprintf(stderr, __VA_ARGS__);                         \
            std::fputc('\n', stderr);                                  \
            ++failures;                                                \
        }                                                              \
    } while (0)

std::vector<MH_MidiEvent> notes(int n) {
    return std::vector<MH_MidiEvent>((size_t)n, MH_MidiEvent{0, 0x90, 60, 100});
}

struct Chain {
    std::vector<MH_Plugin> plugins;
    MH_PluginChain* c = nullptr;
    float in[kBlock] = {};
    float out[kBlock] = {};

    explicit Chain(std::vector<int> factors) : plugins(factors.size()) {
        std::vector<MH_Plugin*> ptrs;
        for (size_t i = 0; i < factors.size(); ++i) {
            plugins[i].factor = factors[i];
            ptrs.push_back(&plugins[i]);
        }
        char err[256] = {};
        c = mh_chain_create(ptrs.data(), (int)ptrs.size(), err, sizeof err);
        if (!c) std::fprintf(stderr, "create: %s\n", err);
    }
    ~Chain() { if (c) mh_chain_close(c); }

    // Drains into a heap buffer of exactly `capacity`, so ASan sees any
    // write past it. Returns the count written.
    int run(const std::vector<MH_MidiEvent>& ev, int capacity) {
        const float* ins[1] = {in};
        float* outs[1] = {out};
        std::vector<MH_MidiEvent> buf((size_t)capacity);
        int n = -1;
        mh_chain_process_midi_io(c, ins, outs, kBlock, ev.data(), (int)ev.size(),
                                 capacity ? buf.data() : nullptr, capacity, &n);
        return n;
    }

    int run_auto(const std::vector<MH_MidiEvent>& ev, int capacity) {
        const float* ins[1] = {in};
        float* outs[1] = {out};
        std::vector<MH_MidiEvent> buf((size_t)capacity);
        MH_ChainParamChange pc{};
        pc.sample_offset = kBlock / 2;
        int n = -1;
        mh_chain_process_auto(c, ins, outs, kBlock, ev.data(), (int)ev.size(),
                              buf.data(), capacity, &n, &pc, 1);
        return n;
    }
};

void test_stage_overflow_is_counted() {
    Chain ch({3});  // 2000 in -> 6000 out -> 4096 fit
    const int n = ch.run(notes(2000), 65536);
    EXPECT(n == kStage, "out %d", n);
    EXPECT(mh_chain_get_midi_dropped(ch.c) == 6000 - kStage, "dropped %d",
           mh_chain_get_midi_dropped(ch.c));
}

void test_stage_drop_counted_without_midi_out() {
    // The loss reaches the second plugin whether or not the caller collects.
    Chain ch({3, 1});
    ch.run(notes(2000), 0);
    EXPECT(mh_chain_get_midi_dropped(ch.c) == 6000 - kStage, "dropped %d",
           mh_chain_get_midi_dropped(ch.c));
}

void test_caller_capacity_overflow_is_counted() {
    Chain ch({1});
    const int n = ch.run(notes(300), 100);
    EXPECT(n == 100, "out %d", n);
    EXPECT(mh_chain_get_midi_dropped(ch.c) == 200, "dropped %d",
           mh_chain_get_midi_dropped(ch.c));
}

void test_300_events_pass_the_old_256_limit() {
    Chain ch({1, 1});
    const int n = ch.run(notes(300), 4096);
    EXPECT(n == 300, "out %d", n);
    EXPECT(mh_chain_get_midi_dropped(ch.c) == 0, "dropped %d",
           mh_chain_get_midi_dropped(ch.c));
}

void test_auto_chunk_input_is_capped_and_counted() {
    // All 5000 events fall in the first chunk; its input holds 4096. It used
    // to push_back past its reserve, allocating on the audio thread.
    Chain ch({1});
    const int n = ch.run_auto(notes(5000), 65536);
    EXPECT(n == kStage, "out %d", n);
    EXPECT(mh_chain_get_midi_dropped(ch.c) == 5000 - kStage, "dropped %d",
           mh_chain_get_midi_dropped(ch.c));
}

void test_dropped_resets_per_call() {
    Chain ch({1});
    ch.run(notes(300), 100);
    ch.run(notes(10), 100);
    EXPECT(mh_chain_get_midi_dropped(ch.c) == 0, "dropped %d",
           mh_chain_get_midi_dropped(ch.c));
}

// A dry/wet blend on a plugin with latency comb-filtered: the dry path was
// not delayed. An impulse through latency 100 at mix 0.5 came out at 0 and
// 100, each 0.5. Now at any mix it is one impulse at 100, value 1.
void test_mix_is_latency_compensated() {
    for (float mix : {0.0f, 0.5f, 1.0f}) {
        Chain ch({1});
        ch.plugins[0].latency = 100;
        mh_chain_set_mix(ch.c, 0, mix);
        std::vector<float> out;
        for (int b = 0; b < 4; ++b) {  // 256 frames: the impulse, then 100 later
            std::memset(ch.in, 0, sizeof ch.in);
            if (b == 0) ch.in[0] = 1.0f;
            ch.run({}, 0);
            out.insert(out.end(), ch.out, ch.out + kBlock);
        }
        for (int n = 0; n < (int)out.size(); ++n) {
            const float want = n == 100 ? 1.0f : 0.0f;
            EXPECT(std::fabs(out[n] - want) < 1e-6f, "mix %.1f: out[%d] = %f",
                   mix, n, out[n]);
        }
    }
}

void test_mix_refused_past_one_second_of_latency() {
    Chain ch({1});
    ch.plugins[0].latency = (int)kRate + 1;
    EXPECT(!mh_chain_set_mix(ch.c, 0, 0.5f), "accepted latency over 1 s");
    EXPECT(mh_chain_set_mix(ch.c, 0, 1.0f), "refused full wet");
}

// Tails in series add: a 2 s delay into a 3 s reverb rings for about 5 s.
void test_tail_is_the_sum() {
    Chain ch({1, 1});
    ch.plugins[0].tail = 2.0;
    ch.plugins[1].tail = 3.0;
    EXPECT(mh_chain_get_tail_seconds(ch.c) == 5.0, "tail %f",
           mh_chain_get_tail_seconds(ch.c));
}

// The header allows a NULL output table; with a mix below 1 the blend read
// through it and crashed.
void test_null_outputs_with_mix() {
    Chain ch({1});
    mh_chain_set_mix(ch.c, 0, 0.5f);
    const float* ins[1] = {ch.in};
    EXPECT(mh_chain_process(ch.c, ins, nullptr, kBlock), "process failed");
}

// A change at or past the block end was never applied.
void test_trailing_automation_is_applied() {
    Chain ch({1});
    const float* ins[1] = {ch.in};
    float* outs[1] = {ch.out};
    MH_ChainParamChange pc{};
    pc.sample_offset = kBlock + 5;
    pc.value = 0.75f;
    mh_chain_process_auto(ch.c, ins, outs, kBlock, nullptr, 0, nullptr, 0,
                          nullptr, &pc, 1);
    EXPECT(ch.plugins[0].last_param == 0.75f, "param %f",
           ch.plugins[0].last_param);
}

} // namespace

int main() {
    test_stage_overflow_is_counted();
    test_stage_drop_counted_without_midi_out();
    test_caller_capacity_overflow_is_counted();
    test_300_events_pass_the_old_256_limit();
    test_auto_chunk_input_is_capped_and_counted();
    test_dropped_resets_per_call();
    test_mix_is_latency_compensated();
    test_mix_refused_past_one_second_of_latency();
    test_tail_is_the_sum();
    test_null_outputs_with_mix();
    test_trailing_automation_is_applied();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("chain midi bounds: all clean\n");
    return 0;
}
