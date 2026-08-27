// minihost_audio.c
// Real-time audio playback using miniaudio

#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "minihost_audio.h"
#include "minihost_midi.h"
#include "minihost_osc.h"
#include "midi_ringbuffer.h"
#include "param_ringbuffer.h"
#include "transport_ringbuffer.h"
#include "audio_ringbuffer.h"
#include "minihost.h"
#include "minihost_chain.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// Portable acquire/release atomics for a single pointer-sized slot, used for
// the audio input-callback pointer (published by the app thread, read by the
// audio thread). We avoid C11 <stdatomic.h> because MSVC gates it behind an
// opt-in flag that the Visual Studio generator does not reliably pass; the
// builtins/intrinsics below need no flag and work on every platform minihost
// builds for (x86-64 and arm64, Clang / GCC / MSVC).
#if defined(_MSC_VER)
#include <intrin.h>
static inline void* mh_atomic_load_acquire_ptr(void* volatile* slot) {
    // A no-op compare-exchange returns the current value with a full barrier
    // (correct on x64 and arm64).
    return _InterlockedCompareExchangePointer(slot, NULL, NULL);
}
static inline void mh_atomic_store_release_ptr(void* volatile* slot, void* value) {
    (void)_InterlockedExchangePointer(slot, value);  // full barrier
}
#else
static inline void* mh_atomic_load_acquire_ptr(void* volatile* slot) {
    return __atomic_load_n(slot, __ATOMIC_ACQUIRE);
}
static inline void mh_atomic_store_release_ptr(void* volatile* slot, void* value) {
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
}
#endif

#define MH_TRANSPORT_PUB_SLOTS 4
#define MH_MAX_SLOT_NAMES 64
#define MH_SLOT_NAME_LEN 64
#define MH_TRANSPORT_MAX_TARGETS 64

struct MH_AudioDevice {
    ma_device device;
    ma_context context;
    MH_Plugin* plugin;           // Single plugin (NULL if chain)
    MH_PluginChain* chain;       // Plugin chain (NULL if single plugin)

    // Audio configuration
    double sample_rate;
    int buffer_frames;
    int channels;
    int capture;             // 1 if duplex (capture enabled), 0 if playback only

    // Input callback for effects. Read on the audio thread, written from the
    // app thread (mh_audio_set_input_callback). Stored type-erased as void*
    // and accessed through the acquire/release pointer atomics above, so the
    // audio thread never reads a torn pointer; user_data is published before
    // the pointer, so observing a non-NULL callback implies its user_data is
    // visible too. Callers must clear (set NULL) before installing a different
    // callback (the existing contract -- the live source goes start -> stop ->
    // start, never a hot swap between two distinct non-NULL callbacks).
    void* input_callback;  // holds an MH_AudioInputCallback
    void* input_callback_user_data;

    // Pre-allocated conversion buffers (non-interleaved).
    //
    // These are handed straight to mh_process / mh_chain_process, which read
    // the plugin's input-channel count and write its output-channel count --
    // neither of which is bounded by the device's channel count. Sizing them
    // to `channels` alone lets a plugin with more channels than the device
    // (or a device that negotiates fewer channels than requested) index past
    // the end of the pointer array. They are therefore allocated with
    // `buffer_channels` = max(device channels, plugin inputs, plugin outputs)
    // channels; `channels` still governs what is exchanged with the device.
    float** input_buffers;   // [channel][frame], buffer_channels entries
    float** output_buffers;  // [channel][frame], buffer_channels entries
    int buffer_channels;     // channels allocated (>= channels)
    int buffer_capacity;     // frames allocated
    // Largest block we may hand the processor: min(buffer_capacity, the
    // processor's own max block size). The device period is normally well
    // under this; it bounds the pathological case of a backend delivering a
    // larger callback than its reported period.
    int max_process_frames;

    // MIDI I/O
    MH_MidiIn* midi_in;
    MH_MidiOut* midi_out;
    // MH_MidiRingBuffer is single-producer/single-consumer (see its header).
    // midi_in_buffer therefore belongs exclusively to the libremidi input
    // thread; mh_audio_send_midi gets its own ring rather than becoming a
    // second producer on the same one, which would corrupt the indices and
    // lose or duplicate events. The audio thread drains both.
    MH_MidiRingBuffer* midi_in_buffer;   // MIDI thread -> audio thread
    MH_MidiRingBuffer* midi_send_buffer; // app thread  -> audio thread
    MH_MidiRingBuffer* midi_out_buffer;  // audio thread -> MIDI output

    // Parameter changes, same one-producer-per-ring rule as the MIDI pair
    // above and for the same reason. param_ctl_buffer belongs to whichever
    // thread drives a control surface (the MIDI input callback today, an OSC
    // socket thread later); param_send_buffer belongs to application code
    // calling mh_audio_send_param. The audio thread drains both into one
    // coalesced array and hands it to the _auto process entry point, so a
    // parameter write lands at a defined point in the block instead of
    // racing processBlock through mh_set_param's mutex.
    MH_ParamRingBuffer* param_ctl_buffer;  // control thread -> audio thread
    MH_ParamRingBuffer* param_send_buffer; // app thread     -> audio thread

    // OSC input. Its socket thread is the single producer on
    // param_ctl_buffer, which is why mh_audio_send_param_control is
    // documented as belonging to one control thread: connecting OSC claims it.
    MH_OscServer* osc_server;

    // Optional per-slot names for OSC addressing. Written only while the OSC
    // socket thread does not exist (mh_audio_set_slot_name refuses once
    // connected), so the socket thread reads it with no synchronisation and
    // none is needed.
    char slot_names[MH_MAX_SLOT_NAMES][MH_SLOT_NAME_LEN];

    // Host playhead. `transport` is owned outright by the audio thread; every
    // other thread posts commands to transport_commands and reads back
    // through the published snapshots. transport_enabled is written only by
    // the audio thread (via a command) and read by mh_audio_get_transport_*.
    MH_TransportRingBuffer* transport_commands;
    MH_TransportInfo transport;
    int transport_enabled;
    // Rotating published snapshots. The audio thread fills the next slot and
    // publishes its address; readers take the address and copy. See the note
    // on mh_audio_get_transport about what this does and does not guarantee.
    MH_TransportInfo transport_pub[MH_TRANSPORT_PUB_SLOTS];
    int transport_pub_next;
    void* transport_pub_current;  // holds an MH_TransportInfo*
    // Every plugin the playhead must be handed to, resolved once at open.
    // A chain holds several, and mh_chain_get_plugin is a thread-safe (i.e.
    // locking) accessor -- calling it per block from the audio thread would
    // put a mutex on the audio path for no reason, since a chain's membership
    // is fixed when it is created.
    MH_Plugin* transport_targets[MH_TRANSPORT_MAX_TARGETS];
    int transport_num_targets;

    int midi_in_port;   // -1 if not connected or virtual
    int midi_out_port;  // -1 if not connected or virtual
    int midi_in_virtual;   // 1 if virtual port, 0 if physical
    int midi_out_virtual;  // 1 if virtual port, 0 if physical

    // Audio input ring buffer (for write_input / effect processing)
    MH_AudioRingBuffer* audio_in_buffer;

    // State
    int is_playing;
};

