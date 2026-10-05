// live.cpp -- see live.h for design.

#include "live.h"

#include "midi_ringbuffer.h"
#include "minihost_audiofile.h"
#include "sequencer.h"

#include <climits>
#include <cmath>
#include <cstring>

namespace minihost_desktop {

namespace {

// Fills `out` with one chunk of a transport-driven MIDI source, offsets
// local to the chunk. range(s0, s1, emit) must call emit(sample, event)
// for each event at an absolute transport sample in [s0, s1), in time
// order. Follows the loop region; held notes get note-offs at each wrap
// and, if `release_first`, at offset 0.
template <class Range>
void collectTransportMidi(std::vector<MH_MidiEvent>& out,
                          std::array<bool, 16 * 128>& held, Range&& range,
                          long long pos, int n, bool play, bool release_first,
                          bool loop_on, long long loop_s, long long loop_e)
{
    out.clear();
    auto push = [&](MH_MidiEvent e, int off) {
        if (out.size() == out.capacity()) return;   // never reallocate
        e.sample_offset = off;
        out.push_back(e);
        const int type = e.status & 0xF0;
        const size_t key = (size_t) (e.status & 0x0F) * 128 + (e.data1 & 0x7F);
        if (type == 0x90 && e.data2 > 0)       held[key] = true;
        else if (type == 0x80 || type == 0x90) held[key] = false;
    };
    auto release = [&](int off) {
        for (size_t key = 0; key < held.size(); ++key)
            if (held[key])
                push({ 0, (unsigned char) (0x80 | (key / 128)),
                       (unsigned char) (key % 128), 0 }, off);
    };

    if (release_first) release(0);
    if (!play) return;
    if (loop_on && pos >= loop_e)
    {
        release(0);
        pos = loop_s + (pos - loop_s) % (loop_e - loop_s);
    }
    int local = 0;
    while (local < n)
    {
        long long seg_end = pos + (n - local);
        const bool wraps = loop_on && seg_end >= loop_e;
        if (wraps) seg_end = loop_e;
        const long long seg_start = pos;
        const int seg_local = local;
        range(seg_start, seg_end, [&](long long at, MH_MidiEvent e) {
            push(e, seg_local + (int) (at - seg_start));
        });
        local += (int) (seg_end - pos);
        if (!wraps) break;
        // The loop end can fall on the chunk end; offsets must stay < n.
        release(std::min(local, n - 1));
        pos = loop_s;
    }
}

} // namespace


LiveEngine::LiveEngine() {}

LiveEngine::~LiveEngine()
{
    stopTimer();
    stop();
    stopPreview();
    if (preview_attached_) dm_.removeAudioCallback(&preview_player_);
    preview_player_.setSource(nullptr);
    writer_thread_.stopThread(2000);
    setMidiInputDevice({});  // closes any open mh_midi_in
}

void LiveEngine::setMidiInputDevice(const juce::String& port_name)
{
    // Tear down any existing input.
    if (midi_in_)
    {
        mh_midi_in_close(midi_in_);
        midi_in_ = nullptr;
    }
    midi_input_port_name_ = port_name;
    if (port_name.isEmpty()) return;

    // Look up the port index by name. libremidi indices are not stable
    // across sessions (devices come and go), so we re-resolve every
    // time. If the device is not present, leave MIDI disabled.
    const int n = mh_midi_get_num_inputs();
    int found = -1;
    char buf[256];
    for (int i = 0; i < n; ++i)
    {
        if (mh_midi_get_input_name(i, buf, sizeof(buf))
            && port_name == juce::String::fromUTF8(buf))
        {
            found = i;
            break;
        }
    }
    if (found < 0)
    {
        std::fprintf(stderr,
            "MIDI input port '%s' not found among %d available ports\n",
            port_name.toRawUTF8(), n);
        return;
    }

    char err[256] = {0};
    midi_in_ = mh_midi_in_open(found, &LiveEngine::midiCallback, this,
                               err, sizeof(err));
    if (!midi_in_)
        std::fprintf(stderr,
            "MIDI input open failed for '%s': %s\n",
            port_name.toRawUTF8(), err);
}

void LiveEngine::midiCallback(const unsigned char* data, size_t len,
                              void* user_data)
{
    if (user_data == nullptr) return;
    static_cast<LiveEngine*>(user_data)->pushIncomingMidi(data, len);
}

void LiveEngine::pushIncomingMidi(const unsigned char* data, size_t len)
{
    // Convert to MH_MidiEvent. We only forward 1-3 byte channel
    // messages; SysEx and longer streams are dropped (the v1 contract
    // mirrors what mh_process_midi accepts).
    if (len == 0 || len > 3) return;
    MH_MidiEvent ev{};
    ev.sample_offset = 0;        // placed from time_ns by the audio thread
    if (len >= 1) ev.status = data[0];
    if (len >= 2) ev.data1  = data[1];
    if (len >= 3) ev.data2  = data[2];

    // SPSC ring push (single-producer here: libremidi calls this on a
    // dedicated MIDI thread per port).
    const auto head = midi_head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) % kMidiRingCapacity;
    if (next == midi_tail_.load(std::memory_order_acquire))
        return;  // full: drop newest
    midi_ring_[head].ev      = ev;
    midi_ring_[head].time_ns = mh_midi_clock_ns();
    midi_head_.store(next, std::memory_order_release);
}

