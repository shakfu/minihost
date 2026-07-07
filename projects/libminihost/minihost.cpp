// minihost.cpp
#include "minihost.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#ifdef MINIHOST_HEADLESS
 #include <juce_audio_processors_headless/juce_audio_processors_headless.h>
 using VST3Format = juce::VST3PluginFormatHeadless;
 #if JUCE_MAC
  using AUFormat = juce::AudioUnitPluginFormatHeadless;
 #endif
 #if JUCE_PLUGINHOST_LV2
  using LV2Format = juce::LV2PluginFormatHeadless;
 #endif
#else
 #include <juce_audio_processors/juce_audio_processors.h>
 using VST3Format = juce::VST3PluginFormat;
 #if JUCE_MAC
  using AUFormat = juce::AudioUnitPluginFormat;
 #endif
 #if JUCE_PLUGINHOST_LV2
  using LV2Format = juce::LV2PluginFormat;
 #endif
#endif
#include <juce_events/juce_events.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace juce;

// ---------------------------------------------------------------------------
// Dedicated JUCE plugin thread.
//
// JUCE VST3/AU plugin instances are thread-affine: construction, destruction,
// and control-plane queries (parameter text, state, program names, ...) must
// all run on one consistent thread. In a headless Python host there is no
// natural message thread, and loading/using a plugin across threads (e.g.
// open_async) deadlocks.
//
// This singleton owns one persistent background thread that becomes the JUCE
// message thread and executes every thread-affine plugin operation. Callers on
// any thread push a task + promise onto a plain mutex-protected queue and
// block until it runs -- they never touch JUCE's own marshaling
// (callFunctionOnMessageThread / posted CallbackMessage both proved unreliable
// on macOS). The real-time process*() path is deliberately NOT marshaled.
//
// Enabled by default; set MINIHOST_MESSAGE_THREAD=0 to disable (run() then
// executes inline on the caller's thread, the pre-existing behavior).
// ---------------------------------------------------------------------------
namespace {

class MinihostMessageThread
{
public:
    static MinihostMessageThread& instance()
    {
        static MinihostMessageThread inst;
        return inst;
    }

    void init()
    {
        std::call_once(initFlag_, [this]()
        {
            const char* env = std::getenv("MINIHOST_MESSAGE_THREAD");
            if (env != nullptr && env[0] == '0')
                return;   // explicitly disabled

            std::promise<void> ready;
            auto fut = ready.get_future();
            std::thread([this, &ready]()
            {
                juce::initialiseJuce_GUI();   // this thread becomes the message thread
                enabled_.store(true);
                ready.set_value();
                // Execute queued tasks directly on this (message) thread. They
                // are self-contained -- construction ran to completion
                // synchronously in earlier experiments -- so no JUCE dispatch
                // loop is needed (runDispatchLoopUntil is unavailable in the
                // headless build anyway). A condition variable gives immediate
                // wake-up with no busy polling.
                for (;;)
                {
                    Task t{ nullptr, nullptr };
                    {
                        std::unique_lock<std::mutex> lk(mtx_);
                        cv_.wait(lk, [this] {
                            return ! queue_.empty() || ! running_.load();
                        });
                        if (queue_.empty())
                            break;   // woken for shutdown with nothing pending
                        t = queue_.front();
                        queue_.pop_front();
                    }
                    try
                    {
                        (*t.fn)();
                        t.prom->set_value();
                    }
                    catch (...)
                    {
                        t.prom->set_exception(std::current_exception());
                    }
                }
                juce::shutdownJuce_GUI();
            }).detach();
            fut.wait();
        });
    }

    // Run fn on the message thread and block until it finishes. Inline when the
    // message thread is disabled or we are already on it.
    void run(const std::function<void()>& fn)
    {
        if (! enabled_.load())
        {
            fn();
            return;
        }
        auto* mm = juce::MessageManager::getInstanceWithoutCreating();
        if (mm != nullptr && mm->isThisTheMessageThread())
        {
            fn();
            return;
        }
        std::promise<void> prom;
        auto fut = prom.get_future();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(Task{ &fn, &prom });
        }
        cv_.notify_one();
        fut.get();   // blocks; rethrows if the task threw
    }

private:
    MinihostMessageThread() = default;

    struct Task
    {
        const std::function<void()>* fn;
        std::promise<void>* prom;
    };

    std::once_flag initFlag_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{true};
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
};

} // namespace

extern "C" void mh_message_thread_init(void)
{
    MinihostMessageThread::instance().init();
}

// Run a callable on the JUCE plugin thread and return its result (or void).
// Wrap the body of any thread-affine control function in this. When the plugin
// thread is disabled, it runs inline. NOT for the process*() hot path.
namespace {
template <typename Fn>
static auto runOnMsg(Fn&& fn) -> decltype(fn())
{
    using R = decltype(fn());
    if constexpr (std::is_void_v<R>)
    {
        MinihostMessageThread::instance().run([&]() { fn(); });
    }
    else
    {
        R result{};
        MinihostMessageThread::instance().run([&]() { result = fn(); });
        return result;
    }
}
} // namespace

// ABI version reporting. The number returned reflects the version the
// implementation was built against. Callers should compare with the
// MH_API_VERSION_NUMBER macro from the header at startup.
extern "C" int mh_api_version(void)
{
    return MH_API_VERSION_NUMBER;
}

extern "C" const char* mh_api_version_string(void)
{
    return MH_API_VERSION_STRING;
}

class MH_PlayHead : public AudioPlayHead
{
public:
    // Transport state snapshot -- copied atomically via seqlock
    struct State
    {
        bool hasTransport = false;
        double bpm = 120.0;
        int timeSigNum = 4;
        int timeSigDenom = 4;
        int64_t positionSamples = 0;
        double positionBeats = 0.0;
        bool isPlaying = false;
        bool isRecording = false;
        bool isLooping = false;
        int64_t loopStartSamples = 0;
        int64_t loopEndSamples = 0;
    };

    double sampleRate = 44100.0;

    // Seqlock: writer increments seq_ before and after updating state_.
    // Reader retries if seq_ changed during read (ensures torn-read safety
    // without blocking the audio thread).
    void write(const State& s)
    {
        seq_.fetch_add(1, std::memory_order_release);    // odd  = write in progress
        state_ = s;
        seq_.fetch_add(1, std::memory_order_release);    // even = write complete
    }

    State read() const
    {
        State s;
        unsigned seq0, seq1;
        do {
            seq0 = seq_.load(std::memory_order_acquire);
            s = state_;
            seq1 = seq_.load(std::memory_order_acquire);
        } while (seq0 != seq1 || (seq0 & 1));            // retry if torn or mid-write
        return s;
    }

    Optional<PositionInfo> getPosition() const override
    {
        State s = read();

        if (!s.hasTransport)
            return nullopt;

        PositionInfo info;
        info.setBpm(s.bpm);
        info.setTimeSignature(TimeSignature{s.timeSigNum, s.timeSigDenom});
        info.setTimeInSamples(s.positionSamples);
        info.setTimeInSeconds(static_cast<double>(s.positionSamples) / sampleRate);
        info.setPpqPosition(s.positionBeats);
        info.setIsPlaying(s.isPlaying);
        info.setIsRecording(s.isRecording);
        info.setIsLooping(s.isLooping);
        if (s.isLooping)
        {
            info.setLoopPoints(LoopPoints{
                static_cast<double>(s.loopStartSamples) / sampleRate * (s.bpm / 60.0),
                static_cast<double>(s.loopEndSamples) / sampleRate * (s.bpm / 60.0)
            });
        }
        return info;
    }

private:
    State state_;
    std::atomic<unsigned> seq_{0};
};

struct MH_Plugin;