// Resolve playback/capture device IDs from MH_AudioConfig indices via an already-initialized context.
// On success, stores pointers to the resolved ma_device_id into *out_playback_id / *out_capture_id
// (which may be NULL if no selection was made). The pointed-to ma_device_info arrays remain valid
// as long as the context is not re-enumerated.
// Returns MA_SUCCESS on success (including "no selection requested"), or an ma_result on failure.
static ma_result resolve_device_ids(ma_context* ctx,
                                    const MH_AudioConfig* config,
                                    int capture_enabled,
                                    const ma_device_id** out_playback_id,
                                    const ma_device_id** out_capture_id,
                                    char* err_buf, size_t err_buf_size) {
    *out_playback_id = NULL;
    *out_capture_id = NULL;

    int want_playback = (config && config->playback_device_index >= 0);
    int want_capture = (capture_enabled && config && config->capture_device_index >= 0);
    if (!want_playback && !want_capture) {
        return MA_SUCCESS;
    }

    ma_device_info* playback_infos = NULL;
    ma_uint32 playback_count = 0;
    ma_device_info* capture_infos = NULL;
    ma_uint32 capture_count = 0;

    ma_result r = ma_context_get_devices(ctx, &playback_infos, &playback_count,
                                         &capture_infos, &capture_count);
    if (r != MA_SUCCESS) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to enumerate audio devices: %d", r);
        }
        return r;
    }

    if (want_playback) {
        if ((ma_uint32)config->playback_device_index >= playback_count) {
            if (err_buf && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size,
                         "Playback device index %d out of range (found %u device(s))",
                         config->playback_device_index, (unsigned)playback_count);
            }
            return MA_INVALID_ARGS;
        }
        *out_playback_id = &playback_infos[config->playback_device_index].id;
    }

    if (want_capture) {
        if ((ma_uint32)config->capture_device_index >= capture_count) {
            if (err_buf && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size,
                         "Capture device index %d out of range (found %u device(s))",
                         config->capture_device_index, (unsigned)capture_count);
            }
            return MA_INVALID_ARGS;
        }
        *out_capture_id = &capture_infos[config->capture_device_index].id;
    }

    return MA_SUCCESS;
}

// Reject a device whose block size the processor cannot handle.
//
// Every mh_process* rejects nframes above the plugin's max_block_size, and
// discovering that per-block is useless: the callback has nowhere to report it
// and the user hears silence or a repeating buzz. Check it once, at open, and
// say exactly which number to change.
//
// `needed` is the device *period*, not the 2x internal headroom on the
// conversion buffers -- requiring double the period would reject the common
// and perfectly workable case of a plugin and device sized alike. The headroom
// is handled by clamping instead (see max_process_frames). Returns 1 if usable.
static int validate_block_size(int processor_max_block, int needed,
                               const char* what,
                               char* err_buf, size_t err_buf_size) {
    if (processor_max_block > 0 && processor_max_block >= needed) {
        return 1;
    }
    if (err_buf && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size,
                 "%s was opened with max_block_size=%d but the audio device "
                 "needs up to %d frames per callback. Reopen the %s with "
                 "max_block_size >= %d, or request a smaller buffer_frames.",
                 what, processor_max_block, needed, what, needed);
    }
    return 0;
}

// Allocate non-interleaved buffer array
static float** alloc_channel_buffers(int channels, int frames) {
    float** buffers = (float**)malloc(channels * sizeof(float*));
    if (!buffers) return NULL;

    for (int ch = 0; ch < channels; ch++) {
        buffers[ch] = (float*)calloc(frames, sizeof(float));
        if (!buffers[ch]) {
            // Cleanup on failure
            for (int i = 0; i < ch; i++) {
                free(buffers[i]);
            }
            free(buffers);
            return NULL;
        }
    }
    return buffers;
}

// Free non-interleaved buffer array
static void free_channel_buffers(float** buffers, int channels) {
    if (!buffers) return;
    for (int ch = 0; ch < channels; ch++) {
        free(buffers[ch]);
    }
    free(buffers);
}

// MIDI input callback - called from MIDI thread when messages arrive
static void midi_input_callback(const unsigned char* data, size_t len, void* user_data) {
    MH_AudioDevice* dev = (MH_AudioDevice*)user_data;
    if (!dev || !dev->midi_in_buffer || len < 1) return;

    MH_MidiEvent event;
    event.sample_offset = 0;  // Will be processed at start of next audio buffer
    event.status = data[0];
    event.data1 = (len >= 2) ? data[1] : 0;
    event.data2 = (len >= 3) ? data[2] : 0;

    mh_midi_ringbuffer_push(dev->midi_in_buffer, &event);
}

// Resolve, once at open, every plugin the playhead is handed to. A chain
// beyond MH_TRANSPORT_MAX_TARGETS plugins simply gets the first that many;
// the alternative is an allocation per device for a case that does not exist.
static void transport_resolve_targets(MH_AudioDevice* dev) {
    dev->transport_num_targets = 0;

    if (dev->plugin) {
        dev->transport_targets[dev->transport_num_targets++] = dev->plugin;
        return;
    }
    if (!dev->chain) return;

    int n = mh_chain_get_num_plugins(dev->chain);
    if (n > MH_TRANSPORT_MAX_TARGETS) n = MH_TRANSPORT_MAX_TARGETS;
    for (int i = 0; i < n; i++) {
        MH_Plugin* p = mh_chain_get_plugin(dev->chain, i);
        if (p) dev->transport_targets[dev->transport_num_targets++] = p;
    }
}

// Apply queued transport commands. Audio thread only.
static void transport_apply_commands(MH_AudioDevice* dev) {
    if (!dev->transport_commands) return;

    MH_TransportCommand cmds[32];
    int n = mh_transport_ringbuffer_pop_all(dev->transport_commands, cmds, 32);
    for (int i = 0; i < n; i++) {
        MH_TransportCommand* c = &cmds[i];
        switch (c->type) {
            case MH_TRANSPORT_CMD_PLAY:
                dev->transport.is_playing = 1;
                break;
            case MH_TRANSPORT_CMD_STOP:
                dev->transport.is_playing = 0;
                break;
            case MH_TRANSPORT_CMD_SET_BPM:
                dev->transport.bpm = c->dvalue;
                break;
            case MH_TRANSPORT_CMD_SET_TIME_SIG:
                dev->transport.time_sig_numerator = c->ivalue;
                dev->transport.time_sig_denominator = c->ivalue2;
                break;
            case MH_TRANSPORT_CMD_SET_POSITION_SAMPLES:
                dev->transport.position_samples = c->lvalue;
                break;
            case MH_TRANSPORT_CMD_SET_LOOP:
                dev->transport.is_looping = c->ivalue;
                dev->transport.loop_start_samples = c->lvalue;
                dev->transport.loop_end_samples = c->lvalue2;
                break;
            case MH_TRANSPORT_CMD_SET_RECORDING:
                dev->transport.is_recording = c->ivalue;
                break;
            default:
                break;
        }
    }

    // position_beats is derived, never commanded: two sources of truth for
    // the same instant is how a playhead ends up disagreeing with itself.
    double sr = dev->sample_rate > 0 ? dev->sample_rate : 48000.0;
    double seconds = (double)dev->transport.position_samples / sr;
    dev->transport.position_beats = seconds * (dev->transport.bpm / 60.0);
}

// Advance the playhead past a rendered block, wrapping at the loop end.
// Audio thread only.
static void transport_advance(MH_AudioDevice* dev, int frames) {
    if (!dev->transport.is_playing) return;

    dev->transport.position_samples += frames;

    if (dev->transport.is_looping &&
        dev->transport.loop_end_samples > dev->transport.loop_start_samples &&
        dev->transport.position_samples >= dev->transport.loop_end_samples) {
        long long span = dev->transport.loop_end_samples -
                         dev->transport.loop_start_samples;
        long long over = dev->transport.position_samples -
                         dev->transport.loop_start_samples;
        // Modulo rather than a subtract: a loop shorter than one block would
        // otherwise still sit past the end after wrapping.
        dev->transport.position_samples =
            dev->transport.loop_start_samples + (over % span);
    }
}

