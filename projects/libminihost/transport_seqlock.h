// transport_seqlock.h
// Lock-free transport snapshot shared between a control thread (writer) and
// the audio thread (reader).
//
// Header-only and free of JUCE so the TSan harness (tests/tsan) can link it
// without a plugin host.
//
// A seqlock over a plain struct is a data race under the C++ memory model
// however carefully the sequence counter is ordered: the reader's copy of the
// payload is unsynchronized with the writer's store by construction, and a
// retry loop cannot make it defined. The fields are therefore individual
// atomics.
//
// Every access is seq_cst rather than relaxed-plus-fences. Relaxed payload
// accesses leave the classic seqlock hole -- nothing stops the reader's
// payload loads from being ordered after its second counter load, so a
// mid-write snapshot can escape with both counter reads agreeing -- and the
// standalone fences that close it are not modelled by GCC's ThreadSanitizer,
// which would make the stress test in tests/tsan unable to see the bug it
// exists to catch. Under seq_cst the single total order puts the payload
// loads between the two counter loads, so an escaped snapshot is impossible.
// The reader pays 13 acquiring loads per block, the writer 13 releasing
// stores off the audio thread.

#ifndef MINIHOST_TRANSPORT_SEQLOCK_H
#define MINIHOST_TRANSPORT_SEQLOCK_H

#include <atomic>
#include <cstdint>

namespace minihost {

// Plain-struct view of the transport, used by callers on both sides.
struct TransportState
{
    bool hasTransport = false;
    double bpm = 120.0;
    int timeSigNum = 4;
    int timeSigDenom = 4;
    int64_t positionSamples = 0;
    double positionBeats = 0.0;
    bool isPlaying = false;
    bool isRecording = false;
    bool isLooping = false;
    int64_t loopStartSamples = 0;
    int64_t loopEndSamples = 0;
};

class TransportSeqlock
{
public:
    // Single writer. Concurrent writers are not supported.
    void write(const TransportState& s)
    {
        seq_.fetch_add(1);    // odd  = write in progress
        store(s);
        seq_.fetch_add(1);    // even = write complete
    }

    // Any number of readers, including the audio thread. Never blocks the
    // reader beyond retrying while a write is in flight.
    TransportState read() const
    {
        TransportState s;
        unsigned seq0, seq1;
        do {
            seq0 = seq_.load();
            load(s);
            seq1 = seq_.load();
        } while (seq0 != seq1 || (seq0 & 1));            // retry if torn or mid-write
        return s;
    }

private:
    void store(const TransportState& s)
    {
        hasTransport_.store(s.hasTransport);
        bpm_.store(s.bpm);
        timeSigNum_.store(s.timeSigNum);
        timeSigDenom_.store(s.timeSigDenom);
        positionSamples_.store(s.positionSamples);
        positionBeats_.store(s.positionBeats);
        isPlaying_.store(s.isPlaying);
        isRecording_.store(s.isRecording);
        isLooping_.store(s.isLooping);
        loopStartSamples_.store(s.loopStartSamples);
        loopEndSamples_.store(s.loopEndSamples);
    }

    void load(TransportState& s) const
    {
        s.hasTransport = hasTransport_.load();
        s.bpm = bpm_.load();
        s.timeSigNum = timeSigNum_.load();
        s.timeSigDenom = timeSigDenom_.load();
        s.positionSamples = positionSamples_.load();
        s.positionBeats = positionBeats_.load();
        s.isPlaying = isPlaying_.load();
        s.isRecording = isRecording_.load();
        s.isLooping = isLooping_.load();
        s.loopStartSamples = loopStartSamples_.load();
        s.loopEndSamples = loopEndSamples_.load();
    }

    std::atomic<bool>    hasTransport_{false};
    std::atomic<double>  bpm_{120.0};
    std::atomic<int>     timeSigNum_{4};
    std::atomic<int>     timeSigDenom_{4};
    std::atomic<int64_t> positionSamples_{0};
    std::atomic<double>  positionBeats_{0.0};
    std::atomic<bool>    isPlaying_{false};
    std::atomic<bool>    isRecording_{false};
    std::atomic<bool>    isLooping_{false};
    std::atomic<int64_t> loopStartSamples_{0};
    std::atomic<int64_t> loopEndSamples_{0};
    std::atomic<unsigned> seq_{0};
};

} // namespace minihost

#endif // MINIHOST_TRANSPORT_SEQLOCK_H