struct MH_Listener : public AudioProcessorListener
{
    MH_Plugin* owner = nullptr;

    std::atomic<MH_ChangeCallback> changeCb{nullptr};
    std::atomic<void*> changeUserData{nullptr};
    std::atomic<MH_ParamValueCallback> paramValueCb{nullptr};
    std::atomic<void*> paramValueUserData{nullptr};
    std::atomic<MH_ParamGestureCallback> paramGestureCb{nullptr};
    std::atomic<void*> paramGestureUserData{nullptr};

    void audioProcessorParameterChanged(AudioProcessor*, int paramIndex, float newValue) override
    {
        auto cb = paramValueCb.load(std::memory_order_acquire);
        if (cb)
            cb(owner, paramIndex, newValue, paramValueUserData.load(std::memory_order_relaxed));
    }

    void audioProcessorChanged(AudioProcessor*, const ChangeDetails& details) override
    {
        auto cb = changeCb.load(std::memory_order_acquire);
        if (!cb) return;

        int flags = 0;
        if (details.latencyChanged)            flags |= MH_CHANGE_LATENCY;
        if (details.parameterInfoChanged)      flags |= MH_CHANGE_PARAM_INFO;
        if (details.programChanged)            flags |= MH_CHANGE_PROGRAM;
        if (details.nonParameterStateChanged)  flags |= MH_CHANGE_NON_PARAM_STATE;

        if (flags != 0)
            cb(owner, flags, changeUserData.load(std::memory_order_relaxed));
    }

    void audioProcessorParameterChangeGestureBegin(AudioProcessor*, int paramIndex) override
    {
        auto cb = paramGestureCb.load(std::memory_order_acquire);
        if (cb)
            cb(owner, paramIndex, 1, paramGestureUserData.load(std::memory_order_relaxed));
    }

    void audioProcessorParameterChangeGestureEnd(AudioProcessor*, int paramIndex) override
    {
        auto cb = paramGestureCb.load(std::memory_order_acquire);
        if (cb)
            cb(owner, paramIndex, 0, paramGestureUserData.load(std::memory_order_relaxed));
    }
};

struct MH_Plugin
{
    // No persistent AudioPluginFormatManager here. The manager is only
    // needed during construction (to discover the plugin's
    // PluginDescription and instantiate it) and can be discarded
    // afterwards -- the AudioPluginInstance is self-contained. Open
    // paths construct a local manager (mh_open_ex) or reuse one
    // belonging to an MH_Session (mh_session_open).
    std::unique_ptr<AudioPluginInstance> inst;
    MH_PlayHead playHead;
    MH_Listener listener;

    double sampleRate = 0.0;
    int maxBlockSize = 0;
    int inCh = 0;
    int outCh = 0;
    int sidechainCh = 0;  // sidechain input channels (0 if none)
    std::string path;     // plugin file path passed to mh_open / mh_open_ex

    // Single processing buffer sized to max(inCh + sidechainCh, outCh) channels.
    // JUCE's processBlock contract requires the buffer to have enough channels
    // for both inputs and outputs; sizing to the max handles asymmetric layouts
    // (inCh > outCh, outCh > inCh) and the sidechain case uniformly.
    // Inputs are copied in at the start; outputs are copied out at the end.
    AudioBuffer<float> processBuf;
    AudioBuffer<double> processBufD;   // mirror of processBuf for mh_process_double
    MidiBuffer midi;

    // Mutex for thread-safe access to plugin state from non-audio threads
    // Note: mh_process* functions do NOT lock (audio thread must not block)
    // Use this for param/state access from UI or control threads
    std::mutex stateMutex;

    MH_Plugin()
    {
        listener.owner = this;
    }
};

// Session owns one AudioPluginFormatManager and reuses it across calls.
// Protected by a mutex because JUCE's AudioPluginFormatManager is not
// internally thread-safe.
struct MH_Session
{
    std::mutex mtx;
    AudioPluginFormatManager fm;
};

static void setErr(char* buf, size_t n, const String& msg)
{
    if (buf == nullptr || n == 0) return;
    auto s = msg.toRawUTF8();
    std::snprintf(buf, n, "%s", s ? s : "Unknown error");
}

static bool findFirstTypeForFile(AudioPluginFormatManager& fm,
                                 const File& pluginFile,
                                 PluginDescription& outDesc,
                                 String& err)
{
    bool foundAny = false;

    for (int i = 0; i < fm.getNumFormats(); ++i)
    {
        auto* format = fm.getFormat(i);
        OwnedArray<PluginDescription> types;
        format->findAllTypesForFile(types, pluginFile.getFullPathName());

        if (types.size() > 0)
        {
            outDesc = *types[0]; // pick first
            foundAny = true;
            break;
        }
    }

    if (! foundAny)
        err = "No compatible plugin types found for file: " + pluginFile.getFullPathName();

    return foundAny;
}

static void tryConfigureBusesEx(AudioPluginInstance& inst, int reqIn, int reqOut, int reqSidechain)
{
    // Extended bus configuration with sidechain support
    inst.enableAllBuses();

    // Main buses (bus 0)
    if (inst.getBusCount(true) > 0)
    {
        if (reqIn > 0)
            inst.setChannelLayoutOfBus(true, 0, AudioChannelSet::canonicalChannelSet(reqIn));
    }

    if (inst.getBusCount(false) > 0)
    {
        if (reqOut > 0)
            inst.setChannelLayoutOfBus(false, 0, AudioChannelSet::canonicalChannelSet(reqOut));
    }

    // Sidechain bus (typically bus 1 for inputs)
    if (reqSidechain > 0 && inst.getBusCount(true) > 1)
    {
        auto* bus = inst.getBus(true, 1);
        if (bus != nullptr)
        {
            bus->enable(true);
            inst.setChannelLayoutOfBus(true, 1, AudioChannelSet::canonicalChannelSet(reqSidechain));
        }
    }

    // Apply combined layout
    auto layout = inst.getBusesLayout();
    inst.setBusesLayout(layout);
}

// Forward declaration -- mh_open delegates to mh_open_ex (defined below).
extern "C" MH_Plugin* mh_open_ex(const char* plugin_path,
                                 double sample_rate,
                                 int max_block_size,
                                 int main_in_ch,
                                 int main_out_ch,
                                 int sidechain_in_ch,
                                 char* err_buf,
                                 size_t err_buf_size);

extern "C" MH_Plugin* mh_open(const char* plugin_path,
                              double sample_rate,
                              int max_block_size,
                              int requested_in_ch,
                              int requested_out_ch,
                              char* err_buf,
                              size_t err_buf_size)
{
    return mh_open_ex(plugin_path, sample_rate, max_block_size,
                      requested_in_ch, requested_out_ch, /*sidechain_in_ch=*/0,
                      err_buf, err_buf_size);
}

extern "C" void mh_close(MH_Plugin* p)
{
    if (! p) return;
    // Destruction is thread-affine to JUCE's message thread, same as
    // construction. Marshal there when enabled (inline no-op otherwise).
    MinihostMessageThread::instance().run([&]()
    {
        if (p->inst)
        {
            p->inst->removeListener(&p->listener);
            p->inst->releaseResources();
        }
        delete p;
    });
}

extern "C" const char* mh_get_path(const MH_Plugin* p)
{
    if (!p) return "";
    return p->path.c_str();
}

extern "C" void* mh_get_juce_processor(MH_Plugin* p)
{
    if (!p || !p->inst) return nullptr;
    return static_cast<juce::AudioProcessor*>(p->inst.get());
}