// Publish a snapshot for readers. Audio thread only.
static void transport_publish(MH_AudioDevice* dev) {
    MH_TransportInfo* slot = &dev->transport_pub[dev->transport_pub_next];
    *slot = dev->transport;
    dev->transport_pub_next =
        (dev->transport_pub_next + 1) % MH_TRANSPORT_PUB_SLOTS;
    mh_atomic_store_release_ptr(&dev->transport_pub_current, slot);
}

// Audio callback - called from miniaudio's audio thread
static void audio_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    MH_AudioDevice* dev = (MH_AudioDevice*)device->pUserData;

    float* interleaved_output = (float*)output;
    int channels = dev->channels;
    int buf_ch = dev->buffer_channels;
    int frames = (int)frame_count;

    // Clamp to what we can actually process. Any excess is zero-filled at the
    // end of the callback, so an over-large callback degrades to a short block
    // plus silence rather than being refused outright.
    if (frames > dev->max_process_frames) {
        frames = dev->max_process_frames;
    }

    // Get input audio: capture (duplex) > input callback > silence.
    // Every path below fills only the device's own `channels`; the sources
    // (capture stream, ring buffer, user callback) are all device-shaped.
    void* cbp;
    if (dev->capture && input) {
        // De-interleave capture input into per-channel buffers
        const float* interleaved_input = (const float*)input;
        for (int f = 0; f < frames; f++) {
            for (int ch = 0; ch < channels; ch++) {
                dev->input_buffers[ch][f] = interleaved_input[f * channels + ch];
            }
        }
    } else if ((cbp = mh_atomic_load_acquire_ptr(&dev->input_callback)) != NULL) {
        ((MH_AudioInputCallback)cbp)(dev->input_buffers, frames,
                                     dev->input_callback_user_data);
    } else {
        // Zero input buffers for synth plugins
        for (int ch = 0; ch < channels; ch++) {
            memset(dev->input_buffers[ch], 0, frames * sizeof(float));
        }
    }

    // Channels beyond the device's own count exist only because the plugin
    // requires more inputs than the device supplies; feed them silence.
    for (int ch = channels; ch < buf_ch; ch++) {
        memset(dev->input_buffers[ch], 0, frames * sizeof(float));
    }

    // Drain MIDI input buffer
    MH_MidiEvent midi_events[256];
    int num_midi_events = 0;
    if (dev->midi_in_buffer) {
        num_midi_events = mh_midi_ringbuffer_pop_all(dev->midi_in_buffer, midi_events, 256);
    }
    // Programmatic sends live in their own ring (see the struct comment).
    if (dev->midi_send_buffer && num_midi_events < 256) {
        num_midi_events += mh_midi_ringbuffer_pop_all(
            dev->midi_send_buffer, midi_events + num_midi_events,
            256 - num_midi_events);
    }

    // Drain parameter changes from both producer rings into one coalesced
    // array. The control ring is drained first, so on the rare block that
    // fills the array it is application writes that get deferred rather than
    // a surface's -- the surface is the one with a human waiting on it.
    MH_ChainParamChange param_changes[MH_AUDIO_MAX_PARAM_CHANGES];
    int num_param_changes = 0;
    if (dev->param_ctl_buffer) {
        mh_param_ringbuffer_drain(dev->param_ctl_buffer, param_changes,
                                  MH_AUDIO_MAX_PARAM_CHANGES, &num_param_changes);
    }
    if (dev->param_send_buffer) {
        mh_param_ringbuffer_drain(dev->param_send_buffer, param_changes,
                                  MH_AUDIO_MAX_PARAM_CHANGES, &num_param_changes);
    }

    // Transport: commands, then hand the plugin the playhead for this block.
    // Before processing, so the plugin sees the position of the samples it is
    // about to render rather than the position after them.
    if (dev->transport_commands) {
        transport_apply_commands(dev);
        if (dev->transport_enabled) {
            for (int i = 0; i < dev->transport_num_targets; i++) {
                mh_set_transport(dev->transport_targets[i], &dev->transport);
            }
            transport_publish(dev);
        }
    }

    // Process through the plugin or chain with MIDI
    MH_MidiEvent midi_out[256];
    int num_midi_out = 0;

    // The _auto entry points are used only when there is something to
    // automate. With no pending change they are equivalent to the plain ones
    // (mh_process_auto delegates outright), but going through them
    // unconditionally would make every silent block pay for the branch and
    // would change the code path that years of use have exercised.
    const int have_params = num_param_changes > 0;

    // Every process entry point returns 0 on refusal (most commonly nframes
    // above the plugin's max block size). The return value used to be ignored,
    // and because output_buffers are allocated once and never cleared, a
    // refused block left the *previous* block's samples in place -- so the
    // device happily played stale audio on repeat, with no error anywhere.
    // Open-time validation now makes this unreachable in practice; the check
    // is kept so an unexpected refusal degrades to silence rather than a buzz.
    int processed;
    if (dev->chain) {
        // Process through plugin chain
        if (have_params) {
            processed = mh_chain_process_auto(dev->chain,
                              (const float* const*)dev->input_buffers,
                              dev->output_buffers,
                              frames,
                              midi_events, num_midi_events,
                              midi_out, 256, &num_midi_out,
                              param_changes, num_param_changes);
        } else if (num_midi_events > 0) {
            processed = mh_chain_process_midi_io(dev->chain,
                              (const float* const*)dev->input_buffers,
                              dev->output_buffers,
                              frames,
                              midi_events, num_midi_events,
                              midi_out, 256, &num_midi_out);
        } else {
            processed = mh_chain_process(dev->chain,
                       (const float* const*)dev->input_buffers,
                       dev->output_buffers,
                       frames);
        }
    } else {
        // Process through single plugin
        if (have_params) {
            // Project the drained array down to the single-plugin form. A
            // write addressed to a slot other than 0 has no meaning on a
            // device opened with mh_audio_open and is discarded rather than
            // applied to the only plugin there is.
            MH_ParamChange flat[MH_AUDIO_MAX_PARAM_CHANGES];
            int num_flat = 0;
            for (int i = 0; i < num_param_changes; i++) {
                if (param_changes[i].plugin_index != 0) continue;
                flat[num_flat].sample_offset = param_changes[i].sample_offset;
                flat[num_flat].param_index = param_changes[i].param_index;
                flat[num_flat].value = param_changes[i].value;
                num_flat++;
            }
            processed = mh_process_auto(dev->plugin,
                              (const float* const*)dev->input_buffers,
                              dev->output_buffers,
                              frames,
                              midi_events, num_midi_events,
                              midi_out, 256, &num_midi_out,
                              flat, num_flat);
        } else if (num_midi_events > 0) {
            processed = mh_process_midi_io(dev->plugin,
                              (const float* const*)dev->input_buffers,
                              dev->output_buffers,
                              frames,
                              midi_events, num_midi_events,
                              midi_out, 256, &num_midi_out);
        } else {
            processed = mh_process(dev->plugin,
                       (const float* const*)dev->input_buffers,
                       dev->output_buffers,
                       frames);
        }
    }

    if (!processed) {
        memset(interleaved_output, 0,
               (size_t)frame_count * channels * sizeof(float));
        return;
    }

    // Send MIDI output
    if (num_midi_out > 0 && dev->midi_out) {
        for (int i = 0; i < num_midi_out; i++) {
            unsigned char msg[3];
            msg[0] = midi_out[i].status;
            msg[1] = midi_out[i].data1;
            msg[2] = midi_out[i].data2;
            mh_midi_out_send(dev->midi_out, msg, 3);
        }
    }

    if (dev->transport_enabled) {
        transport_advance(dev, frames);
    }

    // Interleave output: non-interleaved [[L0,L1,...], [R0,R1,...]] -> interleaved [L0,R0,L1,R1,...]
    for (int f = 0; f < frames; f++) {
        for (int ch = 0; ch < channels; ch++) {
            interleaved_output[f * channels + ch] = dev->output_buffers[ch][f];
        }
    }

    // Zero any remaining frames if we clamped
    if ((int)frame_count > frames) {
        memset(interleaved_output + frames * channels, 0,
               ((int)frame_count - frames) * channels * sizeof(float));
    }
}

