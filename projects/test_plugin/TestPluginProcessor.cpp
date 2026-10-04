// TestPluginProcessor.cpp
// See TestPluginProcessor.h for what this fixture guarantees.

#include "TestPluginProcessor.h"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {

// Thread that constructed the first instance: the host's plugin thread.
std::thread::id gCreatorThread;

// With MINIHOST_TEST_AFFINITY_LOG set to a path, append `what` to that file
// whenever a control callback runs on any other thread. The host promises to
// marshal these to one thread; tests/test_thread_affinity.py checks it.
void traceAffinity (const char* what)
{
    static const char* path = std::getenv ("MINIHOST_TEST_AFFINITY_LOG");
    if (path == nullptr || std::this_thread::get_id() == gCreatorThread)
        return;
    static std::mutex m;
    std::lock_guard<std::mutex> lock (m);
    if (auto* f = std::fopen (path, "a"))
    {
        std::fprintf (f, "%s\n", what);
        std::fclose (f);
    }
}

// JUCE's default text for a 0.01-step float (two decimals), traced.
juce::AudioParameterFloatAttributes tracedText()
{
    return juce::AudioParameterFloatAttributes().withStringFromValueFunction (
        [] (float v, int length)
        {
            traceAffinity ("getText");
            juce::String asText (v, 2);
            return length > 0 ? asText.substring (0, length) : asText;
        });
}

// Peak amplitude at velocity 127. See the note at the assignment below.
constexpr float kVoiceScale = MinihostTestProcessor::kVoicePeak;

// Equal-tempered A440, so a test can compute the expected tone itself.
double noteHz (int midiNote)
{
    return 440.0 * std::pow (2.0, (midiNote - 69) / 12.0);
}

} // namespace

juce::AudioProcessor::BusesProperties MinihostTestProcessor::busLayout()
{
   #if MINIHOST_TEST_SYNTH
    return BusesProperties()
        .withOutput ("Output", juce::AudioChannelSet::stereo(), true);
   #else
    return BusesProperties()
        .withInput  ("Input",      juce::AudioChannelSet::stereo(), true)
        .withInput  ("Sidechain",  juce::AudioChannelSet::stereo(), false)
        .withOutput ("Output",     juce::AudioChannelSet::stereo(), true);
   #endif
}

MinihostTestProcessor::MinihostTestProcessor()
    : juce::AudioProcessor (busLayout())
{
    if (gCreatorThread == std::thread::id())
        gCreatorThread = std::this_thread::get_id();

    // Added in ParamIndex order.
    addParameter (gain_ = new juce::AudioParameterFloat (
        juce::ParameterID { "gain", 1 }, "Gain",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f, tracedText()));
    addParameter (latency_ = new juce::AudioParameterInt (
        juce::ParameterID { "latency", 1 }, "Latency",
        0, kMaxLatencySamples, 0));
    addParameter (scMix_ = new juce::AudioParameterFloat (
        juce::ParameterID { "scmix", 1 }, "Sidechain Mix",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.0f, tracedText()));
}

void MinihostTestProcessor::releaseResources()          { traceAffinity ("releaseResources"); }
double MinihostTestProcessor::getTailLengthSeconds() const { traceAffinity ("getTailLengthSeconds"); return 0.0; }
int MinihostTestProcessor::getNumPrograms()              { traceAffinity ("getNumPrograms"); return 1; }
int MinihostTestProcessor::getCurrentProgram()           { traceAffinity ("getCurrentProgram"); return 0; }
void MinihostTestProcessor::setCurrentProgram (int)      { traceAffinity ("setCurrentProgram"); }
const juce::String MinihostTestProcessor::getProgramName (int)
{
    traceAffinity ("getProgramName");
    return "Default";
}
void MinihostTestProcessor::reset()                      { traceAffinity ("reset"); }
bool MinihostTestProcessor::supportsDoublePrecisionProcessing() const
{
    traceAffinity ("supportsDoublePrecisionProcessing");
    return false;
}
void MinihostTestProcessor::updateTrackProperties (const TrackProperties&)
{
    traceAffinity ("updateTrackProperties");
}

void MinihostTestProcessor::prepareToPlay (double sampleRate, int)
{
    traceAffinity ("prepareToPlay");
    sampleRate_ = sampleRate;

    // Allocated for the worst case once: a latency change mid-stream then
    // only moves a read offset.
    delayBuf_.setSize (juce::jmax (2, getTotalNumOutputChannels()),
                       kMaxLatencySamples + 1);
    delayBuf_.clear();
    delayWritePos_  = 0;
    currentLatency_ = latency_->get();
    setLatencySamples (currentLatency_);

    phase_ = 0.0;
    phaseDelta_ = 0.0;
    voiceLevel_ = 0.0f;
    activeNote_ = -1;

}