extern "C" int mh_get_info(MH_Plugin* p, MH_Info* out_info)
{
    if (!p || !out_info || !p->inst) return 0;

    out_info->num_params      = (int) p->inst->getParameters().size();
    out_info->num_input_ch    = p->inCh;
    out_info->num_output_ch   = p->outCh;
    out_info->latency_samples = p->inst->getLatencySamples();
    out_info->accepts_midi    = p->inst->acceptsMidi() ? 1 : 0;
    out_info->produces_midi   = p->inst->producesMidi() ? 1 : 0;
    out_info->is_midi_effect  = p->inst->isMidiEffect() ? 1 : 0;
    out_info->supports_mpe    = p->inst->supportsMPE() ? 1 : 0;
    return 1;
}

extern "C" int mh_process_midi_io(MH_Plugin* p,
                                  const float* const* inputs,
                                  float* const* outputs,
                                  int nframes,
                                  const MH_MidiEvent* midi_in,
                                  int num_midi_in,
                                  MH_MidiEvent* midi_out,
                                  int midi_out_capacity,
                                  int* num_midi_out)
{
    if (!p || !p->inst) return 0;
    if (nframes < 0 || nframes > p->maxBlockSize) return 0;

    auto& buf = p->processBuf;
    const int totalCh = buf.getNumChannels();
    const size_t bytes = sizeof(float) * (size_t)nframes;

    // Match the buffer's reported sample count to nframes so processBlock
    // processes the right number of frames. avoidReallocating=true ensures no
    // heap activity as long as nframes <= maxBlockSize (already validated).
    buf.setSize(totalCh, nframes, false, false, true);

    // Copy main inputs into the first p->inCh channels of the combined buffer;
    // zero any remaining channels (sidechain slots, output-only channels when
    // outCh > inCh). This ensures the plugin sees a buffer with
    // max(inCh + sidechainCh, outCh) channels of valid data, as JUCE requires.
    if (inputs)
    {
        for (int ch = 0; ch < p->inCh; ++ch)
            std::memcpy(buf.getWritePointer(ch), inputs[ch], bytes);
    }
    else
    {
        for (int ch = 0; ch < p->inCh; ++ch)
            std::memset(buf.getWritePointer(ch), 0, bytes);
    }
    for (int ch = p->inCh; ch < totalCh; ++ch)
        std::memset(buf.getWritePointer(ch), 0, bytes);

    // Build MIDI input buffer from events
    p->midi.clear();
    if (midi_in && num_midi_in > 0)
    {
        for (int i = 0; i < num_midi_in; ++i)
        {
            const auto& ev = midi_in[i];
            int samplePos = jlimit(0, nframes - 1, ev.sample_offset);
            p->midi.addEvent(MidiMessage(ev.status, ev.data1, ev.data2), samplePos);
        }
    }

    p->inst->processBlock(buf, p->midi);

    // Copy outputs back from the first p->outCh channels.
    if (outputs)
    {
        for (int ch = 0; ch < p->outCh; ++ch)
            std::memcpy(outputs[ch], buf.getReadPointer(ch), bytes);
    }

    // Extract MIDI output events
    if (num_midi_out)
        *num_midi_out = 0;

    if (midi_out && midi_out_capacity > 0)
    {
        int outIdx = 0;
        for (const auto metadata : p->midi)
        {
            if (outIdx >= midi_out_capacity)
                break;

            auto msg = metadata.getMessage();
            if (msg.getRawDataSize() >= 1)
            {
                const auto* data = msg.getRawData();
                midi_out[outIdx].sample_offset = metadata.samplePosition;
                midi_out[outIdx].status = data[0];
                midi_out[outIdx].data1 = msg.getRawDataSize() > 1 ? data[1] : 0;
                midi_out[outIdx].data2 = msg.getRawDataSize() > 2 ? data[2] : 0;
                ++outIdx;
            }
        }
        if (num_midi_out)
            *num_midi_out = outIdx;
    }

    return 1;
}

extern "C" int mh_process_midi(MH_Plugin* p,
                               const float* const* inputs,
                               float* const* outputs,
                               int nframes,
                               const MH_MidiEvent* midi_events,
                               int num_midi_events)
{
    return mh_process_midi_io(p, inputs, outputs, nframes,
                              midi_events, num_midi_events,
                              nullptr, 0, nullptr);
}

extern "C" int mh_process(MH_Plugin* p,
                          const float* const* inputs,
                          float* const* outputs,
                          int nframes)
{
    return mh_process_midi_io(p, inputs, outputs, nframes,
                              nullptr, 0, nullptr, 0, nullptr);
}

extern "C" int mh_get_num_params(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    return (int) p->inst->getParameters().size();
}

extern "C" float mh_get_param(MH_Plugin* p, int index)
{
    if (!p || !p->inst) return 0.0f;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    auto& params = p->inst->getParameters();
    if (index < 0 || index >= params.size()) return 0.0f;
    return params.getUnchecked(index)->getValue();
}

extern "C" int mh_set_param(MH_Plugin* p, int index, float normalized_0_1)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    auto& params = p->inst->getParameters();
    if (index < 0 || index >= params.size()) return 0;

    normalized_0_1 = jlimit(0.0f, 1.0f, normalized_0_1);
    params.getUnchecked(index)->setValueNotifyingHost(normalized_0_1);
    return 1;
}

extern "C" int mh_get_param_info(MH_Plugin* p, int index, MH_ParamInfo* out_info)
{
    if (!p || !p->inst || !out_info) return 0;
    int result = 0;
    MinihostMessageThread::instance().run([&]()
    {
    std::lock_guard<std::mutex> lock(p->stateMutex);
    auto& params = p->inst->getParameters();
    if (index < 0 || index >= params.size()) return;

    auto* param = params.getUnchecked(index);

    // Name
    auto name = param->getName(MH_PARAM_NAME_LEN - 1);
    std::snprintf(out_info->name, MH_PARAM_NAME_LEN, "%s", name.toRawUTF8());

    // Stable parameter ID (via HostedAudioProcessorParameter)
    auto* hosted = p->inst->getHostedParameter(index);
    if (hosted)
        std::snprintf(out_info->id, MH_PARAM_NAME_LEN, "%s", hosted->getParameterID().toRawUTF8());
    else
        out_info->id[0] = '\0';

    // Label (unit)
    auto label = param->getLabel();
    std::snprintf(out_info->label, MH_PARAM_NAME_LEN, "%s", label.toRawUTF8());

    // Current value as display string
    auto valueStr = param->getCurrentValueAsText();
    std::snprintf(out_info->current_value_str, MH_PARAM_NAME_LEN, "%s", valueStr.toRawUTF8());

    // Normalized range (always 0-1 for JUCE parameters)
    out_info->min_value = 0.0f;
    out_info->max_value = 1.0f;
    out_info->default_value = param->getDefaultValue();

    // Discrete/continuous info
    out_info->num_steps = param->isDiscrete() ? param->getNumSteps() : 0;
    out_info->is_automatable = param->isAutomatable() ? 1 : 0;
    out_info->is_boolean = param->isBoolean() ? 1 : 0;
    out_info->category = static_cast<int>(param->getCategory());

    result = 1;
    });
    return result;
}

extern "C" int mh_get_state_size(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        MemoryBlock mb;
        p->inst->getStateInformation(mb);
        return (int) mb.getSize();
    });
}

extern "C" int mh_get_state(MH_Plugin* p, void* buffer, int buffer_size)
{
    if (!p || !p->inst || !buffer || buffer_size <= 0) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        MemoryBlock mb;
        p->inst->getStateInformation(mb);

        if ((int) mb.getSize() > buffer_size)
            return 0;

        std::memcpy(buffer, mb.getData(), mb.getSize());
        return 1;
    });
}

extern "C" int mh_set_state(MH_Plugin* p, const void* data, int data_size)
{
    if (!p || !p->inst || !data || data_size <= 0) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        p->inst->setStateInformation(data, data_size);
        return 1;
    });
}