MH_AudioDevice* mh_audio_open(MH_Plugin* plugin, const MH_AudioConfig* config,
                               char* err_buf, size_t err_buf_size) {
    if (!plugin) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Plugin is NULL");
        }
        return NULL;
    }

    MH_AudioDevice* dev = (MH_AudioDevice*)calloc(1, sizeof(MH_AudioDevice));
    if (!dev) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate audio device");
        }
        return NULL;
    }

    dev->plugin = plugin;

    // Get plugin info
    MH_Info info;
    if (!mh_get_info(plugin, &info)) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to get plugin info");
        }
        free(dev);
        return NULL;
    }

    // Determine configuration
    double requested_sample_rate = (config && config->sample_rate > 0) ? config->sample_rate : 0;
    int requested_buffer_frames = (config && config->buffer_frames > 0) ? config->buffer_frames : 512;
    int requested_channels = (config && config->output_channels > 0) ? config->output_channels : info.num_output_ch;
    int capture = (config && config->capture) ? 1 : 0;

    dev->capture = capture;

    // Initialize miniaudio context
    ma_result result = ma_context_init(NULL, 0, NULL, &dev->context);
    if (result != MA_SUCCESS) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to initialize audio context: %d", result);
        }
        free(dev);
        return NULL;
    }

    // Configure device (duplex if capture requested, playback-only otherwise)
    ma_device_type dev_type = capture ? ma_device_type_duplex : ma_device_type_playback;
    ma_device_config device_config = ma_device_config_init(dev_type);
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = requested_channels;
    if (capture) {
        device_config.capture.format = ma_format_f32;
        device_config.capture.channels = requested_channels;
    }
    device_config.sampleRate = (ma_uint32)requested_sample_rate; // 0 = device default
    device_config.periodSizeInFrames = requested_buffer_frames;
    device_config.dataCallback = audio_callback;
    device_config.pUserData = dev;

    // Resolve explicit device selection (if any)
    const ma_device_id* playback_id = NULL;
    const ma_device_id* capture_id = NULL;
    result = resolve_device_ids(&dev->context, config, capture,
                                &playback_id, &capture_id, err_buf, err_buf_size);
    if (result != MA_SUCCESS) {
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }
    device_config.playback.pDeviceID = (ma_device_id*)playback_id;
    if (capture) {
        device_config.capture.pDeviceID = (ma_device_id*)capture_id;
    }

    // Initialize device
    result = ma_device_init(&dev->context, &device_config, &dev->device);
    if (result != MA_SUCCESS) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to initialize audio device: %d", result);
        }
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    // Store actual configuration
    dev->sample_rate = dev->device.sampleRate;
    dev->channels = dev->device.playback.channels;
    // Buffer frames: use period size, with some headroom
    dev->buffer_frames = dev->device.playback.internalPeriodSizeInFrames;
    if (dev->buffer_frames == 0) {
        dev->buffer_frames = requested_buffer_frames;
    }

    // If device sample rate differs from plugin, update plugin
    double plugin_sample_rate = mh_get_sample_rate(plugin);
    if (plugin_sample_rate != dev->sample_rate) {
        if (!mh_set_sample_rate(plugin, dev->sample_rate)) {
            if (err_buf && err_buf_size > 0) {
                snprintf(err_buf, err_buf_size,
                         "Failed to set plugin sample rate to match device (%.0f Hz)",
                         dev->sample_rate);
            }
            ma_device_uninit(&dev->device);
            ma_context_uninit(&dev->context);
            free(dev);
            return NULL;
        }
    }

    // Allocate conversion buffers with extra headroom. The channel count must
    // cover both what the device exchanges and what the plugin reads/writes --
    // see the buffer_channels note on MH_AudioDevice.
    dev->buffer_capacity = dev->buffer_frames * 2; // 2x headroom for safety

    if (!validate_block_size(mh_get_max_block_size(plugin), dev->buffer_frames,
                             "plugin", err_buf, err_buf_size)) {
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }
    dev->max_process_frames = mh_get_max_block_size(plugin);
    if (dev->max_process_frames > dev->buffer_capacity)
        dev->max_process_frames = dev->buffer_capacity;
    dev->buffer_channels = dev->channels;
    if (info.num_input_ch > dev->buffer_channels)
        dev->buffer_channels = info.num_input_ch;
    if (info.num_output_ch > dev->buffer_channels)
        dev->buffer_channels = info.num_output_ch;

    dev->input_buffers = alloc_channel_buffers(dev->buffer_channels, dev->buffer_capacity);
    if (!dev->input_buffers) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate input buffers");
        }
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    dev->output_buffers = alloc_channel_buffers(dev->buffer_channels, dev->buffer_capacity);
    if (!dev->output_buffers) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate output buffers");
        }
        free_channel_buffers(dev->input_buffers, dev->buffer_channels);
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    // Initialize MIDI
    dev->midi_in_port = -1;
    dev->midi_out_port = -1;

    // Create MIDI ring buffers
    dev->midi_in_buffer = mh_midi_ringbuffer_create(256);
    dev->midi_send_buffer = mh_midi_ringbuffer_create(256);
    dev->midi_out_buffer = mh_midi_ringbuffer_create(256);

    // Create parameter ring buffers. Sized well past any plausible control
    // rate: a surface sending a thousand values a second fills two of these
    // per second, and the audio thread drains them every few milliseconds.
    dev->param_ctl_buffer = mh_param_ringbuffer_create(1024);
    dev->param_send_buffer = mh_param_ringbuffer_create(1024);

    // Transport starts disabled and at a musically sane default, so enabling
    // it does not first hand the plugin a tempo of zero.
    dev->transport_commands = mh_transport_ringbuffer_create(64);
    dev->transport.bpm = 120.0;
    dev->transport.time_sig_numerator = 4;
    dev->transport.time_sig_denominator = 4;
    transport_resolve_targets(dev);

    // Connect MIDI ports if specified in config
    if (config) {
        if (config->midi_input_port >= 0) {
            char midi_err[256];
            dev->midi_in = mh_midi_in_open(config->midi_input_port,
                                           midi_input_callback, dev,
                                           midi_err, sizeof(midi_err));
            if (dev->midi_in) {
                dev->midi_in_port = config->midi_input_port;
            }
            // Don't fail if MIDI connection fails - audio still works
        }
        if (config->midi_output_port >= 0) {
            char midi_err[256];
            dev->midi_out = mh_midi_out_open(config->midi_output_port,
                                             midi_err, sizeof(midi_err));
            if (dev->midi_out) {
                dev->midi_out_port = config->midi_output_port;
            }
        }
    }

    return dev;
}

