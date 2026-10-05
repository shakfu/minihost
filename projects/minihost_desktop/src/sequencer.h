// sequencer.h
//
// Note generation for SequencerNodeSpec. Each step's note, velocity and
// on/off decision is hashed from (seed, step), so any sample range can be
// generated independently: live chunks, loop wraps and offline blocks all
// agree. A step repeats every `steps` steps; `mutate` is the chance that one
// occurrence is hashed from its absolute index instead, and so varies.

#pragma once

#include "project.h"

#include <algorithm>
#include <array>
#include <utility>
#include <cmath>
#include <cstdint>

namespace minihost_desktop::project {

// Semitone offsets per scale, and their count.
inline const int* scaleSteps(Scale s, int& count)
{
    static const int major[]  = { 0, 2, 4, 5, 7, 9, 11 };
    static const int minor[]  = { 0, 2, 3, 5, 7, 8, 10 };
    static const int dorian[] = { 0, 2, 3, 5, 7, 9, 10 };
    static const int maj_p[]  = { 0, 2, 4, 7, 9 };
    static const int min_p[]  = { 0, 3, 5, 7, 10 };
    static const int chrom[]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    switch (s)
    {
    case Scale::Major:           count = 7;  return major;
    case Scale::Dorian:          count = 7;  return dorian;
    case Scale::MajorPentatonic: count = 5;  return maj_p;
    case Scale::MinorPentatonic: count = 5;  return min_p;
    case Scale::Chromatic:       count = 12; return chrom;
    case Scale::Minor:
    default:                     count = 7;  return minor;
    }
}

inline const juce::StringArray& scaleNames()
{
    static const juce::StringArray names{ "major", "minor", "dorian",
        "major_pentatonic", "minor_pentatonic", "chromatic" };
    return names;
}

// Clamps every field to its valid range (input from JSON or a dialog).
inline void clampSequencerParams(SequencerParams& p)
{
    p.steps   = std::clamp(p.steps, 1, 64);
    p.rate    = std::isfinite(p.rate) ? std::clamp(p.rate, 1.0 / 16.0, 4.0) : 0.25;
    p.root    = std::clamp(p.root, 0, 127);
    p.octaves = std::clamp(p.octaves, 1, 4);
    p.density = std::isfinite(p.density) ? std::clamp(p.density, 0.0, 1.0) : 0.7;
    p.gate    = std::isfinite(p.gate) ? std::clamp(p.gate, 0.01, 1.0) : 0.5;
    p.vel_min = std::clamp(p.vel_min, 1, 127);
    p.vel_max = std::clamp(p.vel_max, p.vel_min, 127);
    p.mutate  = std::isfinite(p.mutate) ? std::clamp(p.mutate, 0.0, 1.0) : 0.0;
    p.channel = std::clamp(p.channel, 1, 16);
    if ((int) p.scale < 0 || (int) p.scale > (int) Scale::Chromatic)
        p.scale = Scale::Minor;
}

// Step lengths offered in the dialog, in beats.
inline const std::array<std::pair<const char*, double>, 6>& sequencerRates()
{
    static const std::array<std::pair<const char*, double>, 6> rates{ {
        { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/8T", 1.0 / 3.0 },
        { "1/16", 0.25 }, { "1/16T", 1.0 / 6.0 }, { "1/32", 0.125 } } };
    return rates;
}

struct SequencerStep {
    bool on       = false;
    int  note     = 0;
    int  velocity = 0;
};

namespace detail {
inline uint64_t splitmix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
// Uniform in [0, 1).
inline double unit(uint64_t seed, uint64_t key, uint64_t salt)
{
    return (double) (splitmix(splitmix(seed) ^ splitmix(key * 8 + salt)) >> 11)
           * 0x1.0p-53;
}
} // namespace detail

inline SequencerStep sequencerStep(const SequencerParams& p, long long step)
{
    const long long steps = std::max(1, p.steps);
    uint64_t key = (uint64_t) (step % steps);
    if (p.mutate > 0.0 && detail::unit(p.seed, (uint64_t) step, 7) < p.mutate)
        key = (uint64_t) (steps + step);   // this occurrence only

    SequencerStep s;
    s.on = detail::unit(p.seed, key, 1) < p.density;
    int count = 0;
    const int* degrees = scaleSteps(p.scale, count);
    const int range = count * std::max(1, p.octaves);
    const int d = std::min(range - 1, (int) (detail::unit(p.seed, key, 2) * range));
    s.note = std::clamp(p.root + 12 * (d / count) + degrees[d % count], 0, 127);
    const int lo = std::clamp(std::min(p.vel_min, p.vel_max), 1, 127);
    const int hi = std::clamp(std::max(p.vel_min, p.vel_max), 1, 127);
    s.velocity = std::min(hi, lo + (int) (detail::unit(p.seed, key, 3) * (hi - lo + 1)));
    return s;
}

// Calls emit(sample_position, event) for every note-on and note-off at an
// absolute transport sample in [s0, s1), in time order. A note-off at the
// same sample as the next note-on comes first.
template <class Emit>
void sequencerEvents(const SequencerParams& p, double sample_rate, double bpm,
                     long long s0, long long s1, Emit&& emit)
{
    if (bpm <= 0.0 || p.rate <= 0.0 || s1 <= s0) return;
    const double step = p.rate * 60.0 * sample_rate / bpm;
    if (step < 1.0) return;
    const double gate = std::clamp(p.gate, 0.01, 1.0);
    const unsigned char ch = (unsigned char) (std::clamp(p.channel, 1, 16) - 1);

    // The earliest step whose note-off can still fall in [s0, s1).
    long long i = std::max(0LL, (long long) std::floor((double) s0 / step - gate) - 1);
    for (;; ++i)
    {
        const long long on = std::llround((double) i * step);
        if (on >= s1) break;
        const auto st = sequencerStep(p, i);
        if (!st.on) continue;
        const long long off = std::llround(((double) i + gate) * step);
        if (on >= s0)
            emit(on, MH_MidiEvent{ 0, (unsigned char) (0x90 | ch),
                                   (unsigned char) st.note,
                                   (unsigned char) st.velocity });
        if (off >= s0 && off < s1)
            emit(off, MH_MidiEvent{ 0, (unsigned char) (0x80 | ch),
                                    (unsigned char) st.note, 0 });
    }
}

} // namespace minihost_desktop::project