extern "C" int mh_set_transport(MH_Plugin* p, const MH_TransportInfo* transport)
{
    if (!p) return 0;

    if (!transport)
    {
        MH_PlayHead::State s;
        s.hasTransport = false;
        p->playHead.write(s);
        return 1;
    }

    MH_PlayHead::State s;
    s.hasTransport = true;
    s.bpm = transport->bpm;
    s.timeSigNum = transport->time_sig_numerator;
    s.timeSigDenom = transport->time_sig_denominator;
    s.positionSamples = transport->position_samples;
    s.positionBeats = transport->position_beats;
    s.isPlaying = transport->is_playing != 0;
    s.isRecording = transport->is_recording != 0;
    s.isLooping = transport->is_looping != 0;
    s.loopStartSamples = transport->loop_start_samples;
    s.loopEndSamples = transport->loop_end_samples;
    p->playHead.write(s);

    return 1;
}

extern "C" double mh_get_tail_seconds(MH_Plugin* p)
{
    if (!p || !p->inst) return 0.0;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    return p->inst->getTailLengthSeconds();
}

extern "C" int mh_get_bypass(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);

    // Check if plugin has a bypass parameter
    auto* bypassParam = p->inst->getBypassParameter();
    if (bypassParam)
        return bypassParam->getValue() > 0.5f ? 1 : 0;

    return 0;
}

extern "C" int mh_set_bypass(MH_Plugin* p, int bypass)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);

    auto* bypassParam = p->inst->getBypassParameter();
    if (bypassParam)
    {
        bypassParam->setValueNotifyingHost(bypass ? 1.0f : 0.0f);
        return 1;
    }

    // Plugin doesn't support bypass parameter - could implement manual bypass here
    // by skipping processBlock, but that's host-level behavior
    return 0;
}

extern "C" int mh_get_latency_samples(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    return p->inst->getLatencySamples();
}

extern "C" int mh_process_auto(MH_Plugin* p,
                               const float* const* inputs,
                               float* const* outputs,
                               int nframes,
                               const MH_MidiEvent* midi_in,
                               int num_midi_in,
                               MH_MidiEvent* midi_out,
                               int midi_out_capacity,
                               int* num_midi_out,
                               const MH_ParamChange* param_changes,
                               int num_param_changes)
{
    if (!p || !p->inst) return 0;
    if (nframes < 0 || nframes > p->maxBlockSize) return 0;

    // Initialize MIDI output count
    if (num_midi_out)
        *num_midi_out = 0;
    int midi_out_idx = 0;

    // If no param changes, just use regular processing
    if (!param_changes || num_param_changes <= 0)
    {
        return mh_process_midi_io(p, inputs, outputs, nframes,
                                  midi_in, num_midi_in,
                                  midi_out, midi_out_capacity, num_midi_out);
    }

    // Process in chunks, applying parameter changes at the right sample positions
    int current_sample = 0;
    int midi_idx = 0;
    int param_idx = 0;

    while (current_sample < nframes)
    {
        // Apply every parameter change due at or before current_sample,
        // advancing param_idx past them. This must happen BEFORE computing
        // the chunk boundary: otherwise a change due exactly at
        // current_sample (which is not > current_sample) fails to bound the
        // chunk, chunk_end jumps to nframes, and any later change in this
        // block is silently dropped.
        while (param_idx < num_param_changes &&
               param_changes[param_idx].sample_offset <= current_sample)
        {
            const auto& pc = param_changes[param_idx];
            auto& params = p->inst->getParameters();
            if (pc.param_index >= 0 && pc.param_index < params.size())
            {
                float val = jlimit(0.0f, 1.0f, pc.value);
                params.getUnchecked(pc.param_index)->setValueNotifyingHost(val);
            }
            ++param_idx;
        }

        // Chunk boundary = the next still-pending change, or end of buffer.
        int chunk_end = nframes;
        if (param_idx < num_param_changes)
        {
            int next_change = param_changes[param_idx].sample_offset;
            if (next_change > current_sample && next_change < chunk_end)
                chunk_end = next_change;
        }

        int chunk_size = chunk_end - current_sample;
        if (chunk_size <= 0)
            break;

        // Stage chunk inputs into the combined processBuf. Same pattern as
        // mh_process_midi_io: copy main inputs to the first inCh channels,
        // zero any output-only channels above inCh.
        auto& buf = p->processBuf;
        const int totalCh = buf.getNumChannels();
        const size_t chunk_bytes = sizeof(float) * (size_t)chunk_size;
        buf.setSize(totalCh, chunk_size, false, false, true);

        if (inputs)
        {
            for (int ch = 0; ch < p->inCh; ++ch)
                std::memcpy(buf.getWritePointer(ch),
                            inputs[ch] + current_sample, chunk_bytes);
        }
        else
        {
            for (int ch = 0; ch < p->inCh; ++ch)
                std::memset(buf.getWritePointer(ch), 0, chunk_bytes);
        }
        for (int ch = p->inCh; ch < totalCh; ++ch)
            std::memset(buf.getWritePointer(ch), 0, chunk_bytes);

        // Collect MIDI events for this chunk
        p->midi.clear();
        while (midi_idx < num_midi_in)
        {
            const auto& ev = midi_in[midi_idx];
            if (ev.sample_offset >= chunk_end)
                break;
            if (ev.sample_offset >= current_sample)
            {
                int local_offset = ev.sample_offset - current_sample;
                p->midi.addEvent(MidiMessage(ev.status, ev.data1, ev.data2), local_offset);
            }
            ++midi_idx;
        }

        // Process this chunk
        p->inst->processBlock(buf, p->midi);

        // Copy outputs back into the caller's buffer at current_sample offset.
        if (outputs)
        {
            for (int ch = 0; ch < p->outCh; ++ch)
                std::memcpy(outputs[ch] + current_sample,
                            buf.getReadPointer(ch), chunk_bytes);
        }

        // Collect MIDI output from this chunk
        if (midi_out && midi_out_capacity > 0)
        {
            for (const auto metadata : p->midi)
            {
                if (midi_out_idx >= midi_out_capacity)
                    break;
                auto msg = metadata.getMessage();
                if (msg.getRawDataSize() >= 1)
                {
                    const auto* data = msg.getRawData();
                    midi_out[midi_out_idx].sample_offset = metadata.samplePosition + current_sample;
                    midi_out[midi_out_idx].status = data[0];
                    midi_out[midi_out_idx].data1 = msg.getRawDataSize() > 1 ? data[1] : 0;
                    midi_out[midi_out_idx].data2 = msg.getRawDataSize() > 2 ? data[2] : 0;
                    ++midi_out_idx;
                }
            }
        }

        current_sample = chunk_end;
    }

    if (num_midi_out)
        *num_midi_out = midi_out_idx;

    return 1;
}

extern "C" int mh_reset(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        p->inst->reset();
        return 1;
    });
}

extern "C" int mh_set_non_realtime(MH_Plugin* p, int non_realtime)
{
    if (!p || !p->inst) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        p->inst->setNonRealtime(non_realtime != 0);
        return 1;
    });
}

// Populate a format manager with all supported plugin formats.
static void initFormatManager(AudioPluginFormatManager& fm)
{
    fm.addFormat(std::make_unique<VST3Format>());
   #if JUCE_MAC
    fm.addFormat(std::make_unique<AUFormat>());
   #endif
   #if JUCE_PLUGINHOST_LV2
    fm.addFormat(std::make_unique<LV2Format>());
   #endif
}

