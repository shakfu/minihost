// ThreadSanitizer stress harness for minihost's lock-free SPSC ring buffers.
//
// The MIDI and audio ring buffers (midi_ringbuffer.cpp / audio_ringbuffer.cpp)
// are the project's only hand-rolled lock-free code: a single producer thread
// and a single consumer thread coordinate through release/acquire atomics with
// no lock. That is exactly the kind of code where a missing fence is invisible
// on x86 (strong ordering) and in ordinary tests, yet corrupts data on a
// weakly-ordered CPU (the Apple-silicon / ARM Linux wheels minihost ships).
//
// This harness drives each buffer from two real threads under TSan, which flags
// any unsynchronized access, and additionally asserts SPSC *correctness* every
// item is delivered exactly once, in order, with no field tearing so a wrong
// memory-order (a race TSan might not classify, but that still reorders data)
// is caught as a functional failure too.
//
// Build + run:  make tsan        (see tests/tsan/README.md)
// Scope:        the ring buffers only. The audio-callback input_callback path
//               needs a live audio device and is not reachable headlessly; its
//               fix is a single atomic pointer validated by inspection.
//
// Exit code: non-zero on a functional failure. Run with
//   TSAN_OPTIONS=halt_on_error=1 to also abort on the first data race.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "audio_ringbuffer.h"
#include "midi_ringbuffer.h"

namespace {

long stress_count() {
    const char* env = std::getenv("TSAN_STRESS_N");
    long n = env ? std::atol(env) : 200000;
    return n > 0 ? n : 200000;
}

// Encode a sequence number into every field of a MIDI event so a torn write
// (one field from a different push) is detectable on the consumer side.
MH_MidiEvent encode_midi(long seq) {
    MH_MidiEvent e;
    e.sample_offset = static_cast<int>(seq);
    e.status = static_cast<unsigned char>(0x80 | (seq & 0x0F));
    e.data1 = static_cast<unsigned char>(seq & 0x7F);
    e.data2 = static_cast<unsigned char>((seq >> 7) & 0x7F);
    return e;
}

bool midi_matches(const MH_MidiEvent& e, long seq) {
    return e.sample_offset == static_cast<int>(seq) &&
           e.status == static_cast<unsigned char>(0x80 | (seq & 0x0F)) &&
           e.data1 == static_cast<unsigned char>(seq & 0x7F) &&
           e.data2 == static_cast<unsigned char>((seq >> 7) & 0x7F);
}

// --- MIDI ring buffer: producer pushes 0..N-1; consumer pops one at a time
// and verifies strict in-order, exactly-once delivery with intact fields.
long stress_midi_pop(long N) {
    MH_MidiRingBuffer* rb = mh_midi_ringbuffer_create(1024);
    if (!rb) {
        std::fprintf(stderr, "FAIL: midi ringbuffer create\n");
        return 1;
    }
    long fails = 0;

    std::thread producer([&] {
        for (long i = 0; i < N; ++i) {
            MH_MidiEvent e = encode_midi(i);
            while (!mh_midi_ringbuffer_push(rb, &e)) {
                std::this_thread::yield();  // buffer full: spin
            }
        }
    });

    std::thread consumer([&] {
        long expect = 0;
        MH_MidiEvent e;
        while (expect < N) {
            if (mh_midi_ringbuffer_pop(rb, &e)) {
                if (!midi_matches(e, expect)) {
                    if (fails < 10) {
                        std::fprintf(stderr,
                            "FAIL: midi pop seq %ld got offset=%d status=%u\n",
                            expect, e.sample_offset, e.status);
                    }
                    ++fails;
                }
                ++expect;
            } else {
                std::this_thread::yield();  // empty: spin
            }
        }
    });

    producer.join();
    consumer.join();
    mh_midi_ringbuffer_free(rb);
    return fails;
}

// --- MIDI ring buffer via pop_all(): same guarantee, exercises the batch
// drain path the audio callback actually uses.
long stress_midi_pop_all(long N) {
    MH_MidiRingBuffer* rb = mh_midi_ringbuffer_create(1024);
    if (!rb) {
        std::fprintf(stderr, "FAIL: midi ringbuffer create (pop_all)\n");
        return 1;
    }
    long fails = 0;

    std::thread producer([&] {
        for (long i = 0; i < N; ++i) {
            MH_MidiEvent e = encode_midi(i);
            while (!mh_midi_ringbuffer_push(rb, &e)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        long expect = 0;
        MH_MidiEvent batch[64];
        while (expect < N) {
            int n = mh_midi_ringbuffer_pop_all(rb, batch, 64);
            for (int k = 0; k < n; ++k) {
                if (!midi_matches(batch[k], expect)) {
                    ++fails;
                }
                ++expect;
            }
            if (n == 0) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    mh_midi_ringbuffer_free(rb);
    return fails;
}

// --- Audio ring buffer: producer pushes N single-frame writes carrying a
// strictly increasing value (1..N, same on every channel). Consumer reads in
// chunks and checks (a) channels agree within a frame (no interleave tearing)
// and (b) real-frame values strictly increase (no reorder / duplication).
long stress_audio(long N) {
    const int CH = 2;
    MH_AudioRingBuffer* rb = mh_audio_ringbuffer_create(CH, 2048);
    if (!rb) {
        std::fprintf(stderr, "FAIL: audio ringbuffer create\n");
        return 1;
    }
    long fails = 0;

    std::thread producer([&] {
        for (long i = 1; i <= N; ++i) {
            float frame[CH];
            for (int c = 0; c < CH; ++c) frame[c] = static_cast<float>(i);
            while (mh_audio_ringbuffer_push(rb, frame, 1) == 0) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        float ch0[64], ch1[64];
        float* bufs[CH] = {ch0, ch1};
        long got = 0;
        double last = 0.0;  // values are exact small ints in float range
        while (got < N) {
            int n = mh_audio_ringbuffer_read_into(rb, bufs, 64, CH);
            // The first `n` frames are real data; the rest (if any) is silence.
            for (int f = 0; f < n; ++f) {
                float v = ch0[f];
                if (ch1[f] != v) ++fails;          // interleave tear
                if (!(static_cast<double>(v) > last)) ++fails;  // reorder/dup
                last = v;
                ++got;
            }
            if (n == 0) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    mh_audio_ringbuffer_free(rb);
    return fails;
}

}  // namespace

int main() {
    const long N = stress_count();
    std::printf("TSan ring-buffer stress: N=%ld events/frames per test\n", N);

    long fails = 0;
    std::printf("  midi (pop)......."); std::fflush(stdout);
    long f = stress_midi_pop(N);   fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    std::printf("  midi (pop_all)..."); std::fflush(stdout);
    f = stress_midi_pop_all(N);    fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    std::printf("  audio............"); std::fflush(stdout);
    f = stress_audio(N);           fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    if (fails) {
        std::fprintf(stderr, "\n%ld functional failure(s)\n", fails);
        return 1;
    }
    std::printf("all clean (no data races, SPSC correctness held)\n");
    return 0;
}
