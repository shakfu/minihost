// rt_param_queue.h
//
// Lock-free single-producer / single-consumer (SPSC) ring buffer for
// parameter-write commands. The GUI / message thread is the producer;
// the audio thread is the consumer.
//
// Header-only. Fixed power-of-two capacity. Drop-newest-on-overflow:
// a producer call that would wrap into the consumer's tail returns
// false without enqueueing. Drain reads up to capacity items in FIFO
// order.
//
// Why we need this:
//   `mh_set_param` takes libminihost's internal mutex, which a GUI
//   thread may already hold (e.g. during topology edits). Calling
//   it from the audio callback can therefore block. The queue lets
//   the GUI hand commands off cheaply; the audio thread drains them
//   at the top of each block and applies them via
//   `juce::AudioProcessorParameter::setValue` (RT-safe by JUCE
//   contract) on the underlying processor.
//
// The queue itself doesn't know about plugins; the consumer maps
// `plugin_node_index` to a processor at drain time.

#pragma once

#include <atomic>
#include <array>
#include <cstdint>

namespace minihost_desktop {

struct ParamWriteCommand {
    int   plugin_node_index = -1;  // index into LoadedProject::plugins
    int   param_index       = -1;
    float value             = 0.0f;
};

// Capacity must be a power of two. 1024 commands ~= 16 kB; plenty for
// typical knob-twiddling rates against any sensible block boundary.
template <std::size_t Capacity>
class RtParamQueue
{
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
public:
    // Producer (GUI/message thread). Returns false if full (overflow).
    bool push(const ParamWriteCommand& cmd) noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        const auto next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire))
            return false;  // full: drop newest
        buf_[head] = cmd;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer (audio thread). Returns false if empty.
    bool pop(ParamWriteCommand& out) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false;
        out = buf_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Approximate size (transiently inconsistent under concurrent
    // access; for diagnostics only).
    std::size_t approxSize() const noexcept
    {
        const auto h = head_.load(std::memory_order_relaxed);
        const auto t = tail_.load(std::memory_order_relaxed);
        return (h - t) & kMask;
    }

    constexpr std::size_t capacity() const noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    alignas(64) std::atomic<std::size_t> head_{ 0 };
    alignas(64) std::atomic<std::size_t> tail_{ 0 };
    std::array<ParamWriteCommand, Capacity> buf_{};
};

} // namespace minihost_desktop