void LiveEngine::loadSettingsFromXml(const juce::String& xml)
{
    initialiseDeviceManagerOnce();
    if (xml.isEmpty()) return;
    auto root = juce::parseXML(xml);
    if (root == nullptr) return;
    if (auto* audio = root->getChildByName("audio"))
        dm_.initialise(0, 2, audio, /*selectDefaultDeviceOnFailure=*/true);
    if (auto* midi  = root->getChildByName("midi_input"))
    {
        // Preferred attribute is `port_name`. We tolerate legacy
        // `identifier` values (written by older builds that used JUCE
        // MidiInput identifiers) by treating them as port names; if
        // they no longer match a real port, MIDI just stays disabled.
        auto name = midi->getStringAttribute("port_name");
        if (name.isEmpty()) name = midi->getStringAttribute("identifier");
        if (name.isNotEmpty()) setMidiInputDevice(name);
    }
}

juce::String LiveEngine::saveSettingsAsXml() const
{
    juce::XmlElement root("minihost_desktop_settings");
    if (auto audio = dm_.createStateXml())
        root.addChildElement(audio.release());
    auto* midi = new juce::XmlElement("midi_input");
    midi->setAttribute("port_name", midi_input_port_name_);
    root.addChildElement(midi);
    return root.toString();
}

void LiveEngine::initialiseDeviceManagerOnce()
{
    if (dm_initialised_) return;
    // Default config: 0 input ch, 2 output ch. Users can override
    // via the Audio Device Settings dialog.
    const auto err = dm_.initialiseWithDefaultDevices(0, 2);
    if (err.isNotEmpty())
        std::fprintf(stderr, "AudioDeviceManager init: %s\n",
                     err.toRawUTF8());
    dm_initialised_ = true;
}

