// minihost_audiofile.c
// Audio file read/write using miniaudio decoder/encoder

#include "minihost_audiofile.h"
#include "miniaudio.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

MH_AudioData* mh_audio_read(const char* path, char* err, size_t err_size) {
    if (!path) {
        if (err && err_size > 0) snprintf(err, err_size, "Path is NULL");
        return NULL;
    }

    // Configure decoder: output as interleaved f32, preserve native channels/sample rate
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);

    ma_uint64 frame_count = 0;
    void* frames = NULL;
    ma_result result = ma_decode_file(path, &config, &frame_count, &frames);
    if (result != MA_SUCCESS) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "Failed to decode audio file: %s (error %d)", path, result);
        }
        return NULL;
    }

    // We need to know the actual channel count and sample rate from the file.
    // ma_decode_file doesn't return these directly when we pass 0.
    // Re-open a decoder just to get the info.
    ma_decoder decoder;
    ma_decoder_config info_config = ma_decoder_config_init(ma_format_f32, 0, 0);
    result = ma_decoder_init_file(path, &info_config, &decoder);
    if (result != MA_SUCCESS) {
        ma_free(frames, NULL);
        if (err && err_size > 0) {
            snprintf(err, err_size, "Failed to open audio file for info: %s (error %d)", path, result);
        }
        return NULL;
    }

    unsigned int channels = decoder.outputChannels;
    unsigned int sample_rate = decoder.outputSampleRate;
    ma_decoder_uninit(&decoder);

    MH_AudioData* data = (MH_AudioData*)malloc(sizeof(MH_AudioData));
    if (!data) {
        ma_free(frames, NULL);
        if (err && err_size > 0) snprintf(err, err_size, "Out of memory");
        return NULL;
    }

    data->data = (float*)frames;
    data->channels = channels;
    data->frames = (unsigned int)frame_count;
    data->sample_rate = sample_rate;

    return data;
}

void mh_audio_data_free(MH_AudioData* data) {
    if (!data) return;
    if (data->data) {
        ma_free(data->data, NULL);
    }
    free(data);
}

int mh_audio_write(const char* path, const float* data,
                   unsigned int channels, unsigned int frames,
                   unsigned int sample_rate, int bit_depth,
                   char* err, size_t err_size) {
    if (!path || !data) {
        if (err && err_size > 0) snprintf(err, err_size, "Invalid arguments");
        return 0;
    }

    ma_format format;
    switch (bit_depth) {
        case 16: format = ma_format_s16; break;
        case 24: format = ma_format_s24; break;
        case 32: format = ma_format_f32; break;
        default:
            if (err && err_size > 0)
                snprintf(err, err_size, "Unsupported bit depth: %d (use 16, 24, or 32)", bit_depth);
            return 0;
    }

    ma_encoder_config config = ma_encoder_config_init(
        ma_encoding_format_wav, format, channels, sample_rate);

    ma_encoder encoder;
    ma_result result = ma_encoder_init_file(path, &config, &encoder);
    if (result != MA_SUCCESS) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "Failed to open file for writing: %s (error %d)", path, result);
        }
        return 0;
    }

    ma_uint64 total_samples = (ma_uint64)frames * channels;

    if (format == ma_format_f32) {
        // Write float32 directly
        ma_uint64 written = 0;
        result = ma_encoder_write_pcm_frames(&encoder, data, frames, &written);
    } else {
        // Convert from float32 to target format, then write
        size_t bytes_per_sample = ma_get_bytes_per_sample(format);
        size_t buffer_size = total_samples * bytes_per_sample;

        void* converted = malloc(buffer_size);
        if (!converted) {
            ma_encoder_uninit(&encoder);
            if (err && err_size > 0) snprintf(err, err_size, "Out of memory");
            return 0;
        }

        if (format == ma_format_s16) {
            ma_pcm_f32_to_s16(converted, data, total_samples, ma_dither_mode_triangle);
        } else if (format == ma_format_s24) {
            ma_pcm_f32_to_s24(converted, data, total_samples, ma_dither_mode_triangle);
        }

        ma_uint64 written = 0;
        result = ma_encoder_write_pcm_frames(&encoder, converted, frames, &written);
        free(converted);
    }

    ma_encoder_uninit(&encoder);

    if (result != MA_SUCCESS) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "Failed to write audio data (error %d)", result);
        }
        return 0;
    }

    return 1;
}

int mh_audio_get_file_info(const char* path, MH_AudioFileInfo* info,
                           char* err, size_t err_size) {
    if (!path || !info) {
        if (err && err_size > 0) snprintf(err, err_size, "Invalid arguments");
        return 0;
    }

    memset(info, 0, sizeof(MH_AudioFileInfo));

    ma_decoder decoder;
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_result result = ma_decoder_init_file(path, &config, &decoder);
    if (result != MA_SUCCESS) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "Failed to open audio file: %s (error %d)", path, result);
        }
        return 0;
    }

    info->channels = decoder.outputChannels;
    info->sample_rate = decoder.outputSampleRate;

    ma_uint64 length = 0;
    result = ma_decoder_get_length_in_pcm_frames(&decoder, &length);
    if (result == MA_SUCCESS) {
        info->frames = (unsigned long long)length;
        if (info->sample_rate > 0) {
            info->duration = (double)length / (double)info->sample_rate;
        }
    }

    ma_decoder_uninit(&decoder);
    return 1;
}
