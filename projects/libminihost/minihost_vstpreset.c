// minihost_vstpreset.c
// Read/write Steinberg .vstpreset files.
//
// Binary layout and chunk IDs are documented in minihost_vstpreset.h.

#include "minihost_vstpreset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEADER_SIZE 48
#define ENTRY_SIZE 20  // 4-byte id + int64 offset + int64 size
static const char MAGIC[4] = { 'V', 'S', 'T', '3' };
static const char CHUNK_LIST[4] = { 'L', 'i', 's', 't' };
static const char CHUNK_COMP[4] = { 'C', 'o', 'm', 'p' };
static const char CHUNK_CONT[4] = { 'C', 'o', 'n', 't' };

// ---- little-endian helpers ----------------------------------------------
// These explicitly byte-pack so the code is portable across host endianness.

static void write_le_i32(unsigned char* dst, int value) {
    dst[0] = (unsigned char)(value & 0xFF);
    dst[1] = (unsigned char)((value >> 8) & 0xFF);
    dst[2] = (unsigned char)((value >> 16) & 0xFF);
    dst[3] = (unsigned char)((value >> 24) & 0xFF);
}

static void write_le_i64(unsigned char* dst, long long value) {
    for (int i = 0; i < 8; i++) {
        dst[i] = (unsigned char)((value >> (8 * i)) & 0xFF);
    }
}

static int read_le_i32(const unsigned char* src) {
    return (int)((unsigned)src[0]
                 | ((unsigned)src[1] << 8)
                 | ((unsigned)src[2] << 16)
                 | ((unsigned)src[3] << 24));
}

static long long read_le_i64(const unsigned char* src) {
    long long v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((long long)src[i]) << (8 * i);
    }
    return v;
}

static void set_err(char* err_buf, size_t err_buf_size, const char* msg) {
    if (err_buf && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, "%s", msg);
    }
}

static void set_errf(char* err_buf, size_t err_buf_size, const char* fmt,
                     long long a, long long b) {
    if (err_buf && err_buf_size > 0) {
        snprintf(err_buf, err_buf_size, fmt, a, b);
    }
}

// ---- API ----------------------------------------------------------------

void mh_vstpreset_free(MH_VstPreset* preset) {
    if (!preset) return;
    free(preset->component_state);
    free(preset->controller_state);
    memset(preset, 0, sizeof(*preset));
}

int mh_vstpreset_read(const char* path, MH_VstPreset* out,
                      char* err_buf, size_t err_buf_size) {
    if (!path || !out) {
        set_err(err_buf, err_buf_size, "Invalid arguments");
        return 0;
    }
    memset(out, 0, sizeof(*out));

    FILE* f = fopen(path, "rb");
    if (!f) {
        set_err(err_buf, err_buf_size, "Cannot open preset file");
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err_buf, err_buf_size, "Failed to seek in preset file");
        return 0;
    }
    long flen = ftell(f);
    if (flen < 0) {
        fclose(f);
        set_err(err_buf, err_buf_size, "Failed to determine preset file size");
        return 0;
    }
    rewind(f);

    if (flen < HEADER_SIZE) {
        fclose(f);
        set_err(err_buf, err_buf_size, "File too small to be a .vstpreset");
        return 0;
    }

    // Load entire file into memory (presets are typically < 1 MB).
    unsigned char* data = (unsigned char*)malloc((size_t)flen);
    if (!data) {
        fclose(f);
        set_err(err_buf, err_buf_size, "Out of memory");
        return 0;
    }
    if (fread(data, 1, (size_t)flen, f) != (size_t)flen) {
        free(data);
        fclose(f);
        set_err(err_buf, err_buf_size, "Failed to read preset file");
        return 0;
    }
    fclose(f);

    if (memcmp(data, MAGIC, 4) != 0) {
        free(data);
        set_err(err_buf, err_buf_size, "Invalid .vstpreset magic");
        return 0;
    }

    int version = read_le_i32(data + 4);
    if (version != 1) {
        free(data);
        set_err(err_buf, err_buf_size, "Unsupported .vstpreset version");
        return 0;
    }

    // Copy class_id: 32 bytes, strip trailing NULs
    memcpy(out->class_id, data + 8, MH_VSTPRESET_CLASS_ID_LEN);
    out->class_id[MH_VSTPRESET_CLASS_ID_LEN] = '\0';
    // Strip trailing NULs so substring searches work
    for (int i = MH_VSTPRESET_CLASS_ID_LEN - 1; i >= 0; i--) {
        if (out->class_id[i] == '\0') continue;
        out->class_id[i + 1] = '\0';
        break;
    }

    long long list_offset = read_le_i64(data + 40);
    if (list_offset < HEADER_SIZE || list_offset >= flen) {
        free(data);
        set_errf(err_buf, err_buf_size,
                 "Invalid chunk list offset: %lld (file size: %lld)",
                 list_offset, (long long)flen);
        return 0;
    }
    if (list_offset + 8 > flen) {
        free(data);
        set_err(err_buf, err_buf_size, "Chunk list header truncated");
        return 0;
    }

    const unsigned char* list_ptr = data + list_offset;
    if (memcmp(list_ptr, CHUNK_LIST, 4) != 0) {
        free(data);
        set_err(err_buf, err_buf_size, "Invalid chunk list magic");
        return 0;
    }

    int entry_count = read_le_i32(list_ptr + 4);
    if (entry_count < 0 || entry_count > 128) {
        free(data);
        set_err(err_buf, err_buf_size, "Invalid chunk entry count");
        return 0;
    }

    long long entries_start = list_offset + 8;
    long long entries_end = entries_start + (long long)entry_count * ENTRY_SIZE;
    if (entries_end > flen) {
        free(data);
        set_err(err_buf, err_buf_size, "Chunk list entries truncated");
        return 0;
    }

    for (int i = 0; i < entry_count; i++) {
        const unsigned char* entry = data + entries_start + (long long)i * ENTRY_SIZE;
        long long chunk_offset = read_le_i64(entry + 4);
        long long chunk_size = read_le_i64(entry + 12);

        if (chunk_offset < 0 || chunk_size < 0) continue;
        if (chunk_offset + chunk_size > flen) {
            free(data);
            mh_vstpreset_free(out);
            set_err(err_buf, err_buf_size, "Chunk extends beyond file");
            return 0;
        }

        if (memcmp(entry, CHUNK_COMP, 4) == 0) {
            out->component_state = malloc((size_t)chunk_size);
            if (!out->component_state && chunk_size > 0) {
                free(data);
                mh_vstpreset_free(out);
                set_err(err_buf, err_buf_size, "Out of memory");
                return 0;
            }
            memcpy(out->component_state, data + chunk_offset, (size_t)chunk_size);
            out->component_size = (int)chunk_size;
        } else if (memcmp(entry, CHUNK_CONT, 4) == 0) {
            out->controller_state = malloc((size_t)chunk_size);
            if (!out->controller_state && chunk_size > 0) {
                free(data);
                mh_vstpreset_free(out);
                set_err(err_buf, err_buf_size, "Out of memory");
                return 0;
            }
            memcpy(out->controller_state, data + chunk_offset, (size_t)chunk_size);
            out->controller_size = (int)chunk_size;
        }
        // Unknown chunks are silently ignored (matches Python reader behaviour).
    }

    free(data);
    return 1;
}

