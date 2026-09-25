// MIDI buffer bounds and dropped-event accounting in the plugin graph.
//
// mh_graph_get_midi_output_events once copied the full upstream count out of
// a 1024-slot buffer. Python returned the heap contents past the end, and the
// pytest suite passed over it. Here the graph is built under ASan, so any
// read or write past a MIDI buffer aborts the run.
//
// minihost_graph_v2.cpp calls seven mh_* plugin functions. They are stubbed
// below with an instrument that echoes its MIDI input, stopping at the output
// capacity as mh_process_midi_io does. No JUCE. Built and run by
// `make native-tests`.

#include <cstdio>
#include <cstring>
#include <vector>

#include "minihost_graph_v2.h"

// ---------------------------------------------------------------- stubs --

struct MH_Plugin {
    int out_ch = 1;
    int dropped = 0;  // output events past capacity, last call
};

namespace {
constexpr double kRate  = 48000.0;
constexpr int    kBlock = 64;
constexpr int    kCap   = MH_GRAPH_MIDI_OUTPUT_CAPACITY;

void silence(MH_Plugin* p, float* const* outputs, int nframes) {
    for (int c = 0; c < p->out_ch; ++c)
        std::memset(outputs[c], 0, (size_t)nframes * sizeof(float));
}
} // namespace

