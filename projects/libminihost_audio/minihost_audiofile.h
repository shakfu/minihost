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

// Write interleaved float32 data to an audio file (WAV or FLAC).
// bit_depth: 16, 24, or 32 (32 = IEEE float).
// Returns 1 on success, 0 on error.
int mh_audio_write(const char* path, const float* data,
                   unsigned int channels, unsigned int frames,
                   unsigned int sample_rate, int bit_depth,
                   char* err, size_t err_size);

// Broadcast Wave Format (BWF) metadata written into a WAV `bext` chunk
// (EBU Tech 3285). All string fields are optional (NULL is treated as empty)
// and are truncated to the field's fixed size. Dates/times follow the BWF
// convention: origination_date "yyyy-mm-dd" (<=10 chars), origination_time
// "hh:mm:ss" (<=8 chars). time_reference is the sample count from midnight of
// the recording's start (a timecode anchor for film/broadcast alignment).
typedef struct {
    const char* description;            // free text, <=256 chars
    const char* originator;             // <=32 chars
    const char* originator_reference;   // <=32 chars
    const char* origination_date;       // "yyyy-mm-dd", <=10 chars
    const char* origination_time;       // "hh:mm:ss", <=8 chars
    unsigned long long time_reference;  // samples since midnight
} MH_BwfMetadata;

// Write interleaved float32 data to a WAV file, optionally embedding a BWF
// `bext` chunk. Identical to mh_audio_write when `bwf` is NULL. BWF metadata
// is only supported for WAV output (bit_depth 16/24/32); passing non-NULL
// `bwf` with a non-WAV path returns an error. Returns 1 on success, 0 on
// error.
int mh_audio_write_bwf(const char* path, const float* data,
                       unsigned int channels, unsigned int frames,
                       unsigned int sample_rate, int bit_depth,
                       const MH_BwfMetadata* bwf,
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

// Resample interleaved float32 audio data.
// data_in: interleaved float32 samples [frames_in * channels]
// Returns a new MH_AudioData with resampled audio, or NULL on error.
// Caller must free with mh_audio_data_free().
MH_AudioData* mh_audio_resample(const float* data_in,
                                unsigned int channels,
                                unsigned int frames_in,
                                unsigned int sample_rate_in,
                                unsigned int sample_rate_out,
                                char* err, size_t err_size);

#ifdef __cplusplus
}
#endif

#endif // MINIHOST_AUDIOFILE_H