MH_AudioDevice* mh_audio_open_chain(MH_PluginChain* chain, const MH_AudioConfig* config,
                                     char* err_buf, size_t err_buf_size) {
    if (!chain) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Plugin chain is NULL");
        }
        return NULL;
    }

    int num_plugins = mh_chain_get_num_plugins(chain);
    if (num_plugins == 0) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Plugin chain is empty");
        }
        return NULL;
    }

    MH_AudioDevice* dev = (MH_AudioDevice*)calloc(1, sizeof(MH_AudioDevice));
    if (!dev) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate audio device");
        }
        return NULL;
    }

    dev->chain = chain;
    dev->plugin = NULL;  // Explicitly NULL since we're using a chain

    // Get chain info
    int num_in_ch = mh_chain_get_num_input_channels(chain);
    int num_out_ch = mh_chain_get_num_output_channels(chain);

    // Determine configuration
    double requested_sample_rate = (config && config->sample_rate > 0) ? config->sample_rate : 0;
    int requested_buffer_frames = (config && config->buffer_frames > 0) ? config->buffer_frames : 512;
    int requested_channels = (config && config->output_channels > 0) ? config->output_channels : num_out_ch;
    int capture = (config && config->capture) ? 1 : 0;

    dev->capture = capture;

    // Initialize miniaudio context
    ma_result result = ma_context_init(NULL, 0, NULL, &dev->context);
    if (result != MA_SUCCESS) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to initialize audio context: %d", result);
        }
        free(dev);
        return NULL;
    }

    // Configure device (duplex if capture requested, playback-only otherwise)
    ma_device_type dev_type = capture ? ma_device_type_duplex : ma_device_type_playback;
    ma_device_config device_config = ma_device_config_init(dev_type);
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = requested_channels;
    if (capture) {
        device_config.capture.format = ma_format_f32;
        device_config.capture.channels = requested_channels;
    }
    device_config.sampleRate = (ma_uint32)requested_sample_rate; // 0 = device default
    device_config.periodSizeInFrames = requested_buffer_frames;
    device_config.dataCallback = audio_callback;
    device_config.pUserData = dev;

    // Resolve explicit device selection (if any)
    const ma_device_id* playback_id = NULL;
    const ma_device_id* capture_id = NULL;
    result = resolve_device_ids(&dev->context, config, capture,
                                &playback_id, &capture_id, err_buf, err_buf_size);
    if (result != MA_SUCCESS) {
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }
    device_config.playback.pDeviceID = (ma_device_id*)playback_id;
    if (capture) {
        device_config.capture.pDeviceID = (ma_device_id*)capture_id;
    }

    // Initialize device
    result = ma_device_init(&dev->context, &device_config, &dev->device);
    if (result != MA_SUCCESS) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to initialize audio device: %d", result);
        }
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    // Store actual configuration
    dev->sample_rate = dev->device.sampleRate;
    dev->channels = dev->device.playback.channels;
    dev->buffer_frames = dev->device.playback.internalPeriodSizeInFrames;
    if (dev->buffer_frames == 0) {
        dev->buffer_frames = requested_buffer_frames;
    }

    // Note: For chains, we don't adjust sample rate of individual plugins here.
    // The chain was already created with all plugins at the same sample rate.
    // If the device sample rate differs, the caller should recreate the chain.
    double chain_sample_rate = mh_chain_get_sample_rate(chain);
    if (chain_sample_rate != dev->sample_rate) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size,
                     "Chain sample rate (%.0f Hz) differs from device (%.0f Hz). "
                     "Recreate chain with matching sample rate.",
                     chain_sample_rate, dev->sample_rate);
        }
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    // Allocate conversion buffers with extra headroom. The channel count must
    // cover both what the device exchanges and what the chain reads/writes --
    // see the buffer_channels note on MH_AudioDevice.
    dev->buffer_capacity = dev->buffer_frames * 2; // 2x headroom for safety

    if (!validate_block_size(mh_chain_get_max_block_size(chain),
                             dev->buffer_frames, "chain",
                             err_buf, err_buf_size)) {
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }
    dev->max_process_frames = mh_chain_get_max_block_size(chain);
    if (dev->max_process_frames > dev->buffer_capacity)
        dev->max_process_frames = dev->buffer_capacity;
    dev->buffer_channels = dev->channels;
    if (num_in_ch > dev->buffer_channels)
        dev->buffer_channels = num_in_ch;
    if (num_out_ch > dev->buffer_channels)
        dev->buffer_channels = num_out_ch;

    dev->input_buffers = alloc_channel_buffers(dev->buffer_channels, dev->buffer_capacity);
    if (!dev->input_buffers) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate input buffers");
        }
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    dev->output_buffers = alloc_channel_buffers(dev->buffer_channels, dev->buffer_capacity);
    if (!dev->output_buffers) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate output buffers");
        }
        free_channel_buffers(dev->input_buffers, dev->buffer_channels);
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    // Initialize MIDI
    dev->midi_in_port = -1;
    dev->midi_out_port = -1;

    // Create MIDI ring buffers
    dev->midi_in_buffer = mh_midi_ringbuffer_create(256);
    dev->midi_send_buffer = mh_midi_ringbuffer_create(256);
    dev->midi_out_buffer = mh_midi_ringbuffer_create(256);

    // Create parameter ring buffers. Sized well past any plausible control
    // rate: a surface sending a thousand values a second fills two of these
    // per second, and the audio thread drains them every few milliseconds.
    dev->param_ctl_buffer = mh_param_ringbuffer_create(1024);
    dev->param_send_buffer = mh_param_ringbuffer_create(1024);

    // Transport starts disabled and at a musically sane default, so enabling
    // it does not first hand the plugin a tempo of zero.
    dev->transport_commands = mh_transport_ringbuffer_create(64);
    dev->transport.bpm = 120.0;
    dev->transport.time_sig_numerator = 4;
    dev->transport.time_sig_denominator = 4;
    transport_resolve_targets(dev);

    // Connect MIDI ports if specified in config
    if (config) {
        if (config->midi_input_port >= 0) {
            char midi_err[256];
            dev->midi_in = mh_midi_in_open(config->midi_input_port,
                                           midi_input_callback, dev,
                                           midi_err, sizeof(midi_err));
            if (dev->midi_in) {
                dev->midi_in_port = config->midi_input_port;
            }
        }
        if (config->midi_output_port >= 0) {
            char midi_err[256];
            dev->midi_out = mh_midi_out_open(config->midi_output_port,
                                             midi_err, sizeof(midi_err));
            if (dev->midi_out) {
                dev->midi_out_port = config->midi_output_port;
            }
        }
    }

    return dev;
}

void mh_audio_close(MH_AudioDevice* dev) {
    if (!dev) return;

    // Stop if playing
    if (dev->is_playing) {
        ma_device_stop(&dev->device);
    }

    // Cleanup MIDI
    if (dev->midi_in) {
        mh_midi_in_close(dev->midi_in);
    }
    if (dev->midi_out) {
        mh_midi_out_close(dev->midi_out);
    }
    if (dev->midi_in_buffer) {
        mh_midi_ringbuffer_free(dev->midi_in_buffer);
    }
    if (dev->midi_send_buffer) {
        mh_midi_ringbuffer_free(dev->midi_send_buffer);
    }
    if (dev->midi_out_buffer) {
        mh_midi_ringbuffer_free(dev->midi_out_buffer);
    }
    // Before the ring it produces into is freed. mh_osc_server_close joins
    // the socket thread, so no push can be in flight afterwards.
    if (dev->osc_server) {
        mh_osc_server_close(dev->osc_server);
        dev->osc_server = NULL;
    }
    if (dev->param_ctl_buffer) {
        mh_param_ringbuffer_free(dev->param_ctl_buffer);
    }
    if (dev->param_send_buffer) {
        mh_param_ringbuffer_free(dev->param_send_buffer);
    }
    if (dev->transport_commands) {
        mh_transport_ringbuffer_free(dev->transport_commands);
    }

    // Cleanup audio input ring buffer
    if (dev->audio_in_buffer) {
        mh_audio_ringbuffer_free(dev->audio_in_buffer);
    }

    // Cleanup audio
    ma_device_uninit(&dev->device);
    ma_context_uninit(&dev->context);
    free_channel_buffers(dev->input_buffers, dev->buffer_channels);
    free_channel_buffers(dev->output_buffers, dev->buffer_channels);
    free(dev);
}

