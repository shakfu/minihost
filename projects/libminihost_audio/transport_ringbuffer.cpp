// transport_ringbuffer.cpp
// Lock-free SPSC command ring for the host playhead

#include "transport_ringbuffer.h"

#include <atomic>
#include <cstdlib>
#include <new>

struct MH_TransportRingBuffer {
    MH_TransportCommand* buffer;
    int capacity;
    int mask;
    std::atomic<int> write_pos;
    std::atomic<int> read_pos;
};

// Largest power of two an int holds. Rounding a larger request would
// overflow the shift sequence and yield a negative capacity and a mask of
// INT_MAX, so the create functions reject it instead.
#define MH_RINGBUFFER_MAX_CAPACITY (1 << 30)

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

MH_TransportRingBuffer* mh_transport_ringbuffer_create(int capacity) {
    if (capacity <= 0) capacity = 64;
    capacity = next_power_of_2(capacity);
    if (capacity == 0) return nullptr;

    MH_TransportRingBuffer* rb = new (std::nothrow) MH_TransportRingBuffer();
    if (!rb) return nullptr;

    rb->buffer = static_cast<MH_TransportCommand*>(
        std::calloc(capacity, sizeof(MH_TransportCommand)));
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

void mh_transport_ringbuffer_free(MH_TransportRingBuffer* rb) {
    if (!rb) return;
    std::free(rb->buffer);
    delete rb;
}

int mh_transport_ringbuffer_push(MH_TransportRingBuffer* rb,
                                 const MH_TransportCommand* cmd) {
    if (!rb || !cmd) return 0;

    int write = rb->write_pos.load(std::memory_order_relaxed);
    int next_write = (write + 1) & rb->mask;
    int read = rb->read_pos.load(std::memory_order_acquire);
    if (next_write == read) return 0;

    rb->buffer[write] = *cmd;
    rb->write_pos.store(next_write, std::memory_order_release);
    return 1;
}

int mh_transport_ringbuffer_pop_all(MH_TransportRingBuffer* rb,
                                    MH_TransportCommand* out, int max_out) {
    if (!rb || !out || max_out <= 0) return 0;

    int count = 0;
    int read = rb->read_pos.load(std::memory_order_relaxed);
    int write = rb->write_pos.load(std::memory_order_acquire);

    while (read != write && count < max_out) {
        out[count] = rb->buffer[read];
        read = (read + 1) & rb->mask;
        count++;
    }

    if (count > 0) {
        rb->read_pos.store(read, std::memory_order_release);
    }
    return count;
}

}  // extern "C"
