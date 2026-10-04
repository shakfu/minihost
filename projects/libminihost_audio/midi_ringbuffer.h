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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MH_MidiRingBuffer MH_MidiRingBuffer;

// Create a ring buffer. Capacity is rounded up to the next power of 2; a
// non-positive capacity uses the default of 256. Returns NULL on failure,
// which includes a capacity above the largest power of 2 an int holds.
MH_MidiRingBuffer* mh_midi_ringbuffer_create(int capacity);

// Free a ring buffer
void mh_midi_ringbuffer_free(MH_MidiRingBuffer* rb);

// Push an event to the ring buffer (producer/MIDI thread)
// Returns 1 on success, 0 if buffer is full
int mh_midi_ringbuffer_push(MH_MidiRingBuffer* rb, const MH_MidiEvent* event);

// Push with an arrival time from mh_midi_clock_ns(). mh_midi_ringbuffer_push
// stores a time of 0.
int mh_midi_ringbuffer_push_at(MH_MidiRingBuffer* rb, const MH_MidiEvent* event,
                               uint64_t time_ns);

// Pop a single event from the ring buffer (consumer/audio thread)
// Returns 1 on success, 0 if buffer is empty
int mh_midi_ringbuffer_pop(MH_MidiRingBuffer* rb, MH_MidiEvent* event);

// Pop all available events from the ring buffer (consumer/audio thread)
// Returns number of events popped
int mh_midi_ringbuffer_pop_all(MH_MidiRingBuffer* rb, MH_MidiEvent* events, int max_events);

// As pop_all, also returning each event's arrival time in times_ns.
int mh_midi_ringbuffer_pop_all_at(MH_MidiRingBuffer* rb, MH_MidiEvent* events,
                                  uint64_t* times_ns, int max_events);

// Monotonic clock for stamping live MIDI. Lock-free; safe on any thread.
uint64_t mh_midi_clock_ns(void);

// Sample offset in a block of `frames` for an event that arrived at time_ns.
// prev_cb_ns and this_cb_ns are the start times of the previous and current
// audio callbacks. An event keeps its relative position within the previous
// callback period, so live MIDI is delayed by one block rather than jittered
// by up to one. Returns 0 when prev_cb_ns is 0 (first block) or the event
// predates it, and frames - 1 when it arrived after this_cb_ns.
int mh_midi_time_to_offset(uint64_t time_ns, uint64_t prev_cb_ns,
                           uint64_t this_cb_ns, int frames);

// Check if buffer is empty (approximate, for debugging)
int mh_midi_ringbuffer_is_empty(MH_MidiRingBuffer* rb);

// Get number of items in buffer (approximate, for debugging)
int mh_midi_ringbuffer_count(MH_MidiRingBuffer* rb);

#ifdef __cplusplus
}
#endif
