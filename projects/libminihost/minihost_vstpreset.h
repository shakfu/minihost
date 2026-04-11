// minihost_vstpreset.h
// Read/write Steinberg .vstpreset files (VST3 preset binary format).
//
// File layout:
//   [Header: 48 bytes]
//     0..3    char[4]   Magic 'VST3'
//     4..7    int32_LE  Version (1)
//     8..39   char[32]  Processor class ID (ASCII FUID)
//     40..47  int64_LE  Offset to chunk list
//
//   [Data area: variable]
//
//   [Chunk list]
//     0..3    char[4]   'List'
//     4..7    int32_LE  Entry count
//     8..     Entry[N]  20 bytes each: 4-byte chunk id, int64 offset, int64 size
//
// Chunk IDs:
//   'Comp' = component (processor) state
//   'Cont' = controller state (optional)
//
// Only 'Comp' and 'Cont' are recognised; any other chunks are ignored on read.
//
// All allocations use malloc/free. Pass the resulting pointer to
// mh_vstpreset_free() to release the buffer.

#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MH_VSTPRESET_CLASS_ID_LEN 32

// Parsed preset contents.
//
// class_id is NUL-terminated (up to 32 non-NUL chars followed by a NUL).
// component_state / controller_state are heap-allocated buffers owned by this
// struct. Sizes are the number of bytes in each buffer.
// If a chunk was not present in the file, the pointer is NULL and size is 0.
typedef struct MH_VstPreset {
    char class_id[MH_VSTPRESET_CLASS_ID_LEN + 1];
    void* component_state;
    int component_size;
    void* controller_state;
    int controller_size;
} MH_VstPreset;

// Read a .vstpreset file.
// On success, fills *out and returns 1.
// On failure, writes an error message to err_buf (if provided) and returns 0.
// The caller must call mh_vstpreset_free(out) on success.
int mh_vstpreset_read(const char* path, MH_VstPreset* out,
                      char* err_buf, size_t err_buf_size);

// Free any allocations owned by a MH_VstPreset and zero it out.
// Safe to call on a zero-initialised struct.
void mh_vstpreset_free(MH_VstPreset* preset);

// Write a .vstpreset file.
// class_id is truncated to 32 bytes or NUL-padded as needed.
// controller_state may be NULL (in which case only the 'Comp' chunk is written).
// Returns 1 on success, 0 on failure (writes error to err_buf).
int mh_vstpreset_write(const char* path,
                       const char* class_id,
                       const void* component_state, int component_size,
                       const void* controller_state, int controller_size,
                       char* err_buf, size_t err_buf_size);

// Read the processor class ID (FUID) from a VST3 bundle's moduleinfo.json.
//
// vst3_path: path to the .vst3 bundle (a directory on macOS/Linux, or a
//   bundle directory on Windows). Looks for
//   <vst3_path>/Contents/Resources/moduleinfo.json.
//
// Picks the first entry in the "Classes" array whose "Category" is
// "Audio Module Class" (the processor component) and copies its 32-char
// uppercase hex CID into out_class_id, NUL-terminating it.
//
// out_class_id: caller-provided buffer of at least
//   MH_VSTPRESET_CLASS_ID_LEN + 1 bytes. On success, contains a 32-char
//   uppercase hex string followed by '\0'.
//
// Returns 1 on success, 0 on failure (writes error to err_buf).
//
// Failure modes:
//   - moduleinfo.json missing (older plugins built against VST3 SDK < 3.7.5)
//   - file unreadable or larger than 1 MB
//   - JSON malformed beyond recovery
//   - no class with Category "Audio Module Class" present
//   - CID is not exactly 32 hex characters
//
// Note: this only works for VST3 plugins. For non-VST3 formats, callers
// must supply class_id explicitly to mh_vstpreset_write().
int mh_vstpreset_read_class_id_from_bundle(const char* vst3_path,
                                           char* out_class_id,
                                           char* err_buf, size_t err_buf_size);

#ifdef __cplusplus
}
#endif
