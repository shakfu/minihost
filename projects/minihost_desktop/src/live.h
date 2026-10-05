// live.h
//
// Realtime engine for the desktop app. Holds a juce::AudioDeviceManager
// + a LoadedProject and pumps mh_graph_render_block from an
// AudioIODeviceCallback.
//
// Live contract:
//   - The open project document provides the graph topology.
//   - File input nodes play their decoded audio from the transport
//     position while the transport plays, silence otherwise. midi_input
//     nodes with a .mid source do the same, merged with device MIDI, and
//     sequencer nodes generate notes on the step grid. Held notes get
//     note-offs at each loop wrap and on Stop.
//   - device_output nodes are summed to the device's output channels
//     (legacy projects without one route output node 0 instead).
//   - While the transport plays, every file output node is recorded to
//     its sink. Stop finalizes the files and rewinds to 0.
//
// Threading:
//   - start()/stop()/play()/stopTransport() are message-thread only.
//   - The audio callback runs on the device thread. It does no
//     mutex'd work and no allocations after start().

#pragma once

#include "project.h"
#include "rt_param_queue.h"

#include "minihost_midi.h"

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

namespace minihost_desktop {

class LiveEngine : public juce::AudioIODeviceCallback,
                   private juce::Timer
{
public:
    LiveEngine();
    ~LiveEngine() override;

    // Loads `project_file` (parses + opens plugins + compiles graph),
    // configures the audio device with the project's sample rate +
    // block size, attaches the callback. On failure, error is set
    // and the engine is left stopped. Safe to call when already
    // running (will stop the previous run first).
    // attach_device=false skips the device callback; the caller then
    // drives audioDeviceIOCallbackWithContext itself (headless selftest).
    bool start(const project::ProjectDocument& doc, juce::String& error,
               bool attach_device = true);
    void stop();
    bool isRunning() const noexcept { return running_.load(); }

    /** True when the audio device is running at a different sample rate than
        the loaded project was built for.

        The project's plugins were instantiated at the project rate and every
        transport / metronome / MIDI-clock calculation uses it, so a mismatch
        renders everything at the wrong speed and pitch. This used to be logged
        as an ordinary informational line and otherwise ignored; the engine now
        flags it so the app can tell the user instead of leaving them to work
        out why playback sounds wrong. Set on audioDeviceAboutToStart, cleared
        by stop(). */
    bool hasSampleRateMismatch() const noexcept
    {
        return rate_mismatch_.load(std::memory_order_relaxed);
    }

    /** The device rate that conflicted with the project rate, or 0. */
    double mismatchedDeviceSampleRate() const noexcept
    {
        return mismatched_device_rate_.load(std::memory_order_relaxed);
    }

    juce::AudioDeviceManager& deviceManager() noexcept { return dm_; }

    // Currently-loaded live project, or nullptr if not running.
    // Valid only on the GUI thread; both start()/stop() and the
    // returned pointer's consumers live there, so no locking is
    // required as long as callers don't stash the pointer across
    // event-loop iterations without re-checking.
    project::LoadedProject* loadedProject() noexcept
    { return compiled_.get(); }

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
    // play() opens a recorder per file output node (overwriting its sink)
    // and starts the transport; it fails if a sink cannot be opened.
    // stopTransport() finalizes the recordings and rewinds to 0.
    bool play(juce::String& error);
    void stopTransport();
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
        loop_wanted_.store(is_looping);
        loop_enabled_.store(is_looping
                            && start_samples < end_samples);
    }
    // The Loop toggle. Start Live sets the loop region to the project
    // length (Set Loop Region can narrow it). Also loops Play File.
    // Persists across Start / Stop Live.
    void setLooping(bool on);
    bool isLooping() const noexcept { return loop_wanted_.load(); }

    // ----- State for the GUI (message thread; values may lag a block) ----- //
    long long positionSamples() const noexcept { return published_pos_.load(); }
    int       projectFrames() const noexcept
    { return compiled_ ? compiled_->duration_frames : 0; }
    double    projectSampleRate() const noexcept
    { return compiled_ ? (double) compiled_->doc.sample_rate : 0.0; }
    bool      isRecording() const noexcept { return recording_.load(); }
    // Frames accepted for recording (a block the FIFO could not take is
    // dropped and not counted), and those not yet written to disk.
    long long recordedFrames() const noexcept { return recorded_frames_.load(); }
    long long recordingBacklog() const noexcept
    { return recorded_frames_.load() - written_.frames.load(); }
    const juce::File& previewFile() const noexcept { return preview_file_; }
    double    previewPositionSeconds() const;
    double    previewLengthSeconds() const
    { return preview_transport_.getLengthInSeconds(); }

    long long loopStart()   const noexcept { return loop_start_.load(); }
    long long loopEnd()     const noexcept { return loop_end_.load();   }
    bool      loopEnabled() const noexcept { return loop_enabled_.load(); }

    // MIDI input device selection. Pass a MIDI port name (as returned
    // by mh_midi_get_input_name / mh_midi_enumerate_inputs) or an empty
    // string to disable MIDI input. Safe to call at any time.
    //
    // The port name is matched against the system's current MIDI input
    // ports; if no match is found, MIDI input is left disabled and an
    // error is logged.
    void setMidiInputDevice(const juce::String& port_name);
    juce::String midiInputDevice() const noexcept
    { return midi_input_port_name_; }

    // Plays an audio file straight to the device, bypassing the graph.
    // Mixes with live output if the engine is running. Decoded with the
    // same reader as project inputs; resampled to the device rate.
    bool startPreview(const juce::File& file, juce::String& error);
    void stopPreview();
    bool isPreviewing() const { return preview_transport_.isPlaying(); }

