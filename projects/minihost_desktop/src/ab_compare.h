// ab_compare.h
//
// A/B comparison of two renders: the difference between them (a null
// test) and a source that plays either one from the same position, so a
// switch is gapless and sample-aligned. GUI-free so `--ab-selftest`
// checks it headlessly.

#pragma once

#include "minihost_audiofile.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace minihost_desktop {

struct AbDiff {
    int    frames         = 0;      // compared: the shorter length
    int    channels       = 0;      // compared: the fewer channels
    bool   length_differs = false;  // frames or channels differ
    double rms_db         = -std::numeric_limits<double>::infinity();
    double peak_db        = -std::numeric_limits<double>::infinity();
    // Sample-identical over the compared span.
    bool   nulls() const { return std::isinf(peak_db); }
};

// Level of (a - b) over their common frames and channels, in dB relative to
// full scale. -inf when the renders are sample-identical there.
inline AbDiff compareAudio(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    AbDiff d;
    d.frames   = std::min(a.getNumSamples(), b.getNumSamples());
    d.channels = std::min(a.getNumChannels(), b.getNumChannels());
    d.length_differs = a.getNumSamples() != b.getNumSamples()
                    || a.getNumChannels() != b.getNumChannels();
    double sum = 0.0, peak = 0.0;
    for (int c = 0; c < d.channels; ++c)
    {
        const float* x = a.getReadPointer(c);
        const float* y = b.getReadPointer(c);
        for (int i = 0; i < d.frames; ++i)
        {
            const double e = (double) x[i] - (double) y[i];
            sum += e * e;
            peak = std::max(peak, std::abs(e));
        }
    }
    const double count = (double) d.frames * (double) d.channels;
    if (peak > 0.0 && count > 0.0)
    {
        d.rms_db  = 10.0 * std::log10(sum / count);
        d.peak_db = 20.0 * std::log10(peak);
    }
    return d;
}

// Decodes an audio file into a planar buffer.
inline bool readAudioFile(const juce::File& file, juce::AudioBuffer<float>& out,
                          double& sample_rate, juce::String& error)
{
    char err[256] = { 0 };
    MH_AudioData* ad = mh_audio_read(file.getFullPathName().toRawUTF8(), err, sizeof(err));
    if (ad == nullptr)
    {
        error = "failed to read " + file.getFullPathName() + ": " + juce::String(err);
        return false;
    }
    out.setSize((int) ad->channels, (int) ad->frames);
    for (int c = 0; c < (int) ad->channels; ++c)
    {
        float* dst = out.getWritePointer(c);
        for (int i = 0; i < (int) ad->frames; ++i)
            dst[i] = ad->data[(size_t) i * ad->channels + (size_t) c];
    }
    sample_rate = (double) ad->sample_rate;
    mh_audio_data_free(ad);
    return true;
}

// Plays buffer A or B from one shared position. select() takes effect at
// the next block. The shorter buffer is silent past its end. Both buffers
// must outlive the source and keep their size while it plays.
class AbSource final : public juce::PositionableAudioSource
{
public:
    AbSource(const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, bool loop)
        : a_(a), b_(b), loop_(loop) {}

    void select(bool b) { use_b_.store(b, std::memory_order_relaxed); }
    bool selected() const { return use_b_.load(std::memory_order_relaxed); }

    void prepareToPlay(int, double) override {}
    void releaseResources() override {}

    void getNextAudioBlock(const juce::AudioSourceChannelInfo& info) override
    {
        auto& dst = *info.buffer;
        const auto& src = use_b_.load(std::memory_order_relaxed) ? b_ : a_;
        const juce::int64 total = getTotalLength();
        juce::int64 pos = pos_.load(std::memory_order_relaxed);
        for (int done = 0; done < info.numSamples; )
        {
            if (total == 0 || (pos >= total && !loop_.load(std::memory_order_relaxed)))
            {
                dst.clear(info.startSample + done, info.numSamples - done);
                break;
            }
            pos %= total;
            const int len = (int) std::min<juce::int64>(info.numSamples - done, total - pos);
            for (int c = 0; c < dst.getNumChannels(); ++c)
            {
                const int avail = (int) std::clamp<juce::int64>(src.getNumSamples() - pos, 0, len);
                if (c < src.getNumChannels() && avail > 0)
                    dst.copyFrom(c, info.startSample + done, src, c, (int) pos, avail);
                if (avail < len || c >= src.getNumChannels())
                {
                    const int from = c < src.getNumChannels() ? avail : 0;
                    dst.clear(c, info.startSample + done + from, len - from);
                }
            }
            done += len;
            pos  += len;
        }
        pos_.store(pos, std::memory_order_relaxed);
    }

    void setNextReadPosition(juce::int64 p) override { pos_.store(p); }
    juce::int64 getNextReadPosition() const override { return pos_.load(); }
    juce::int64 getTotalLength() const override
    { return std::max(a_.getNumSamples(), b_.getNumSamples()); }
    bool isLooping() const override { return loop_.load(); }
    void setLooping(bool on) override { loop_.store(on); }

private:
    const juce::AudioBuffer<float>& a_;
    const juce::AudioBuffer<float>& b_;
    std::atomic<bool>               use_b_{ false };
    std::atomic<bool>               loop_;
    std::atomic<juce::int64>        pos_{ 0 };
};

} // namespace minihost_desktop