int mh_audio_start(MH_AudioDevice* dev) {
    if (!dev) return 0;
    if (dev->is_playing) return 1; // Already playing

    ma_result result = ma_device_start(&dev->device);
    if (result != MA_SUCCESS) {
        return 0;
    }

    dev->is_playing = 1;
    return 1;
}

int mh_audio_stop(MH_AudioDevice* dev) {
    if (!dev) return 0;
    if (!dev->is_playing) return 1; // Already stopped

    ma_result result = ma_device_stop(&dev->device);
    if (result != MA_SUCCESS) {
        return 0;
    }

    dev->is_playing = 0;
    return 1;
}

int mh_audio_is_playing(MH_AudioDevice* dev) {
    if (!dev) return 0;
    return dev->is_playing;
}

void mh_audio_set_input_callback(MH_AudioDevice* dev, MH_AudioInputCallback cb, void* user_data) {
    if (!dev) return;
    // Publish user_data before the callback pointer so the audio thread,
    // which loads the callback with acquire ordering, sees a matching
    // user_data once it observes a non-NULL callback (release store below).
    dev->input_callback_user_data = user_data;
    mh_atomic_store_release_ptr(&dev->input_callback, (void*)cb);
}

double mh_audio_get_sample_rate(MH_AudioDevice* dev) {
    if (!dev) return 0.0;
    return dev->sample_rate;
}

int mh_audio_get_buffer_frames(MH_AudioDevice* dev) {
    if (!dev) return 0;
    return dev->buffer_frames;
}

int mh_audio_get_channels(MH_AudioDevice* dev) {
    if (!dev) return 0;
    return dev->channels;
}

int mh_audio_connect_midi_input(MH_AudioDevice* dev, int port_index) {
    if (!dev) return 0;

    // Disconnect existing if any
    if (dev->midi_in) {
        mh_midi_in_close(dev->midi_in);
        dev->midi_in = NULL;
        dev->midi_in_port = -1;
        dev->midi_in_virtual = 0;
    }

    if (port_index < 0) {
        return 1;  // Just disconnect, success
    }

    char err[256];
    dev->midi_in = mh_midi_in_open(port_index, midi_input_callback, dev, err, sizeof(err));
    if (!dev->midi_in) {
        return 0;
    }

    dev->midi_in_port = port_index;
    dev->midi_in_virtual = 0;
    return 1;
}

int mh_audio_connect_midi_output(MH_AudioDevice* dev, int port_index) {
    if (!dev) return 0;

    // Disconnect existing if any
    if (dev->midi_out) {
        mh_midi_out_close(dev->midi_out);
        dev->midi_out = NULL;
        dev->midi_out_port = -1;
        dev->midi_out_virtual = 0;
    }

    if (port_index < 0) {
        return 1;  // Just disconnect, success
    }

    char err[256];
    dev->midi_out = mh_midi_out_open(port_index, err, sizeof(err));
    if (!dev->midi_out) {
        return 0;
    }

    dev->midi_out_port = port_index;
    dev->midi_out_virtual = 0;
    return 1;
}

int mh_audio_disconnect_midi_input(MH_AudioDevice* dev) {
    return mh_audio_connect_midi_input(dev, -1);
}

int mh_audio_disconnect_midi_output(MH_AudioDevice* dev) {
    return mh_audio_connect_midi_output(dev, -1);
}

int mh_audio_get_midi_input_port(MH_AudioDevice* dev) {
    if (!dev) return -1;
    return dev->midi_in_port;
}

int mh_audio_get_midi_output_port(MH_AudioDevice* dev) {
    if (!dev) return -1;
    return dev->midi_out_port;
}

int mh_audio_create_virtual_midi_input(MH_AudioDevice* dev, const char* port_name) {
    if (!dev || !port_name) return 0;

    // Disconnect existing if any
    if (dev->midi_in) {
        mh_midi_in_close(dev->midi_in);
        dev->midi_in = NULL;
        dev->midi_in_port = -1;
        dev->midi_in_virtual = 0;
    }

    char err[256];
    dev->midi_in = mh_midi_in_open_virtual(port_name, midi_input_callback, dev, err, sizeof(err));
    if (!dev->midi_in) {
        return 0;
    }

    dev->midi_in_port = -1;  // Virtual ports don't have an index
    dev->midi_in_virtual = 1;
    return 1;
}

int mh_audio_create_virtual_midi_output(MH_AudioDevice* dev, const char* port_name) {
    if (!dev || !port_name) return 0;

    // Disconnect existing if any
    if (dev->midi_out) {
        mh_midi_out_close(dev->midi_out);
        dev->midi_out = NULL;
        dev->midi_out_port = -1;
        dev->midi_out_virtual = 0;
    }

    char err[256];
    dev->midi_out = mh_midi_out_open_virtual(port_name, err, sizeof(err));
    if (!dev->midi_out) {
        return 0;
    }

    dev->midi_out_port = -1;  // Virtual ports don't have an index
    dev->midi_out_virtual = 1;
    return 1;
}

int mh_audio_is_midi_input_virtual(MH_AudioDevice* dev) {
    if (!dev) return 0;
    return dev->midi_in_virtual;
}

int mh_audio_is_midi_output_virtual(MH_AudioDevice* dev) {
    if (!dev) return 0;
    return dev->midi_out_virtual;
}

int mh_audio_send_midi(MH_AudioDevice* dev, unsigned char status, unsigned char data1, unsigned char data2) {
    // Note: NOT midi_in_buffer -- that ring has the libremidi input thread as
    // its single producer. See the struct comment.
    if (!dev || !dev->midi_send_buffer) return 0;

    MH_MidiEvent event;
    event.sample_offset = 0;  // Will be processed at start of next audio buffer
    event.status = status;
    event.data1 = data1;
    event.data2 = data2;

    return mh_midi_ringbuffer_push(dev->midi_send_buffer, &event);
}

// Parse a decimal integer occupying the whole of [begin, end).
// Returns -1 on anything else, including empty, negative and overflowing.
static int parse_index(const char* begin, const char* end) {
    if (begin >= end) return -1;
    long value = 0;
    for (const char* p = begin; p < end; p++) {
        if (*p < '0' || *p > '9') return -1;
        value = value * 10 + (*p - '0');
        if (value > 1000000) return -1;  // far past any real parameter count
    }
    return (int)value;
}

// Defined with the other slot-name plumbing further down, next to the setter
// whose invariants it depends on.
static int slot_for_name(MH_AudioDevice* dev, const char* begin, const char* end);