bool LiveEngine::start(const project::ProjectDocument& doc,
                       juce::String& error, bool attach_device)
{
    if (attach_device) initialiseDeviceManagerOnce();
    stop();

    try {
        compiled_ = project::loadProject(doc);
    } catch (const std::exception& e) {
        error = juce::String("loadProject failed: ") + e.what();
        return false;
    }

    cb_block_size_ = compiled_->doc.block_size;

    // If the project has device_input nodes, ensure the audio device
    // is open with at least that many input channels. JUCE's default
    // init is 0 inputs; we widen the setup only when needed and leave
    // the user's other choices (device, sr, buf) intact.
    int needed_in_ch = 0;
    for (const auto& di : compiled_->doc.device_inputs)
        needed_in_ch = std::max(needed_in_ch, di.channels);
    if (needed_in_ch > 0)
    {
        auto setup = dm_.getAudioDeviceSetup();
        const int have = setup.inputChannels.countNumberOfSetBits();
        if (have < needed_in_ch)
        {
            setup.inputChannels.clear();
            setup.inputChannels.setRange(0, needed_in_ch, true);
            setup.useDefaultInputChannels = false;
            const auto err = dm_.setAudioDeviceSetup(setup, /*treatAsChosen=*/true);
            if (err.isNotEmpty())
                std::fprintf(stderr,
                    "audio device input setup failed: %s\n",
                    err.toRawUTF8());
        }
    }

    // Pre-allocate input/output buffer storage + pointer tables.
    // File-source inputs come first, then device_inputs, then
    // metronomes -- matches the loader's add-order.
    in_planar_.clear();
    in_ch_ptrs_.clear();
    in_top_ptrs_.clear();
    auto pushIn = [&](int channels) {
        std::vector<float> buf((size_t) channels
                               * (size_t) cb_block_size_, 0.0f);
        std::vector<const float*> ptrs((size_t) channels);
        for (int c = 0; c < channels; ++c)
            ptrs[(size_t) c] = buf.data() + (size_t) c * cb_block_size_;
        in_planar_.push_back(std::move(buf));
        in_ch_ptrs_.push_back(std::move(ptrs));
    };
    for (const auto& in : compiled_->doc.inputs)        pushIn(in.channels);
    for (const auto& di : compiled_->doc.device_inputs) pushIn(di.channels);
    for (const auto& mn : compiled_->doc.metronomes)    pushIn(mn.channels);
    in_top_ptrs_.resize(in_ch_ptrs_.size());
    for (size_t i = 0; i < in_ch_ptrs_.size(); ++i)
        in_top_ptrs_[i] = in_ch_ptrs_[i].data();

    out_planar_.clear();
    out_ch_ptrs_.clear();
    out_top_ptrs_.clear();
    // File-sink outputs come first, then device_outputs, then meters --
    // ordering matches the loader. File-sink buffers are recorded while
    // the transport plays.
    auto pushOut = [&](int channels) {
        std::vector<float> buf((size_t) channels
                               * (size_t) cb_block_size_, 0.0f);
        std::vector<float*> ptrs((size_t) channels);
        for (int c = 0; c < channels; ++c)
            ptrs[(size_t) c] = buf.data() + (size_t) c * cb_block_size_;
        out_planar_.push_back(std::move(buf));
        out_ch_ptrs_.push_back(std::move(ptrs));
    };
    for (const auto& on : compiled_->doc.outputs)        pushOut(on.channels);
    for (const auto& dn : compiled_->doc.device_outputs) pushOut(dn.channels);
    for (const auto& mn : compiled_->doc.meters)         pushOut(mn.channels);
    out_top_ptrs_.resize(out_ch_ptrs_.size());
    for (size_t i = 0; i < out_ch_ptrs_.size(); ++i)
        out_top_ptrs_[i] = out_ch_ptrs_[i].data();

    if (out_planar_.empty())
    {
        error = "project has no output nodes; nothing to play live";
        compiled_.reset();
        return false;
    }

    // One-shot diagnostic so silent-output debugging has a starting
    // point: log the graph shape, audio device state, and per-node
    // role at the moment the engine attaches its callback.
    {
        auto* dev = dm_.getCurrentAudioDevice();
        std::fprintf(stderr,
            "[live] start: plugins=%d audio_in_bufs=%d audio_out_bufs=%d "
            "midi_input_nodes=%zu device_outs=%zu metronomes=%zu\n",
            (int) compiled_->plugins.size(),
            (int) in_planar_.size(),
            (int) out_planar_.size(),
            compiled_->midi_input_node_ids.size(),
            compiled_->device_output_buffer_indices.size(),
            compiled_->metronome_buffer_indices.size());
        if (dev != nullptr)
        {
            std::fprintf(stderr,
                "[live] audio device='%s' type='%s' "
                "sr=%.1f buf=%d in_ch=%d out_ch=%d\n",
                dev->getName().toRawUTF8(),
                dev->getTypeName().toRawUTF8(),
                dev->getCurrentSampleRate(),
                dev->getCurrentBufferSizeSamples(),
                dev->getActiveInputChannels().countNumberOfSetBits(),
                dev->getActiveOutputChannels().countNumberOfSetBits());
        }
        else
        {
            std::fprintf(stderr,
                "[live] WARNING: no current audio device -- "
                "callback will fire but produce no sound\n");
        }
    }

    // The Loop toggle covers the whole project until Set Loop Region
    // narrows it.
    setLoop(0, compiled_->duration_frames, loop_wanted_.load());

    midi_scratch_.reserve(256);
    midi_chunk_.reserve(256);
    // Reserved so the audio thread never reallocates; overflow drops.
    file_midi_.assign(compiled_->file_midi_inputs.size(), TransportMidi{});
    for (auto& f : file_midi_)
    {
        f.events.reserve(MH_GRAPH_MIDI_OUTPUT_CAPACITY);
        f.merged.reserve(MH_GRAPH_MIDI_OUTPUT_CAPACITY + 256);
    }
    seq_midi_.assign(compiled_->sequencer_node_ids.size(), SequencerPlay{});
    for (size_t i = 0; i < seq_midi_.size(); ++i)
    {
        seq_midi_[i].params = compiled_->doc.sequencers[i].params;
        seq_midi_[i].events.reserve(MH_GRAPH_MIDI_OUTPUT_CAPACITY);
    }
    SequencerUpdate stale;
    while (seq_queue_.pop(stale)) {}   // edits for a previous project
    was_playing_ = false;
    last_bpm_    = 0.0;
    running_.store(true, std::memory_order_release);
    if (attach_device)
    {
        dm_.addAudioCallback(this);
        attached_ = true;
    }
    if (!(first_midi_logged_ && first_audio_logged_))
        startTimer(250);
    return true;
}

void LiveEngine::detachCallback()
{
    if (attached_)
    {
        dm_.removeAudioCallback(this);
        attached_ = false;
    }
    running_.store(false, std::memory_order_release);
}

void LiveEngine::timerCallback()
{
    if (!first_midi_logged_ && first_midi_seen_.load(std::memory_order_acquire))
    {
        std::fprintf(stderr,
            "[live] first MIDI event reached engine: "
            "status=0x%02X d1=%d d2=%d (staged into %zu MIDI_INPUT nodes)\n",
            (unsigned) first_midi_.status, (int) first_midi_.data1,
            (int) first_midi_.data2, first_midi_nodes_);
        first_midi_logged_ = true;
    }
    if (!first_audio_logged_ && first_audio_seen_.load(std::memory_order_acquire))
    {
        std::fprintf(stderr,
            "[live] first non-silent block produced "
            "(plugin chain is generating audio)\n");
        first_audio_logged_ = true;
    }
    if (first_midi_logged_ && first_audio_logged_)
        stopTimer();
}

void LiveEngine::stop()
{
    detachCallback();
    closeRecorders();
    rate_mismatch_.store(false, std::memory_order_relaxed);
    mismatched_device_rate_.store(0.0, std::memory_order_relaxed);
    transport_playing_.store(false);
    rewind_pending_.store(false);
    transport_pos_samples_ = 0;
    transport_pos_beats_   = 0.0;
    published_pos_.store(0);
    compiled_.reset();
}

