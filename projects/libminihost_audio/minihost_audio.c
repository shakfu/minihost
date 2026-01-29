// minihost_audio.c
// Real-time audio playback using miniaudio

#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "minihost_audio.h"
#include "minihost_midi.h"
#include "midi_ringbuffer.h"
#include "minihost.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct MH_AudioDevice {
    ma_device device;
    ma_context context;
    MH_Plugin* plugin;

    // Audio configuration
    double sample_rate;
    int buffer_frames;
    int channels;

    // Input callback for effects
    MH_AudioInputCallback input_callback;
    void* input_callback_user_data;

    // Pre-allocated conversion buffers (non-interleaved)
    float** input_buffers;   // [channel][frame]
    float** output_buffers;  // [channel][frame]
    int buffer_capacity;     // frames allocated

    // MIDI I/O
    MH_MidiIn* midi_in;
    MH_MidiOut* midi_out;
    MH_MidiRingBuffer* midi_in_buffer;   // MIDI thread -> audio thread
    MH_MidiRingBuffer* midi_out_buffer;  // audio thread -> MIDI output
    int midi_in_port;   // -1 if not connected or virtual
    int midi_out_port;  // -1 if not connected or virtual
    int midi_in_virtual;   // 1 if virtual port, 0 if physical
    int midi_out_virtual;  // 1 if virtual port, 0 if physical

    // State
    int is_playing;
};

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

// Audio callback - called from miniaudio's audio thread
static void audio_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    MH_AudioDevice* dev = (MH_AudioDevice*)device->pUserData;
    (void)input; // We don't use capture input

    float* interleaved_output = (float*)output;
    int channels = dev->channels;
    int frames = (int)frame_count;

    // Clamp to our buffer capacity
    if (frames > dev->buffer_capacity) {
        frames = dev->buffer_capacity;
    }

    // Get input audio (for effects) or zero the buffers (for synths)
    if (dev->input_callback) {
        dev->input_callback(dev->input_buffers, frames, dev->input_callback_user_data);
    } else {
        // Zero input buffers for synth plugins
        for (int ch = 0; ch < channels; ch++) {
            memset(dev->input_buffers[ch], 0, frames * sizeof(float));
        }
    }

    // Drain MIDI input buffer
    MH_MidiEvent midi_events[256];
    int num_midi_events = 0;
    if (dev->midi_in_buffer) {
        num_midi_events = mh_midi_ringbuffer_pop_all(dev->midi_in_buffer, midi_events, 256);
    }

    // Process through the plugin with MIDI
    MH_MidiEvent midi_out[256];
    int num_midi_out = 0;

    if (num_midi_events > 0) {
        mh_process_midi_io(dev->plugin,
                          (const float* const*)dev->input_buffers,
                          dev->output_buffers,
                          frames,
                          midi_events, num_midi_events,
                          midi_out, 256, &num_midi_out);
    } else {
        mh_process(dev->plugin,
                   (const float* const*)dev->input_buffers,
                   dev->output_buffers,
                   frames);
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

    // Initialize miniaudio context
    ma_result result = ma_context_init(NULL, 0, NULL, &dev->context);
    if (result != MA_SUCCESS) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to initialize audio context: %d", result);
        }
        free(dev);
        return NULL;
    }

    // Configure device
    ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = requested_channels;
    device_config.sampleRate = (ma_uint32)requested_sample_rate; // 0 = device default
    device_config.periodSizeInFrames = requested_buffer_frames;
    device_config.dataCallback = audio_callback;
    device_config.pUserData = dev;

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

    // Allocate conversion buffers with extra headroom
    dev->buffer_capacity = dev->buffer_frames * 2; // 2x headroom for safety

    dev->input_buffers = alloc_channel_buffers(dev->channels, dev->buffer_capacity);
    if (!dev->input_buffers) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate input buffers");
        }
        ma_device_uninit(&dev->device);
        ma_context_uninit(&dev->context);
        free(dev);
        return NULL;
    }

    dev->output_buffers = alloc_channel_buffers(dev->channels, dev->buffer_capacity);
    if (!dev->output_buffers) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size, "Failed to allocate output buffers");
        }
        free_channel_buffers(dev->input_buffers, dev->channels);
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
    dev->midi_out_buffer = mh_midi_ringbuffer_create(256);

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
    if (dev->midi_out_buffer) {
        mh_midi_ringbuffer_free(dev->midi_out_buffer);
    }

    // Cleanup audio
    ma_device_uninit(&dev->device);
    ma_context_uninit(&dev->context);
    free_channel_buffers(dev->input_buffers, dev->channels);
    free_channel_buffers(dev->output_buffers, dev->channels);
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
    dev->input_callback = cb;
    dev->input_callback_user_data = user_data;
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
