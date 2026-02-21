// minihost_audiofile.c
// Audio file read/write using miniaudio decoder/encoder + tflac for FLAC output

#include "minihost_audiofile.h"
#include "miniaudio.h"
#include "tflac.h"
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

static int write_wav(const char* path, const float* data,
                     unsigned int channels, unsigned int frames,
                     unsigned int sample_rate, int bit_depth,
                     char* err, size_t err_size) {
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
        ma_uint64 written = 0;
        result = ma_encoder_write_pcm_frames(&encoder, data, frames, &written);
    } else {
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

#define FLAC_BLOCKSIZE 4096

static int write_flac(const char* path, const float* data,
                      unsigned int channels, unsigned int frames,
                      unsigned int sample_rate, int bit_depth,
                      char* err, size_t err_size) {
    if (bit_depth != 16 && bit_depth != 24) {
        if (err && err_size > 0)
            snprintf(err, err_size,
                     "FLAC does not support %d-bit; use 16 or 24", bit_depth);
        return 0;
    }

    tflac t;
    tflac_init(&t);
    t.blocksize = FLAC_BLOCKSIZE;
    t.samplerate = sample_rate;
    t.channels = channels;
    t.bitdepth = (tflac_u32)bit_depth;

    tflac_u32 mem_size = tflac_size_memory(FLAC_BLOCKSIZE);
    void* mem = malloc(mem_size);
    if (!mem) {
        if (err && err_size > 0) snprintf(err, err_size, "Out of memory");
        return 0;
    }

    if (tflac_validate(&t, mem, mem_size) != 0) {
        free(mem);
        if (err && err_size > 0) snprintf(err, err_size, "tflac_validate failed");
        return 0;
    }

    tflac_u32 frame_buf_size = tflac_size_frame(FLAC_BLOCKSIZE, channels, (tflac_u32)bit_depth);
    void* frame_buf = malloc(frame_buf_size);
    if (!frame_buf) {
        free(mem);
        if (err && err_size > 0) snprintf(err, err_size, "Out of memory");
        return 0;
    }

    FILE* fp = fopen(path, "wb");
    if (!fp) {
        free(frame_buf);
        free(mem);
        if (err && err_size > 0) snprintf(err, err_size, "Failed to open file: %s", path);
        return 0;
    }

    // Write fLaC marker
    const unsigned char flac_marker[4] = {'f', 'L', 'a', 'C'};
    fwrite(flac_marker, 1, 4, fp);

    // Reserve space for STREAMINFO metadata block (4-byte header + 34-byte body = 38 bytes)
    unsigned char streaminfo_placeholder[38];
    memset(streaminfo_placeholder, 0, sizeof(streaminfo_placeholder));
    fwrite(streaminfo_placeholder, 1, 38, fp);

    // Allocate conversion buffer for one block of interleaved samples
    size_t block_samples = (size_t)FLAC_BLOCKSIZE * channels;
    void* conv_buf = NULL;
    if (bit_depth == 16) {
        conv_buf = malloc(block_samples * sizeof(tflac_s16));
    } else {
        conv_buf = malloc(block_samples * sizeof(tflac_s32));
    }
    if (!conv_buf) {
        fclose(fp);
        free(frame_buf);
        free(mem);
        if (err && err_size > 0) snprintf(err, err_size, "Out of memory");
        return 0;
    }

    int ok = 1;
    unsigned int pos = 0;
    while (pos < frames) {
        unsigned int block_frames = frames - pos;
        if (block_frames > FLAC_BLOCKSIZE) block_frames = FLAC_BLOCKSIZE;

        tflac_u32 used = 0;
        int r;
        ma_uint64 block_total = (ma_uint64)block_frames * channels;
        const float* block_data = data + (size_t)pos * channels;

        if (bit_depth == 16) {
            tflac_s16* s16_buf = (tflac_s16*)conv_buf;
            ma_pcm_f32_to_s16(s16_buf, block_data, block_total, ma_dither_mode_triangle);
            r = tflac_encode_s16i(&t, block_frames, s16_buf, frame_buf, frame_buf_size, &used);
        } else {
            // 24-bit: scale f32 to s32 range for 24-bit (shift into upper bits)
            tflac_s32* s32_buf = (tflac_s32*)conv_buf;
            for (ma_uint64 i = 0; i < block_total; i++) {
                float s = block_data[i];
                // Clamp to [-1, 1)
                if (s > 1.0f) s = 1.0f;
                if (s < -1.0f) s = -1.0f;
                // Scale to 24-bit range
                double scaled = (double)s * 8388607.0;
                tflac_s32 v = (tflac_s32)scaled;
                if (v > 8388607) v = 8388607;
                if (v < -8388607) v = -8388607;
                s32_buf[i] = v;
            }
            r = tflac_encode_s32i(&t, block_frames, s32_buf, frame_buf, frame_buf_size, &used);
        }

        if (r != 0) {
            if (err && err_size > 0)
                snprintf(err, err_size, "FLAC encode error %d at frame %u", r, pos);
            ok = 0;
            break;
        }

        if (fwrite(frame_buf, 1, used, fp) != used) {
            if (err && err_size > 0) snprintf(err, err_size, "Write error");
            ok = 0;
            break;
        }

        pos += block_frames;
    }

    if (ok) {
        tflac_finalize(&t);

        // Encode final STREAMINFO
        unsigned char si_buf[38];
        tflac_u32 si_used = 0;
        // lastflag=1 means this is the last metadata block
        if (tflac_encode_streaminfo(&t, 1, si_buf, sizeof(si_buf), &si_used) != 0) {
            if (err && err_size > 0) snprintf(err, err_size, "Failed to encode STREAMINFO");
            ok = 0;
        } else {
            // Seek back and overwrite STREAMINFO
            fseek(fp, 4, SEEK_SET);
            fwrite(si_buf, 1, si_used, fp);
        }
    }

    fclose(fp);
    free(conv_buf);
    free(frame_buf);
    free(mem);

    if (!ok) {
        remove(path);
    }

    return ok;
}

static const char* get_extension(const char* path) {
    const char* dot = strrchr(path, '.');
    return dot ? dot : "";
}

static int strcasecmp_ext(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        char cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int mh_audio_write(const char* path, const float* data,
                   unsigned int channels, unsigned int frames,
                   unsigned int sample_rate, int bit_depth,
                   char* err, size_t err_size) {
    if (!path || !data) {
        if (err && err_size > 0) snprintf(err, err_size, "Invalid arguments");
        return 0;
    }

    const char* ext = get_extension(path);

    if (strcasecmp_ext(ext, ".wav") == 0) {
        return write_wav(path, data, channels, frames, sample_rate, bit_depth, err, err_size);
    } else if (strcasecmp_ext(ext, ".flac") == 0) {
        return write_flac(path, data, channels, frames, sample_rate, bit_depth, err, err_size);
    } else {
        if (err && err_size > 0)
            snprintf(err, err_size, "Unsupported format '%s' (use .wav or .flac)", ext);
        return 0;
    }
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