// Match "/mh/param/<index>", "/mh/<slot>/param/<index>" or
// "/mh/<name>/param/<index>".
// Returns 1 and fills the outputs on a match, 0 otherwise.
static int parse_param_address(MH_AudioDevice* dev, const char* address,
                               int* out_slot, int* out_param) {
    static const char prefix[] = "/mh/";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(address, prefix, prefix_len) != 0) return 0;

    const char* rest = address + prefix_len;

    // "/mh/param/<index>"
    static const char param[] = "param/";
    const size_t param_len = sizeof(param) - 1;
    if (strncmp(rest, param, param_len) == 0) {
        int index = parse_index(rest + param_len, rest + strlen(rest));
        if (index < 0) return 0;
        *out_slot = 0;
        *out_param = index;
        return 1;
    }

    // "/mh/<slot-or-name>/param/<index>"
    const char* slash = strchr(rest, '/');
    if (!slash) return 0;
    if (strncmp(slash + 1, param, param_len) != 0) return 0;
    const char* tail = slash + 1 + param_len;
    int index = parse_index(tail, tail + strlen(tail));
    if (index < 0) return 0;

    // A digit run is a position; anything else is a name. The two cannot be
    // confused because an acceptable slot name must start with a letter.
    int slot = parse_index(rest, slash);
    if (slot < 0) slot = slot_for_name(dev, rest, slash);
    if (slot < 0) return 0;

    *out_slot = slot;
    *out_param = index;
    return 1;
}

// Transport addresses, recognised alongside the parameter ones.
//
//   /mh/transport/play           (no argument, or non-zero to play)
//   /mh/transport/stop
//   /mh/transport/bpm      f
//   /mh/transport/position f     beats
//   /mh/transport/loop     f     non-zero enables
//   /mh/transport/record   f     non-zero arms
//
// Handled here rather than in the Python mapper so a transport surface works
// through the native path too, and so play/stop stays lock-free.
static int handle_transport_address(MH_AudioDevice* dev, const char* address,
                                    const float* args, int num_args) {
    static const char prefix[] = "/mh/transport/";
    const size_t prefix_len = sizeof(prefix) - 1;
    if (strncmp(address, prefix, prefix_len) != 0) return 0;

    const char* what = address + prefix_len;
    // A surface's button sends 1.0 on press and 0.0 on release; treating the
    // release as a command would make every press a press-and-undo.
    float value = (num_args >= 1 && args) ? args[0] : 1.0f;

    if (strcmp(what, "play") == 0) {
        if (value != 0.0f) mh_audio_transport_play(dev);
        return 1;
    }
    if (strcmp(what, "stop") == 0) {
        if (value != 0.0f) mh_audio_transport_stop(dev);
        return 1;
    }
    if (strcmp(what, "bpm") == 0) {
        if (num_args >= 1) mh_audio_transport_set_bpm(dev, (double)value);
        return 1;
    }
    if (strcmp(what, "position") == 0) {
        if (num_args >= 1) {
            // The wire carries beats, which is what a surface displays; the
            // playhead counts samples, which is what a plugin needs.
            double bpm = dev->transport.bpm > 0.0 ? dev->transport.bpm : 120.0;
            double sr = dev->sample_rate > 0 ? dev->sample_rate : 48000.0;
            double seconds = ((double)value) * (60.0 / bpm);
            long long samples = (long long)(seconds * sr);
            if (samples < 0) samples = 0;
            mh_audio_transport_set_position(dev, samples);
        }
        return 1;
    }
    if (strcmp(what, "loop") == 0) {
        if (num_args >= 1) {
            mh_audio_transport_set_loop(dev, value != 0.0f,
                                        dev->transport.loop_start_samples,
                                        dev->transport.loop_end_samples);
        }
        return 1;
    }
    if (strcmp(what, "record") == 0) {
        if (num_args >= 1) mh_audio_transport_set_recording(dev, value != 0.0f);
        return 1;
    }
    return 0;
}

// Called on the OSC socket thread. Pushes to the ring and returns; no lock,
// no allocation, no GIL.
static void osc_param_callback(const char* address, const float* args,
                               int num_args, void* user_data) {
    MH_AudioDevice* dev = (MH_AudioDevice*)user_data;
    if (!dev) return;

    if (handle_transport_address(dev, address, args, num_args)) return;

    if (!dev->param_ctl_buffer || num_args < 1 || !args) return;

    int slot = 0, param = 0;
    if (!parse_param_address(dev, address, &slot, &param)) return;

    mh_param_ringbuffer_push(dev->param_ctl_buffer, slot, param, args[0]);
}

// ---------------------------------------------------------------------------
// Host playhead
// ---------------------------------------------------------------------------

static int transport_push(MH_AudioDevice* dev, const MH_TransportCommand* cmd) {
    if (!dev || !dev->transport_commands) return 0;
    return mh_transport_ringbuffer_push(dev->transport_commands, cmd);
}

int mh_audio_set_transport_enabled(MH_AudioDevice* dev, int enabled) {
    if (!dev) return 0;
    // Read by the audio thread each block. A plain int store of 0 or 1 is the
    // one case where tearing is not a concern, and gating it behind the
    // command ring would mean the flag lags the commands that depend on it.
    dev->transport_enabled = enabled ? 1 : 0;
    return 1;
}

int mh_audio_get_transport_enabled(MH_AudioDevice* dev) {
    return dev ? dev->transport_enabled : 0;
}

int mh_audio_transport_play(MH_AudioDevice* dev) {
    MH_TransportCommand cmd = {0};
    cmd.type = MH_TRANSPORT_CMD_PLAY;
    return transport_push(dev, &cmd);
}

int mh_audio_transport_stop(MH_AudioDevice* dev) {
    MH_TransportCommand cmd = {0};
    cmd.type = MH_TRANSPORT_CMD_STOP;
    return transport_push(dev, &cmd);
}

int mh_audio_transport_set_bpm(MH_AudioDevice* dev, double bpm) {
    if (bpm <= 0.0) return 0;
    MH_TransportCommand cmd = {0};
    cmd.type = MH_TRANSPORT_CMD_SET_BPM;
    cmd.dvalue = bpm;
    return transport_push(dev, &cmd);
}

int mh_audio_transport_set_time_sig(MH_AudioDevice* dev, int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) return 0;
    MH_TransportCommand cmd = {0};
    cmd.type = MH_TRANSPORT_CMD_SET_TIME_SIG;
    cmd.ivalue = numerator;
    cmd.ivalue2 = denominator;
    return transport_push(dev, &cmd);
}

int mh_audio_transport_set_position(MH_AudioDevice* dev, long long position_samples) {
    if (position_samples < 0) return 0;
    MH_TransportCommand cmd = {0};
    cmd.type = MH_TRANSPORT_CMD_SET_POSITION_SAMPLES;
    cmd.lvalue = position_samples;
    return transport_push(dev, &cmd);
}

int mh_audio_transport_set_loop(MH_AudioDevice* dev, int enabled,
                                long long start_samples, long long end_samples) {
    if (enabled) {
        if (start_samples < 0 || end_samples <= start_samples) return 0;
    }
    MH_TransportCommand cmd = {0};
    cmd.type = MH_TRANSPORT_CMD_SET_LOOP;
    cmd.ivalue = enabled ? 1 : 0;
    cmd.lvalue = start_samples;
    cmd.lvalue2 = end_samples;
    return transport_push(dev, &cmd);
}

int mh_audio_transport_set_recording(MH_AudioDevice* dev, int recording) {
    MH_TransportCommand cmd = {0};
    cmd.type = MH_TRANSPORT_CMD_SET_RECORDING;
    cmd.ivalue = recording ? 1 : 0;
    return transport_push(dev, &cmd);
}

int mh_audio_get_transport(MH_AudioDevice* dev, MH_TransportInfo* out) {
    if (!dev || !out || !dev->transport_enabled) return 0;
    MH_TransportInfo* published =
        (MH_TransportInfo*)mh_atomic_load_acquire_ptr(&dev->transport_pub_current);
    if (!published) return 0;
    *out = *published;
    return 1;
}

