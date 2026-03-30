// audio_ringbuffer.h
// Lock-free single-producer single-consumer ring buffer for audio frames.
//
// Stores non-interleaved audio: each push/pop operates on a full frame
// (one sample per channel). Internally stored as interleaved for cache
// efficiency during the audio callback's sequential read.
//
// Thread Safety:
//   - push(): Call from producer thread only (Python/main thread)
//   - read_into(): Call from consumer thread only (audio thread)
//   - create()/free()/clear(): Not thread-safe, call before/after use
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MH_AudioRingBuffer MH_AudioRingBuffer;

// Create a ring buffer for the given number of channels and frame capacity.
// Capacity is rounded up to the next power of 2.
// Returns NULL on failure.
MH_AudioRingBuffer* mh_audio_ringbuffer_create(int channels, int capacity_frames);

// Free a ring buffer.
void mh_audio_ringbuffer_free(MH_AudioRingBuffer* rb);

// Push interleaved frames into the ring buffer (producer thread).
// data: interleaved float audio [frame0_ch0, frame0_ch1, ..., frame1_ch0, ...]
// nframes: number of frames to push
// Returns number of frames actually written (may be less if buffer is full).
int mh_audio_ringbuffer_push(MH_AudioRingBuffer* rb, const float* data, int nframes);

// Read frames from the ring buffer into non-interleaved channel buffers (consumer/audio thread).
// buffers: array of per-channel float pointers [channel][frame]
// nframes: number of frames to read
// channels: number of channels in buffers
// Fills with silence if not enough data is available.
// Returns number of frames actually read from the buffer (rest is silence).
int mh_audio_ringbuffer_read_into(MH_AudioRingBuffer* rb, float* const* buffers,
                                   int nframes, int channels);

// Clear the ring buffer (not thread-safe, call when not playing).
void mh_audio_ringbuffer_clear(MH_AudioRingBuffer* rb);

// Get number of frames available for reading (approximate, for diagnostics).
int mh_audio_ringbuffer_available(MH_AudioRingBuffer* rb);

// Get the channel count.
int mh_audio_ringbuffer_channels(MH_AudioRingBuffer* rb);

#ifdef __cplusplus
}
#endif