// Internal probe using an existing format manager (avoids recreating one per call).
static int probeWithFm(AudioPluginFormatManager& fm,
                       const char* plugin_path,
                       MH_PluginDesc* out_desc,
                       char* err_buf,
                       size_t err_buf_size)
{
    if (!plugin_path || plugin_path[0] == '\0')
    {
        setErr(err_buf, err_buf_size, "plugin_path is empty");
        return 0;
    }

    if (!out_desc)
    {
        setErr(err_buf, err_buf_size, "out_desc is null");
        return 0;
    }

    std::memset(out_desc, 0, sizeof(MH_PluginDesc));

    File f(String::fromUTF8(plugin_path));
    if (!f.exists())
    {
        setErr(err_buf, err_buf_size, "Plugin file does not exist: " + f.getFullPathName());
        return 0;
    }

    PluginDescription desc;
    bool found = false;
    String formatName;

    for (int i = 0; i < fm.getNumFormats(); ++i)
    {
        auto* format = fm.getFormat(i);
        OwnedArray<PluginDescription> types;
        format->findAllTypesForFile(types, f.getFullPathName());

        if (types.size() > 0)
        {
            desc = *types[0];
            formatName = format->getName();
            found = true;
            break;
        }
    }

    if (!found)
    {
        setErr(err_buf, err_buf_size, "No compatible plugin types found for file: " + f.getFullPathName());
        return 0;
    }

    std::snprintf(out_desc->name, sizeof(out_desc->name), "%s", desc.name.toRawUTF8());
    std::snprintf(out_desc->vendor, sizeof(out_desc->vendor), "%s", desc.manufacturerName.toRawUTF8());
    std::snprintf(out_desc->version, sizeof(out_desc->version), "%s", desc.version.toRawUTF8());
    std::snprintf(out_desc->format, sizeof(out_desc->format), "%s", formatName.toRawUTF8());

    std::snprintf(out_desc->unique_id, sizeof(out_desc->unique_id), "%08X", desc.uniqueId);

    // accepts_midi/produces_midi are a best-effort heuristic at probe time.
    // JUCE's PluginDescription does not expose dedicated MIDI-input/output
    // flags in this version, so the value is conservatively derived from
    // isInstrument (instruments accept MIDI). MIDI effects, MIDI generators,
    // and analyzer plugins are NOT correctly classified by this heuristic.
    // Callers needing authoritative values must call mh_open + mh_get_info,
    // which queries the instantiated plugin directly.
    out_desc->accepts_midi = desc.isInstrument ? 1 : 0;
    out_desc->produces_midi = 0;
    out_desc->num_inputs = desc.numInputChannels;
    out_desc->num_outputs = desc.numOutputChannels;

    return 1;
}

extern "C" int mh_probe(const char* plugin_path,
                        MH_PluginDesc* out_desc,
                        char* err_buf,
                        size_t err_buf_size)
{
    AudioPluginFormatManager fm;
    initFormatManager(fm);
    return probeWithFm(fm, plugin_path, out_desc, err_buf, err_buf_size);
}

extern "C" int mh_param_to_text(MH_Plugin* p, int index, float value, char* buf, size_t buf_size)
{
    if (!p || !p->inst || !buf || buf_size == 0) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        auto& params = p->inst->getParameters();
        if (index < 0 || index >= params.size()) return 0;

        auto* param = params.getUnchecked(index);
        value = jlimit(0.0f, 1.0f, value);

        // getText returns the display string for a given normalized value.
        String text = param->getText(value, static_cast<int>(buf_size) - 1);
        std::snprintf(buf, buf_size, "%s", text.toRawUTF8());
        return 1;
    });
}

extern "C" int mh_param_from_text(MH_Plugin* p, int index, const char* text, float* out_value)
{
    if (!p || !p->inst || !text || !out_value) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        auto& params = p->inst->getParameters();
        if (index < 0 || index >= params.size()) return 0;

        auto* param = params.getUnchecked(index);
        // getValueForText converts display string to normalized value.
        float value = param->getValueForText(String::fromUTF8(text));
        *out_value = jlimit(0.0f, 1.0f, value);
        return 1;
    });
}

extern "C" int mh_get_num_programs(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        return p->inst->getNumPrograms();
    });
}

extern "C" int mh_get_program_name(MH_Plugin* p, int index, char* buf, size_t buf_size)
{
    if (!p || !p->inst || !buf || buf_size == 0) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        int numPrograms = p->inst->getNumPrograms();
        if (index < 0 || index >= numPrograms) return 0;

        String name = p->inst->getProgramName(index);
        std::snprintf(buf, buf_size, "%s", name.toRawUTF8());
        return 1;
    });
}

extern "C" int mh_get_program(MH_Plugin* p)
{
    if (!p || !p->inst) return -1;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        return p->inst->getCurrentProgram();
    });
}

extern "C" int mh_set_program(MH_Plugin* p, int index)
{
    if (!p || !p->inst) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        int numPrograms = p->inst->getNumPrograms();
        if (index < 0 || index >= numPrograms) return 0;

        p->inst->setCurrentProgram(index);
        return 1;
    });
}

extern "C" int mh_get_num_buses(MH_Plugin* p, int is_input)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    return p->inst->getBusCount(is_input != 0);
}

extern "C" int mh_get_bus_info(MH_Plugin* p, int is_input, int bus_index, MH_BusInfo* out_info)
{
    if (!p || !p->inst || !out_info) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);

    bool input = (is_input != 0);
    int numBuses = p->inst->getBusCount(input);
    if (bus_index < 0 || bus_index >= numBuses) return 0;

    auto* bus = p->inst->getBus(input, bus_index);
    if (!bus) return 0;

    // Clear output struct
    std::memset(out_info, 0, sizeof(MH_BusInfo));

    // Bus name
    String name = bus->getName();
    std::snprintf(out_info->name, sizeof(out_info->name), "%s", name.toRawUTF8());

    // Channel count
    out_info->num_channels = bus->getNumberOfChannels();

    // Is main bus (bus 0 is typically the main bus)
    out_info->is_main = (bus_index == 0) ? 1 : 0;

    // Is enabled
    out_info->is_enabled = bus->isEnabled() ? 1 : 0;

    return 1;
}

