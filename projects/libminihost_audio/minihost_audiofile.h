// minihost_audiofile.h
// Audio file read/write using miniaudio decoder/encoder

#ifndef MINIHOST_AUDIOFILE_H
#define MINIHOST_AUDIOFILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decoded audio data (interleaved float32)
typedef struct {
    float* data;              // interleaved float32 samples
    unsigned int channels;
    unsigned int frames;
    unsigned int sample_rate;
} MH_AudioData;

// Read an audio file, decoding to interleaved float32.
// Supports WAV, FLAC, MP3, Vorbis.
// Returns NULL on error (writes message to err buffer).
// Caller must free with mh_audio_data_free().
MH_AudioData* mh_audio_read(const char* path, char* err, size_t err_size);

// Free decoded audio data.
void mh_audio_data_free(MH_AudioData* data);

// Write interleaved float32 data to a WAV file.
// bit_depth: 16, 24, or 32 (32 = IEEE float).
// Returns 1 on success, 0 on error.
int mh_audio_write(const char* path, const float* data,
                   unsigned int channels, unsigned int frames,
                   unsigned int sample_rate, int bit_depth,
                   char* err, size_t err_size);

// Audio file metadata (without full decode)
typedef struct {
    unsigned int channels;
    unsigned int sample_rate;
    unsigned long long frames;
    double duration;
} MH_AudioFileInfo;

// Get audio file metadata without decoding.
// Returns 1 on success, 0 on error.
int mh_audio_get_file_info(const char* path, MH_AudioFileInfo* info,
                           char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif // MINIHOST_AUDIOFILE_H
