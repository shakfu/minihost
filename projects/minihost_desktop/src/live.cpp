// live.cpp -- see live.h for design.

#include "live.h"

#include <climits>
#include <cstring>

namespace minihost_desktop {

LiveEngine::LiveEngine() {}

LiveEngine::~LiveEngine()
{
    stop();
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
    ev.sample_offset = 0;        // anchored to start of next block
    if (len >= 1) ev.status = data[0];
    if (len >= 2) ev.data1  = data[1];
    if (len >= 3) ev.data2  = data[2];

    // SPSC ring push (single-producer here: libremidi calls this on a
    // dedicated MIDI thread per port).
    const auto head = midi_head_.load(std::memory_order_relaxed);
    const auto next = (head + 1) % kMidiRingCapacity;
    if (next == midi_tail_.load(std::memory_order_acquire))
        return;  // full: drop newest
    midi_ring_[head].ev         = ev;
    midi_ring_[head].age_blocks = 0;
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

bool LiveEngine::start(const juce::File& project_file, juce::String& error)
{
    initialiseDeviceManagerOnce();
    stop();

    try {
        compiled_ = project::loadProject(project_file);
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
    // File-source inputs come first, then device_inputs -- matches the
    // loader's add-order. Both are zero-filled here; per-block the
    // audio callback overwrites device_input slots with the live
    // device input channels, while file-source slots stay silent
    // (their WAV data is only staged during file rendering).
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
    // File-sink outputs come first, then device_outputs -- ordering
    // matches the loader. LiveEngine writes file-sink buffers nowhere
    // (file rendering is a separate path); they're allocated so the
    // graph has somewhere to write each block.
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

    dm_.addAudioCallback(this);
    running_.store(true, std::memory_order_release);
    return true;
}

void LiveEngine::detachCallback()
{
    if (running_.load(std::memory_order_acquire))
    {
        dm_.removeAudioCallback(this);
        running_.store(false, std::memory_order_release);
    }
}

void LiveEngine::stop()
{
    detachCallback();
    transport_playing_.store(false);
    transport_pos_samples_ = 0;
    transport_pos_beats_   = 0.0;
    compiled_.reset();
}

void LiveEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    if (device != nullptr)
        std::fprintf(stderr,
            "live: device starting (sr=%d, block=%d, in_ch=%d, out_ch=%d)\n",
            (int) device->getCurrentSampleRate(),
            device->getCurrentBufferSizeSamples(),
            device->getActiveInputChannels().countNumberOfSetBits(),
            device->getActiveOutputChannels().countNumberOfSetBits());
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

    // Render in chunks of up to cb_block_size_. Device callback sizes
    // are usually small (64-1024) so a single chunk is the norm.
    int frames_left = numSamples;
    int offset      = 0;
    while (frames_left > 0)
    {
        const int n = std::min(frames_left, cb_block_size_);

        // Push transport info to every plugin node before rendering.
        // mh_set_transport copies into the plugin's internal state
        // without I/O; on the audio thread it's fine to call.
        {
            const double sr   = (double) compiled_->doc.sample_rate;
            const double bpm  = transport_bpm_.load(std::memory_order_relaxed);
            const bool   play = transport_playing_.load(std::memory_order_relaxed);
            MH_TransportInfo ti{};
            const long long loop_s = loop_start_.load(std::memory_order_relaxed);
            const long long loop_e = loop_end_.load(std::memory_order_relaxed);
            const bool loop_on     = loop_enabled_.load(std::memory_order_relaxed);
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

        // Drain MIDI events from the ring buffer. All events go to
        // every plugin node (v1 fan-out routing). Per-plugin
        // designation is a follow-up.
        midi_scratch_.clear();
        {
            std::size_t t = midi_tail_.load(std::memory_order_relaxed);
            const std::size_t h = midi_head_.load(std::memory_order_acquire);
            while (t != h && midi_scratch_.size() < 256)
            {
                midi_scratch_.push_back(midi_ring_[t].ev);
                t = (t + 1) % kMidiRingCapacity;
            }
            midi_tail_.store(t, std::memory_order_release);
        }
        // Stage the drained events on every MIDI_INPUT node in the
        // project. Routing within the graph (MIDI edges) fans the
        // events out to plugins. Legacy receives_midi projects get a
        // synthesized MIDI_INPUT node at load time (see project.cpp
        // migration), so a single staging path covers both formats.
        if (!midi_scratch_.empty() && compiled_ != nullptr)
        {
            auto* graph = compiled_->graph->handle();
            for (MH_NodeId nid : compiled_->midi_input_node_ids)
                mh_graph_v2_set_midi_input_events(
                    graph, nid,
                    midi_scratch_.data(),
                    (int) midi_scratch_.size());

            static std::atomic<bool> logged_midi{false};
            if (!logged_midi.load(std::memory_order_relaxed))
            {
                const auto& ev = midi_scratch_.front();
                std::fprintf(stderr,
                    "[live] first MIDI event reached engine: "
                    "status=0x%02X d1=%d d2=%d (staged into %zu MIDI_INPUT nodes)\n",
                    (unsigned) ev.status, (int) ev.data1, (int) ev.data2,
                    compiled_->midi_input_node_ids.size());
                logged_midi.store(true, std::memory_order_relaxed);
            }
        }

        // Stage input buffers for this block:
        //   - File-source inputs (compiled_->doc.inputs): silence (their
        //     WAV data is only consumed by file rendering, not live).
        //   - device_input nodes: copy from inputChannelData. Extra
        //     graph channels (beyond what the device supplies) get
        //     silence; extra device channels are ignored. Multiple
        //     device_input nodes share the same device channels (each
        //     consumer sees the same live signal).
        for (auto& buf : in_planar_)
            std::memset(buf.data(), 0,
                        (size_t) n * (size_t) (buf.size() / (size_t) cb_block_size_)
                                   * sizeof(float));
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

            // One-shot diagnostic: log the first block whose output
            // buffer contains a non-zero sample, and separately log
            // the first staged MIDI events. Helps confirm whether
            // audio is being produced at all and whether MIDI is
            // reaching the engine. Not RT-safe in the strictest
            // sense (fprintf can lock); kept guarded by static
            // flags so each fires at most once per app run.
            static std::atomic<bool> logged_audio{false};
            if (!logged_audio.load(std::memory_order_relaxed))
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
                {
                    std::fprintf(stderr,
                        "[live] first non-silent block produced "
                        "(plugin chain is generating audio)\n");
                    logged_audio.store(true, std::memory_order_relaxed);
                }
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

        frames_left -= n;
        offset      += n;
    }
}

} // namespace minihost_desktop