bool MinihostTestProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    traceAffinity ("isBusesLayoutSupported");
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! MINIHOST_TEST_SYNTH
    if (layouts.getMainInputChannelSet() != layouts.getMainOutputChannelSet())
        return false;
   #endif
    return true;
}

void MinihostTestProcessor::applyLatency (juce::AudioBuffer<float>& buffer, int n)
{
    const int want = latency_->get();
    if (want != currentLatency_)
    {
        currentLatency_ = want;
        delayBuf_.clear();
        delayWritePos_ = 0;
        setLatencySamples (want);
    }
    if (currentLatency_ <= 0)
        return;

    const int ring = delayBuf_.getNumSamples();
    const int chans = juce::jmin (buffer.getNumChannels(), delayBuf_.getNumChannels());

    for (int ch = 0; ch < chans; ++ch)
    {
        float* io  = buffer.getWritePointer (ch);
        float* mem = delayBuf_.getWritePointer (ch);
        int    w   = delayWritePos_;
        for (int i = 0; i < n; ++i)
        {
            const int r = (w + ring - currentLatency_) % ring;
            const float out = mem[r];
            mem[w] = io[i];
            io[i] = out;
            w = (w + 1) % ring;
        }
    }
    delayWritePos_ = (delayWritePos_ + n) % ring;
}

void MinihostTestProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;

    const int n     = buffer.getNumSamples();
    const int outCh = getTotalNumOutputChannels();
    const float g   = gain_->get();

   #if MINIHOST_TEST_SYNTH
    // Instrument: a monophonic sine, last note wins. Rendered before gain so
    // `gain` scales the voice exactly.
    for (int ch = 0; ch < outCh; ++ch)
        buffer.clear (ch, 0, n);

    int pos = 0;
    for (const auto meta : midi)
    {
        const int at = juce::jlimit (0, n, meta.samplePosition);
        for (; pos < at; ++pos)
        {
            const float s = voiceLevel_ * (float) std::sin (phase_);
            for (int ch = 0; ch < outCh; ++ch) buffer.setSample (ch, pos, s);
            phase_ += phaseDelta_;
        }
        const auto msg = meta.getMessage();
        if (msg.isNoteOn())
        {
            activeNote_ = msg.getNoteNumber();
            phaseDelta_ = 2.0 * juce::MathConstants<double>::pi
                              * noteHz (activeNote_) / sampleRate_;
            phase_      = 0.0;
            // Quarter scale: a test that sums several instances (a bus,
            // a chain) must not clip at the file writer and stop being an
            // exact multiple of one instance.
            voiceLevel_ = kVoiceScale * (float) msg.getVelocity() / 127.0f;
        }
        else if (msg.isNoteOff() && msg.getNoteNumber() == activeNote_)
        {
            activeNote_ = -1;
            voiceLevel_ = 0.0f;
        }
    }
    for (; pos < n; ++pos)
    {
        const float s = voiceLevel_ * (float) std::sin (phase_);
        for (int ch = 0; ch < outCh; ++ch) buffer.setSample (ch, pos, s);
        phase_ += phaseDelta_;
    }
   #else
    // Effect: main input, plus the sidechain bus scaled by scmix.
    const float scMix = scMix_->get();
    if (scMix != 0.0f)
    {
        auto sc = getBusBuffer (buffer, true, 1);
        for (int ch = 0; ch < outCh && ch < sc.getNumChannels(); ++ch)
            buffer.addFrom (ch, 0, sc, ch, 0, n, scMix);
    }
   #endif

    if (g != 1.0f)
        for (int ch = 0; ch < outCh; ++ch)
            buffer.applyGain (ch, 0, n, g);

    applyLatency (buffer, n);

    // MIDI passes through byte for byte, at the same offsets: the host's
    // MIDI-in and MIDI-out paths are then one assertion apart.
}

void MinihostTestProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    traceAffinity ("getStateInformation");
    juce::MemoryOutputStream out (destData, true);
    out.writeFloat (gain_->get());
    out.writeInt (latency_->get());
    out.writeFloat (scMix_->get());
}

void MinihostTestProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    traceAffinity ("setStateInformation");
    juce::MemoryInputStream in (data, (size_t) sizeInBytes, false);
    if (in.getNumBytesRemaining() < 12) return;
    *gain_    = in.readFloat();
    *latency_ = in.readInt();
    *scMix_   = in.readFloat();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MinihostTestProcessor();
}
