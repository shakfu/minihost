// param_ringbuffer.cpp
// Lock-free SPSC ring buffer implementation for parameter changes

#include "param_ringbuffer.h"
#include <atomic>
#include <cstdlib>
#include <new>

struct MH_ParamRingBuffer {
    MH_ChainParamChange* buffer;
    int capacity;
    int mask;  // capacity - 1, for fast modulo with power of 2
    std::atomic<int> write_pos;
    std::atomic<int> read_pos;
};

// Round up to next power of 2
static int next_power_of_2(int n) {
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

MH_ParamRingBuffer* mh_param_ringbuffer_create(int capacity) {
    if (capacity <= 0) {
        capacity = 1024;  // Default
    }

    capacity = next_power_of_2(capacity);

    MH_ParamRingBuffer* rb = new (std::nothrow) MH_ParamRingBuffer();
    if (!rb) return nullptr;

    rb->buffer = static_cast<MH_ChainParamChange*>(
        std::calloc(capacity, sizeof(MH_ChainParamChange)));
    if (!rb->buffer) {
        delete rb;
        return nullptr;
    }

    rb->capacity = capacity;
    rb->mask = capacity - 1;
    rb->write_pos.store(0, std::memory_order_relaxed);
    rb->read_pos.store(0, std::memory_order_relaxed);

    return rb;
}

void mh_param_ringbuffer_free(MH_ParamRingBuffer* rb) {
    if (!rb) return;
    std::free(rb->buffer);
    delete rb;
}

int mh_param_ringbuffer_push(MH_ParamRingBuffer* rb, int plugin_index,
                             int param_index, float value) {
    if (!rb) return 0;

    int write = rb->write_pos.load(std::memory_order_relaxed);
    int next_write = (write + 1) & rb->mask;

    // Check if full (would overwrite unread data)
    int read = rb->read_pos.load(std::memory_order_acquire);
    if (next_write == read) {
        return 0;  // Buffer full
    }

    rb->buffer[write].sample_offset = 0;
    rb->buffer[write].plugin_index = plugin_index;
    rb->buffer[write].param_index = param_index;
    rb->buffer[write].value = value;

    // Publish the write
    rb->write_pos.store(next_write, std::memory_order_release);

    return 1;
}

int mh_param_ringbuffer_drain(MH_ParamRingBuffer* rb,
                              MH_ChainParamChange* out, int max_out, int* count) {
    if (!rb || !out || !count || max_out <= 0) return 0;

    int have = *count;
    if (have < 0) have = 0;

    int read = rb->read_pos.load(std::memory_order_relaxed);
    const int start = read;
    int write = rb->write_pos.load(std::memory_order_acquire);

    while (read != write) {
        const MH_ChainParamChange& pending = rb->buffer[read];

        // Coalesce: a parameter already staged keeps its slot and takes the
        // newer value. Linear because `have` is bounded by max_out (a handful
        // of parameters in any real block) and a scan over that beats a map
        // the audio thread would have to allocate.
        int slot = -1;
        for (int i = 0; i < have; i++) {
            if (out[i].param_index == pending.param_index &&
                out[i].plugin_index == pending.plugin_index) {
                slot = i;
                break;
            }
        }

        if (slot >= 0) {
            out[slot].value = pending.value;
        } else if (have < max_out) {
            out[have] = pending;
            have++;
        } else {
            // `out` is full and this change is for a parameter not in it.
            // Stop here rather than consuming it: leaving it in the buffer
            // defers it to the next drain, where it will fit. Discarding it
            // instead would lose the value outright, because nothing upstream
            // resends -- the producer has already moved on.
            break;
        }

        read = (read + 1) & rb->mask;
    }

    // Publish all reads at once. Skipped when nothing was consumed, so an
    // idle block costs the audio thread a load and no store.
    if (read != start) {
        rb->read_pos.store(read, std::memory_order_release);
    }

    *count = have;

    // What is left for the next drain. Non-zero only when more distinct
    // parameters changed in one block than `out` can hold.
    return (write - read) & rb->mask;
}

int mh_param_ringbuffer_is_empty(MH_ParamRingBuffer* rb) {
    if (!rb) return 1;
    int read = rb->read_pos.load(std::memory_order_acquire);
    int write = rb->write_pos.load(std::memory_order_acquire);
    return read == write;
}

int mh_param_ringbuffer_count(MH_ParamRingBuffer* rb) {
    if (!rb) return 0;
    int read = rb->read_pos.load(std::memory_order_acquire);
    int write = rb->write_pos.load(std::memory_order_acquire);
    return (write - read) & rb->mask;
}

}  // extern "C"