bool LiveEngine::updateSequencer(const juce::String& id,
                                 const project::SequencerParams& params)
{
    if (compiled_ == nullptr) return false;
    auto& seqs = compiled_->doc.sequencers;
    for (size_t i = 0; i < seqs.size(); ++i)
    {
        if (seqs[i].id != id) continue;
        if (seqs[i].params == params) return true;   // e.g. a node drag
        if (!seq_queue_.push({ (int) i, params })) return false;
        seqs[i].params = params;   // message-thread copy; audio has its own
        return true;
    }
    return false;
}

void LiveEngine::setLooping(bool on)
{
    loop_wanted_.store(on);
    loop_enabled_.store(on && loop_start_.load() < loop_end_.load());
    if (preview_source_ != nullptr) preview_source_->setLooping(on);
}

double LiveEngine::previewPositionSeconds() const
{
    // A looping source's read position keeps growing; fold it back.
    const double len = preview_transport_.getLengthInSeconds();
    const double pos = preview_transport_.getCurrentPosition();
    return len > 0.0 ? std::fmod(pos, len) : pos;
}

bool LiveEngine::play(juce::String& error)
{
    if (compiled_ == nullptr)
    {
        error = "live mode is not running";
        return false;
    }
    if (transport_playing_.load()) return true;

    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> recs;
    const auto& doc = compiled_->doc;
    for (const auto& on : doc.outputs)
    {
        // Sinks are WAV or FLAC, as for the offline renderer.
        std::unique_ptr<juce::AudioFormat> fmt;
        if (on.sink.hasFileExtension("flac"))
            fmt = std::make_unique<juce::FlacAudioFormat>();
        else
            fmt = std::make_unique<juce::WavAudioFormat>();

        on.sink.getParentDirectory().createDirectory();
        on.sink.deleteFile();
        std::unique_ptr<juce::OutputStream> stream
            = std::make_unique<juce::FileOutputStream>(on.sink);
        if (static_cast<juce::FileOutputStream*>(stream.get())->failedToOpen())
        {
            error = "cannot open " + on.sink.getFullPathName();
            return false;
        }
        const auto opts = juce::AudioFormatWriterOptions{}
            .withSampleRate((double) doc.sample_rate)
            .withNumChannels(on.channels)
            .withBitsPerSample(on.bit_depth);
        auto writer = fmt->createWriterFor(stream, opts);
        if (writer == nullptr)
        {
            error = "cannot record " + juce::String(on.bit_depth)
                  + "-bit " + fmt->getFormatName() + " to "
                  + on.sink.getFullPathName();
            return false;
        }
        // Two seconds of FIFO absorbs disk stalls.
        recs.push_back(std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
            writer.release(), writer_thread_, doc.sample_rate * 2));
    }
    if (!recs.empty() && !writer_thread_.isThreadRunning())
        writer_thread_.startThread();
    written_.frames.store(0);
    if (!recs.empty()) recs.front()->setDataReceiver(&written_);

    {
        const juce::SpinLock::ScopedLockType lock(recorder_lock_);
        recorders_ = std::move(recs);
    }
    dropped_frames_.store(0);
    recorded_frames_.store(0);
    recording_.store(!recorders_.empty());
    transport_playing_.store(true);
    return true;
}

void LiveEngine::stopTransport()
{
    transport_playing_.store(false);
    rewind_pending_.store(true);
    closeRecorders();
}

void LiveEngine::closeRecorders()
{
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> old;
    {
        const juce::SpinLock::ScopedLockType lock(recorder_lock_);
        old.swap(recorders_);
    }
    recording_.store(false);
    // Destroying a ThreadedWriter flushes its FIFO and closes the file.
    old.clear();
    if (const auto dropped = dropped_frames_.exchange(0); dropped > 0)
        std::fprintf(stderr,
            "[live] recording dropped %lld frames (disk too slow)\n",
            dropped);
}

bool LiveEngine::startPreview(const juce::File& file, juce::String& error)
{
    initialiseDeviceManagerOnce();
    stopPreview();

    char err[256] = {0};
    MH_AudioData* ad = mh_audio_read(file.getFullPathName().toRawUTF8(),
                                     err, sizeof(err));
    if (ad == nullptr)
    {
        error = "failed to read " + file.getFullPathName() + ": "
              + juce::String(static_cast<const char*>(err));
        return false;
    }
    preview_buffer_.setSize((int) ad->channels, (int) ad->frames);
    for (int c = 0; c < (int) ad->channels; ++c)
    {
        float* dst = preview_buffer_.getWritePointer(c);
        for (int i = 0; i < (int) ad->frames; ++i)
            dst[i] = ad->data[(size_t) i * ad->channels + (size_t) c];
    }
    const double file_rate = (double) ad->sample_rate;
    mh_audio_data_free(ad);

    preview_source_ = std::make_unique<juce::MemoryAudioSource>(
        preview_buffer_, /*copyMemory=*/false, loop_wanted_.load());
    preview_file_ = file;
    preview_transport_.setSource(preview_source_.get(), 0, nullptr,
                                 file_rate, preview_buffer_.getNumChannels());
    if (!preview_attached_)
    {
        preview_player_.setSource(&preview_transport_);
        dm_.addAudioCallback(&preview_player_);
        preview_attached_ = true;
    }
    preview_transport_.setPosition(0.0);
    preview_transport_.start();
    return true;
}

