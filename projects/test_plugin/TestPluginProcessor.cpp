// TestPluginProcessor.cpp
// See TestPluginProcessor.h for what this fixture guarantees.

#include "TestPluginProcessor.h"

namespace {

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
    // Added in ParamIndex order.
    addParameter (gain_ = new juce::AudioParameterFloat (
        juce::ParameterID { "gain", 1 }, "Gain", 0.0f, 1.0f, 1.0f));
    addParameter (latency_ = new juce::AudioParameterInt (
        juce::ParameterID { "latency", 1 }, "Latency",
        0, kMaxLatencySamples, 0));
    addParameter (scMix_ = new juce::AudioParameterFloat (
        juce::ParameterID { "scmix", 1 }, "Sidechain Mix", 0.0f, 1.0f, 0.0f));
}

void MinihostTestProcessor::prepareToPlay (double sampleRate, int)
{
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
    juce::MemoryOutputStream out (destData, true);
    out.writeFloat (gain_->get());
    out.writeInt (latency_->get());
    out.writeFloat (scMix_->get());
}

void MinihostTestProcessor::setStateInformation (const void* data, int sizeInBytes)
{
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
