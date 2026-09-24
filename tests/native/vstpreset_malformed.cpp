// .vstpreset chunk offsets and sizes with the top bit set.
//
// Offsets and sizes are int64 fields read from an untrusted file. Decoding
// them by shifting into a signed type is undefined once the last byte has
// its high bit set; the Python tests cover the same files, but only UBSan
// sees the shift. Built and run by `make native-tests`.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "minihost_vstpreset.h"

namespace {

int failures = 0;

void put_le(std::vector<unsigned char>& v, size_t at, unsigned long long x,
            int n) {
    for (int i = 0; i < n; ++i) v[at + i] = (unsigned char) (x >> (8 * i));
}

// Header, 4 bytes of data, then a chunk list with one 'Comp' entry.
std::vector<unsigned char> build(unsigned long long list_offset,
                                 unsigned long long chunk_offset,
                                 unsigned long long chunk_size) {
    std::vector<unsigned char> v(48 + 4 + 8 + 20, 0);
    std::memcpy(&v[0], "VST3", 4);
    put_le(v, 4, 1, 4);
    std::memcpy(&v[8], "0123456789ABCDEF0123456789ABCDEF", 32);
    put_le(v, 40, list_offset, 8);
    std::memcpy(&v[52], "List", 4);
    put_le(v, 56, 1, 4);
    std::memcpy(&v[60], "Comp", 4);
    put_le(v, 64, chunk_offset, 8);
    put_le(v, 72, chunk_size, 8);
    return v;
}

// Returns mh_vstpreset_read's result; *has_comp says whether a chunk loaded.
int read(const std::vector<unsigned char>& data, const std::string& path,
         bool* has_comp) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        return -1;
    }
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);

    MH_VstPreset p;
    char err[256];
    const int ok = mh_vstpreset_read(path.c_str(), &p, err, sizeof err);
    *has_comp = ok && p.component_state != nullptr;
    if (ok) mh_vstpreset_free(&p);
    std::remove(path.c_str());
    return ok;
}

void expect(const char* what, int want_ok, bool want_comp,
            const std::vector<unsigned char>& data, const std::string& path) {
    bool comp = false;
    const int ok = read(data, path, &comp);
    if (ok != want_ok || comp != want_comp) {
        std::fprintf(stderr, "FAIL: %s: want ok=%d comp=%d, got ok=%d comp=%d\n",
                     what, want_ok, (int) want_comp, ok, (int) comp);
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path =
        std::string(argc > 1 ? argv[1] : ".") + "/vstpreset_malformed.tmp";
    const unsigned long long top = 1ULL << 63;

    expect("valid chunk loads", 1, true, build(52, 48, 4), path);
    // A top-bit entry is skipped, as a negative one always was.
    expect("top-bit chunk offset", 1, false, build(52, top, 4), path);
    expect("top-bit chunk size", 1, false, build(52, 48, top | 4), path);
    expect("all-ones offset and size", 1, false, build(52, ~0ULL, ~0ULL), path);
    // A top-bit list offset is out of range, not a negative index.
    expect("top-bit list offset", 0, false, build(top | 52, 48, 4), path);

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("vstpreset_malformed: ok\n");
    return 0;
}