void LiveEngine::stopPreview()
{
    preview_transport_.stop();
    // setSource takes the transport's callback lock, so the audio thread
    // is not reading the source when it is released.
    preview_transport_.setSource(nullptr);
    preview_source_.reset();
    preview_file_ = juce::File();
}

void LiveEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device == nullptr) return;
    midi_prev_cb_ns_ = 0;  // callbacks have not started

    const double device_rate = device->getCurrentSampleRate();
    std::fprintf(stderr,
        "live: device starting (sr=%d, block=%d, in_ch=%d, out_ch=%d)\n",
        (int) device_rate,
        device->getCurrentBufferSizeSamples(),
        device->getActiveInputChannels().countNumberOfSetBits(),
        device->getActiveOutputChannels().countNumberOfSetBits());

    // The project's plugins were instantiated at the project rate, and the
    // transport, metronome and MIDI-clock maths all use it. If the device is
    // running at a different rate, everything plays at the wrong speed and
    // pitch and the tempo drifts against the metronome. This was previously
    // just part of the informational line above, so the only symptom a user
    // got was "it sounds wrong".
    const double project_rate =
        compiled_ != nullptr ? (double) compiled_->doc.sample_rate : 0.0;
    const bool mismatch =
        project_rate > 0.0 && std::abs(device_rate - project_rate) > 0.5;

    rate_mismatch_.store(mismatch, std::memory_order_relaxed);
    mismatched_device_rate_.store(mismatch ? device_rate : 0.0,
                                  std::memory_order_relaxed);

    if (mismatch)
        std::fprintf(stderr,
            "live: ERROR project sample rate is %.0f Hz but the audio device is "
            "running at %.0f Hz -- playback will be at the wrong speed and "
            "pitch. Set the device to %.0f Hz, or rebuild the project at "
            "%.0f Hz.\n",
            project_rate, device_rate, project_rate, device_rate);
}

void LiveEngine::audioDeviceStopped() {}

void LiveEngine::audioDeviceError(const juce::String& errorMessage)
{
    std::fprintf(stderr, "live audio error: %s\n", errorMessage.toRawUTF8());
}

bool LiveEngine::enqueueParamWrite(int plugin_node_index,
                                   int param_index, float value) noexcept
{
    ParamWriteCommand cmd{ plugin_node_index, param_index, value };
    return param_queue_.push(cmd);
}

int LiveEngine::drainParamWritesForTest()
{
    return drainParamWrites_(/*max=*/INT_MAX);
}

int LiveEngine::drainParamWrites_(int max)
{
    if (compiled_ == nullptr) return 0;
    int applied = 0;
    ParamWriteCommand cmd;
    while (applied < max && param_queue_.pop(cmd))
    {
        if (cmd.plugin_node_index < 0
            || cmd.plugin_node_index
                 >= (int) compiled_->plugins.size())
            continue;
        MH_Plugin* p = compiled_->plugins[(size_t) cmd.plugin_node_index];
        if (p == nullptr) continue;

        auto* proc = static_cast<juce::AudioProcessor*>(
            mh_get_juce_processor(p));
        if (proc == nullptr) continue;

        const auto& params = proc->getParameters();
        if (cmd.param_index < 0 || cmd.param_index >= params.size())
            continue;
        auto* param = params[cmd.param_index];
        if (param == nullptr) continue;
        // setValue is RT-safe per JUCE's contract -- atomic store
        // into the parameter's value cache. Does NOT acquire
        // libminihost's mutex (which mh_set_param would).
        param->setValue(juce::jlimit(0.0f, 1.0f, cmd.value));
        ++applied;
    }
    return applied;
}

