// audio_ringbuffer.cpp
// Lock-free SPSC ring buffer for audio frames.
//
// Stores audio as interleaved samples internally so the consumer (audio thread)
// reads sequentially through memory. The producer pushes interleaved data
// (matching numpy's row-major layout after transpose), and the consumer
// de-interleaves into per-channel buffers during read.

#include "audio_ringbuffer.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

struct MH_AudioRingBuffer {
    float* buffer;          // interleaved storage: [frame * channels + ch]
    int channels;
    int capacity;           // in frames, always power of 2
    int mask;               // capacity - 1
    std::atomic<int> write_pos;  // frame index
    std::atomic<int> read_pos;   // frame index
};

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

MH_AudioRingBuffer* mh_audio_ringbuffer_create(int channels, int capacity_frames) {
    if (channels <= 0 || capacity_frames <= 0) return nullptr;

    capacity_frames = next_power_of_2(capacity_frames);

    auto* rb = new (std::nothrow) MH_AudioRingBuffer();
    if (!rb) return nullptr;

    rb->buffer = static_cast<float*>(std::calloc(
        static_cast<size_t>(capacity_frames) * channels, sizeof(float)));
    if (!rb->buffer) {
        delete rb;
        return nullptr;
    }

    rb->channels = channels;
    rb->capacity = capacity_frames;
    rb->mask = capacity_frames - 1;
    rb->write_pos.store(0, std::memory_order_relaxed);
    rb->read_pos.store(0, std::memory_order_relaxed);

    return rb;
}

void mh_audio_ringbuffer_free(MH_AudioRingBuffer* rb) {
    if (!rb) return;
    std::free(rb->buffer);
    delete rb;
}

int mh_audio_ringbuffer_push(MH_AudioRingBuffer* rb, const float* data, int nframes) {
    if (!rb || !data || nframes <= 0) return 0;

    int write = rb->write_pos.load(std::memory_order_relaxed);
    int read = rb->read_pos.load(std::memory_order_acquire);

    // Available space: capacity - 1 - used (one slot wasted to distinguish full from empty)
    int used = (write - read) & rb->mask;
    int available = rb->capacity - 1 - used;
    if (available <= 0) return 0;

    int to_write = nframes < available ? nframes : available;
    int ch = rb->channels;

    for (int f = 0; f < to_write; f++) {
        int idx = ((write + f) & rb->mask) * ch;
        std::memcpy(&rb->buffer[idx], &data[f * ch], ch * sizeof(float));
    }

    rb->write_pos.store((write + to_write) & rb->mask, std::memory_order_release);
    return to_write;
}

int mh_audio_ringbuffer_read_into(MH_AudioRingBuffer* rb, float* const* buffers,
                                   int nframes, int channels) {
    if (!rb || !buffers || nframes <= 0) {
        // Zero output on error
        if (buffers && nframes > 0) {
            for (int c = 0; c < channels; c++) {
                if (buffers[c]) std::memset(buffers[c], 0, nframes * sizeof(float));
            }
        }
        return 0;
    }

    int read = rb->read_pos.load(std::memory_order_relaxed);
    int write = rb->write_pos.load(std::memory_order_acquire);

    int avail = (write - read) & rb->mask;
    int to_read = nframes < avail ? nframes : avail;
    int rb_ch = rb->channels;
    int copy_ch = channels < rb_ch ? channels : rb_ch;

    // De-interleave from ring buffer into per-channel output buffers
    for (int f = 0; f < to_read; f++) {
        int idx = ((read + f) & rb->mask) * rb_ch;
        for (int c = 0; c < copy_ch; c++) {
            buffers[c][f] = rb->buffer[idx + c];
        }
        // Zero extra output channels not in ring buffer
        for (int c = copy_ch; c < channels; c++) {
            buffers[c][f] = 0.0f;
        }
    }

    // Zero remaining frames (underrun silence)
    for (int f = to_read; f < nframes; f++) {
        for (int c = 0; c < channels; c++) {
            buffers[c][f] = 0.0f;
        }
    }

    if (to_read > 0) {
        rb->read_pos.store((read + to_read) & rb->mask, std::memory_order_release);
    }

    return to_read;
}

void mh_audio_ringbuffer_clear(MH_AudioRingBuffer* rb) {
    if (!rb) return;
    rb->write_pos.store(0, std::memory_order_relaxed);
    rb->read_pos.store(0, std::memory_order_relaxed);
}

int mh_audio_ringbuffer_available(MH_AudioRingBuffer* rb) {
    if (!rb) return 0;
    int read = rb->read_pos.load(std::memory_order_acquire);
    int write = rb->write_pos.load(std::memory_order_acquire);
    return (write - read) & rb->mask;
}

int mh_audio_ringbuffer_channels(MH_AudioRingBuffer* rb) {
    if (!rb) return 0;
    return rb->channels;
}

}  // extern "C"