// Plugin-construction core. Takes an already-initialized format manager
// (caller's responsibility) so a session can pass its shared manager.
static MH_Plugin* createPluginWithFm_impl(AudioPluginFormatManager& fm,
                                      const char* plugin_path,
                                      double sample_rate,
                                      int max_block_size,
                                      int main_in_ch,
                                      int main_out_ch,
                                      int sidechain_in_ch,
                                      char* err_buf,
                                      size_t err_buf_size)
{
    if (plugin_path == nullptr || plugin_path[0] == '\0')
    {
        setErr(err_buf, err_buf_size, "plugin_path is empty");
        return nullptr;
    }

    std::unique_ptr<MH_Plugin> p(new MH_Plugin());
    p->sampleRate = sample_rate;
    p->maxBlockSize = max_block_size;
    p->path = plugin_path;

    File f(String::fromUTF8(plugin_path));
    if (! f.exists())
    {
        setErr(err_buf, err_buf_size, "Plugin file does not exist: " + f.getFullPathName());
        return nullptr;
    }

    PluginDescription desc;
    String err;
    if (! findFirstTypeForFile(fm, f, desc, err))
    {
        setErr(err_buf, err_buf_size, err);
        return nullptr;
    }

    String createErr;
    std::unique_ptr<AudioPluginInstance> inst(
        fm.createPluginInstance(desc, sample_rate, max_block_size, createErr)
    );

    if (! inst)
    {
        setErr(err_buf, err_buf_size, "createPluginInstance failed: " + createErr);
        return nullptr;
    }

    // Extended bus/channel layout with sidechain
    tryConfigureBusesEx(*inst, main_in_ch, main_out_ch, sidechain_in_ch);

    // Determine actual sidechain channels
    p->sidechainCh = 0;
    if (sidechain_in_ch > 0 && inst->getBusCount(true) > 1)
    {
        auto* scBus = inst->getBus(true, 1);
        if (scBus && scBus->isEnabled())
        {
            p->sidechainCh = scBus->getNumberOfChannels();
        }
    }

    inst->setRateAndBufferSizeDetails(sample_rate, max_block_size);
    inst->prepareToPlay(sample_rate, max_block_size);

    // Report the plugin's true channel counts after bus config and
    // prepareToPlay. A synthesizer with no audio input honestly reports 0
    // inputs (not an inflated 1), and the rare 0-output plugin reports 0
    // outputs, so callers can distinguish instruments from effects. Buffer
    // validation is "at least" (see validate_process_shape in the bindings),
    // so honest lower counts never reject an over-provisioned caller.
    p->inCh  = jmax(0, inst->getTotalNumInputChannels());
    p->outCh = jmax(0, inst->getTotalNumOutputChannels());

    // Allocate the combined processing buffer once. Size: max channels
    // required across input (main + sidechain) and output. JUCE's processBlock
    // reads inputs from channels 0..inCh+sidechainCh-1 and writes outputs to
    // channels 0..outCh-1, so the buffer must accommodate both. Keep at least
    // one channel so a pure-MIDI plugin (0 in / 0 out) still receives a valid
    // (if unused) buffer to hand to processBlock.
    int totalProcessCh = jmax(1, jmax(p->inCh + p->sidechainCh, p->outCh));
    p->processBuf.setSize(totalProcessCh, max_block_size, false, false, true);
    p->processBufD.setSize(totalProcessCh, max_block_size, false, false, true);

    // Set up playhead for transport info
    p->playHead.sampleRate = sample_rate;
    inst->setPlayHead(&p->playHead);

    p->inst = std::move(inst);
    p->inst->addListener(&p->listener);
    return p.release();
}

// Thread-marshaling wrapper: run construction on the JUCE message thread when
// enabled (SPIKE), else inline on the caller's thread (default behavior).
static MH_Plugin* createPluginWithFm(AudioPluginFormatManager& fm,
                                     const char* plugin_path,
                                     double sample_rate,
                                     int max_block_size,
                                     int main_in_ch,
                                     int main_out_ch,
                                     int sidechain_in_ch,
                                     char* err_buf,
                                     size_t err_buf_size)
{
    MH_Plugin* result = nullptr;
    MinihostMessageThread::instance().run([&]()
    {
        result = createPluginWithFm_impl(fm, plugin_path, sample_rate,
                                         max_block_size, main_in_ch, main_out_ch,
                                         sidechain_in_ch, err_buf, err_buf_size);
    });
    return result;
}

extern "C" MH_Plugin* mh_open_ex(const char* plugin_path,
                                 double sample_rate,
                                 int max_block_size,
                                 int main_in_ch,
                                 int main_out_ch,
                                 int sidechain_in_ch,
                                 char* err_buf,
                                 size_t err_buf_size)
{
    // Non-session entry point: build a one-shot format manager local
    // to this call. Callers wanting to amortize the registration cost
    // across many loads should use mh_session_open instead.
    AudioPluginFormatManager fm;
    initFormatManager(fm);
    return createPluginWithFm(fm, plugin_path, sample_rate, max_block_size,
                               main_in_ch, main_out_ch, sidechain_in_ch,
                               err_buf, err_buf_size);
}

extern "C" int mh_process_sidechain(MH_Plugin* p,
                                    const float* const* main_in,
                                    float* const* main_out,
                                    const float* const* sidechain_in,
                                    int nframes)
{
    if (!p || !p->inst) return 0;
    if (nframes < 0 || nframes > p->maxBlockSize) return 0;

    // Use the pre-allocated combined processBuf (sized in mh_open_ex to
    // max(inCh + sidechainCh, outCh)) to avoid per-call heap allocation.
    auto& buffer = p->processBuf;
    int totalInCh = p->inCh + p->sidechainCh;
    int totalCh = buffer.getNumChannels();
    buffer.setSize(totalCh, nframes, false, false, true);

    // Copy main input to first channels
    if (main_in)
    {
        for (int ch = 0; ch < p->inCh; ++ch)
            buffer.copyFrom(ch, 0, main_in[ch], nframes);
    }
    else
    {
        for (int ch = 0; ch < p->inCh; ++ch)
            buffer.clear(ch, 0, nframes);
    }

    // Copy sidechain input to subsequent channels
    if (sidechain_in && p->sidechainCh > 0)
    {
        for (int ch = 0; ch < p->sidechainCh; ++ch)
            buffer.copyFrom(p->inCh + ch, 0, sidechain_in[ch], nframes);
    }
    else if (p->sidechainCh > 0)
    {
        for (int ch = p->inCh; ch < totalInCh; ++ch)
            buffer.clear(ch, 0, nframes);
    }

    // Clear any remaining output channels
    for (int ch = totalInCh; ch < totalCh; ++ch)
        buffer.clear(ch, 0, nframes);

    // Clear MIDI buffer
    p->midi.clear();

    // Process
    p->inst->processBlock(buffer, p->midi);

    // Copy output back to caller's buffer
    if (main_out)
    {
        for (int ch = 0; ch < p->outCh; ++ch)
            std::memcpy(main_out[ch], buffer.getReadPointer(ch), sizeof(float) * nframes);
    }

    return 1;
}

extern "C" int mh_get_sidechain_channels(MH_Plugin* p)
{
    if (!p) return 0;
    return p->sidechainCh;
}

extern "C" int mh_check_buses_layout(MH_Plugin* p,
                                     const int* input_channels, int num_input_buses,
                                     const int* output_channels, int num_output_buses)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);

    AudioProcessor::BusesLayout layout;

    for (int i = 0; i < num_input_buses; ++i)
    {
        int ch = (input_channels && i < num_input_buses) ? input_channels[i] : 0;
        layout.inputBuses.add(AudioChannelSet::canonicalChannelSet(ch));
    }

    for (int i = 0; i < num_output_buses; ++i)
    {
        int ch = (output_channels && i < num_output_buses) ? output_channels[i] : 0;
        layout.outputBuses.add(AudioChannelSet::canonicalChannelSet(ch));
    }

    return p->inst->checkBusesLayoutSupported(layout) ? 1 : 0;
}

extern "C" int mh_set_change_callback(MH_Plugin* p, MH_ChangeCallback cb, void* user_data)
{
    if (!p) return 0;
    p->listener.changeUserData.store(user_data, std::memory_order_relaxed);
    p->listener.changeCb.store(cb, std::memory_order_release);
    return 1;
}

extern "C" int mh_set_param_value_callback(MH_Plugin* p, MH_ParamValueCallback cb, void* user_data)
{
    if (!p) return 0;
    p->listener.paramValueUserData.store(user_data, std::memory_order_relaxed);
    p->listener.paramValueCb.store(cb, std::memory_order_release);
    return 1;
}

extern "C" int mh_set_param_gesture_callback(MH_Plugin* p, MH_ParamGestureCallback cb, void* user_data)
{
    if (!p) return 0;
    p->listener.paramGestureUserData.store(user_data, std::memory_order_relaxed);
    p->listener.paramGestureCb.store(cb, std::memory_order_release);
    return 1;
}

extern "C" int mh_begin_param_gesture(MH_Plugin* p, int index)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    auto& params = p->inst->getParameters();
    if (index < 0 || index >= params.size()) return 0;
    params.getUnchecked(index)->beginChangeGesture();
    return 1;
}

extern "C" int mh_end_param_gesture(MH_Plugin* p, int index)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);
    auto& params = p->inst->getParameters();
    if (index < 0 || index >= params.size()) return 0;
    params.getUnchecked(index)->endChangeGesture();
    return 1;
}

