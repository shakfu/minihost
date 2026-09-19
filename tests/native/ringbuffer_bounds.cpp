// Boundary cases for the four ring-buffer constructors.
//
// These are only reachable through the C API (every in-tree caller passes a
// constant), so there is no Python path to drive them; the harness links the
// ring-buffer sources directly. Built and run by tests/test_native.py.
//
// The case that motivated the harness: a capacity above 1<<30 used to round up
// through a signed-overflowing shift sequence to INT_MIN, leaving mask =
// INT_MAX. The corrupt geometry never reached index arithmetic because calloc
// rejected the resulting size, so nothing observable failed -- the invariant
// held by accident. It is now rejected before rounding.

#include <climits>
#include <cstdio>

#include "audio_ringbuffer.h"
#include "midi_ringbuffer.h"
#include "param_ringbuffer.h"
#include "transport_ringbuffer.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const int OVER = (1 << 30) + 1;   // first capacity that used to overflow

// Usable slots, measured through the public API. A ring buffer of capacity C
// holds C-1 items (one slot separates full from empty), so this is the only
// way to observe the rounded capacity without adding an accessor for a test.
int usable_slots(MH_MidiRingBuffer* rb) {
    MH_MidiEvent e{};
    int n = 0;
    while (mh_midi_ringbuffer_push(rb, &e)) ++n;
    return n;
}

void test_midi() {
    // Non-positive falls back to the documented default of 256.
    MH_MidiRingBuffer* rb = mh_midi_ringbuffer_create(0);
    check(rb != nullptr, "midi create(0) should use the default capacity");
    check(rb && usable_slots(rb) == 255, "midi create(0) gives the 256 default");
    mh_midi_ringbuffer_free(rb);

    rb = mh_midi_ringbuffer_create(-1);
    check(rb != nullptr, "midi create(-1) should use the default capacity");
    mh_midi_ringbuffer_free(rb);

    // Rounding up to a power of two.
    rb = mh_midi_ringbuffer_create(300);
    check(rb != nullptr, "midi create(300)");
    check(rb && usable_slots(rb) == 511, "midi create(300) rounds up to 512");
    mh_midi_ringbuffer_free(rb);

    rb = mh_midi_ringbuffer_create(512);
    check(rb && usable_slots(rb) == 511, "midi create(512) stays at 512");
    mh_midi_ringbuffer_free(rb);

    // Above the largest representable power of two: rejected, not rounded.
    check(mh_midi_ringbuffer_create(OVER) == nullptr, "midi create(1<<30 + 1) rejected");
    check(mh_midi_ringbuffer_create(INT_MAX) == nullptr, "midi create(INT_MAX) rejected");

    // The largest valid request may fail on allocation -- 1<<30 events is
    // 8 GB -- but if it succeeds the buffer must work.
    rb = mh_midi_ringbuffer_create(1 << 30);
    if (rb) {
        MH_MidiEvent in{}, out{};
        in.sample_offset = 42;
        check(mh_midi_ringbuffer_push(rb, &in) == 1, "midi create(1<<30) push");
        check(mh_midi_ringbuffer_pop(rb, &out) == 1, "midi create(1<<30) pop");
        check(out.sample_offset == 42, "midi create(1<<30) round-trip");
        mh_midi_ringbuffer_free(rb);
    }
}

void test_param() {
    MH_ParamRingBuffer* rb = mh_param_ringbuffer_create(0);
    check(rb != nullptr, "param create(0) should use the default capacity");
    mh_param_ringbuffer_free(rb);

    check(mh_param_ringbuffer_create(OVER) == nullptr, "param create(1<<30 + 1) rejected");
    check(mh_param_ringbuffer_create(INT_MAX) == nullptr, "param create(INT_MAX) rejected");
}

void test_transport() {
    MH_TransportRingBuffer* rb = mh_transport_ringbuffer_create(0);
    check(rb != nullptr, "transport create(0) should use the default capacity");
    mh_transport_ringbuffer_free(rb);

    check(mh_transport_ringbuffer_create(OVER) == nullptr,
          "transport create(1<<30 + 1) rejected");
    check(mh_transport_ringbuffer_create(INT_MAX) == nullptr,
          "transport create(INT_MAX) rejected");
}

void test_audio() {
    // Audio has no default: non-positive is an error on either argument.
    check(mh_audio_ringbuffer_create(0, 256) == nullptr, "audio create(0 channels) rejected");
    check(mh_audio_ringbuffer_create(-1, 256) == nullptr, "audio create(-1 channels) rejected");
    check(mh_audio_ringbuffer_create(2, 0) == nullptr, "audio create(0 frames) rejected");
    check(mh_audio_ringbuffer_create(2, -1) == nullptr, "audio create(-1 frames) rejected");

    MH_AudioRingBuffer* rb = mh_audio_ringbuffer_create(2, 300);
    check(rb != nullptr, "audio create(2, 300)");
    if (rb) {
        check(mh_audio_ringbuffer_channels(rb) == 2, "audio create(2, 300) channels");
        // 300 rounds to 512 frames, so 511 are usable.
        float frame[2] = {1.0f, 2.0f};
        int n = 0;
        while (mh_audio_ringbuffer_push(rb, frame, 1) == 1) ++n;
        check(n == 511, "audio create(2, 300) rounds up to 512 frames");
        mh_audio_ringbuffer_free(rb);
    }

    check(mh_audio_ringbuffer_create(2, OVER) == nullptr, "audio create(1<<30 + 1) rejected");
    check(mh_audio_ringbuffer_create(2, INT_MAX) == nullptr, "audio create(INT_MAX) rejected");

    // Samples are addressed as frame * channels, so the product is bounded too.
    check(mh_audio_ringbuffer_create(4, 1 << 30) == nullptr,
          "audio create(4, 1<<30) rejected: frame * channels overflows int");
}

} // namespace

int main() {
    test_midi();
    test_param();
    test_transport();
    test_audio();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ring-buffer bounds: all clean\n");
    return 0;
}
