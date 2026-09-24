// ThreadSanitizer stress harness for minihost's lock-free SPSC ring buffers.
//
// The MIDI, parameter and audio ring buffers (midi_ringbuffer.cpp,
// param_ringbuffer.cpp / audio_ringbuffer.cpp)
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
// Scope:        the ring buffers and transport snapshots. The input-callback
//               handshake in minihost_audio.c needs a live audio device and
//               is not reachable headlessly.
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
#include "param_ringbuffer.h"
#include "transport_ringbuffer.h"
#include "transport_seqlock.h"

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

// --- parameter ring buffer: the drain coalesces, so exactly-once delivery is
// the wrong guarantee to assert. What must hold instead, and what a wrong
// memory order would break, is that the consumer never sees a value that was
// not pushed, never sees a torn (plugin_index, param_index, value) triple, and
// ends up holding the producer's *last* value for every parameter once the
// producer has finished.
//
// The value encodes its own sequence number so a torn write is detectable, and
// the parameters are spread over more slots than the drain array holds so the
// overflow branch is exercised too.
long stress_param(long N) {
    const int PARAMS = 96;      // > MAX_OUT below, so drains overflow
    const int MAX_OUT = 64;     // mirrors MH_AUDIO_MAX_PARAM_CHANGES

    MH_ParamRingBuffer* rb = mh_param_ringbuffer_create(1024);
    if (!rb) {
        std::fprintf(stderr, "FAIL: param ringbuffer create\n");
        return 1;
    }
    long fails = 0;

    // The last value the producer pushed for each parameter, so the consumer's
    // final state can be checked against it.
    float last_pushed[PARAMS];
    for (int i = 0; i < PARAMS; ++i) last_pushed[i] = -1.0f;

    // What the consumer has most recently applied for each parameter.
    float applied[PARAMS];
    for (int i = 0; i < PARAMS; ++i) applied[i] = -1.0f;

    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (long i = 0; i < N; ++i) {
            int param = static_cast<int>(i % PARAMS);
            // A value that is a function of the sequence number, so the
            // consumer can tell a real value from a torn or invented one.
            float value = static_cast<float>(i & 0xFFFF) / 65535.0f;
            last_pushed[param] = value;
            while (!mh_param_ringbuffer_push(rb, 0, param, value)) {
                std::this_thread::yield();  // buffer full: spin
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        MH_ChainParamChange out[MAX_OUT];
        for (;;) {
            bool was_done = done.load(std::memory_order_acquire);

            int count = 0;
            mh_param_ringbuffer_drain(rb, out, MAX_OUT, &count);

            for (int i = 0; i < count; ++i) {
                if (out[i].plugin_index != 0 ||
                    out[i].param_index < 0 || out[i].param_index >= PARAMS ||
                    out[i].sample_offset != 0 ||
                    out[i].value < 0.0f || out[i].value > 1.0f) {
                    if (fails < 10) {
                        std::fprintf(stderr,
                            "FAIL: param drain torn: slot=%d param=%d offset=%d value=%f\n",
                            out[i].plugin_index, out[i].param_index,
                            out[i].sample_offset, (double)out[i].value);
                    }
                    ++fails;
                    continue;
                }
                applied[out[i].param_index] = out[i].value;

                // Coalescing invariant: a drain never reports one parameter
                // twice, so every index in `out` must be distinct.
                for (int j = 0; j < i; ++j) {
                    if (out[j].param_index == out[i].param_index &&
                        out[j].plugin_index == out[i].plugin_index) {
                        if (fails < 10) {
                            std::fprintf(stderr,
                                "FAIL: param %d appears twice in one drain\n",
                                out[i].param_index);
                        }
                        ++fails;
                    }
                }
            }

            // Drain once more after seeing `done` so nothing is left behind.
            if (was_done && mh_param_ringbuffer_is_empty(rb)) break;
            if (count == 0) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();

    // Every parameter must have converged on the producer's final value: the
    // whole point of coalescing is that intermediate values may be dropped and
    // the last one may not.
    for (int i = 0; i < PARAMS; ++i) {
        if (last_pushed[i] != applied[i]) {
            if (fails < 10) {
                std::fprintf(stderr,
                    "FAIL: param %d settled at %f, producer last pushed %f\n",
                    i, (double)applied[i], (double)last_pushed[i]);
            }
            ++fails;
        }
    }

    mh_param_ringbuffer_free(rb);
    return fails;
}

// --- transport command ring: ordinary SPSC, so the guarantee is the strict
// one -- every command delivered exactly once, in order, with intact fields.
// Unlike parameters, transport commands do not supersede each other: dropping
// a PLAY because a later STOP arrived would leave the playhead wrong.
long stress_transport(long N) {
    MH_TransportRingBuffer* rb = mh_transport_ringbuffer_create(256);
    if (!rb) {
        std::fprintf(stderr, "FAIL: transport ringbuffer create\n");
        return 1;
    }
    long fails = 0;

    std::thread producer([&] {
        for (long i = 0; i < N; ++i) {
            MH_TransportCommand cmd;
            cmd.type = (int) (i % 7);
            cmd.dvalue = (double) i;
            cmd.lvalue = i;
            cmd.lvalue2 = -i;
            cmd.ivalue = (int) (i & 0xFFFF);
            cmd.ivalue2 = (int) ((i >> 4) & 0xFFFF);
            while (!mh_transport_ringbuffer_push(rb, &cmd)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        MH_TransportCommand out[64];
        long expect = 0;
        while (expect < N) {
            int n = mh_transport_ringbuffer_pop_all(rb, out, 64);
            for (int i = 0; i < n; ++i) {
                const MH_TransportCommand& c = out[i];
                if (c.type != (int) (expect % 7) ||
                    c.dvalue != (double) expect ||
                    c.lvalue != expect ||
                    c.lvalue2 != -expect ||
                    c.ivalue != (int) (expect & 0xFFFF) ||
                    c.ivalue2 != (int) ((expect >> 4) & 0xFFFF)) {
                    if (fails < 10) {
                        std::fprintf(stderr,
                            "FAIL: transport seq %ld got type=%d lvalue=%lld\n",
                            expect, c.type, (long long) c.lvalue);
                    }
                    ++fails;
                }
                ++expect;
            }
            if (n == 0) std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    mh_transport_ringbuffer_free(rb);
    return fails;
}

// --- transport seqlock: not a ring buffer. One writer publishes a whole
// TransportState; readers (the audio thread calls this from getPosition())
// must never observe a snapshot mixed from two writes. Every field is derived
// from the same seed, so any mixed read is detectable.
minihost::TransportState encode_transport(long seq) {
    minihost::TransportState s;
    s.hasTransport = true;
    s.bpm = (double) seq;
    s.timeSigNum = (int) (seq & 0xFFFF);
    s.timeSigDenom = (int) ((seq >> 4) & 0xFFFF);
    s.positionSamples = (int64_t) seq;
    s.positionBeats = (double) -seq;
    s.isPlaying = (seq & 1) != 0;
    s.isRecording = (seq & 2) != 0;
    s.isLooping = (seq & 4) != 0;
    s.loopStartSamples = (int64_t) (seq * 2);
    s.loopEndSamples = (int64_t) (seq * 3);
    return s;
}

bool transport_state_consistent(const minihost::TransportState& s) {
    const long seq = (long) s.positionSamples;
    const minihost::TransportState want = encode_transport(seq);
    return s.hasTransport == want.hasTransport &&
           s.bpm == want.bpm &&
           s.timeSigNum == want.timeSigNum &&
           s.timeSigDenom == want.timeSigDenom &&
           s.positionBeats == want.positionBeats &&
           s.isPlaying == want.isPlaying &&
           s.isRecording == want.isRecording &&
           s.isLooping == want.isLooping &&
           s.loopStartSamples == want.loopStartSamples &&
           s.loopEndSamples == want.loopEndSamples;
}

long stress_seqlock(long N) {
    minihost::TransportSeqlock lock;
    lock.write(encode_transport(0));

    std::atomic<bool> done{false};
    std::atomic<long> fails{0};

    std::thread writer([&] {
        for (long i = 1; i <= N; ++i) {
            lock.write(encode_transport(i));
        }
        done.store(true, std::memory_order_release);
    });

    // Two readers, because the seqlock permits any number and a second one
    // widens the window in which a mid-write snapshot could escape.
    auto reader = [&] {
        long local = 0;
        long last = -1;
        while (!done.load(std::memory_order_acquire)) {
            const minihost::TransportState s = lock.read();
            if (!transport_state_consistent(s)) {
                if (local < 10) {
                    std::fprintf(stderr,
                        "FAIL: seqlock torn snapshot at positionSamples=%lld\n",
                        (long long) s.positionSamples);
                }
                ++local;
            }
            // The writer only moves forward, so a reader must too.
            if ((long) s.positionSamples < last) {
                if (local < 10) {
                    std::fprintf(stderr,
                        "FAIL: seqlock went backwards %ld -> %lld\n",
                        last, (long long) s.positionSamples);
                }
                ++local;
            }
            last = (long) s.positionSamples;
        }
        fails.fetch_add(local, std::memory_order_relaxed);
    };

    std::thread r1(reader);
    std::thread r2(reader);

    writer.join();
    r1.join();
    r2.join();

    const minihost::TransportState final_state = lock.read();
    if (final_state.positionSamples != (int64_t) N ||
        !transport_state_consistent(final_state)) {
        std::fprintf(stderr, "FAIL: seqlock final snapshot wrong\n");
        fails.fetch_add(1, std::memory_order_relaxed);
    }

    return fails.load(std::memory_order_relaxed);
}

// --- transport snapshot: the C API minihost_audio.c publishes the playhead
// through. The audio thread writes one per block; any thread reads it back.
MH_TransportInfo encode_transport_info(long seq) {
    const minihost::TransportState s = encode_transport(seq);
    MH_TransportInfo t;
    t.bpm = s.bpm;
    t.time_sig_numerator = s.timeSigNum;
    t.time_sig_denominator = s.timeSigDenom;
    t.position_samples = s.positionSamples;
    t.position_beats = s.positionBeats;
    t.is_playing = s.isPlaying;
    t.is_recording = s.isRecording;
    t.is_looping = s.isLooping;
    t.loop_start_samples = s.loopStartSamples;
    t.loop_end_samples = s.loopEndSamples;
    return t;
}

bool transport_info_consistent(const MH_TransportInfo& t) {
    const MH_TransportInfo want = encode_transport_info((long) t.position_samples);
    // Field by field: memcmp would also compare the struct's padding.
    return t.bpm == want.bpm &&
           t.time_sig_numerator == want.time_sig_numerator &&
           t.time_sig_denominator == want.time_sig_denominator &&
           t.position_beats == want.position_beats &&
           t.is_playing == want.is_playing &&
           t.is_recording == want.is_recording &&
           t.is_looping == want.is_looping &&
           t.loop_start_samples == want.loop_start_samples &&
           t.loop_end_samples == want.loop_end_samples;
}

long stress_transport_snapshot(long N) {
    MH_TransportSnapshot* snap = mh_transport_snapshot_create();
    long fails = 0;
    MH_TransportInfo out;
    if (mh_transport_snapshot_read(snap, &out)) {
        std::fprintf(stderr, "FAIL: snapshot readable before first write\n");
        ++fails;
    }

    std::atomic<bool> done{false};
    std::atomic<long> reader_fails{0};

    std::thread writer([&] {
        for (long i = 0; i <= N; ++i) {
            const MH_TransportInfo t = encode_transport_info(i);
            mh_transport_snapshot_write(snap, &t);
        }
        done.store(true, std::memory_order_release);
    });

    auto reader = [&] {
        long local = 0;
        long long last = -1;
        while (!done.load(std::memory_order_acquire)) {
            MH_TransportInfo t;
            if (!mh_transport_snapshot_read(snap, &t)) continue;
            if (!transport_info_consistent(t) || t.position_samples < last) {
                if (local < 10) {
                    std::fprintf(stderr,
                        "FAIL: snapshot torn or backwards at %lld\n",
                        t.position_samples);
                }
                ++local;
            }
            last = t.position_samples;
        }
        reader_fails.fetch_add(local, std::memory_order_relaxed);
    };

    std::thread r1(reader);
    std::thread r2(reader);
    writer.join();
    r1.join();
    r2.join();

    if (!mh_transport_snapshot_read(snap, &out) ||
        out.position_samples != (long long) N ||
        !transport_info_consistent(out)) {
        std::fprintf(stderr, "FAIL: snapshot final value wrong\n");
        ++fails;
    }
    mh_transport_snapshot_free(snap);
    return fails + reader_fails.load(std::memory_order_relaxed);
}

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

    std::printf("  param (drain)...."); std::fflush(stdout);
    f = stress_param(N);           fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    std::printf("  transport........"); std::fflush(stdout);
    f = stress_transport(N);       fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    std::printf("  audio............"); std::fflush(stdout);
    f = stress_audio(N);           fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    std::printf("  seqlock.........."); std::fflush(stdout);
    f = stress_seqlock(N);         fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    std::printf("  snapshot........."); std::fflush(stdout);
    f = stress_transport_snapshot(N); fails += f;
    std::printf(" %s\n", f ? "FAIL" : "ok");

    if (fails) {
        std::fprintf(stderr, "\n%ld functional failure(s)\n", fails);
        return 1;
    }
    std::printf("all clean (no data races, SPSC correctness held)\n");
    return 0;
}
