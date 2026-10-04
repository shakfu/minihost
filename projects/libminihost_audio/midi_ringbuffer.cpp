// midi_ringbuffer.cpp
// Lock-free SPSC ring buffer implementation

#include "midi_ringbuffer.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <new>

struct MH_MidiRingBuffer {
    MH_MidiEvent* buffer;
    uint64_t* times;  // arrival time per slot, parallel to buffer
    int capacity;
    int mask;  // capacity - 1, for fast modulo with power of 2
    std::atomic<int> write_pos;
    std::atomic<int> read_pos;
};

// Largest power of two an int holds. Rounding a larger request would
// overflow the shift sequence and yield a negative capacity and a mask of
// INT_MAX, so the create functions reject it instead.
#define MH_RINGBUFFER_MAX_CAPACITY (1 << 30)

// Round up to next power of 2
// Returns 0 when n exceeds the largest representable power of two.
static int next_power_of_2(int n) {
    if (n > MH_RINGBUFFER_MAX_CAPACITY) return 0;
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n++;
    return n;
}

extern "C" {

MH_MidiRingBuffer* mh_midi_ringbuffer_create(int capacity) {
    if (capacity <= 0) {
        capacity = 256;  // Default
    }

    // Round up to power of 2 for efficient modulo
    capacity = next_power_of_2(capacity);
    if (capacity == 0) return nullptr;

    MH_MidiRingBuffer* rb = new (std::nothrow) MH_MidiRingBuffer();
    if (!rb) return nullptr;

    rb->buffer = static_cast<MH_MidiEvent*>(std::calloc(capacity, sizeof(MH_MidiEvent)));
    rb->times = static_cast<uint64_t*>(std::calloc(capacity, sizeof(uint64_t)));
    if (!rb->buffer || !rb->times) {
        std::free(rb->buffer);
        std::free(rb->times);
        delete rb;
        return nullptr;
    }

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->write_pos.store(0, std::memory_order_relaxed);
    rb->read_pos.store(0, std::memory_order_relaxed);

    return rb;
}

void mh_midi_ringbuffer_free(MH_MidiRingBuffer* rb) {
    if (!rb) return;
    std::free(rb->buffer);
    std::free(rb->times);
    delete rb;
}

int mh_midi_ringbuffer_push(MH_MidiRingBuffer* rb, const MH_MidiEvent* event) {
    return mh_midi_ringbuffer_push_at(rb, event, 0);
}

int mh_midi_ringbuffer_push_at(MH_MidiRingBuffer* rb, const MH_MidiEvent* event,
                               uint64_t time_ns) {
    if (!rb || !event) return 0;

    int write = rb->write_pos.load(std::memory_order_relaxed);
    int next_write = (write + 1) & rb->mask;

    // Check if full (would overwrite unread data)
    int read = rb->read_pos.load(std::memory_order_acquire);
    if (next_write == read) {
        return 0;  // Buffer full
    }

    // Write the event
    rb->buffer[write] = *event;
    rb->times[write] = time_ns;

    // Publish the write
    rb->write_pos.store(next_write, std::memory_order_release);

    return 1;
}

int mh_midi_ringbuffer_pop(MH_MidiRingBuffer* rb, MH_MidiEvent* event) {
    if (!rb || !event) return 0;

    int read = rb->read_pos.load(std::memory_order_relaxed);
    int write = rb->write_pos.load(std::memory_order_acquire);

    if (read == write) {
        return 0;  // Buffer empty
    }

    // Read the event
    *event = rb->buffer[read];

    // Publish the read
    rb->read_pos.store((read + 1) & rb->mask, std::memory_order_release);

    return 1;
}

int mh_midi_ringbuffer_pop_all(MH_MidiRingBuffer* rb, MH_MidiEvent* events, int max_events) {
    return mh_midi_ringbuffer_pop_all_at(rb, events, nullptr, max_events);
}

int mh_midi_ringbuffer_pop_all_at(MH_MidiRingBuffer* rb, MH_MidiEvent* events,
                                  uint64_t* times_ns, int max_events) {
    if (!rb || !events || max_events <= 0) return 0;

    int count = 0;
    int read = rb->read_pos.load(std::memory_order_relaxed);
    int write = rb->write_pos.load(std::memory_order_acquire);

    while (read != write && count < max_events) {
        events[count] = rb->buffer[read];
        if (times_ns) times_ns[count] = rb->times[read];
        read = (read + 1) & rb->mask;
        count++;
    }

    // Publish all reads at once
    if (count > 0) {
        rb->read_pos.store(read, std::memory_order_release);
    }

    return count;
}

int mh_midi_ringbuffer_is_empty(MH_MidiRingBuffer* rb) {
    if (!rb) return 1;
    int read = rb->read_pos.load(std::memory_order_acquire);
    int write = rb->write_pos.load(std::memory_order_acquire);
    return read == write;
}

int mh_midi_ringbuffer_count(MH_MidiRingBuffer* rb) {
    if (!rb) return 0;
    int read = rb->read_pos.load(std::memory_order_acquire);
    int write = rb->write_pos.load(std::memory_order_acquire);
    return (write - read) & rb->mask;
}

uint64_t mh_midi_clock_ns(void) {
    return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int mh_midi_time_to_offset(uint64_t time_ns, uint64_t prev_cb_ns,
                           uint64_t this_cb_ns, int frames) {
    if (frames <= 1 || prev_cb_ns == 0 || this_cb_ns <= prev_cb_ns
        || time_ns <= prev_cb_ns)
        return 0;
    if (time_ns >= this_cb_ns) return frames - 1;
    // Scaled by the measured period rather than frames / sample rate, so a
    // device clock that drifts from the system clock cannot push events out
    // of the block.
    const double pos = (double) (time_ns - prev_cb_ns)
                     / (double) (this_cb_ns - prev_cb_ns);
    const int off = (int) (pos * frames);
    return off < frames ? off : frames - 1;
}

}  // extern "C"
