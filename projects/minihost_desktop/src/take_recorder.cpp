// take_recorder.cpp -- see take_recorder.h.

#include "take_recorder.h"

#include <algorithm>
#include <cstring>

namespace minihost_desktop {

TakeRecorder::TakeRecorder(
    std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers,
    int fifo_frames)
    : writers_(std::move(writers)),
      // AbstractFifo holds one frame less than its size.
      fifo_(std::max(2, fifo_frames + 1))
{
    bufs_.reserve(writers_.size());
    for (const auto& w : writers_)
        bufs_.emplace_back((int) w->getNumChannels(), fifo_.getTotalSize());
}

TakeRecorder::~TakeRecorder()
{
    finish();
}

void TakeRecorder::push(const float* const* const* chans, int n) noexcept
{
    int s1 = 0, z1 = 0, s2 = 0, z2 = 0;
    fifo_.prepareToWrite(n, s1, z1, s2, z2);
    for (size_t i = 0; i < bufs_.size(); ++i)
        for (int c = 0; c < bufs_[i].getNumChannels(); ++c)
        {
            float* dst = bufs_[i].getWritePointer(c);
            if (chans != nullptr)
            {
                const float* src = chans[i][c];
                std::memcpy(dst + s1, src,      (size_t) z1 * sizeof(float));
                std::memcpy(dst + s2, src + z1, (size_t) z2 * sizeof(float));
            }
            else
            {
                std::memset(dst + s1, 0, (size_t) z1 * sizeof(float));
                std::memset(dst + s2, 0, (size_t) z2 * sizeof(float));
            }
        }
    fifo_.finishedWrite(z1 + z2);
    placed_.fetch_add(z1 + z2, std::memory_order_relaxed);
}

void TakeRecorder::write(const float* const* const* chans, int n) noexcept
{
    if (n <= 0) return;
    total_.fetch_add(n, std::memory_order_relaxed);
    // Silence owed for earlier drops goes first, so the order holds.
    long long owed = owed_.load(std::memory_order_relaxed);
    if (owed > 0)
    {
        const int s = (int) std::min<long long>(owed, fifo_.getFreeSpace());
        if (s > 0) { push(nullptr, s); owed -= s; }
    }
    if (owed > 0 || fifo_.getFreeSpace() < n)
    {
        owed_.store(owed + n, std::memory_order_relaxed);
        dropped_.fetch_add(n, std::memory_order_relaxed);
        return;
    }
    owed_.store(owed, std::memory_order_relaxed);
    push(chans, n);
}

void TakeRecorder::writeOut(int start, int n)
{
    if (n <= 0) return;
    if (!failed_.load(std::memory_order_relaxed))
        for (size_t i = 0; i < writers_.size(); ++i)
            if (!writers_[i]->writeFromAudioSampleBuffer(bufs_[i], start, n))
            {
                failed_.store(true, std::memory_order_relaxed);
                break;
            }
}

void TakeRecorder::drain()
{
    const int ready = fifo_.getNumReady();
    if (ready <= 0) return;
    int s1 = 0, z1 = 0, s2 = 0, z2 = 0;
    fifo_.prepareToRead(ready, s1, z1, s2, z2);
    writeOut(s1, z1);
    writeOut(s2, z2);
    fifo_.finishedRead(z1 + z2);
    written_.fetch_add(z1 + z2, std::memory_order_relaxed);
}

void TakeRecorder::finish()
{
    if (finished_) return;
    finished_ = true;
    drain();
    // Silence owed for dropped blocks keeps the take at full length.
    long long owed = owed_.exchange(0, std::memory_order_relaxed);
    if (owed > 0 && !failed_.load(std::memory_order_relaxed))
    {
        const int chunk = std::min(4096, fifo_.getTotalSize() - 1);
        for (auto& b : bufs_) b.clear();
        while (owed > 0 && !failed_.load(std::memory_order_relaxed))
        {
            const int n = (int) std::min<long long>(owed, chunk);
            writeOut(0, n);
            owed -= n;
        }
    }
    writers_.clear();   // flushes and closes each file
}

} // namespace minihost_desktop