int mh_vstpreset_write(const char* path,
                       const char* class_id,
                       const void* component_state, int component_size,
                       const void* controller_state, int controller_size,
                       char* err_buf, size_t err_buf_size) {
    if (!path || !class_id || class_id[0] == '\0') {
        set_err(err_buf, err_buf_size, "path and non-empty class_id are required");
        return 0;
    }
    if (component_size < 0 || controller_size < 0) {
        set_err(err_buf, err_buf_size, "Negative chunk size");
        return 0;
    }
    if (component_size > 0 && !component_state) {
        set_err(err_buf, err_buf_size, "component_state pointer is NULL but size > 0");
        return 0;
    }
    if (controller_size > 0 && !controller_state) {
        set_err(err_buf, err_buf_size, "controller_state pointer is NULL but size > 0");
        return 0;
    }

    int has_controller = (controller_state != NULL && controller_size > 0);
    int entry_count = has_controller ? 2 : 1;

    // Layout:
    //   header(48) + component_state + [controller_state] + chunk_list
    long long comp_offset = HEADER_SIZE;
    long long cont_offset = comp_offset + component_size;
    long long data_end = has_controller
        ? cont_offset + controller_size
        : comp_offset + component_size;
    long long list_offset = data_end;
    long long list_bytes = 8 + (long long)entry_count * ENTRY_SIZE;
    long long total = list_offset + list_bytes;

    unsigned char* buf = (unsigned char*)calloc(1, (size_t)total);
    if (!buf) {
        set_err(err_buf, err_buf_size, "Out of memory");
        return 0;
    }

    // Header
    memcpy(buf, MAGIC, 4);
    write_le_i32(buf + 4, 1);
    // class_id: up to 32 ASCII chars, NUL-padded
    size_t cid_len = strlen(class_id);
    if (cid_len > MH_VSTPRESET_CLASS_ID_LEN) cid_len = MH_VSTPRESET_CLASS_ID_LEN;
    memcpy(buf + 8, class_id, cid_len);
    // Remaining bytes are already zero from calloc.
    write_le_i64(buf + 40, list_offset);

    // Data area
    if (component_size > 0) {
        memcpy(buf + comp_offset, component_state, (size_t)component_size);
    }
    if (has_controller) {
        memcpy(buf + cont_offset, controller_state, (size_t)controller_size);
    }

    // Chunk list
    unsigned char* list_ptr = buf + list_offset;
    memcpy(list_ptr, CHUNK_LIST, 4);
    write_le_i32(list_ptr + 4, entry_count);

    unsigned char* entry_ptr = list_ptr + 8;
    memcpy(entry_ptr, CHUNK_COMP, 4);
    write_le_i64(entry_ptr + 4, comp_offset);
    write_le_i64(entry_ptr + 12, component_size);
    entry_ptr += ENTRY_SIZE;

    if (has_controller) {
        memcpy(entry_ptr, CHUNK_CONT, 4);
        write_le_i64(entry_ptr + 4, cont_offset);
        write_le_i64(entry_ptr + 12, controller_size);
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        free(buf);
        set_err(err_buf, err_buf_size, "Cannot open preset file for writing");
        return 0;
    }
    size_t written = fwrite(buf, 1, (size_t)total, f);
    int ok = (fclose(f) == 0) && (written == (size_t)total);
    free(buf);
    if (!ok) {
        set_err(err_buf, err_buf_size, "Failed to write preset file");
        return 0;
    }
    return 1;
}