extern "C" int mh_get_program_state_size(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        MemoryBlock mb;
        p->inst->getCurrentProgramStateInformation(mb);
        return (int) mb.getSize();
    });
}

extern "C" int mh_get_program_state(MH_Plugin* p, void* buffer, int buffer_size)
{
    if (!p || !p->inst || !buffer || buffer_size <= 0) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        MemoryBlock mb;
        p->inst->getCurrentProgramStateInformation(mb);

        if ((int) mb.getSize() > buffer_size)
            return 0;

        std::memcpy(buffer, mb.getData(), mb.getSize());
        return 1;
    });
}

extern "C" int mh_set_program_state(MH_Plugin* p, const void* data, int data_size)
{
    if (!p || !p->inst || !data || data_size <= 0) return 0;
    return runOnMsg([&]() -> int
    {
        std::lock_guard<std::mutex> lock(p->stateMutex);
        p->inst->setCurrentProgramStateInformation(data, data_size);
        return 1;
    });
}

extern "C" int mh_set_sample_rate(MH_Plugin* p, double new_sample_rate)
{
    if (!p || !p->inst) return 0;
    // Reject obviously invalid rates up front; common SR range is 8000-384000.
    // We don't enforce a hard upper bound (some plugins support 768 kHz) but
    // negative/zero/NaN must fail fast.
    if (!(new_sample_rate > 0.0)) return 0;

    return runOnMsg([&]() -> int
    {
    std::lock_guard<std::mutex> lock(p->stateMutex);

    // Save current state (both blob and individual param values as fallback)
    MemoryBlock stateData;
    p->inst->getStateInformation(stateData);

    auto& params = p->inst->getParameters();
    std::vector<float> paramValues(params.size());
    for (int i = 0; i < (int)params.size(); ++i)
        paramValues[i] = params[i]->getValue();

    // Release current resources
    p->inst->releaseResources();

    // Update sample rate
    p->sampleRate = new_sample_rate;
    p->playHead.sampleRate = new_sample_rate;

    // Re-prepare with new sample rate
    p->inst->setRateAndBufferSizeDetails(new_sample_rate, p->maxBlockSize);
    p->inst->prepareToPlay(new_sample_rate, p->maxBlockSize);

    // Verify the plugin actually accepted the rate. JUCE's prepareToPlay is
    // void, so detection is via getSampleRate() reflecting back. If the
    // plugin internally clamped or ignored the request, fail loudly so the
    // caller doesn't silently process at the wrong rate.
    double actual = p->inst->getSampleRate();
    if (std::abs(actual - new_sample_rate) > 0.5)
    {
        // Best-effort recovery: re-prepare at whatever the plugin reports,
        // and roll back our own bookkeeping so subsequent process calls
        // match reality.
        p->sampleRate = actual;
        p->playHead.sampleRate = actual;
        return 0;
    }

    // Restore state from blob
    if (stateData.getSize() > 0)
    {
        p->inst->setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));
    }

    // Fix up any parameters that the state blob didn't restore correctly
    for (int i = 0; i < (int)params.size(); ++i)
    {
        float current = params[i]->getValue();
        float diff = current - paramValues[i];
        if (diff > 1e-6f || diff < -1e-6f)
            params[i]->setValueNotifyingHost(paramValues[i]);
    }

    return 1;
    });
}

extern "C" double mh_get_sample_rate(MH_Plugin* p)
{
    if (!p) return 0.0;
    return p->sampleRate;
}

extern "C" int mh_scan_directory(const char* directory_path,
                                 MH_ScanCallback callback,
                                 void* user_data)
{
    if (!directory_path || directory_path[0] == '\0' || !callback)
        return -1;

    File dir(String::fromUTF8(directory_path));
    if (!dir.exists() || !dir.isDirectory())
        return -1;

    int count = 0;

    // Find all .vst3 and .component files recursively
    Array<File> pluginFiles;

    // VST3 plugins (.vst3 bundles)
    dir.findChildFiles(pluginFiles, File::findDirectories, true, "*.vst3");

    // AudioUnit plugins (.component bundles) - macOS only
   #if JUCE_MAC
    dir.findChildFiles(pluginFiles, File::findDirectories, true, "*.component");
   #endif

    // LV2 plugins (.lv2 bundles)
   #if JUCE_PLUGINHOST_LV2
    dir.findChildFiles(pluginFiles, File::findDirectories, true, "*.lv2");
   #endif

    // Create one format manager for the entire scan instead of one per plugin.
    AudioPluginFormatManager fm;
    initFormatManager(fm);

    for (const auto& pluginFile : pluginFiles)
    {
        MH_PluginDesc desc;
        char errBuf[256];

        if (probeWithFm(fm, pluginFile.getFullPathName().toRawUTF8(), &desc, errBuf, sizeof(errBuf)))
        {
            std::snprintf(desc.path, sizeof(desc.path), "%s",
                          pluginFile.getFullPathName().toRawUTF8());

            callback(&desc, user_data);
            ++count;
        }
    }

    return count;
}

extern "C" int mh_supports_double(MH_Plugin* p)
{
    if (!p || !p->inst) return 0;
    return p->inst->supportsDoublePrecisionProcessing() ? 1 : 0;
}

extern "C" int mh_get_processing_precision(MH_Plugin* p)
{
    if (!p || !p->inst) return MH_PRECISION_SINGLE;
    return p->inst->getProcessingPrecision() == AudioProcessor::doublePrecision
        ? MH_PRECISION_DOUBLE : MH_PRECISION_SINGLE;
}

extern "C" int mh_set_processing_precision(MH_Plugin* p, int precision)
{
    if (!p || !p->inst) return 0;
    if (precision != MH_PRECISION_SINGLE && precision != MH_PRECISION_DOUBLE) return 0;

    return runOnMsg([&]() -> int
    {
    // Double precision requires plugin support
    if (precision == MH_PRECISION_DOUBLE && !p->inst->supportsDoublePrecisionProcessing())
        return 0;

    std::lock_guard<std::mutex> lock(p->stateMutex);

    auto newPrecision = (precision == MH_PRECISION_DOUBLE)
        ? AudioProcessor::doublePrecision
        : AudioProcessor::singlePrecision;

    // Skip if already at the requested precision
    if (p->inst->getProcessingPrecision() == newPrecision)
        return 1;

    // Save state, release, set precision, re-prepare, restore state
    MemoryBlock stateData;
    p->inst->getStateInformation(stateData);

    p->inst->releaseResources();
    p->inst->setProcessingPrecision(newPrecision);
    p->inst->setRateAndBufferSizeDetails(p->sampleRate, p->maxBlockSize);
    p->inst->prepareToPlay(p->sampleRate, p->maxBlockSize);

    // Verify the plugin actually switched precision. setProcessingPrecision
    // is a request, not a guarantee -- some plugins decline doublePrecision
    // even after supportsDoublePrecisionProcessing() returns true (e.g.,
    // when an internal DSP unit only has a float code path). Surface the
    // mismatch instead of silently processing at the wrong precision.
    if (p->inst->getProcessingPrecision() != newPrecision)
    {
        // Plugin is now in some other (its preferred) precision; restore
        // state so we leave it consistent before returning failure.
        if (stateData.getSize() > 0)
            p->inst->setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));
        return 0;
    }

    if (stateData.getSize() > 0)
        p->inst->setStateInformation(stateData.getData(), static_cast<int>(stateData.getSize()));

    return 1;
    });
}

