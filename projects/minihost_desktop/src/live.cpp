// live.cpp -- see live.h for design.

#include "live.h"

#include <climits>
#include <cstring>

namespace minihost_desktop {

LiveEngine::LiveEngine() {}

LiveEngine::~LiveEngine()
{
    stop();
    setMidiInputDevice({});  // closes any open MidiInput
}

void LiveEngine::setMidiInputDevice(const juce::String& identifier)
{
    // Tear down any existing input.
    if (midi_input_)
    {
        midi_input_->stop();
        midi_input_.reset();
    }
    midi_input_identifier_ = identifier;
    if (identifier.isEmpty()) return;

    midi_input_ = juce::MidiInput::openDevice(identifier, this);
    if (midi_input_)
        midi_input_->start();
    else
        std::fprintf(stderr,
            "MIDI input open failed for '%s'\n",
            identifier.toRawUTF8());
}

void LiveEngine::handleIncomingMidiMessage(juce::MidiInput* /*source*/,
                                           const juce::MidiMessage& m)
{
    // Convert to MH_MidiEvent. We only forward 1-3 byte channel
    // messages; SysEx and longer streams are dropped (the v1 contract
    // mirrors what mh_process_midi accepts).
    if (m.getRawDataSize() > 3) return;
    MH_MidiEvent ev{};
    const auto* raw = m.getRawData();
    ev.sample_offset = 0;        // anchored to start of next block
    if (m.getRawDataSize() >= 1) ev.status = raw[0];
    if (m.getRawDataSize() >= 2) ev.data1  = raw[1];
    if (m.getRawDataSize() >= 3) ev.data2  = raw[2];

    // SPSC ring push (single-producer here: the OS MIDI thread is the
    // only writer for a given MidiInput).
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
        const auto id = midi->getStringAttribute("identifier");
        if (id.isNotEmpty()) setMidiInputDevice(id);
    }
}

juce::String LiveEngine::saveSettingsAsXml() const
{
    juce::XmlElement root("minihost_desktop_settings");
    if (auto audio = dm_.createStateXml())
        root.addChildElement(audio.release());
    auto* midi = new juce::XmlElement("midi_input");
    midi->setAttribute("identifier", midi_input_identifier_);
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

    // Pre-allocate input/output buffer storage + pointer tables.
    in_planar_.clear();
    in_ch_ptrs_.clear();
    in_top_ptrs_.clear();
    for (const auto& in : compiled_->doc.inputs)
    {
        std::vector<float> buf((size_t) in.channels
                               * (size_t) cb_block_size_, 0.0f);
        std::vector<const float*> ptrs((size_t) in.channels);
        for (int c = 0; c < in.channels; ++c)
            ptrs[(size_t) c] = buf.data() + (size_t) c * cb_block_size_;
        in_planar_.push_back(std::move(buf));
        in_ch_ptrs_.push_back(std::move(ptrs));
    }
    in_top_ptrs_.resize(in_ch_ptrs_.size());
    for (size_t i = 0; i < in_ch_ptrs_.size(); ++i)
        in_top_ptrs_[i] = in_ch_ptrs_[i].data();

    out_planar_.clear();
    out_ch_ptrs_.clear();
    out_top_ptrs_.clear();
    for (const auto& on : compiled_->doc.outputs)
    {
        std::vector<float> buf((size_t) on.channels
                               * (size_t) cb_block_size_, 0.0f);
        std::vector<float*> ptrs((size_t) on.channels);
        for (int c = 0; c < on.channels; ++c)
            ptrs[(size_t) c] = buf.data() + (size_t) c * cb_block_size_;
        out_planar_.push_back(std::move(buf));
        out_ch_ptrs_.push_back(std::move(ptrs));
    }
    out_top_ptrs_.resize(out_ch_ptrs_.size());
    for (size_t i = 0; i < out_ch_ptrs_.size(); ++i)
        out_top_ptrs_[i] = out_ch_ptrs_[i].data();

    if (compiled_->doc.outputs.empty())
    {
        error = "project has no output nodes; nothing to play live";
        compiled_.reset();
        return false;
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
    const float* const* /*inputChannelData*/,
    int /*numInputChannels*/,
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
        // Stage the drained events on every plugin node that opted
        // into MIDI input.
        if (!midi_scratch_.empty() && compiled_ != nullptr)
        {
            auto* graph = compiled_->graph->handle();
            for (int i = 0; i < (int) compiled_->doc.plugins.size(); ++i)
            {
                if (!compiled_->doc.plugins[(size_t) i].receives_midi)
                    continue;
                const int node_id
                    = (int) compiled_->doc.inputs.size() + i;
                mh_graph_v2_set_node_midi(
                    graph, node_id,
                    midi_scratch_.data(),
                    (int) midi_scratch_.size());
            }
        }

        // Feed silence to all input nodes (v1 live contract).
        for (auto& buf : in_planar_)
            std::memset(buf.data(), 0,
                        (size_t) n * (size_t) (buf.size() / (size_t) cb_block_size_)
                                   * sizeof(float));

        try {
            compiled_->graph->renderBlock(
                in_top_ptrs_.data(),  (int) in_top_ptrs_.size(),
                out_top_ptrs_.data(), (int) out_top_ptrs_.size(),
                n);
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

        // Copy output node 0 to the device output. Map by index;
        // silence any device channels beyond the graph's count, drop
        // any graph channels beyond the device's count.
        if (!out_planar_.empty())
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
