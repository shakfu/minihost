// Placement of timestamped live MIDI: mh_midi_time_to_offset and the
// timestamped ring-buffer calls. Built and run by `make native-tests`.

#include <cstdio>

#include "midi_ringbuffer.h"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void test_offsets() {
    const uint64_t prev = 1000000, now = prev + 10000000;  // 10 ms period
    const int n = 512;

    check(mh_midi_time_to_offset(prev + 5000000, prev, now, n) == 256,
          "mid-period event lands mid-block");
    check(mh_midi_time_to_offset(prev + 2500000, prev, now, n) == 128,
          "quarter-period event lands a quarter in");
    check(mh_midi_time_to_offset(prev + 1, prev, now, n) == 0,
          "event just after the previous callback lands at 0");
    check(mh_midi_time_to_offset(now - 1, prev, now, n) == n - 1,
          "event just before this callback lands at the end");

    check(mh_midi_time_to_offset(prev - 5, prev, now, n) == 0,
          "event older than the period clamps to 0");
    check(mh_midi_time_to_offset(0, prev, now, n) == 0,
          "unstamped event (time 0) lands at 0");
    check(mh_midi_time_to_offset(now + 5, prev, now, n) == n - 1,
          "event after this callback started clamps to the end");
    check(mh_midi_time_to_offset(prev + 5000000, 0, now, n) == 0,
          "first callback (no previous time) places everything at 0");
    check(mh_midi_time_to_offset(prev + 5, now, now, n) == 0,
          "zero-length period does not divide by zero");
    check(mh_midi_time_to_offset(prev + 5000000, prev, now, 1) == 0,
          "one-frame block");

    // Order is preserved: later arrivals never land earlier.
    int last = -1;
    bool monotonic = true;
    for (uint64_t t = prev; t <= now + 100; t += 997) {
        const int off = mh_midi_time_to_offset(t, prev, now, n);
        if (off < last || off < 0 || off >= n) monotonic = false;
        last = off;
    }
    check(monotonic, "offsets are monotonic in arrival time and in range");
}

void test_ring_times() {
    MH_MidiRingBuffer* rb = mh_midi_ringbuffer_create(8);
    MH_MidiEvent e{0, 0x90, 60, 100};
    check(mh_midi_ringbuffer_push_at(rb, &e, 42) == 1, "push_at");
    check(mh_midi_ringbuffer_push(rb, &e) == 1, "push");
    MH_MidiEvent out[4];
    uint64_t times[4] = {7, 7, 7, 7};
    check(mh_midi_ringbuffer_pop_all_at(rb, out, times, 4) == 2, "pop_all_at count");
    check(times[0] == 42 && times[1] == 0, "times travel with their events");
    mh_midi_ringbuffer_free(rb);

    const uint64_t a = mh_midi_clock_ns(), b = mh_midi_clock_ns();
    check(a > 0 && b >= a, "clock is nonzero and monotonic");
}

} // namespace

int main() {
    test_offsets();
    test_ring_times();
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("midi timestamps: all clean\n");
    return 0;
}