void LiveEngine::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData,
    int numInputChannels,
    float* const* outputChannelData,
    int numOutputChannels,
    int numSamples,
    const juce::AudioIODeviceCallbackContext& /*context*/)
{
    // Belt-and-braces: if we're not running or compiled is null,
    // silence the device output and return.
    if (!running_.load(std::memory_order_acquire) || compiled_ == nullptr)
    {
        for (int c = 0; c < numOutputChannels; ++c)
            if (outputChannelData[c] != nullptr)
                std::memset(outputChannelData[c], 0,
                            (size_t) numSamples * sizeof(float));
        return;
    }

    // Drain MIDI once per callback and place each event by arrival time
    // across the whole callback: a fixed one-callback delay rather than up
    // to one callback of jitter. The ring has one producer, so the
    // offsets come out in order.
    const uint64_t cb_ns = mh_midi_clock_ns();
    midi_scratch_.clear();
    {
        std::size_t t = midi_tail_.load(std::memory_order_relaxed);
        const std::size_t h = midi_head_.load(std::memory_order_acquire);
        while (t != h && midi_scratch_.size() < 256)
        {
            MH_MidiEvent ev = midi_ring_[t].ev;
            ev.sample_offset = mh_midi_time_to_offset(
                midi_ring_[t].time_ns, midi_prev_cb_ns_, cb_ns, numSamples);
            midi_scratch_.push_back(ev);
            t = (t + 1) % kMidiRingCapacity;
        }
        midi_tail_.store(t, std::memory_order_release);
    }
    midi_prev_cb_ns_ = cb_ns;
    std::size_t midi_next = 0;

    if (rewind_pending_.exchange(false, std::memory_order_acq_rel))
    {
        transport_pos_samples_ = 0;
        transport_pos_beats_   = 0.0;
    }

    // Render in chunks of up to cb_block_size_. Device callback sizes
    // are usually small (64-1024) so a single chunk is the norm.
    int frames_left = numSamples;
    int offset      = 0;
    while (frames_left > 0)
    {
        const int n = std::min(frames_left, cb_block_size_);

        // Transport state for this chunk, read once so file playback,
        // recording and the plugin playhead agree.
        const bool      play    = transport_playing_.load(std::memory_order_relaxed);
        const double    chunk_bpm = transport_bpm_.load(std::memory_order_relaxed);
        const long long chunk_pos = transport_pos_samples_;
        const long long loop_s  = loop_start_.load(std::memory_order_relaxed);
        const long long loop_e  = loop_end_.load(std::memory_order_relaxed);
        const bool      loop_on = loop_enabled_.load(std::memory_order_relaxed)
                                  && loop_e > loop_s;

        // Push transport info to every plugin node before rendering.
        // mh_set_transport copies into the plugin's internal state
        // without I/O; on the audio thread it's fine to call.
        {
            const double sr   = (double) compiled_->doc.sample_rate;
            const double bpm  = transport_bpm_.load(std::memory_order_relaxed);
            MH_TransportInfo ti{};
            ti.bpm                  = bpm;
            ti.time_sig_numerator   = 4;
            ti.time_sig_denominator = 4;
            ti.position_samples     = transport_pos_samples_;
            ti.position_beats       = transport_pos_beats_;
            ti.is_playing           = play ? 1 : 0;
            ti.is_looping           = loop_on ? 1 : 0;
            ti.loop_start_samples   = loop_s;
            ti.loop_end_samples     = loop_e;
            for (MH_Plugin* p : compiled_->plugins)
                if (p != nullptr) mh_set_transport(p, &ti);
            if (play)
            {
                transport_pos_samples_ += n;
                transport_pos_beats_
                    += (double) n * bpm / (60.0 * sr);
                // Wrap inside the loop region. Phase-coherent wrap
                // is good enough; we don't snap to bar boundaries.
                if (loop_on && transport_pos_samples_ >= loop_e)
                {
                    const long long span = loop_e - loop_s;
                    if (span > 0)
                    {
                        transport_pos_samples_
                            = loop_s + ((transport_pos_samples_ - loop_s) % span);
                        transport_pos_beats_
                            = (double) transport_pos_samples_
                              * bpm / (60.0 * sr);
                    }
                }
            }
        }

        // Drain any pending parameter writes from the GUI before
        // rendering this block. RT-safe (lock-free queue + RT-safe
        // setValue on each AudioProcessorParameter).
        drainParamWrites_(/*max=*/64);

        // This chunk's share of the drained MIDI, rebased to the chunk.
        midi_chunk_.clear();
        while (midi_next < midi_scratch_.size()
               && midi_scratch_[midi_next].sample_offset < offset + n)
        {
            MH_MidiEvent ev = midi_scratch_[midi_next++];
            ev.sample_offset -= offset;
            midi_chunk_.push_back(ev);
        }
        // Stage the drained events on every MIDI_INPUT node in the
        // project. Routing within the graph (MIDI edges) fans the
        // events out to plugins. Legacy receives_midi projects get a
        // synthesized MIDI_INPUT node at load time (see project.cpp
        // migration), so a single staging path covers both formats.
        // Nodes with a .mid source get the file merged in; staging
        // replaces, so they are staged once, below.
        auto* graph = compiled_->graph->handle();
        const auto& fmis = compiled_->file_midi_inputs;
        const bool stopped_now = was_playing_ && !play;
        for (size_t k = 0; k < fmis.size(); ++k)
        {
            auto& f = file_midi_[k];
            const auto& ev = fmis[k].events;
            collectTransportMidi(f.events, f.held,
                [&ev](long long s0, long long s1, auto&& emit) {
                    auto it = std::lower_bound(ev.begin(), ev.end(), s0,
                        [](const MH_MidiEvent& e, long long p) {
                            return e.sample_offset < p; });
                    for (; it != ev.end() && it->sample_offset < s1; ++it)
                        emit(it->sample_offset, *it);
                },
                chunk_pos, n, play, stopped_now, loop_on, loop_s, loop_e);
            f.merged.clear();
            size_t a = 0, b = 0;
            while (a < f.events.size() || b < midi_chunk_.size())
            {
                const bool take_file = b == midi_chunk_.size()
                    || (a < f.events.size()
                        && f.events[a].sample_offset <= midi_chunk_[b].sample_offset);
                if (f.merged.size() == f.merged.capacity()) break;
                f.merged.push_back(take_file ? f.events[a++] : midi_chunk_[b++]);
            }
            if (!f.merged.empty())
                mh_graph_set_midi_input_events(graph, fmis[k].node_id,
                                               f.merged.data(),
                                               (int) f.merged.size());
        }
        // Sequencers. An edit or a tempo change moves the step grid, so
        // sounding notes are released first.
        {
            SequencerUpdate u;
            while (seq_queue_.pop(u))
                if (u.index >= 0 && u.index < (int) seq_midi_.size())
                {
                    seq_midi_[(size_t) u.index].params  = u.params;
                    seq_midi_[(size_t) u.index].changed = true;
                }
            const double sr = (double) compiled_->doc.sample_rate;
            const bool tempo_moved = last_bpm_ != 0.0 && chunk_bpm != last_bpm_;
            for (size_t q = 0; q < seq_midi_.size(); ++q)
            {
                auto& sq = seq_midi_[q];
                const bool release = stopped_now || (play && (sq.changed || tempo_moved));
                if (play || stopped_now) sq.changed = false;
                const auto& params = sq.params;
                collectTransportMidi(sq.events, sq.held,
                    [&](long long s0, long long s1, auto&& emit) {
                        project::sequencerEvents(params, sr, chunk_bpm, s0, s1, emit);
                    },
                    chunk_pos, n, play, release, loop_on, loop_s, loop_e);
                if (!sq.events.empty())
                    mh_graph_set_midi_input_events(graph, compiled_->sequencer_node_ids[q],
                                                   sq.events.data(),
                                                   (int) sq.events.size());
            }
            last_bpm_ = chunk_bpm;
        }

        if (!midi_chunk_.empty())
        {
            for (MH_NodeId nid : compiled_->midi_input_node_ids)
            {
                bool is_file = false;
                for (const auto& fmi : fmis) is_file |= fmi.node_id == nid;
                if (!is_file)
                    mh_graph_set_midi_input_events(
                        graph, nid,
                        midi_chunk_.data(),
                        (int) midi_chunk_.size());
            }

            if (!first_midi_seen_.load(std::memory_order_relaxed))
            {
                first_midi_ = midi_chunk_.front();
                first_midi_nodes_ = compiled_->midi_input_node_ids.size();
                first_midi_seen_.store(true, std::memory_order_release);
            }
        }

        // Stage input buffers for this block:
        //   - File-source inputs (compiled_->doc.inputs): the decoded file
        //     from the transport position while playing, else silence.
        //   - device_input nodes: copy from inputChannelData. Extra
        //     graph channels (beyond what the device supplies) get
        //     silence; extra device channels are ignored. Multiple
        //     device_input nodes share the same device channels (each
        //     consumer sees the same live signal).
        // Clear per channel. These buffers are planar with a stride of
        // cb_block_size_, so a single contiguous memset of n * channels floats
        // only zeroes the first channel's worth (plus part of the next) and
        // leaves the tail of every later channel holding the previous block's
        // samples. That is the normal case, not an edge one: device callbacks
        // are typically smaller than the project block size, so any input node
        // with two or more channels leaked stale audio on every callback.
        for (auto& buf : in_planar_)
        {
            const size_t channels = buf.size() / (size_t) cb_block_size_;
            for (size_t c = 0; c < channels; ++c)
                std::memset(buf.data() + c * (size_t) cb_block_size_, 0,
                            (size_t) n * sizeof(float));
        }
        if (play)
        {
            for (size_t i = 0; i < compiled_->doc.inputs.size(); ++i)
            {
                const int    ch     = compiled_->doc.inputs[i].channels;
                const int    frames = compiled_->input_frames[i];
                const float* src    = compiled_->input_audio[i].data();
                float*       dst    = in_planar_[i].data();
                for (int s = 0; s < n; ++s)
                {
                    long long p = chunk_pos + s;
                    if (loop_on && p >= loop_e)
                        p = loop_s + (p - loop_s) % (loop_e - loop_s);
                    if (p < 0 || p >= frames) continue;
                    for (int c = 0; c < ch; ++c)
                        dst[(size_t) c * cb_block_size_ + (size_t) s]
                            = src[(size_t) c * (size_t) frames + (size_t) p];
                }
            }
        }
        for (size_t k = 0;
             k < compiled_->device_input_buffer_indices.size(); ++k)
        {
            const int buf_i = compiled_->device_input_buffer_indices[k];
            const auto& spec = compiled_->doc.device_inputs[k];
            float* base = in_planar_[(size_t) buf_i].data();
            for (int c = 0; c < spec.channels; ++c)
            {
                if (c < numInputChannels
                    && inputChannelData != nullptr
                    && inputChannelData[c] != nullptr)
                {
                    float* dst = base + (size_t) c * cb_block_size_;
                    std::memcpy(dst,
                                inputChannelData[c] + offset,
                                (size_t) n * sizeof(float));
                }
                // else: already zero-filled above.
            }
        }

        // Transport-driven generators: metronome clicks (audio) and
        // MIDI clock ticks. Both ride on the LiveEngine's transport
        // state (bpm + playing + position), so they must run after
        // the transport-info push earlier in the block and before
        // renderBlock consumes the inputs / staged MIDI.
        {
            const long long ti_pos = transport_pos_samples_;
            const double    ti_bpm = transport_bpm_.load();
            const bool      playing = transport_playing_.load();
            const double    sr =
                compiled_ ? (double) compiled_->doc.sample_rate : 48000.0;
            compiled_->renderMetronomes(in_planar_, cb_block_size_, n,
                                        ti_pos, sr, ti_bpm, playing);
            compiled_->stageMidiClocks(compiled_->graph->handle(),
                                       n, ti_pos, sr, ti_bpm, playing);
        }

        try {
            compiled_->graph->renderBlock(
                in_top_ptrs_.data(),  (int) in_top_ptrs_.size(),
                out_top_ptrs_.data(), (int) out_top_ptrs_.size(),
                n);
            // Surface per-channel peak from each meter node's buffer
            // to the GUI via lock-free atomics. Cheap; we already
            // touched these samples in the graph.
            compiled_->updateMeters(out_top_ptrs_.data(), n);

            // Flag the first block whose output contains a non-zero
            // sample; timerCallback logs it. Confirms audio is being
            // produced at all.
            if (!first_audio_seen_.load(std::memory_order_relaxed))
            {
                bool any_nonzero = false;
                for (size_t bi = 0;
                     bi < out_planar_.size() && !any_nonzero; ++bi)
                {
                    const auto& buf = out_planar_[bi];
                    for (size_t s = 0; s < buf.size(); ++s)
                        if (buf[s] != 0.0f) { any_nonzero = true; break; }
                }
                if (any_nonzero)
                    first_audio_seen_.store(true, std::memory_order_release);
            }
        } catch (...) {
            // Anything throwing on the audio thread means stop now.
            for (int c = 0; c < numOutputChannels; ++c)
                if (outputChannelData[c] != nullptr)
                    std::memset(outputChannelData[c] + offset, 0,
                                (size_t) (numSamples - offset)
                                * sizeof(float));
            running_.store(false, std::memory_order_release);
            return;
        }

        // Record file output nodes. A try-lock: if the message thread is
        // swapping recorders this chunk is skipped, never waited on.
        if (play)
        {
            const juce::SpinLock::ScopedTryLockType lock(recorder_lock_);
            if (lock.isLocked() && !recorders_.empty())
            {
                bool all = true;
                for (size_t i = 0; i < recorders_.size(); ++i)
                    all &= recorders_[i]->write(out_ch_ptrs_[i].data(), n);
                if (all) recorded_frames_.fetch_add(n, std::memory_order_relaxed);
                else     dropped_frames_.fetch_add(n, std::memory_order_relaxed);
            }
        }

        // Route to the device output. If the project has any
        // device_output nodes, sum them per-channel (extra device
        // channels are silenced; extra graph channels are dropped).
        // Otherwise fall back to the legacy "first file-sink output
        // doubles as the live speaker output" rule for back-compat
        // with projects authored before device_output existed.
        const auto& dev_idx = compiled_->device_output_buffer_indices;
        if (!dev_idx.empty())
        {
            // Per-device-channel mix-and-copy.
            for (int c = 0; c < numOutputChannels; ++c)
            {
                float* dst = outputChannelData[c];
                if (dst == nullptr) continue;
                std::memset(dst + offset, 0,
                            (size_t) n * sizeof(float));
                for (int buf_i : dev_idx)
                {
                    const auto& spec = compiled_->doc.device_outputs[
                        (size_t) (buf_i
                            - (int) compiled_->doc.outputs.size())];
                    if (c >= spec.channels) continue;
                    const float* src
                        = out_planar_[(size_t) buf_i].data()
                          + (size_t) c * cb_block_size_;
                    float* d = dst + offset;
                    for (int i = 0; i < n; ++i) d[i] += src[i];
                }
            }
        }
        else if (!compiled_->doc.outputs.empty() && !out_planar_.empty())
        {
            const int graph_ch = compiled_->doc.outputs[0].channels;
            for (int c = 0; c < numOutputChannels; ++c)
            {
                float* dst = outputChannelData[c];
                if (dst == nullptr) continue;
                if (c < graph_ch)
                {
                    const float* src
                        = out_planar_[0].data() + (size_t) c * cb_block_size_;
                    std::memcpy(dst + offset, src,
                                (size_t) n * sizeof(float));
                }
                else
                {
                    std::memset(dst + offset, 0,
                                (size_t) n * sizeof(float));
                }
            }
        }
        else
        {
            for (int c = 0; c < numOutputChannels; ++c)
                if (outputChannelData[c] != nullptr)
                    std::memset(outputChannelData[c] + offset, 0,
                                (size_t) n * sizeof(float));
        }

        was_playing_ = play;
        frames_left -= n;
        offset      += n;
    }
    published_pos_.store(transport_pos_samples_, std::memory_order_relaxed);
}

} // namespace minihost_desktop
