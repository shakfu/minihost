// live.h
//
// Realtime engine for the desktop app. Holds a juce::AudioDeviceManager
// + a LoadedProject and pumps mh_graph_v2_render_block from an
// AudioIODeviceCallback.
//
// v1 live contract:
//   - The currently loaded project provides the graph topology.
//   - Project input nodes are fed silence each block (the GraphV2
//     contract requires they receive data; we don't route device
//     input into them in this slice). Synth chains work; effects
//     over device input require a future "device_in" node kind.
//   - Output node 0 is copied to the device's output channels.
//     If channel counts mismatch the device, extra device channels
//     are silenced, extra graph channels are dropped.
//
// Threading:
//   - start()/stop()/configure are message-thread only.
//   - The audio callback runs on the device thread. It does no
//     mutex'd work and no allocations after start().

#pragma once

#include "project.h"
#include "rt_param_queue.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <mutex>

namespace minihost_desktop {

class LiveEngine : public juce::AudioIODeviceCallback,
                   private juce::MidiInputCallback
{
public:
    LiveEngine();
    ~LiveEngine() override;

    // Loads `project_file` (parses + opens plugins + compiles graph),
    // configures the audio device with the project's sample rate +
    // block size, attaches the callback. On failure, error is set
    // and the engine is left stopped. Safe to call when already
    // running (will stop the previous run first).
    bool start(const juce::File& project_file, juce::String& error);
    void stop();
    bool isRunning() const noexcept { return running_.load(); }

    juce::AudioDeviceManager& deviceManager() noexcept { return dm_; }

    // Settings persistence. The desktop app calls these on startup
    // and shutdown to remember the user's device / MIDI choice
    // across launches.
    void loadSettingsFromXml(const juce::String& xml);
    juce::String saveSettingsAsXml() const;

    // Enqueue a parameter write for the audio thread to apply on the
    // next callback block. Safe to call from the message thread.
    // Returns false if the queue is full (drop-newest).
    bool enqueueParamWrite(int plugin_node_index,
                           int param_index,
                           float value) noexcept;

    // Transport. Default BPM is 120; default state is stopped.
    void setTransportPlaying(bool playing) noexcept
    { transport_playing_.store(playing); }
    bool isTransportPlaying() const noexcept
    { return transport_playing_.load(); }
    void setBpm(double bpm) noexcept
    { transport_bpm_.store(bpm); }
    double bpm() const noexcept
    { return transport_bpm_.load(); }

    // Loop region in samples. start_samples < end_samples enables
    // looping; setting end_samples <= start_samples (or is_looping
    // false) disables.
    void setLoop(long long start_samples,
                 long long end_samples,
                 bool is_looping) noexcept
    {
        loop_start_.store(start_samples);
        loop_end_.store(end_samples);
        loop_enabled_.store(is_looping
                            && start_samples < end_samples);
    }
    long long loopStart()   const noexcept { return loop_start_.load(); }
    long long loopEnd()     const noexcept { return loop_end_.load();   }
    bool      loopEnabled() const noexcept { return loop_enabled_.load(); }

    // MIDI input device selection. Pass a JUCE MidiDeviceInfo
    // identifier (from juce::MidiInput::getAvailableDevices()) or an
    // empty string to disable MIDI input. Safe to call at any time.
    void setMidiInputDevice(const juce::String& identifier);
    juce::String midiInputDevice() const noexcept
    { return midi_input_identifier_; }

    // Test hook: drain pending commands synchronously, applying them
    // to the live engine's plugins. The real audio callback drains
    // before each render_block. Returns the number of commands
    // applied. Not safe to call concurrently with the audio thread.
    int drainParamWritesForTest();

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(
        const float* const* inputChannelData,
        int numInputChannels,
        float* const* outputChannelData,
        int numOutputChannels,
        int numSamples,
        const juce::AudioIODeviceCallbackContext& context) override;

    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceError(const juce::String& errorMessage) override;

private:
    void detachCallback();
    void initialiseDeviceManagerOnce();

    juce::AudioDeviceManager                       dm_;
    bool                                           dm_initialised_ = false;

    // The audio thread reads `compiled_` only; we publish a new value
    // under stop() (callback already detached) and never mutate while
    // running. cancel_ exists for the project loader plumbing only;
    // it is never set under steady-state live operation.
    std::unique_ptr<project::LoadedProject>        compiled_;

    // Pre-allocated buffer pointer tables for the audio callback.
    // Sized once when start() succeeds; not resized while running.
    std::vector<std::vector<float>>                in_planar_;     // [input_node][ch * block]
    std::vector<std::vector<const float*>>         in_ch_ptrs_;
    std::vector<const float* const*>               in_top_ptrs_;
    std::vector<std::vector<float>>                out_planar_;    // [output_node][ch * block]
    std::vector<std::vector<float*>>               out_ch_ptrs_;
    std::vector<float* const*>                     out_top_ptrs_;

    std::atomic<bool>                              running_{ false };
    int                                            cb_block_size_ = 0;

    // Transport state. Atomics so the GUI thread can update without
    // a lock. Position is owned by the audio thread.
    std::atomic<bool>                              transport_playing_{ false };
    std::atomic<double>                            transport_bpm_{ 120.0 };
    long long                                      transport_pos_samples_ = 0;
    double                                         transport_pos_beats_   = 0.0;
    std::atomic<long long>                         loop_start_{ 0 };
    std::atomic<long long>                         loop_end_{ 0 };
    std::atomic<bool>                              loop_enabled_{ false };

    // SPSC queue for live parameter writes. Producer = GUI thread,
    // consumer = audio callback (drainParamWrites_ at top of each
    // block). 1024 commands is plenty for typical knob rates.
    RtParamQueue<1024>                             param_queue_;

    // ----- MIDI input ----- //
    // Receives from the system MIDI thread, drained by the audio
    // thread. Backed by a small lock-free ring; overflow drops new
    // events.
    void handleIncomingMidiMessage(juce::MidiInput* /*source*/,
                                   const juce::MidiMessage& message) override;

    struct MidiSlot {
        MH_MidiEvent ev;
        int          age_blocks;  // for block-anchored sample_offset
    };
    static constexpr std::size_t kMidiRingCapacity = 1024;
    std::array<MidiSlot, kMidiRingCapacity>        midi_ring_{};
    std::atomic<std::size_t>                       midi_head_{ 0 };
    std::atomic<std::size_t>                       midi_tail_{ 0 };

    // Scratch buffer for staging drained MIDI before render_block.
    std::vector<MH_MidiEvent>                      midi_scratch_;

    std::unique_ptr<juce::MidiInput>               midi_input_;
    juce::String                                   midi_input_identifier_;

    // Drains the queue and applies up to `max` commands to live
    // plugin nodes. Called from the audio thread (and from
    // drainParamWritesForTest for unit testing). Uses
    // juce::AudioProcessorParameter::setValue which is RT-safe
    // by JUCE contract -- does NOT take mh_set_param's mutex.
    int drainParamWrites_(int max);
};

} // namespace minihost_desktop