extern "C" int mh_set_track_properties(MH_Plugin* p, const char* name,
                                       int has_colour, unsigned int colour_argb)
{
    if (!p || !p->inst) return 0;
    std::lock_guard<std::mutex> lock(p->stateMutex);

    AudioProcessor::TrackProperties props;

    if (name)
        props.name = String::fromUTF8(name);
    // else props.name stays as nullopt (cleared)

    if (has_colour)
        props.colourARGB = colour_argb;
    // else props.colourARGB stays as nullopt (cleared)

    p->inst->updateTrackProperties(props);
    return 1;
}

extern "C" int mh_process_double(MH_Plugin* p,
                                 const double* const* inputs,
                                 double* const* outputs,
                                 int nframes)
{
    if (!p || !p->inst) return 0;
    if (nframes < 0 || nframes > p->maxBlockSize) return 0;

    // Both branches use persistent buffers (sized once in mh_open_ex) to
    // avoid heap allocation on the audio thread. setSize with
    // avoidReallocating=true updates the reported sample count without
    // reallocating because nframes <= maxBlockSize is already validated.
    if (p->inst->supportsDoublePrecisionProcessing())
    {
        auto& buf = p->processBufD;
        const int totalCh = buf.getNumChannels();
        const size_t bytes = sizeof(double) * (size_t)nframes;
        buf.setSize(totalCh, nframes, false, false, true);

        if (inputs)
        {
            for (int ch = 0; ch < p->inCh; ++ch)
                std::memcpy(buf.getWritePointer(ch), inputs[ch], bytes);
        }
        else
        {
            for (int ch = 0; ch < p->inCh; ++ch)
                std::memset(buf.getWritePointer(ch), 0, bytes);
        }
        for (int ch = p->inCh; ch < totalCh; ++ch)
            std::memset(buf.getWritePointer(ch), 0, bytes);

        p->midi.clear();
        p->inst->processBlock(buf, p->midi);

        if (outputs)
        {
            for (int ch = 0; ch < p->outCh; ++ch)
                std::memcpy(outputs[ch], buf.getReadPointer(ch), bytes);
        }
    }
    else
    {
        // Plugin only supports float; convert double <-> float through the
        // existing float processBuf.
        auto& buf = p->processBuf;
        const int totalCh = buf.getNumChannels();
        buf.setSize(totalCh, nframes, false, false, true);

        if (inputs)
        {
            for (int ch = 0; ch < p->inCh; ++ch)
            {
                auto* dest = buf.getWritePointer(ch);
                const double* src = inputs[ch];
                for (int i = 0; i < nframes; ++i)
                    dest[i] = static_cast<float>(src[i]);
            }
        }
        else
        {
            for (int ch = 0; ch < p->inCh; ++ch)
                std::memset(buf.getWritePointer(ch), 0,
                            sizeof(float) * (size_t)nframes);
        }
        for (int ch = p->inCh; ch < totalCh; ++ch)
            std::memset(buf.getWritePointer(ch), 0,
                        sizeof(float) * (size_t)nframes);

        p->midi.clear();
        p->inst->processBlock(buf, p->midi);

        if (outputs)
        {
            for (int ch = 0; ch < p->outCh; ++ch)
            {
                const auto* src = buf.getReadPointer(ch);
                double* dest = outputs[ch];
                for (int i = 0; i < nframes; ++i)
                    dest[i] = static_cast<double>(src[i]);
            }
        }
    }

    return 1;
}

extern "C" int mh_open_async(const char* plugin_path,
                             double sample_rate,
                             int max_block_size,
                             int requested_in_ch,
                             int requested_out_ch,
                             MH_LoadCallback callback,
                             void* user_data)
{
    if (!plugin_path || plugin_path[0] == '\0' || !callback)
        return 0;

    // Copy parameters for the thread
    std::string path(plugin_path);

    // Launch loading thread (detached - it manages its own lifetime)
    std::thread loadThread([=]() {
        char errBuf[1024] = {0};
        MH_Plugin* plugin = mh_open(path.c_str(), sample_rate, max_block_size,
                                     requested_in_ch, requested_out_ch,
                                     errBuf, sizeof(errBuf));

        if (plugin)
        {
            callback(plugin, nullptr, user_data);
        }
        else
        {
            callback(nullptr, errBuf, user_data);
        }
    });

    loadThread.detach();
    return 1;
}

// ---------------------------------------------------------------------------
// Session entry points
// ---------------------------------------------------------------------------

// Internal: factored out of mh_scan_directory so the session variant
// can pass its own format manager.
static int scanDirectoryWithFm(AudioPluginFormatManager& fm,
                                const char* directory_path,
                                MH_ScanCallback callback,
                                void* user_data)
{
    if (!directory_path || directory_path[0] == '\0' || !callback)
        return -1;

    File dir(String::fromUTF8(directory_path));
    if (!dir.exists() || !dir.isDirectory())
        return -1;

    int count = 0;
    Array<File> pluginFiles;

    dir.findChildFiles(pluginFiles, File::findDirectories, true, "*.vst3");
   #if JUCE_MAC
    dir.findChildFiles(pluginFiles, File::findDirectories, true, "*.component");
   #endif
   #if JUCE_PLUGINHOST_LV2
    dir.findChildFiles(pluginFiles, File::findDirectories, true, "*.lv2");
   #endif

    for (const auto& pluginFile : pluginFiles)
    {
        MH_PluginDesc desc;
        char errBuf[256];

        if (probeWithFm(fm, pluginFile.getFullPathName().toRawUTF8(),
                         &desc, errBuf, sizeof(errBuf)))
        {
            std::snprintf(desc.path, sizeof(desc.path), "%s",
                          pluginFile.getFullPathName().toRawUTF8());
            callback(&desc, user_data);
            ++count;
        }
    }

    return count;
}

extern "C" MH_Session* mh_session_create(char* err_buf, size_t err_buf_size)
{
    try
    {
        std::unique_ptr<MH_Session> s(new MH_Session());
        initFormatManager(s->fm);
        return s.release();
    }
    catch (const std::exception& e)
    {
        setErr(err_buf, err_buf_size,
               String("mh_session_create failed: ") + e.what());
        return nullptr;
    }
}

extern "C" void mh_session_close(MH_Session* session)
{
    // Plugins previously created via this session do not reference
    // the format manager (the AudioPluginInstance is self-contained
    // post-creation), so closing the session is safe even while
    // plugins remain in use.
    if (session) delete session;
}

extern "C" MH_Plugin* mh_session_open(MH_Session* session,
                                       const char* plugin_path,
                                       double sample_rate,
                                       int max_block_size,
                                       int main_in_ch,
                                       int main_out_ch,
                                       int sidechain_in_ch,
                                       char* err_buf,
                                       size_t err_buf_size)
{
    if (!session)
    {
        setErr(err_buf, err_buf_size, "session is null");
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(session->mtx);
    return createPluginWithFm(session->fm, plugin_path,
                               sample_rate, max_block_size,
                               main_in_ch, main_out_ch, sidechain_in_ch,
                               err_buf, err_buf_size);
}

extern "C" int mh_session_probe(MH_Session* session,
                                 const char* plugin_path,
                                 MH_PluginDesc* out_desc,
                                 char* err_buf,
                                 size_t err_buf_size)
{
    if (!session)
    {
        setErr(err_buf, err_buf_size, "session is null");
        return 0;
    }
    std::lock_guard<std::mutex> lock(session->mtx);
    return probeWithFm(session->fm, plugin_path, out_desc,
                        err_buf, err_buf_size);
}

extern "C" int mh_session_scan_directory(MH_Session* session,
                                          const char* directory_path,
                                          MH_ScanCallback callback,
                                          void* user_data)
{
    if (!session) return -1;
    std::lock_guard<std::mutex> lock(session->mtx);
    return scanDirectoryWithFm(session->fm, directory_path, callback, user_data);
}