    // Applies new settings to the running sequencer node `id` at the next
    // block, releasing its sounding notes. Unchanged settings are ignored.
    // Returns false if there is no such node in the live project or the
    // queue is full.
    bool updateSequencer(const juce::String& id,
                         const project::SequencerParams& params);

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

    // Prints the first-audio / first-MIDI diagnostics the audio thread
    // flags below; fprintf can lock, so it stays off the audio thread.
    void timerCallback() override;
    std::atomic<bool>                              first_audio_seen_{ false };
    std::atomic<bool>                              first_midi_seen_{ false };
    MH_MidiEvent                                   first_midi_{};
    std::size_t                                    first_midi_nodes_ = 0;
    bool                                           first_audio_logged_ = false;
    bool                                           first_midi_logged_ = false;

    juce::AudioDeviceManager                       dm_;
    bool                                           dm_initialised_ = false;
    bool                                           attached_ = false;

    // ----- Recording (file output nodes, while the transport plays) ----- //
    // recorders_[i] records doc.outputs[i]. Swapped on the message thread
    // under recorder_lock_; the audio thread only try-locks it, so a
    // block that collides with a swap is dropped rather than blocking.
    void closeRecorders();
    juce::TimeSliceThread                          writer_thread_{ "minihost recorder" };
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> recorders_;
    juce::SpinLock                                 recorder_lock_;
    std::atomic<long long>                         dropped_frames_{ 0 };
    std::atomic<bool>                              recording_{ false };
    std::atomic<long long>                         recorded_frames_{ 0 };

    // Counts frames the writer thread has written (first recorder).
    struct WrittenCounter : juce::AudioFormatWriter::ThreadedWriter::IncomingDataReceiver
    {
        std::atomic<long long> frames{ 0 };
        void reset(int, double, juce::int64) override {}
        void addBlock(juce::int64, const juce::AudioBuffer<float>&, int, int n) override
        { frames.fetch_add(n, std::memory_order_relaxed); }
    };
    WrittenCounter                                 written_;
    std::atomic<bool>                              rewind_pending_{ false };

    // ----- File preview ----- //
    juce::AudioBuffer<float>                       preview_buffer_;
    std::unique_ptr<juce::MemoryAudioSource>       preview_source_;
    juce::AudioTransportSource                     preview_transport_;
    juce::AudioSourcePlayer                        preview_player_;
    bool                                           preview_attached_ = false;
    juce::File                                     preview_file_;

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
    std::atomic<bool>                              rate_mismatch_{ false };
    std::atomic<double>                            mismatched_device_rate_{ 0.0 };
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
    std::atomic<bool>                              loop_wanted_{ false };
    std::atomic<long long>                         published_pos_{ 0 };

    // SPSC queue for live parameter writes. Producer = GUI thread,
    // consumer = audio callback (drainParamWrites_ at top of each
    // block). 1024 commands is plenty for typical knob rates.
    RtParamQueue<1024>                             param_queue_;

    // ----- MIDI input ----- //
    // Receives from the libremidi MIDI thread (via mh_midi_in_open),
    // drained by the audio thread. Backed by a small lock-free ring;
    // overflow drops new events.
    static void midiCallback(const unsigned char* data, size_t len,
                             void* user_data);
    void pushIncomingMidi(const unsigned char* data, size_t len);

    struct MidiSlot {
        MH_MidiEvent ev;
        uint64_t     time_ns;  // arrival, from mh_midi_clock_ns
    };
    static constexpr std::size_t kMidiRingCapacity = 1024;
    std::array<MidiSlot, kMidiRingCapacity>        midi_ring_{};
    std::atomic<std::size_t>                       midi_head_{ 0 };
    std::atomic<std::size_t>                       midi_tail_{ 0 };

    // MIDI drained once per device callback, offsets placed by arrival
    // time across the whole callback; midi_chunk_ holds one render chunk's
    // share, rebased. Both reserved in start().
    std::vector<MH_MidiEvent>                      midi_scratch_;
    std::vector<MH_MidiEvent>                      midi_chunk_;

    // ----- Transport-driven MIDI sources ----- //
    // File MIDI (midi_input nodes with a .mid source) and sequencers. Per
    // source, `events` holds this chunk's events, `merged` those plus device
    // MIDI (file sources only), and `held` marks sounding notes,
    // [channel * 128 + note], so wraps, Stop and edits can release them.
    struct TransportMidi {
        std::vector<MH_MidiEvent>  events;
        std::vector<MH_MidiEvent>  merged;
        std::array<bool, 16 * 128> held{};
    };
    struct SequencerPlay : TransportMidi {
        project::SequencerParams params;
        bool                     changed = false;  // release before next chunk
    };
    struct SequencerUpdate {
        int                      index = -1;
        project::SequencerParams params;
    };
    std::vector<TransportMidi>                     file_midi_;
    std::vector<SequencerPlay>                     seq_midi_;
    RtParamQueue<64, SequencerUpdate>              seq_queue_;
    bool                                           was_playing_ = false;  // audio thread
    double                                         last_bpm_    = 0.0;    // audio thread
    // Start of the previous device callback; 0 until the first one.
    uint64_t                                       midi_prev_cb_ns_ = 0;

    MH_MidiIn*                                     midi_in_ = nullptr;
    juce::String                                   midi_input_port_name_;

    // Drains the queue and applies up to `max` commands to live
    // plugin nodes. Called from the audio thread (and from
    // drainParamWritesForTest for unit testing). Uses
    // juce::AudioProcessorParameter::setValue which is RT-safe
    // by JUCE contract -- does NOT take mh_set_param's mutex.
    int drainParamWrites_(int max);
};

} // namespace minihost_desktop
