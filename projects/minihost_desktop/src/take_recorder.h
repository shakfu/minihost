// take_recorder.h
//
// Records one take of the live engine's file output nodes.
//
// All outputs share one lock-free FIFO (juce::AbstractFifo), so a block is
// accepted or dropped for every output at once and the files stay aligned.
// A dropped block becomes silence of the same length, written once the FIFO
// has room, so the take keeps the transport's timing.
//
// Threads: write() is the audio thread's, drain() a writer thread's, and
// finish() the message thread's once write() is no longer called. write()
// does no locking, signalling or allocation; the writer polls instead of
// being woken, because waking a thread takes a mutex.

#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <vector>

namespace minihost_desktop {

class TakeRecorder {
public:
    // One writer per output, in output order. fifo_frames is the backlog
    // the writer may fall behind by before blocks drop.
    TakeRecorder(std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers,
                 int fifo_frames);
    ~TakeRecorder();

    // Audio thread. chans[i][c] is output i's channel c, n frames.
    void write(const float* const* const* chans, int n) noexcept;

    // Writer thread: writes every ready frame to disk.
    void drain();

    // Message thread, after the last write(): drains the FIFO, appends the
    // silence still owed for dropped blocks, and closes the files.
    void finish();

    // Transport frames recorded so far, including dropped ones (on disk as
    // silence once finished).
    long long frames() const noexcept { return total_.load(std::memory_order_relaxed); }
    long long droppedFrames() const noexcept
    { return dropped_.load(std::memory_order_relaxed); }
    // Frames accepted but not yet on disk.
    long long backlog() const noexcept
    {
        return placed_.load(std::memory_order_relaxed)
             - written_.load(std::memory_order_relaxed);
    }
    // True once a disk write failed; later audio is discarded.
    bool writeFailed() const noexcept { return failed_.load(std::memory_order_relaxed); }

private:
    // Copies n frames into the FIFO, or zeros when chans is null. The
    // caller has checked the free space.
    void push(const float* const* const* chans, int n) noexcept;
    void writeOut(int start, int n);

    std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers_;
    std::vector<juce::AudioBuffer<float>>                 bufs_;
    juce::AbstractFifo                                    fifo_;
    std::atomic<long long> total_{ 0 };
    std::atomic<long long> dropped_{ 0 };
    std::atomic<long long> placed_{ 0 };
    std::atomic<long long> written_{ 0 };
    std::atomic<long long> owed_{ 0 };   // silence not yet in the FIFO
    std::atomic<bool>      failed_{ false };
    bool                   finished_ = false;
};

} // namespace minihost_desktop
