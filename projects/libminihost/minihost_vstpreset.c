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

// ---- moduleinfo.json scanner --------------------------------------------
//
// VST3 SDK 3.7.5+ plugins ship a `Contents/Resources/moduleinfo.json` file
// inside the .vst3 bundle. The format is JSON5 (allows trailing commas), and
// the processor class ID we want is the first entry in the "Classes" array
// whose "Category" is "Audio Module Class". We don't pull in a JSON library;
// instead we do a string-aware linear scan that:
//
//   1. Skips characters inside double-quoted strings (handles `\"` escapes).
//   2. Tracks brace/bracket nesting depth so we can find object boundaries.
//   3. Recognises "key": "value" patterns by matching literal key strings.
//
// This is sufficient because the schema is fixed and well-known; we don't
// need to validate the surrounding structure or handle edge cases that
// real-world bundles never produce.

#define MH_MODULEINFO_MAX_BYTES (1024 * 1024)  // 1 MB cap

// Skip whitespace. Returns the new offset.
static size_t mi_skip_ws(const char* s, size_t i, size_t n) {
    while (i < n) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }
        break;
    }
    return i;
}

// Skip a JSON string starting at s[i], where s[i] must be '"'. Returns
// the offset of the byte AFTER the closing quote, or n on error.
static size_t mi_skip_string(const char* s, size_t i, size_t n) {
    if (i >= n || s[i] != '"') return n;
    i++;  // skip opening quote
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            // Skip escape sequence (we don't decode it)
            i += 2;
            continue;
        }
        if (c == '"') return i + 1;
        i++;
    }
    return n;
}

// Find the next occurrence of `key_quoted` (a literal like "\"CID\"") in
// s[start..end), skipping over quoted strings so we don't match inside one.
// Returns the offset of the first character of the match, or end if not found.
static size_t mi_find_key(const char* s, size_t start, size_t end,
                          const char* key_quoted) {
    size_t klen = strlen(key_quoted);
    size_t i = start;
    while (i < end) {
        char c = s[i];
        if (c == '"') {
            // We're at the start of a string. Check if it matches the key.
            if (i + klen <= end && memcmp(s + i, key_quoted, klen) == 0) {
                // Confirm the next non-ws character is ':' so this is a key,
                // not a value that happens to equal the key text.
                size_t j = mi_skip_ws(s, i + klen, end);
                if (j < end && s[j] == ':') return i;
            }
            i = mi_skip_string(s, i, end);
            continue;
        }
        i++;
    }
    return end;
}

// Parse a string value at s[i] (must point to '"'). Copies the unescaped
// content into out_buf (truncated to out_size - 1) and NUL-terminates.
// We don't actually decode escapes — for our keys (CID, Category) the
// values are plain ASCII without escapes.
// Returns the offset after the closing quote, or n on error.
static size_t mi_parse_string(const char* s, size_t i, size_t n,
                              char* out_buf, size_t out_size) {
    if (i >= n || s[i] != '"') return n;
    i++;  // skip opening quote
    size_t out_i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\\') {
            // Copy the escaped char literally
            if (i + 1 < n && out_i + 1 < out_size) {
                out_buf[out_i++] = s[i + 1];
            }
            i += 2;
            continue;
        }
        if (c == '"') {
            if (out_size > 0) out_buf[out_i] = '\0';
            return i + 1;
        }
        if (out_i + 1 < out_size) out_buf[out_i++] = c;
        i++;
    }
    if (out_size > 0) out_buf[out_size - 1] = '\0';
    return n;
}

// After matching `mi_find_key`, advance past the key and the ':' to the
// start of the value. Returns the offset of the first non-ws character of
// the value, or n on error.
static size_t mi_advance_to_value(const char* s, size_t key_offset, size_t n,
                                  const char* key_quoted) {
    size_t i = key_offset + strlen(key_quoted);
    i = mi_skip_ws(s, i, n);
    if (i >= n || s[i] != ':') return n;
    i++;  // skip ':'
    return mi_skip_ws(s, i, n);
}

// Find the matching closing brace for an opening brace at s[i]. s[i] must
// be '{'. Tracks nesting and skips strings. Returns the offset of the
// matching '}', or n on error.
static size_t mi_find_matching_brace(const char* s, size_t i, size_t n) {
    if (i >= n || s[i] != '{') return n;
    int depth = 0;
    while (i < n) {
        char c = s[i];
        if (c == '"') { i = mi_skip_string(s, i, n); continue; }
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') {
            depth--;
            if (depth == 0) return i;
            i++;
            continue;
        }
        i++;
    }
    return n;
}

// Validate that `s` is exactly 32 hex characters and uppercase it in place.
static int mi_validate_and_uppercase_cid(char* s) {
    int len = 0;
    while (s[len] != '\0') len++;
    if (len != MH_VSTPRESET_CLASS_ID_LEN) return 0;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') continue;
        if (c >= 'A' && c <= 'F') continue;
        if (c >= 'a' && c <= 'f') { s[i] = (char)(c - 'a' + 'A'); continue; }
        return 0;
    }
    return 1;
}

