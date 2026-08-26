// param_ringbuffer.h
// Lock-free single-producer single-consumer ring buffer for parameter changes
//
// The parameter counterpart to midi_ringbuffer.h, and deliberately the same
// shape so the two read alike. The difference is in the drain: MIDI events are
// all distinct and every one must reach the plugin, while parameter changes
// supersede one another, so the drain coalesces to the newest value per
// parameter rather than handing every intermediate value to the processor.
//
// The element is MH_ChainParamChange rather than MH_ParamChange because it is
// the superset of the two: a device may be opened on a plugin or on a chain,
// and only the chain form carries the plugin index that says which one a write
// is for. A single-plugin device uses index 0 throughout and the audio thread
// projects the drained array down to MH_ParamChange before processing.
//
// Thread Safety:
//   - push(): Call from producer thread only (control-input or app thread)
//   - drain(): Call from consumer thread only (audio thread)
//   - create()/free(): Not thread-safe, call before/after use
//
// One producer per buffer. A device that accepts parameter writes from both a
// control-input thread and application code gives each its own buffer; see the
// comment on MH_AudioDevice's param buffers for why sharing one is a bug and
// not a saving.
//
#pragma once

#include "minihost.h"        // For MH_ParamChange
#include "minihost_chain.h"  // For MH_ChainParamChange

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MH_ParamRingBuffer MH_ParamRingBuffer;

// Create a ring buffer with given capacity (rounded up to a power of 2)
// Returns NULL on failure
MH_ParamRingBuffer* mh_param_ringbuffer_create(int capacity);

// Free a ring buffer
void mh_param_ringbuffer_free(MH_ParamRingBuffer* rb);

// Push a parameter change (producer thread)
//
// plugin_index selects the chain slot; pass 0 for a single-plugin device.
//
// sample_offset is not taken: a control surface has no sample position to
// give, so the drain assigns one. Values outside 0..1 are stored as given and
// clamped downstream by mh_process_auto, matching mh_set_param.
//
// Returns 1 on success, 0 if the buffer is full. Full means the audio thread
// has not run for as long as it takes the producer to send `capacity`
// changes; at any plausible control rate that is a stalled device, not a busy
// surface. The push that fails is the newest one, which for a superseding
// value is the wrong one to lose -- accepted because the alternative
// (overwriting the oldest) would have the producer move the consumer's read
// index. Size the buffer so it cannot arise instead.
int mh_param_ringbuffer_push(MH_ParamRingBuffer* rb, int plugin_index,
                             int param_index, float value);

// Drain every pending change into `out`, coalescing per
// (plugin_index, param_index) (consumer/audio thread).
//
// `count` is in/out: on entry the number of entries `out` already holds, so
// several buffers can be drained into one array and coalesce against each
// other; on return the new total. Entries keep first-seen order and carry
// sample_offset 0. A parameter already present is updated in place, so the
// last value pushed is the one that survives -- which is what makes a fader
// drag that emits two hundred values inside one block cost one parameter
// write rather than two hundred.
//
// Returns the number of changes left in the buffer for the next drain. That
// is non-zero only when more distinct parameters changed in one block than
// `out` can hold -- a preset load rather than a gesture. The excess is
// deferred, never discarded: nothing upstream resends, so a dropped value
// would be lost for good and the parameter would sit at a stale setting.
//
// Deferral is in buffer order, so a burst spanning more than `max_out`
// parameters is applied over consecutive blocks rather than all at once.
int mh_param_ringbuffer_drain(MH_ParamRingBuffer* rb,
                              MH_ChainParamChange* out, int max_out, int* count);

// Check if buffer is empty (approximate, for debugging)
int mh_param_ringbuffer_is_empty(MH_ParamRingBuffer* rb);

// Get number of items in buffer (approximate, for debugging)
int mh_param_ringbuffer_count(MH_ParamRingBuffer* rb);

#ifdef __cplusplus
}
#endif