// An acceptable slot name: alphanumeric, starting with a letter, non-empty and
// short enough to store. Starting with a letter is what keeps it distinct from
// the numeric form -- "/mh/2/param/0" must never be ambiguous.
static int slot_name_is_acceptable(const char* name) {
    if (!name || !*name) return 0;
    if (!((name[0] >= 'a' && name[0] <= 'z') || (name[0] >= 'A' && name[0] <= 'Z')))
        return 0;
    size_t len = strlen(name);
    if (len >= MH_SLOT_NAME_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9');
        if (!alnum) return 0;
    }
    return 1;
}

int mh_audio_set_slot_name(MH_AudioDevice* dev, int slot_index, const char* name) {
    if (!dev) return 0;
    if (slot_index < 0 || slot_index >= MH_MAX_SLOT_NAMES) return 0;
    // Refused once the socket thread exists, which is what lets it read the
    // table without synchronisation.
    if (dev->osc_server) return 0;

    if (!name) {
        dev->slot_names[slot_index][0] = '\0';
        return 1;
    }
    if (!slot_name_is_acceptable(name)) return 0;

    // Two slots sharing a name would make one of them unreachable, silently.
    for (int i = 0; i < MH_MAX_SLOT_NAMES; i++) {
        if (i != slot_index && strcmp(dev->slot_names[i], name) == 0) return 0;
    }

    snprintf(dev->slot_names[slot_index], MH_SLOT_NAME_LEN, "%s", name);
    return 1;
}

const char* mh_audio_get_slot_name(MH_AudioDevice* dev, int slot_index) {
    if (!dev || slot_index < 0 || slot_index >= MH_MAX_SLOT_NAMES) return NULL;
    return dev->slot_names[slot_index][0] ? dev->slot_names[slot_index] : NULL;
}

// Which slot a name segment refers to, or -1. A linear scan over at most 64
// short strings, on the socket thread; no lock and no allocation.
static int slot_for_name(MH_AudioDevice* dev, const char* begin, const char* end) {
    size_t len = (size_t)(end - begin);
    if (len == 0 || len >= MH_SLOT_NAME_LEN) return -1;
    for (int i = 0; i < MH_MAX_SLOT_NAMES; i++) {
        const char* candidate = dev->slot_names[i];
        if (!candidate[0]) continue;
        if (strlen(candidate) == len && strncmp(candidate, begin, len) == 0)
            return i;
    }
    return -1;
}

int mh_audio_connect_osc(MH_AudioDevice* dev, int port) {
    if (!dev) return 0;

    mh_audio_disconnect_osc(dev);

    char err[256] = {0};
    dev->osc_server = mh_osc_server_open(port, osc_param_callback, dev,
                                         err, sizeof(err));
    return dev->osc_server ? 1 : 0;
}

int mh_audio_disconnect_osc(MH_AudioDevice* dev) {
    if (!dev || !dev->osc_server) return 0;
    mh_osc_server_close(dev->osc_server);
    dev->osc_server = NULL;
    return 1;
}

int mh_audio_get_osc_port(MH_AudioDevice* dev) {
    if (!dev || !dev->osc_server) return -1;
    return mh_osc_server_get_port(dev->osc_server);
}

int mh_audio_send_param(MH_AudioDevice* dev, int plugin_index, int param_index, float value) {
    // Note: NOT param_ctl_buffer -- that ring belongs to the control-input
    // thread. See the struct comment.
    if (!dev || !dev->param_send_buffer) return 0;
    if (param_index < 0 || plugin_index < 0) return 0;
    return mh_param_ringbuffer_push(dev->param_send_buffer, plugin_index, param_index, value);
}

int mh_audio_send_param_control(MH_AudioDevice* dev, int plugin_index, int param_index, float value) {
    if (!dev || !dev->param_ctl_buffer) return 0;
    if (param_index < 0 || plugin_index < 0) return 0;
    return mh_param_ringbuffer_push(dev->param_ctl_buffer, plugin_index, param_index, value);
}

// Internal callback that reads from the audio ring buffer
static void audio_ringbuffer_input_callback(float* const* buffer, int nframes, void* user_data) {
    MH_AudioDevice* dev = (MH_AudioDevice*)user_data;
    if (!dev || !dev->audio_in_buffer) {
        // Silence fallback
        int channels = dev ? dev->channels : 0;
        for (int ch = 0; ch < channels; ch++) {
            memset(buffer[ch], 0, nframes * sizeof(float));
        }
        return;
    }
    mh_audio_ringbuffer_read_into(dev->audio_in_buffer, buffer, nframes, dev->channels);
}

int mh_audio_enable_input(MH_AudioDevice* dev, int capacity_frames) {
    if (!dev) return 0;

    // Free existing buffer if any
    if (dev->audio_in_buffer) {
        mh_audio_ringbuffer_free(dev->audio_in_buffer);
        dev->audio_in_buffer = NULL;
    }

    dev->audio_in_buffer = mh_audio_ringbuffer_create(dev->channels, capacity_frames);
    if (!dev->audio_in_buffer) return 0;

    // Install the ring buffer reader as the input callback
    mh_audio_set_input_callback(dev, audio_ringbuffer_input_callback, dev);
    return 1;
}

void mh_audio_disable_input(MH_AudioDevice* dev) {
    if (!dev) return;

    // Clear the input callback first (audio thread will see NULL and zero buffers)
    mh_audio_set_input_callback(dev, NULL, NULL);

    // Then free the ring buffer
    if (dev->audio_in_buffer) {
        mh_audio_ringbuffer_free(dev->audio_in_buffer);
        dev->audio_in_buffer = NULL;
    }
}

int mh_audio_write_input(MH_AudioDevice* dev, const float* data, int nframes) {
    if (!dev || !dev->audio_in_buffer || !data || nframes <= 0) return 0;
    return mh_audio_ringbuffer_push(dev->audio_in_buffer, data, nframes);
}

int mh_audio_input_available(MH_AudioDevice* dev) {
    if (!dev || !dev->audio_in_buffer) return 0;
    return mh_audio_ringbuffer_available(dev->audio_in_buffer);
}

// Shared enumeration helper. Set is_capture=0 for playback devices, 1 for capture.
static int enumerate_devices_impl(int is_capture,
                                  MH_AudioDeviceInfo* out_devices,
                                  int max_devices) {
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) {
        return -1;
    }

    ma_device_info* playback_infos = NULL;
    ma_uint32 playback_count = 0;
    ma_device_info* capture_infos = NULL;
    ma_uint32 capture_count = 0;

    if (ma_context_get_devices(&ctx, &playback_infos, &playback_count,
                                &capture_infos, &capture_count) != MA_SUCCESS) {
        ma_context_uninit(&ctx);
        return -1;
    }

    ma_device_info* infos = is_capture ? capture_infos : playback_infos;
    int total = is_capture ? (int)capture_count : (int)playback_count;

    if (out_devices && max_devices > 0) {
        int to_copy = total < max_devices ? total : max_devices;
        for (int i = 0; i < to_copy; i++) {
            strncpy(out_devices[i].name, infos[i].name, sizeof(out_devices[i].name) - 1);
            out_devices[i].name[sizeof(out_devices[i].name) - 1] = '\0';
            out_devices[i].is_default = infos[i].isDefault ? 1 : 0;
        }
    }

    ma_context_uninit(&ctx);
    return total;
}

int mh_audio_enumerate_playback_devices(MH_AudioDeviceInfo* out_devices, int max_devices) {
    return enumerate_devices_impl(0, out_devices, max_devices);
}

int mh_audio_enumerate_capture_devices(MH_AudioDeviceInfo* out_devices, int max_devices) {
    return enumerate_devices_impl(1, out_devices, max_devices);
}
