// midi_ringbuffer.h
// Lock-free single-producer single-consumer ring buffer for MIDI events
//
// Thread Safety:
//   - push(): Call from producer thread only (MIDI input thread)
//   - pop()/pop_all(): Call from consumer thread only (audio thread)
//   - create()/free(): Not thread-safe, call before/after use
//
#pragma once

#include "minihost.h"  // For MH_MidiEvent

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MH_MidiRingBuffer MH_MidiRingBuffer;

// Create a ring buffer with given capacity (must be power of 2 for efficiency)
// Returns NULL on failure
MH_MidiRingBuffer* mh_midi_ringbuffer_create(int capacity);

// Free a ring buffer
void mh_midi_ringbuffer_free(MH_MidiRingBuffer* rb);

// Push an event to the ring buffer (producer/MIDI thread)
// Returns 1 on success, 0 if buffer is full
int mh_midi_ringbuffer_push(MH_MidiRingBuffer* rb, const MH_MidiEvent* event);

// Pop a single event from the ring buffer (consumer/audio thread)
// Returns 1 on success, 0 if buffer is empty
int mh_midi_ringbuffer_pop(MH_MidiRingBuffer* rb, MH_MidiEvent* event);

// Pop all available events from the ring buffer (consumer/audio thread)
// Returns number of events popped
int mh_midi_ringbuffer_pop_all(MH_MidiRingBuffer* rb, MH_MidiEvent* events, int max_events);

// Check if buffer is empty (approximate, for debugging)
int mh_midi_ringbuffer_is_empty(MH_MidiRingBuffer* rb);

// Get number of items in buffer (approximate, for debugging)
int mh_midi_ringbuffer_count(MH_MidiRingBuffer* rb);

#ifdef __cplusplus
}
#endif