int mh_vstpreset_read_class_id_from_bundle(const char* vst3_path,
                                           char* out_class_id,
                                           char* err_buf, size_t err_buf_size) {
    if (!vst3_path || !out_class_id) {
        set_err(err_buf, err_buf_size, "Invalid arguments");
        return 0;
    }
    out_class_id[0] = '\0';

    // Build "<vst3_path>/Contents/Resources/moduleinfo.json".
    // Strip a trailing path separator if present.
    char json_path[2048];
    size_t plen = strlen(vst3_path);
    while (plen > 0 && (vst3_path[plen - 1] == '/' || vst3_path[plen - 1] == '\\')) {
        plen--;
    }
    int written = snprintf(json_path, sizeof(json_path),
                           "%.*s/Contents/Resources/moduleinfo.json",
                           (int)plen, vst3_path);
    if (written < 0 || (size_t)written >= sizeof(json_path)) {
        set_err(err_buf, err_buf_size, "Plugin path too long");
        return 0;
    }

    FILE* f = fopen(json_path, "rb");
    if (!f) {
        if (err_buf && err_buf_size > 0) {
            snprintf(err_buf, err_buf_size,
                     "moduleinfo.json not found at %s "
                     "(plugin may predate VST3 SDK 3.7.5)", json_path);
        }
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        set_err(err_buf, err_buf_size, "Failed to seek moduleinfo.json");
        return 0;
    }
    long flen = ftell(f);
    if (flen < 0) {
        fclose(f);
        set_err(err_buf, err_buf_size, "Failed to size moduleinfo.json");
        return 0;
    }
    if (flen > MH_MODULEINFO_MAX_BYTES) {
        fclose(f);
        set_err(err_buf, err_buf_size, "moduleinfo.json exceeds 1 MB limit");
        return 0;
    }
    rewind(f);

    char* data = (char*)malloc((size_t)flen + 1);
    if (!data) {
        fclose(f);
        set_err(err_buf, err_buf_size, "Out of memory");
        return 0;
    }
    if (fread(data, 1, (size_t)flen, f) != (size_t)flen) {
        free(data);
        fclose(f);
        set_err(err_buf, err_buf_size, "Failed to read moduleinfo.json");
        return 0;
    }
    fclose(f);
    data[flen] = '\0';
    size_t n = (size_t)flen;

    // Locate the "Classes" array.
    size_t classes_key = mi_find_key(data, 0, n, "\"Classes\"");
    if (classes_key >= n) {
        free(data);
        set_err(err_buf, err_buf_size, "moduleinfo.json has no \"Classes\" key");
        return 0;
    }
    size_t i = mi_advance_to_value(data, classes_key, n, "\"Classes\"");
    if (i >= n || data[i] != '[') {
        free(data);
        set_err(err_buf, err_buf_size, "\"Classes\" value is not an array");
        return 0;
    }
    i++;  // step past '['

    // Walk the array. Each top-level '{' opens a class object; we read
    // its body, look for "Category" and "CID", and pick the first whose
    // category equals "Audio Module Class".
    while (i < n) {
        i = mi_skip_ws(data, i, n);
        if (i >= n) break;
        if (data[i] == ']') break;             // end of array
        if (data[i] == ',') { i++; continue; } // separator (or trailing)
        if (data[i] != '{') { i++; continue; } // tolerate stray chars

        size_t obj_start = i;
        size_t obj_end = mi_find_matching_brace(data, i, n);
        if (obj_end >= n) {
            free(data);
            set_err(err_buf, err_buf_size,
                    "Unterminated class object in moduleinfo.json");
            return 0;
        }

        // Look for "Category": "Audio Module Class" and "CID": "<32 hex>".
        char category[64] = { 0 };
        char cid[MH_VSTPRESET_CLASS_ID_LEN + 1] = { 0 };

        size_t cat_key = mi_find_key(data, obj_start, obj_end, "\"Category\"");
        if (cat_key < obj_end) {
            size_t v = mi_advance_to_value(data, cat_key, obj_end, "\"Category\"");
            if (v < obj_end && data[v] == '"') {
                mi_parse_string(data, v, obj_end, category, sizeof(category));
            }
        }

        if (strcmp(category, "Audio Module Class") == 0) {
            size_t cid_key = mi_find_key(data, obj_start, obj_end, "\"CID\"");
            if (cid_key < obj_end) {
                size_t v = mi_advance_to_value(data, cid_key, obj_end, "\"CID\"");
                if (v < obj_end && data[v] == '"') {
                    mi_parse_string(data, v, obj_end, cid, sizeof(cid));
                }
            }
            if (cid[0] != '\0') {
                if (!mi_validate_and_uppercase_cid(cid)) {
                    free(data);
                    if (err_buf && err_buf_size > 0) {
                        snprintf(err_buf, err_buf_size,
                                 "CID is not 32 hex characters: %s", cid);
                    }
                    return 0;
                }
                // Copy out (caller buffer is at least
                // MH_VSTPRESET_CLASS_ID_LEN + 1 bytes).
                memcpy(out_class_id, cid, MH_VSTPRESET_CLASS_ID_LEN);
                out_class_id[MH_VSTPRESET_CLASS_ID_LEN] = '\0';
                free(data);
                return 1;
            }
        }

        i = obj_end + 1;  // move past '}'
    }

    free(data);
    set_err(err_buf, err_buf_size,
            "No \"Audio Module Class\" entry found in moduleinfo.json");
    return 0;
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
