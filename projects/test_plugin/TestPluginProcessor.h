// TestPluginProcessor.h
//
// Deterministic plugin used as a test fixture. Every behavior here is exact
// and checkable sample by sample, so the host's processing, automation,
// MIDI routing, sidechain and latency paths can be asserted rather than
// skipped for want of a third-party plugin.
//
// One source, two targets (see CMakeLists.txt):
//   MinihostTestFx    - 2 in (+ 2 sidechain) / 2 out audio effect
//   MinihostTestSynth - 0 in / 2 out instrument (MINIHOST_TEST_SYNTH=1)
//
// Parameter order is part of the fixture's contract; tests address by index.

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#ifndef MINIHOST_TEST_SYNTH
 #define MINIHOST_TEST_SYNTH 0
#endif

class MinihostTestProcessor final : public juce::AudioProcessor
{
public:
    // Parameter indices. Kept in one place because the tests hard-code them.
    enum ParamIndex { kGain = 0, kLatency = 1, kSidechainMix = 2 };

    // Upper bound on the latency the fixture can be asked for, and the size
    // the delay line is allocated to once, so a latency change never
    // allocates on the audio thread.
    static constexpr int kMaxLatencySamples = 4096;

    // Peak output of the instrument target at velocity 127, before `gain`.
    static constexpr float kVoicePeak = 0.25f;

    MinihostTestProcessor();
    ~MinihostTestProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override                     { return false; }

    const juce::String getName() const override         { return JucePlugin_Name; }
    bool acceptsMidi() const override                   { return true; }
    bool producesMidi() const override                  { return true; }
    bool isMidiEffect() const override                  { return false; }
    double getTailLengthSeconds() const override        { return 0.0; }

    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

private:
    // Bus layout for this build's target. A member because
    // AudioProcessor::BusesProperties is protected.
    static BusesProperties busLayout();

    // Push `n` frames through the per-channel delay line, in place. A length
    // of 0 is a straight copy, so latency 0 is bit-exact pass-through.
    void applyLatency (juce::AudioBuffer<float>& buffer, int n);

    juce::AudioParameterFloat* gain_    = nullptr;
    juce::AudioParameterInt*   latency_ = nullptr;
    juce::AudioParameterFloat* scMix_   = nullptr;

    juce::AudioBuffer<float> delayBuf_;
    int delayWritePos_   = 0;
    int currentLatency_  = 0;

    // Sine voice, for the instrument target. Monophonic and phase-continuous
    // so a rendered note is reproducible to the sample.
    double phase_        = 0.0;
    double phaseDelta_   = 0.0;
    float  voiceLevel_   = 0.0f;
    int    activeNote_   = -1;
    double sampleRate_   = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MinihostTestProcessor)
};