extern "C" {

int mh_get_info(MH_Plugin* p, MH_Info* info) {
    std::memset(info, 0, sizeof(*info));
    info->num_output_ch = p->out_ch;
    info->accepts_midi  = 1;
    info->produces_midi = 1;
    return 1;
}

double mh_get_sample_rate(MH_Plugin*) { return kRate; }
int mh_get_max_block_size(MH_Plugin*) { return kBlock; }

int mh_process(MH_Plugin* p, const float* const*, float* const* outputs,
               int nframes) {
    silence(p, outputs, nframes);
    return 1;
}

int mh_process_midi(MH_Plugin* p, const float* const* in,
                    float* const* outputs, int nframes, const MH_MidiEvent*,
                    int) {
    return mh_process(p, in, outputs, nframes);
}

int mh_process_midi_io(MH_Plugin* p, const float* const*,
                       float* const* outputs, int nframes,
                       const MH_MidiEvent* midi_in, int num_midi_in,
                       MH_MidiEvent* midi_out, int midi_out_capacity,
                       int* num_midi_out) {
    silence(p, outputs, nframes);
    int n = 0;
    for (; n < num_midi_in && n < midi_out_capacity; ++n)
        midi_out[n] = midi_in[n];
    if (num_midi_out) *num_midi_out = n;
    p->dropped = midi_out ? num_midi_in - n : 0;
    return 1;
}

int mh_get_midi_out_dropped(MH_Plugin* p) { return p ? p->dropped : 0; }

int mh_process_auto(MH_Plugin* p, const float* const* in,
                    float* const* outputs, int nframes,
                    const MH_MidiEvent* midi_in, int num_midi_in,
                    MH_MidiEvent* midi_out, int midi_out_capacity,
                    int* num_midi_out, const MH_ParamChange*, int) {
    return mh_process_midi_io(p, in, outputs, nframes, midi_in, num_midi_in,
                              midi_out, midi_out_capacity, num_midi_out);
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

std::vector<MH_MidiEvent> notes(unsigned char note, int n) {
    return std::vector<MH_MidiEvent>((size_t)n,
                                     MH_MidiEvent{0, 0x90, note, 100});
}

MH_MidiProcessorParams op(MH_MidiOp o) {
    MH_MidiProcessorParams p{};
    p.op = o;
    p.max_note = 127;
    p.channel_mask = 0xFFFF;
    p.velocity_gamma = 1.0f;
    return p;
}

// A graph with one audio input -> output path, which compile requires.
struct Graph {
    MH_PluginGraph* g = nullptr;
    float buf_in[kBlock] = {};
    float buf_out[kBlock] = {};

    Graph() {
        g = mh_graph_create(kBlock, kRate, nullptr, 0);
        const MH_NodeId a = mh_graph_add_input(g, 1, nullptr, 0);
        const MH_NodeId b = mh_graph_add_output(g, 1, nullptr, 0);
        mh_graph_connect(g, a, 0, b, 0, nullptr, 0);
    }
    ~Graph() { mh_graph_close(g); }

    bool compile() {
        char err[256] = {};
        if (mh_graph_compile(g, err, sizeof err)) return true;
        std::fprintf(stderr, "compile: %s\n", err);
        return false;
    }

    bool render() {
        const float* in_ch[1] = {buf_in};
        float* out_ch[1] = {buf_out};
        const float* const* ins[1] = {in_ch};
        float* const* outs[1] = {out_ch};
        return mh_graph_render_block(g, ins, 1, outs, 1, kBlock) != 0;
    }

    int count(MH_NodeId mo) {
        int n = -1;
        mh_graph_get_midi_output_events(g, mo, nullptr, 0, &n);
        return n;
    }

    int dropped(MH_NodeId mo) {
        int d = -1;
        mh_graph_get_midi_output_dropped(g, mo, &d);
        return d;
    }

    // Drains into a heap buffer of exactly `capacity`, so ASan sees any
    // write past it.
    std::vector<MH_MidiEvent> drain(MH_NodeId mo, int capacity) {
        std::vector<MH_MidiEvent> out((size_t)capacity);
        int n = 0;
        mh_graph_get_midi_output_events(g, mo, out.data(), capacity, &n);
        out.resize((size_t)(n < capacity ? n : capacity));
        return out;
    }
};

bool all_note(const std::vector<MH_MidiEvent>& evs, unsigned char note) {
    for (const auto& e : evs)
        if (e.status != 0x90 || e.data1 != note || e.data2 != 100) return false;
    return true;
}

void test_direct_overflow() {
    Graph t;
    const MH_NodeId mi = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi(t.g, mi, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto in = notes(60, 3000);
    mh_graph_set_midi_input_events(t.g, mi, in.data(), (int)in.size());
    EXPECT(t.render(), "render failed");
    EXPECT(t.count(mo) == kCap, "count %d, want %d", t.count(mo), kCap);
    EXPECT(t.dropped(mo) == 3000 - kCap, "dropped %d", t.dropped(mo));
    const auto got = t.drain(mo, 4096);
    EXPECT((int)got.size() == kCap && all_note(got, 60), "drained %zu",
           got.size());
}

void test_small_caller_capacity() {
    Graph t;
    const MH_NodeId mi = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi(t.g, mi, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto in = notes(60, 50);
    mh_graph_set_midi_input_events(t.g, mi, in.data(), (int)in.size());
    EXPECT(t.render(), "render failed");
    EXPECT(t.drain(mo, 10).size() == 10, "short drain");
    EXPECT(t.dropped(mo) == 0, "dropped %d", t.dropped(mo));
}

void test_processor_overflow() {
    Graph t;
    const MH_NodeId mi = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId pr =
        mh_graph_add_midi_processor(t.g, op(MH_MIDI_OP_TRANSPOSE), nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi(t.g, mi, pr, nullptr, 0);
    mh_graph_connect_midi(t.g, pr, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto in = notes(60, 2000);
    mh_graph_set_midi_input_events(t.g, mi, in.data(), (int)in.size());
    EXPECT(t.render(), "render failed");
    EXPECT(t.count(mo) == kCap, "count %d", t.count(mo));
    EXPECT(t.dropped(mo) == 2000 - kCap, "dropped %d", t.dropped(mo));
    EXPECT(t.drain(mo, kCap).size() == (size_t)kCap, "short drain");
}

// Merge overflow, then a filter shrinks the stream below the limit. The
// count must be what the node holds, or a caller sized by it reads garbage.
void test_merge_overflow_then_filter() {
    Graph t;
    const MH_NodeId a = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId b = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId mg = mh_graph_add_midi_merge(t.g, 2, nullptr, 0);
    MH_MidiProcessorParams f = op(MH_MIDI_OP_FILTER);
    f.min_note = f.max_note = 60;
    const MH_NodeId pr = mh_graph_add_midi_processor(t.g, f, nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi_port(t.g, a, mg, 0, nullptr, 0);
    mh_graph_connect_midi_port(t.g, b, mg, 1, nullptr, 0);
    mh_graph_connect_midi(t.g, mg, pr, nullptr, 0);
    mh_graph_connect_midi(t.g, pr, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto ea = notes(60, 600);
    const auto eb = notes(30, 600);
    mh_graph_set_midi_input_events(t.g, a, ea.data(), (int)ea.size());
    mh_graph_set_midi_input_events(t.g, b, eb.data(), (int)eb.size());
    EXPECT(t.render(), "render failed");
    EXPECT(t.count(mo) == 600, "count %d, want 600", t.count(mo));
    EXPECT(t.dropped(mo) == 1200 - kCap, "dropped %d", t.dropped(mo));
    const auto got = t.drain(mo, 4096);
    EXPECT(got.size() == 600 && all_note(got, 60), "drained %zu", got.size());
}

// One overflowing processor feeding both merge ports: each port's stream
// lost its events, so the drops count once per path.
void test_fan_out_counts_per_path() {
    Graph t;
    const MH_NodeId mi = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId pr =
        mh_graph_add_midi_processor(t.g, op(MH_MIDI_OP_TRANSPOSE), nullptr, 0);
    const MH_NodeId mg = mh_graph_add_midi_merge(t.g, 2, nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi(t.g, mi, pr, nullptr, 0);
    mh_graph_connect_midi_port(t.g, pr, mg, 0, nullptr, 0);
    mh_graph_connect_midi_port(t.g, pr, mg, 1, nullptr, 0);
    mh_graph_connect_midi(t.g, mg, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto in = notes(60, 2000);
    mh_graph_set_midi_input_events(t.g, mi, in.data(), (int)in.size());
    EXPECT(t.render(), "render failed");
    // 976 lost at the processor, twice; then 2048 -> 1024 at the merge.
    const int want = 2 * (2000 - kCap) + kCap;
    EXPECT(t.dropped(mo) == want, "dropped %d, want %d", t.dropped(mo), want);
    EXPECT(t.count(mo) == kCap, "count %d", t.count(mo));
}

MH_NodeId add_echo_plugin(Graph& t, MH_Plugin* p) {
    const MH_NodeId pl = mh_graph_add_plugin(t.g, p, nullptr, 0);
    const MH_NodeId po = mh_graph_add_output(t.g, 1, nullptr, 0);
    mh_graph_connect(t.g, pl, 0, po, 0, nullptr, 0);
    return pl;
}

bool render_two_outputs(Graph& t) {
    float extra[kBlock] = {};
    const float* in_ch[1] = {t.buf_in};
    float* out_a[1] = {t.buf_out};
    float* out_b[1] = {extra};
    const float* const* ins[1] = {in_ch};
    float* const* outs[2] = {out_a, out_b};
    return mh_graph_render_block(t.g, ins, 1, outs, 2, kBlock) != 0;
}

void test_drop_upstream_of_plugin_is_reported() {
    MH_Plugin p;
    Graph t;
    const MH_NodeId pl = add_echo_plugin(t, &p);
    const MH_NodeId mi = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId pr =
        mh_graph_add_midi_processor(t.g, op(MH_MIDI_OP_TRANSPOSE), nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi(t.g, mi, pr, nullptr, 0);
    mh_graph_connect_midi(t.g, pr, pl, nullptr, 0);
    mh_graph_connect_midi(t.g, pl, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto in = notes(60, 2000);
    mh_graph_set_midi_input_events(t.g, mi, in.data(), (int)in.size());
    EXPECT(render_two_outputs(t), "render failed");
    EXPECT(t.count(mo) == kCap, "count %d", t.count(mo));
    EXPECT(t.dropped(mo) == 2000 - kCap, "dropped %d", t.dropped(mo));
}

// A plugin fed more than the limit directly stops at its capture buffer;
// the buffer is not overrun, and the loss is counted.
void test_plugin_output_overflow_is_bounded_and_counted() {
    MH_Plugin p;
    Graph t;
    const MH_NodeId pl = add_echo_plugin(t, &p);
    const MH_NodeId mi = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi(t.g, mi, pl, nullptr, 0);
    mh_graph_connect_midi(t.g, pl, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto in = notes(60, 1500);
    mh_graph_set_midi_input_events(t.g, mi, in.data(), (int)in.size());
    EXPECT(render_two_outputs(t), "render failed");
    EXPECT(t.count(mo) == kCap, "count %d", t.count(mo));
    EXPECT(t.dropped(mo) == 1500 - kCap, "dropped %d", t.dropped(mo));
    EXPECT(t.drain(mo, 4096).size() == (size_t)kCap, "short drain");
}

// Counts reset each block rather than accumulating.
void test_dropped_resets_per_block() {
    Graph t;
    const MH_NodeId mi = mh_graph_add_midi_input(t.g, nullptr, 0);
    const MH_NodeId mo = mh_graph_add_midi_output(t.g, nullptr, 0);
    mh_graph_connect_midi(t.g, mi, mo, nullptr, 0);
    if (!t.compile()) { ++failures; return; }

    const auto in = notes(60, 2000);
    mh_graph_set_midi_input_events(t.g, mi, in.data(), (int)in.size());
    EXPECT(t.render(), "render failed");
    EXPECT(t.dropped(mo) == 2000 - kCap, "dropped %d", t.dropped(mo));
    EXPECT(t.render(), "render failed");
    EXPECT(t.dropped(mo) == 0 && t.count(mo) == 0, "dropped %d count %d",
           t.dropped(mo), t.count(mo));
}

// C callers zero-initialise params and set only what their op uses, so
// only those fields are checked. An unknown op made the node drop everything.
void test_processor_params_are_checked() {
    Graph t;
    MH_MidiProcessorParams z{};
    z.op = MH_MIDI_OP_TRANSPOSE;  // other fields left zero
    const MH_NodeId pr = mh_graph_add_midi_processor(t.g, z, nullptr, 0);
    EXPECT(pr >= 0, "zero-initialised transpose rejected");

    MH_MidiProcessorParams bad = z;
    // What a C caller can store; a C++ cast to the enum would itself be UB.
    const int seven = 7;
    std::memcpy(&bad.op, &seven, sizeof bad.op);
    char err[128] = {};
    EXPECT(mh_graph_add_midi_processor(t.g, bad, err, sizeof err) < 0,
           "unknown op accepted");
    EXPECT(!mh_graph_set_midi_processor_params(t.g, pr, bad),
           "unknown op accepted by set");
    bad = z;
    bad.op = MH_MIDI_OP_VELOCITY_CURVE;  // gamma left zero
    EXPECT(mh_graph_add_midi_processor(t.g, bad, err, sizeof err) < 0,
           "velocity curve with gamma 0 accepted");
}

} // namespace

int main() {
    test_direct_overflow();
    test_small_caller_capacity();
    test_processor_overflow();
    test_merge_overflow_then_filter();
    test_fan_out_counts_per_path();
    test_drop_upstream_of_plugin_is_reported();
    test_plugin_output_overflow_is_bounded_and_counted();
    test_dropped_resets_per_block();
    test_processor_params_are_checked();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("graph midi bounds: all clean\n");
    return 0;
}
