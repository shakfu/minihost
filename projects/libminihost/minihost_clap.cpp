// minihost_clap.cpp
// CLAP plugin format for JUCE. See minihost_clap.h for scope.
//
// Threads. CLAP's "main thread" is minihost's plugin thread: every control
// call reaches the instance through runOnMsg there. Requests a plugin makes
// from any thread (request_callback, request_restart, latency changed,
// request_flush) only set flags; service() acts on them from the plugin
// thread. JUCE's own async queue is not used for this: on macOS it delivers
// on the process main thread, which is not the thread the plugin was made on.
//
// CLAP's "audio thread" calls (process, start/stop_processing, reset, flush
// while active) are serialised by a spin token. The control thread takes it
// for the short audio-thread calls it has to make itself (reset, a flush
// before saving state, deactivate/activate on restart), so they never run
// concurrently with process(). The thread-check extension reports "audio
// thread" exactly while one of those calls is in progress.

#include "minihost_clap.h"

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#ifdef MINIHOST_HEADLESS
 #include <juce_audio_processors_headless/juce_audio_processors_headless.h>
#else
 #include <juce_audio_processors/juce_audio_processors.h>
#endif

#include <clap/clap.h>

#include <minihost_version.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// Plugin editors: the GUI build only, and Cocoa embedding only so far.
#if ! defined(MINIHOST_HEADLESS) && JUCE_MAC
 #define MINIHOST_CLAP_EDITOR 1
extern "C" void* mh_clap_nsview_create(int width, int height);
extern "C" void  mh_clap_nsview_release(void* view);
#else
 #define MINIHOST_CLAP_EDITOR 0
#endif

#if JUCE_MAC
extern "C" void* mh_clap_bundle_open(const char* path);
extern "C" void* mh_clap_bundle_symbol(void* bundle, const char* name);
extern "C" void  mh_clap_bundle_close(void* bundle);
#endif

namespace minihost::clapfmt {
namespace {

using namespace juce;

constexpr const char* kFormatName = "CLAP";

String str(const char* s) { return s != nullptr ? String::fromUTF8(s) : String(); }

// True while this thread is inside a CLAP [audio-thread] call.
thread_local bool tlAudio = false;

struct AudioScope
{
    bool prev;
    AudioScope() : prev(tlAudio) { tlAudio = true; }
    ~AudioScope() { tlAudio = prev; }
};

// ---------------------------------------------------------------------------
// Modules. One clap_entry init() per binary while any instance or scan uses
// it, deinit() when the last one lets go, as the entry contract requires.
// ---------------------------------------------------------------------------

struct Module
{
    String path;
    int refs = 0;
    const clap_plugin_entry* entry = nullptr;
    std::unique_ptr<DynamicLibrary> lib;
    void* bundle = nullptr;
};

std::mutex gModMtx;
std::map<String, std::unique_ptr<Module>> gModules;

void unloadLocked(Module& m)
{
   #if JUCE_MAC
    if (m.bundle != nullptr) mh_clap_bundle_close(m.bundle);
    m.bundle = nullptr;
   #endif
    m.lib.reset();
}

class ModuleRef
{
public:
    ModuleRef() = default;
    ModuleRef(ModuleRef&& o) noexcept : m_(o.m_) { o.m_ = nullptr; }
    ModuleRef& operator=(ModuleRef&& o) noexcept { if (this != &o) { release(); m_ = o.m_; o.m_ = nullptr; } return *this; }
    ~ModuleRef() { release(); }

    static ModuleRef acquire(const String& path, String& err)
    {
        std::lock_guard<std::mutex> lk(gModMtx);
        auto it = gModules.find(path);
        if (it != gModules.end())
        {
            ++it->second->refs;
            return ModuleRef(it->second.get());
        }

        auto m = std::make_unique<Module>();
        m->path = path;
        void* sym = nullptr;
       #if JUCE_MAC
        if (File(path).isDirectory())
        {
            m->bundle = mh_clap_bundle_open(path.toRawUTF8());
            if (m->bundle != nullptr)
                sym = mh_clap_bundle_symbol(m->bundle, "clap_entry");
        }
        else
       #endif
        {
            m->lib = std::make_unique<DynamicLibrary>();
            if (m->lib->open(path))
                sym = m->lib->getFunction("clap_entry");
        }

        if (sym == nullptr)
        {
            err = "not a loadable CLAP module (no clap_entry): " + path;
            unloadLocked(*m);
            return {};
        }
        m->entry = static_cast<const clap_plugin_entry*>(sym);
        if (! clap_version_is_compatible(m->entry->clap_version))
        {
            err = "incompatible CLAP version " + String(m->entry->clap_version.major)
                  + "." + String(m->entry->clap_version.minor) + ": " + path;
            unloadLocked(*m);
            return {};
        }
        if (! m->entry->init(path.toRawUTF8()))
        {
            err = "clap_entry.init failed: " + path;
            unloadLocked(*m);
            return {};
        }

        m->refs = 1;
        auto* raw = m.get();
        gModules[path] = std::move(m);
        return ModuleRef(raw);
    }

    explicit operator bool() const { return m_ != nullptr; }

    const clap_plugin_factory* factory() const
    {
        if (m_ == nullptr) return nullptr;
        return static_cast<const clap_plugin_factory*>(
            m_->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    }

private:
    explicit ModuleRef(Module* m) : m_(m) {}

    void release()
    {
        if (m_ == nullptr) return;
        std::lock_guard<std::mutex> lk(gModMtx);
        if (--m_->refs == 0)
        {
            m_->entry->deinit();
            unloadLocked(*m_);
            gModules.erase(m_->path);
        }
        m_ = nullptr;
    }

    Module* m_ = nullptr;
};

// ---------------------------------------------------------------------------
// Event lists with fixed capacity: no allocation once prepared.
// ---------------------------------------------------------------------------

class EventList
{
public:
    EventList()
    {
        in_.ctx = this;
        in_.size = [](const clap_input_events* l) -> uint32_t
        { return (uint32_t) static_cast<EventList*>(l->ctx)->offsets_.size(); };
        in_.get = [](const clap_input_events* l, uint32_t i) -> const clap_event_header*
        { return static_cast<EventList*>(l->ctx)->get(i); };
        out_.ctx = this;
        out_.try_push = [](const clap_output_events* l, const clap_event_header* e) -> bool
        { return static_cast<EventList*>(l->ctx)->push(e); };
    }

    void reserve(size_t maxEvents, size_t bytes)
    {
        words_.assign((bytes + 7) / 8, 0);
        offsets_.clear();
        offsets_.reserve(maxEvents);
        used_ = 0;
    }

    void clear() { offsets_.clear(); used_ = 0; }

    bool push(const clap_event_header* e)
    {
        if (e == nullptr || e->size < sizeof(clap_event_header)) return false;
        const size_t need = ((size_t) e->size + 7) & ~(size_t) 7;
        if (offsets_.size() == offsets_.capacity() || used_ + need > words_.size() * 8)
        {
            ++dropped_;
            return false;
        }
        std::memcpy(reinterpret_cast<uint8_t*>(words_.data()) + used_, e, e->size);
        offsets_.push_back((uint32_t) used_);
        used_ += need;
        return true;
    }

    uint32_t size() const { return (uint32_t) offsets_.size(); }

    const clap_event_header* get(uint32_t i) const
    {
        if (i >= offsets_.size()) return nullptr;
        return reinterpret_cast<const clap_event_header*>(
            reinterpret_cast<const uint8_t*>(words_.data()) + offsets_[i]);
    }

    const clap_input_events* in() const { return &in_; }
    const clap_output_events* out() const { return &out_; }

private:
    std::vector<uint64_t> words_;   // 8-byte aligned storage
    std::vector<uint32_t> offsets_;
    size_t used_ = 0;
    uint64_t dropped_ = 0;
    clap_input_events in_{};
    clap_output_events out_{};
};

// ---------------------------------------------------------------------------
// Host. One per plugin instance (and per probe), so host_data identifies it.
// ---------------------------------------------------------------------------

class ClapInstance;
class ClapEditor;
struct Host;

std::mutex gHostMtx;
std::vector<Host*> gHosts;

// Wakes a JUCE message loop so it services the plugins created on its
// thread: the case where the app, not minihost's plugin thread, owns them.
// Leaked, because a pending update may still arrive during static teardown.
struct Notifier final : AsyncUpdater
{
    void handleAsyncUpdate() override { service(); }
};

Notifier& notifier()
{
    static auto* n = new Notifier();
    return *n;
}

struct Host : clap_host
{
    const clap_plugin* plugin = nullptr;
    ClapInstance* inst = nullptr;

    std::atomic<bool> wantCallback{false};
    std::atomic<bool> wantRestart{false};
    std::atomic<bool> wantLatency{false};
    std::atomic<bool> wantFlush{false};

    // CLAP's main thread: the one the plugin is created on.
    const std::thread::id mainThread = std::this_thread::get_id();

    Host()
    {
        clap_version = CLAP_VERSION;
        host_data = this;
        name = "minihost";
        vendor = "minihost";
        url = "https://github.com/shakfu/minihost";
        version = MINIHOST_VERSION;
        get_extension = &Host::getExtension;
        request_restart  = [](const clap_host* h) { from(h)->request(from(h)->wantRestart); };
        request_process  = [](const clap_host*) {};
        request_callback = [](const clap_host* h) { from(h)->request(from(h)->wantCallback); };

        notifier();   // constructed here, never first on the audio thread

        std::lock_guard<std::mutex> lk(gHostMtx);
        gHosts.push_back(this);
    }

    ~Host() { unregister(); }

    void unregister()
    {
        std::lock_guard<std::mutex> lk(gHostMtx);
        gHosts.erase(std::remove(gHosts.begin(), gHosts.end(), this), gHosts.end());
    }

    static Host* from(const clap_host* h) { return static_cast<Host*>(h->host_data); }

    // Any thread. The flag is acted on by service() on mainThread.
    void request(std::atomic<bool>& flag)
    {
        flag = true;
        notifier().triggerAsyncUpdate();
    }

    int serviceLocked();

    static const void* getExtension(const clap_host*, const char* id);
};

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

class ClapParam final : public HostedAudioProcessorParameter
{
public:
    ClapParam(ClapInstance& owner, const clap_param_info& info, double value)
        : owner_(owner), id(info.id), cookie(info.cookie)
    {
        setInfo(info);
        plain.store(value);
    }

    void setInfo(const clap_param_info& info)
    {
        flags.store(info.flags);
        lo.store(info.min_value);
        hi.store(info.max_value);
        def.store(info.default_value);
        name_ = String::fromUTF8(info.name);
    }

    bool stepped() const { return (flags.load() & CLAP_PARAM_IS_STEPPED) != 0; }

    double fromNorm(float v) const
    {
        const double a = lo.load(), b = hi.load();
        double p = a + (double) jlimit(0.0f, 1.0f, v) * (b - a);
        return stepped() ? std::round(p) : p;
    }

    float toNorm(double p) const
    {
        const double a = lo.load(), b = hi.load();
        if (! (b > a)) return 0.0f;
        return (float) jlimit(0.0, 1.0, (p - a) / (b - a));
    }

    float getValue() const override { return toNorm(plain.load()); }

    // Any thread. The plugin sees the change at the start of the next
    // process() or params.flush().
    void setValue(float v) override;

    float getDefaultValue() const override { return toNorm(def.load()); }
    String getName(int maxLen) const override { return maxLen > 0 ? name_.substring(0, maxLen) : name_; }
    String getLabel() const override { return {}; }
    int getNumSteps() const override
    {
        if (! stepped()) return AudioProcessor::getDefaultNumParameterSteps();
        return jmax(2, (int) std::lround(hi.load() - lo.load()) + 1);
    }
    bool isDiscrete() const override { return stepped(); }
    bool isBoolean() const override { return stepped() && std::lround(hi.load() - lo.load()) == 1; }
    bool isAutomatable() const override { return (flags.load() & CLAP_PARAM_IS_AUTOMATABLE) != 0; }
    String getText(float v, int maxLen) const override;
    float getValueForText(const String& text) const override;
    String getParameterID() const override { return String((uint32) id); }

    ClapInstance& owner_;
    const clap_id id;
    void* const cookie;
    std::atomic<uint32_t> flags{0};
    std::atomic<double> lo{0.0}, hi{1.0}, def{0.0};
    std::atomic<double> plain{0.0};
    std::atomic<bool> dirty{false};

private:
    String name_;
};

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

struct PortInfo
{
    String name;
    int channels = 0;
};

struct NotePortInfo
{
    uint32_t supported = 0;
    uint32_t preferred = 0;
};

struct Ports
{
    std::vector<PortInfo> audioIn, audioOut;   // CLAP order, main first
    std::vector<uint32_t> audioInIndex, audioOutIndex;  // CLAP index of each
    std::vector<NotePortInfo> noteIn, noteOut;
};

Ports readPorts(const clap_plugin* plugin)
{
    Ports p;
    if (auto* ap = static_cast<const clap_plugin_audio_ports*>(
            plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS)))
    {
        for (int dir = 0; dir < 2; ++dir)
        {
            const bool isIn = dir == 0;
            auto& list = isIn ? p.audioIn : p.audioOut;
            auto& index = isIn ? p.audioInIndex : p.audioOutIndex;
            const uint32_t n = ap->count(plugin, isIn);
            for (uint32_t i = 0; i < n; ++i)
            {
                clap_audio_port_info info{};
                if (! ap->get(plugin, i, isIn, &info)) continue;
                PortInfo pi{ String::fromUTF8(info.name), (int) info.channel_count };
                // JUCE's bus 0 is the main bus; CLAP may list it anywhere.
                if ((info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0)
                {
                    list.insert(list.begin(), pi);
                    index.insert(index.begin(), i);
                }
                else
                {
                    list.push_back(pi);
                    index.push_back(i);
                }
            }
        }
    }
    if (auto* np = static_cast<const clap_plugin_note_ports*>(
            plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS)))
    {
        for (int dir = 0; dir < 2; ++dir)
        {
            const bool isIn = dir == 0;
            const uint32_t n = np->count(plugin, isIn);
            for (uint32_t i = 0; i < n; ++i)
            {
                clap_note_port_info info{};
                if (! np->get(plugin, i, isIn, &info)) continue;
                (isIn ? p.noteIn : p.noteOut).push_back({ info.supported_dialects, info.preferred_dialect });
            }
        }
    }
    return p;
}

AudioChannelSet channelSetFor(int n)
{
    if (n == 1) return AudioChannelSet::mono();
    if (n == 2) return AudioChannelSet::stereo();
    return AudioChannelSet::discreteChannels(n);
}

void fillDescription(PluginDescription& d, const clap_plugin_descriptor& desc,
                     const String& path, int numIn, int numOut, bool shared)
{
    const File f(path);
    d.name = str(desc.name);
    d.descriptiveName = str(desc.description);
    d.pluginFormatName = kFormatName;
    d.manufacturerName = str(desc.vendor);
    d.version = str(desc.version);
    d.fileOrIdentifier = f.getFullPathName();
    d.lastFileModTime = f.getLastModificationTime();
    d.lastInfoUpdateTime = Time::getCurrentTime();
    // A .clap can hold several plugins; this is how one is told apart.
    d.uniqueId = d.deprecatedUid = str(desc.id).hashCode();
    d.hasSharedContainer = shared;
    d.numInputChannels = numIn;
    d.numOutputChannels = numOut;

    StringArray features;
    if (desc.features != nullptr)
        for (auto* const* ft = desc.features; *ft != nullptr; ++ft)
            features.add(str(*ft));
    d.isInstrument = features.contains(CLAP_PLUGIN_FEATURE_INSTRUMENT);
    d.category = features.joinIntoString("|");
}

int mainChannels(const std::vector<PortInfo>& ports)
{
    return ports.empty() ? 0 : jmax(0, ports.front().channels);
}

// Mutual exclusion for CLAP [audio-thread] calls. Spins: the holders are
// short, and the audio thread must not sleep on a mutex.
class AudioToken
{
public:
    void lock() noexcept
    {
        while (busy_.exchange(true, std::memory_order_acquire))
            std::this_thread::yield();
    }
    void unlock() noexcept { busy_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> busy_{false};
};

class ClapInstance final : public AudioPluginInstance
{
public:
    static std::unique_ptr<ClapInstance> create(ModuleRef mod,
                                                const clap_plugin_descriptor& desc,
                                                const String& path,
                                                bool sharedContainer,
                                                String& err)
    {
        auto* factory = mod.factory();
        auto host = std::make_unique<Host>();
        const clap_plugin* plugin = factory->create_plugin(factory, host.get(), desc.id);
        if (plugin == nullptr)
        {
            err = "create_plugin failed for " + str(desc.id);
            return nullptr;
        }
        host->plugin = plugin;
        if (! plugin->init(plugin))
        {
            host->unregister();
            plugin->destroy(plugin);
            err = "plugin init failed for " + str(desc.id);
            return nullptr;
        }

        auto ports = readPorts(plugin);
        std::unique_ptr<ClapInstance> inst(new ClapInstance(
            std::move(mod), std::move(host), plugin, std::move(ports), desc, path, sharedContainer));
        return inst;
    }

    ~ClapInstance() override
    {
        host_->unregister();
        host_->inst = nullptr;
        {
            std::lock_guard<AudioToken> lk(token_);
            deactivateLocked();
        }
        plugin_->destroy(plugin_);
        plugin_ = nullptr;
        // host_ then mod_ are destroyed after this body, in that order.
    }

    // ---- AudioProcessor -----------------------------------------------------

    const String getName() const override { return desc_.name; }

    void fillInPluginDescription(PluginDescription& d) const override { d = desc_; }

    void prepareToPlay(double sampleRate, int maxBlock) override
    {
        int latency = 0;
        {
            std::lock_guard<AudioToken> lk(token_);
            if (active_ && sampleRate == activeRate_ && maxBlock == activeMax_ && ! layoutChanged_.load())
                return;
            deactivateLocked();
            latency = activateLocked(sampleRate, maxBlock);
        }
        setLatencySamples(latency);
    }

    void releaseResources() override
    {
        std::lock_guard<AudioToken> lk(token_);
        deactivateLocked();
    }

    void reset() override
    {
        std::lock_guard<AudioToken> lk(token_);
        if (! active_) return;
        AudioScope as;
        plugin_->reset(plugin_);
    }

    bool isBusesLayoutSupported(const BusesLayout& layout) const override
    {
        if (layout.inputBuses.size() != (int) busInPorts_.size()
            || layout.outputBuses.size() != (int) busOutPorts_.size())
            return false;
        for (int i = 0; i < layout.inputBuses.size(); ++i)
        {
            const int n = layout.inputBuses.getReference(i).size();
            if (n != 0 && n != ports_.audioIn[(size_t) busInPorts_[(size_t) i]].channels) return false;
        }
        for (int i = 0; i < layout.outputBuses.size(); ++i)
        {
            const int n = layout.outputBuses.getReference(i).size();
            if (n != 0 && n != ports_.audioOut[(size_t) busOutPorts_[(size_t) i]].channels) return false;
        }
        return true;
    }

    void processBlock(AudioBuffer<float>& buffer, MidiBuffer& midi) override
    {
        std::lock_guard<AudioToken> lk(token_);
        AudioScope as;

        const int n = buffer.getNumSamples();
        if (! active_ || n <= 0 || n > activeMax_ || layoutChanged_.load(std::memory_order_relaxed))
        {
            silence(buffer);
            midi.clear();
            return;
        }
        if (! started_)
        {
            started_ = plugin_->start_processing(plugin_);
            if (! started_)
            {
                silence(buffer);
                midi.clear();
                return;
            }
        }

        // Inputs are copied out first: outputs overwrite the same channels.
        for (size_t p = 0; p < inBufs_.size(); ++p)
            for (uint32_t ch = 0; ch < inBufs_[p].channel_count; ++ch)
            {
                float* dst = inBufs_[p].data32[ch];
                const int src = inMap_[p][ch];
                if (src >= 0 && src < buffer.getNumChannels())
                    std::memcpy(dst, buffer.getReadPointer(src), sizeof(float) * (size_t) n);
                else
                    std::memset(dst, 0, sizeof(float) * (size_t) n);
            }
        for (size_t p = 0; p < outBufs_.size(); ++p)
            for (uint32_t ch = 0; ch < outBufs_[p].channel_count; ++ch)
            {
                const int dst = outMap_[p][ch];
                outBufs_[p].data32[ch] = (dst >= 0 && dst < buffer.getNumChannels())
                    ? buffer.getWritePointer(dst)
                    : outScratch_.getWritePointer(outScratchCh_[p] + (int) ch);
            }

        inEvents_.clear();
        outEvents_.clear();
        pushDirtyParams();
        pushMidi(midi, n);

        clap_event_transport transport{};
        const bool haveTransport = fillTransport(transport);

        clap_process proc{};
        proc.steady_time = steadyTime_;
        proc.frames_count = (uint32_t) n;
        proc.transport = haveTransport ? &transport : nullptr;
        proc.audio_inputs = inBufs_.data();
        proc.audio_outputs = outBufs_.data();
        proc.audio_inputs_count = (uint32_t) inBufs_.size();
        proc.audio_outputs_count = (uint32_t) outBufs_.size();
        proc.in_events = inEvents_.in();
        proc.out_events = outEvents_.out();

        const auto status = plugin_->process(plugin_, &proc);
        steadyTime_ += n;
        if (status == CLAP_PROCESS_ERROR)
            silence(buffer);

        midi.clear();
        handleOutputEvents(&midi);
    }

    bool acceptsMidi() const override  { return ! ports_.noteIn.empty(); }
    bool producesMidi() const override { return ! ports_.noteOut.empty(); }
    bool isMidiEffect() const override
    {
        return ports_.audioIn.empty() && ports_.audioOut.empty()
               && acceptsMidi() && producesMidi();
    }

    double getTailLengthSeconds() const override
    {
        if (tail_ == nullptr || ! active_ || activeRate_ <= 0.0) return 0.0;
        const uint32_t t = tail_->get(plugin_);
        if (t >= (uint32_t) std::numeric_limits<int32_t>::max())
            return std::numeric_limits<double>::infinity();
        return (double) t / activeRate_;
    }

    void setNonRealtime(bool nonRealtime) noexcept override
    {
        AudioPluginInstance::setNonRealtime(nonRealtime);
        if (render_ != nullptr)
            render_->set(plugin_, nonRealtime ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
    }

    AudioProcessorParameter* getBypassParameter() const override { return bypass_; }

    // CLAP has no program concept in its core; presets are an extension.
    int getNumPrograms() override { return 0; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const String getProgramName(int) override { return {}; }
    void changeProgramName(int, const String&) override {}

    // The raw clap.state stream, so state moves between CLAP hosts unchanged.
    void getStateInformation(MemoryBlock& dest) override
    {
        dest.reset();
        flushParams();
        if (state_ == nullptr) return;
        clap_ostream os{};
        os.ctx = &dest;
        os.write = [](const clap_ostream* s, const void* buf, uint64_t size) -> int64_t
        {
            static_cast<MemoryBlock*>(s->ctx)->append(buf, (size_t) size);
            return (int64_t) size;
        };
        if (! state_->save(plugin_, &os))
            dest.reset();
    }

    void setStateInformation(const void* data, int size) override
    {
        if (state_ == nullptr || data == nullptr || size <= 0) return;
        flushParams();   // pending changes first, so the state wins over them
        struct Reader { const uint8_t* p; uint64_t left; } r{ static_cast<const uint8_t*>(data), (uint64_t) size };
        clap_istream is{};
        is.ctx = &r;
        is.read = [](const clap_istream* s, void* buf, uint64_t want) -> int64_t
        {
            auto* rd = static_cast<Reader*>(s->ctx);
            const uint64_t n = std::min(want, rd->left);
            std::memcpy(buf, rd->p, (size_t) n);
            rd->p += n;
            rd->left -= n;
            return (int64_t) n;
        };
        if (state_->load(plugin_, &is))
            rescanParams(CLAP_PARAM_RESCAN_VALUES);
    }

   #if MINIHOST_CLAP_EDITOR
    bool hasEditor() const override
    {
        return gui_ != nullptr && gui_->is_api_supported(plugin_, CLAP_WINDOW_API_COCOA, false);
    }
    AudioProcessorEditor* createEditor() override;
    ClapEditor* editor = nullptr;   // main thread only
   #else
    bool hasEditor() const override { return false; }
    AudioProcessorEditor* createEditor() override { return nullptr; }
   #endif

    const clap_plugin* plugin() const { return plugin_; }
    const clap_plugin_gui* gui() const { return gui_; }

    // ---- Host-side services (plugin thread) ---------------------------------

    int serviceMain()
    {
        int n = 0;
        if (host_->wantRestart.exchange(false))
        {
            restart();
            ++n;
        }
        if (host_->wantLatency.exchange(false))
        {
            int latency = -1;
            {
                std::lock_guard<AudioToken> lk(token_);
                if (active_ && latency_ != nullptr)
                    latency = (int) latency_->get(plugin_);
            }
            if (latency >= 0 && latency != getLatencySamples())
                setLatencySamples(latency);
            ++n;
        }
        if (host_->wantFlush.exchange(false))
        {
            flushParams(/*force=*/true);
            ++n;
        }
        return n;
    }

    void rescanParams(uint32_t flags)
    {
        if (params_ == nullptr) return;
        const bool info = (flags & (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL)) != 0;
        if (info)
        {
            const uint32_t count = params_->count(plugin_);
            if (count != (uint32_t) paramList_.size() && ! warnedParamList_)
            {
                warnedParamList_ = true;
                std::fprintf(stderr, "minihost: CLAP plugin '%s' changed its parameter list; "
                                     "minihost keeps the list it opened with\n",
                             desc_.name.toRawUTF8());
            }
            for (uint32_t i = 0; i < count; ++i)
            {
                clap_param_info pi{};
                if (! params_->get_info(plugin_, i, &pi)) continue;
                auto it = byId_.find(pi.id);
                if (it != byId_.end())
                    it->second->setInfo(pi);
            }
        }
        if ((flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL)) != 0)
        {
            for (auto* p : paramList_)
            {
                double v = 0.0;
                if (! params_->get_value(plugin_, p->id, &v)) continue;
                p->dirty.store(false);
                if (p->plain.exchange(v) != v)
                    p->sendValueChangedMessageToListeners(p->toNorm(v));
            }
        }
        if (info)
            updateHostDisplay(ChangeDetails{}.withParameterInfoChanged(true));
    }

    String valueToText(const ClapParam& p, double v) const
    {
        char buf[256] = {};
        if (params_ != nullptr && params_->value_to_text(plugin_, p.id, v, buf, sizeof(buf)))
            return String::fromUTF8(buf);
        return p.stepped() ? String((int64) std::llround(v)) : String(v, 2);
    }

    bool textToValue(const ClapParam& p, const String& text, double& out) const
    {
        return params_ != nullptr
               && params_->text_to_value(plugin_, p.id, text.toRawUTF8(), &out);
    }

    void markParamsDirty() { paramsDirty_.store(true, std::memory_order_release); }

    void markStateDirty() { updateHostDisplay(ChangeDetails{}.withNonParameterStateChanged(true)); }

private:
    ClapInstance(ModuleRef mod, std::unique_ptr<Host> host, const clap_plugin* plugin,
                 Ports ports, const clap_plugin_descriptor& desc, const String& path,
                 bool sharedContainer)
        : AudioPluginInstance(busesFor(ports)),
          mod_(std::move(mod)), host_(std::move(host)), plugin_(plugin), ports_(std::move(ports))
    {
        for (size_t i = 0; i < ports_.audioIn.size(); ++i)
            if (ports_.audioIn[i].channels > 0) busInPorts_.push_back((int) i);
        for (size_t i = 0; i < ports_.audioOut.size(); ++i)
            if (ports_.audioOut[i].channels > 0) busOutPorts_.push_back((int) i);

        fillDescription(desc_, desc, path, mainChannels(ports_.audioIn),
                        mainChannels(ports_.audioOut), sharedContainer);

        params_  = ext<clap_plugin_params>(CLAP_EXT_PARAMS);
        state_   = ext<clap_plugin_state>(CLAP_EXT_STATE);
        latency_ = ext<clap_plugin_latency>(CLAP_EXT_LATENCY);
        tail_    = ext<clap_plugin_tail>(CLAP_EXT_TAIL);
        render_  = ext<clap_plugin_render>(CLAP_EXT_RENDER);
        gui_     = ext<clap_plugin_gui>(CLAP_EXT_GUI);

        if (params_ != nullptr)
        {
            const uint32_t count = params_->count(plugin_);
            for (uint32_t i = 0; i < count; ++i)
            {
                clap_param_info pi{};
                if (! params_->get_info(plugin_, i, &pi)) continue;
                double v = pi.default_value;
                params_->get_value(plugin_, pi.id, &v);
                auto param = std::make_unique<ClapParam>(*this, pi, v);
                auto* raw = param.get();
                if ((pi.flags & CLAP_PARAM_IS_BYPASS) != 0 && bypass_ == nullptr)
                    bypass_ = raw;
                byId_[pi.id] = raw;
                paramList_.push_back(raw);
                addHostedParameter(std::move(param));
            }
        }

        // Room for every parameter changing, plus generous MIDI. Events past
        // capacity are dropped rather than allocated for on the audio thread.
        const size_t maxEvents = paramList_.size() * 2 + 4096;
        inEvents_.reserve(maxEvents, maxEvents * 64);
        outEvents_.reserve(maxEvents, maxEvents * 64);

        host_->inst = this;
    }

    // Zero-channel ports get no JUCE bus; the plugin still sees them, empty.
    static BusesProperties busesFor(const Ports& p)
    {
        BusesProperties props;
        bool first = true;
        for (const auto& pi : p.audioIn)
        {
            if (pi.channels <= 0) continue;
            props = props.withInput(pi.name, channelSetFor(pi.channels), first);
            first = false;
        }
        first = true;
        for (const auto& pi : p.audioOut)
        {
            if (pi.channels <= 0) continue;
            props = props.withOutput(pi.name, channelSetFor(pi.channels), first);
            first = false;
        }
        return props;
    }

    template <typename T>
    const T* ext(const char* id) const
    {
        return static_cast<const T*>(plugin_->get_extension(plugin_, id));
    }

    // The buffers map JUCE channels as of the last activate; a new layout
    // needs a prepareToPlay before processing resumes.
    void processorLayoutsChanged() override { layoutChanged_.store(true); }

    // Token held. Returns the latency the plugin reports once active.
    int activateLocked(double sampleRate, int maxBlock)
    {
        layoutChanged_.store(false);

        // Every CLAP port gets a buffer; JUCE channels map onto them where
        // the corresponding bus is enabled, scratch memory otherwise.
        auto build = [&](bool isIn, const std::vector<PortInfo>& list,
                         const std::vector<int>& busPorts,
                         std::vector<std::vector<int>>& map,
                         std::vector<clap_audio_buffer>& bufs,
                         std::vector<std::vector<float*>>& ptrs,
                         std::vector<int>& scratchBase, int& total)
        {
            map.assign(list.size(), {});
            ptrs.assign(list.size(), {});
            bufs.assign(list.size(), clap_audio_buffer{});
            scratchBase.assign(list.size(), 0);
            total = 0;
            for (size_t p = 0; p < list.size(); ++p)
            {
                const int nch = jmax(0, list[p].channels);
                map[p].assign((size_t) nch, -1);
                scratchBase[p] = total;
                total += nch;
            }
            for (size_t b = 0; b < busPorts.size(); ++b)
            {
                auto* bus = getBus(isIn, (int) b);
                if (bus == nullptr || ! bus->isEnabled()) continue;
                const size_t p = (size_t) busPorts[b];
                for (int ch = 0; ch < (int) map[p].size(); ++ch)
                    map[p][(size_t) ch] = getChannelIndexInProcessBlockBuffer(isIn, (int) b, ch);
            }
        };

        int totalIn = 0, totalOut = 0;
        build(true, ports_.audioIn, busInPorts_, inMap_, inBufs_, inPtrs_, inScratchCh_, totalIn);
        build(false, ports_.audioOut, busOutPorts_, outMap_, outBufs_, outPtrs_, outScratchCh_, totalOut);
        inScratch_.setSize(jmax(1, totalIn), maxBlock);
        inScratch_.clear();
        outScratch_.setSize(jmax(1, totalOut), maxBlock);
        outScratch_.clear();

        for (size_t p = 0; p < inBufs_.size(); ++p)
        {
            inPtrs_[p].resize(inMap_[p].size());
            for (size_t ch = 0; ch < inPtrs_[p].size(); ++ch)
                inPtrs_[p][ch] = inScratch_.getWritePointer(inScratchCh_[p] + (int) ch);
            inBufs_[p].data32 = inPtrs_[p].empty() ? nullptr : inPtrs_[p].data();
            inBufs_[p].channel_count = (uint32_t) inPtrs_[p].size();
        }
        for (size_t p = 0; p < outBufs_.size(); ++p)
        {
            outPtrs_[p].assign(outMap_[p].size(), nullptr);
            outBufs_[p].data32 = outPtrs_[p].empty() ? nullptr : outPtrs_[p].data();
            outBufs_[p].channel_count = (uint32_t) outPtrs_[p].size();
        }

        if (! plugin_->activate(plugin_, sampleRate, 1, (uint32_t) maxBlock))
        {
            std::fprintf(stderr, "minihost: CLAP plugin '%s' refused to activate at %g Hz, %d frames\n",
                         desc_.name.toRawUTF8(), sampleRate, maxBlock);
            return 0;
        }
        active_ = true;
        started_ = false;
        activeRate_ = sampleRate;
        activeMax_ = maxBlock;
        host_->wantLatency = false;   // read below; changed() is only legal during activate
        return latency_ != nullptr ? (int) latency_->get(plugin_) : 0;
    }

    // Token held.
    void deactivateLocked()
    {
        if (started_)
        {
            AudioScope as;
            plugin_->stop_processing(plugin_);
            started_ = false;
        }
        if (active_)
        {
            plugin_->deactivate(plugin_);
            active_ = false;
        }
    }

    void restart()
    {
        int latency = -1;
        {
            std::lock_guard<AudioToken> lk(token_);
            if (! active_) return;
            const double rate = activeRate_;
            const int max = activeMax_;
            deactivateLocked();
            latency = activateLocked(rate, max);
        }
        if (latency != getLatencySamples())
            setLatencySamples(latency);
    }

    // Deliver pending parameter changes outside process(). `force` also
    // flushes with nothing pending, for a plugin's request_flush.
    void flushParams(bool force = false)
    {
        if (params_ == nullptr) return;
        if (! force && ! paramsDirty_.load(std::memory_order_acquire)) return;
        std::lock_guard<AudioToken> lk(token_);
        inEvents_.clear();
        outEvents_.clear();
        pushDirtyParams();
        if (active_)
        {
            AudioScope as;
            params_->flush(plugin_, inEvents_.in(), outEvents_.out());
        }
        else
        {
            params_->flush(plugin_, inEvents_.in(), outEvents_.out());
        }
        handleOutputEvents(nullptr);
    }

    void pushDirtyParams()
    {
        if (! paramsDirty_.exchange(false, std::memory_order_acq_rel)) return;
        for (auto* p : paramList_)
        {
            if (! p->dirty.exchange(false, std::memory_order_acq_rel)) continue;
            clap_event_param_value ev{};
            ev.header.size = sizeof(ev);
            ev.header.time = 0;
            ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            ev.header.type = CLAP_EVENT_PARAM_VALUE;
            ev.param_id = p->id;
            ev.cookie = p->cookie;
            ev.note_id = -1;
            ev.port_index = -1;
            ev.channel = -1;
            ev.key = -1;
            ev.value = p->plain.load();
            if (! inEvents_.push(&ev.header))
            {
                p->dirty.store(true);   // retry next block
                paramsDirty_.store(true);
            }
        }
    }

    void pushMidi(const MidiBuffer& midi, int n)
    {
        if (ports_.noteIn.empty()) return;
        const auto& port = ports_.noteIn.front();
        const bool hasMidi = (port.supported & CLAP_NOTE_DIALECT_MIDI) != 0;
        const bool hasClap = (port.supported & CLAP_NOTE_DIALECT_CLAP) != 0;
        const bool notesAsClap = hasClap && (port.preferred == CLAP_NOTE_DIALECT_CLAP || ! hasMidi);

        for (const auto meta : midi)
        {
            const uint8_t* data = meta.data;
            const int size = meta.numBytes;
            if (size <= 0) continue;
            const uint32_t time = (uint32_t) jlimit(0, n - 1, meta.samplePosition);
            const uint8_t status = data[0];

            if (status == 0xF0)
            {
                if (! hasMidi) continue;
                clap_event_midi_sysex ev{};
                ev.header = header(sizeof(ev), time, CLAP_EVENT_MIDI_SYSEX);
                ev.port_index = 0;
                ev.buffer = data;   // valid until midi is cleared after process()
                ev.size = (uint32_t) size;
                inEvents_.push(&ev.header);
                continue;
            }

            const uint8_t kind = status & 0xF0;
            if (notesAsClap && (kind == 0x90 || kind == 0x80) && size >= 3)
            {
                clap_event_note ev{};
                const bool on = kind == 0x90 && data[2] > 0;
                ev.header = header(sizeof(ev), time, on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF);
                ev.note_id = -1;
                ev.port_index = 0;
                ev.channel = (int16_t) (status & 0x0F);
                ev.key = (int16_t) data[1];
                ev.velocity = data[2] / 127.0;
                inEvents_.push(&ev.header);
                continue;
            }
            if (! hasMidi) continue;
            clap_event_midi ev{};
            ev.header = header(sizeof(ev), time, CLAP_EVENT_MIDI);
            ev.port_index = 0;
            for (int i = 0; i < 3 && i < size; ++i)
                ev.data[i] = data[i];
            inEvents_.push(&ev.header);
        }
    }

    static clap_event_header header(uint32_t size, uint32_t time, uint16_t type)
    {
        clap_event_header h{};
        h.size = size;
        h.time = time;
        h.space_id = CLAP_CORE_EVENT_SPACE_ID;
        h.type = type;
        h.flags = 0;
        return h;
    }

    // midi == nullptr: called from flush, where only parameter events matter.
    void handleOutputEvents(MidiBuffer* midi)
    {
        for (uint32_t i = 0; i < outEvents_.size(); ++i)
        {
            const auto* h = outEvents_.get(i);
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
            switch (h->type)
            {
                case CLAP_EVENT_PARAM_VALUE:
                {
                    auto* ev = reinterpret_cast<const clap_event_param_value*>(h);
                    auto it = byId_.find(ev->param_id);
                    if (it == byId_.end()) break;
                    it->second->plain.store(ev->value);
                    it->second->sendValueChangedMessageToListeners(it->second->toNorm(ev->value));
                    break;
                }
                case CLAP_EVENT_PARAM_GESTURE_BEGIN:
                case CLAP_EVENT_PARAM_GESTURE_END:
                {
                    auto* ev = reinterpret_cast<const clap_event_param_gesture*>(h);
                    auto it = byId_.find(ev->param_id);
                    if (it == byId_.end()) break;
                    if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) it->second->beginChangeGesture();
                    else it->second->endChangeGesture();
                    break;
                }
                case CLAP_EVENT_MIDI:
                {
                    if (midi == nullptr) break;
                    auto* ev = reinterpret_cast<const clap_event_midi*>(h);
                    const uint8_t s = ev->data[0];
                    if (s < 0x80 || s == 0xF0 || s == 0xF7) break;
                    const int len = MidiMessage::getMessageLengthFromFirstByte(s);
                    midi->addEvent(ev->data, len, (int) h->time);
                    break;
                }
                case CLAP_EVENT_MIDI_SYSEX:
                {
                    if (midi == nullptr) break;
                    auto* ev = reinterpret_cast<const clap_event_midi_sysex*>(h);
                    if (ev->buffer != nullptr && ev->size > 0)
                        midi->addEvent(ev->buffer, (int) ev->size, (int) h->time);
                    break;
                }
                case CLAP_EVENT_NOTE_ON:
                case CLAP_EVENT_NOTE_OFF:
                {
                    if (midi == nullptr) break;
                    auto* ev = reinterpret_cast<const clap_event_note*>(h);
                    if (ev->channel < 0 || ev->channel > 15 || ev->key < 0 || ev->key > 127) break;
                    const auto vel = (uint8) jlimit(0, 127, (int) std::lround(ev->velocity * 127.0));
                    const auto msg = h->type == CLAP_EVENT_NOTE_ON
                        ? MidiMessage::noteOn(ev->channel + 1, ev->key, vel)
                        : MidiMessage::noteOff(ev->channel + 1, ev->key, vel);
                    midi->addEvent(msg, (int) h->time);
                    break;
                }
                default:
                    break;
            }
        }
    }

    bool fillTransport(clap_event_transport& t) const
    {
        auto* ph = getPlayHead();
        if (ph == nullptr) return false;
        const auto pos = ph->getPosition();
        if (! pos.hasValue()) return false;

        t.header = header(sizeof(t), 0, CLAP_EVENT_TRANSPORT);
        if (auto bpm = pos->getBpm())
        {
            t.flags |= CLAP_TRANSPORT_HAS_TEMPO;
            t.tempo = *bpm;
        }
        if (auto sec = pos->getTimeInSeconds())
        {
            t.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
            t.song_pos_seconds = (clap_sectime) std::llround(*sec * (double) CLAP_SECTIME_FACTOR);
        }
        const auto sig = pos->getTimeSignature();
        if (sig)
        {
            t.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
            t.tsig_num = (uint16_t) sig->numerator;
            t.tsig_denom = (uint16_t) sig->denominator;
        }
        if (auto ppq = pos->getPpqPosition())
        {
            t.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
            t.song_pos_beats = (clap_beattime) std::llround(*ppq * (double) CLAP_BEATTIME_FACTOR);
            double barStart = 0.0;
            if (auto bs = pos->getPpqPositionOfLastBarStart())
                barStart = *bs;
            else if (sig && sig->denominator > 0)
            {
                const double beatsPerBar = sig->numerator * 4.0 / sig->denominator;
                barStart = beatsPerBar > 0 ? std::floor(*ppq / beatsPerBar) * beatsPerBar : 0.0;
            }
            t.bar_start = (clap_beattime) std::llround(barStart * (double) CLAP_BEATTIME_FACTOR);
            if (auto bars = pos->getBarCount())
                t.bar_number = (int32_t) *bars;
            else if (sig && sig->denominator > 0 && sig->numerator > 0)
                t.bar_number = (int32_t) std::floor(*ppq / (sig->numerator * 4.0 / sig->denominator));
        }
        if (pos->getIsPlaying())   t.flags |= CLAP_TRANSPORT_IS_PLAYING;
        if (pos->getIsRecording()) t.flags |= CLAP_TRANSPORT_IS_RECORDING;
        if (pos->getIsLooping())
        {
            t.flags |= CLAP_TRANSPORT_IS_LOOP_ACTIVE;
            if (auto loop = pos->getLoopPoints())
            {
                t.loop_start_beats = (clap_beattime) std::llround(loop->ppqStart * (double) CLAP_BEATTIME_FACTOR);
                t.loop_end_beats = (clap_beattime) std::llround(loop->ppqEnd * (double) CLAP_BEATTIME_FACTOR);
                if (t.tempo > 0.0)
                {
                    t.loop_start_seconds = (clap_sectime) std::llround(loop->ppqStart * 60.0 / t.tempo * (double) CLAP_SECTIME_FACTOR);
                    t.loop_end_seconds = (clap_sectime) std::llround(loop->ppqEnd * 60.0 / t.tempo * (double) CLAP_SECTIME_FACTOR);
                }
            }
        }
        return true;
    }

    void silence(AudioBuffer<float>& buffer) const
    {
        for (int ch = 0; ch < getTotalNumOutputChannels() && ch < buffer.getNumChannels(); ++ch)
            buffer.clear(ch, 0, buffer.getNumSamples());
    }

    // Declared first: destroyed last, after the plugin and host are gone.
    ModuleRef mod_;
    std::unique_ptr<Host> host_;
    const clap_plugin* plugin_ = nullptr;
    Ports ports_;
    std::vector<int> busInPorts_, busOutPorts_;   // JUCE bus -> index into ports_
    PluginDescription desc_;

    const clap_plugin_params*  params_  = nullptr;
    const clap_plugin_state*   state_   = nullptr;
    const clap_plugin_latency* latency_ = nullptr;
    const clap_plugin_tail*    tail_    = nullptr;
    const clap_plugin_render*  render_  = nullptr;
    const clap_plugin_gui*     gui_     = nullptr;

    std::vector<ClapParam*> paramList_;
    std::unordered_map<clap_id, ClapParam*> byId_;
    ClapParam* bypass_ = nullptr;
    std::atomic<bool> paramsDirty_{false};
    bool warnedParamList_ = false;

    AudioToken token_;
    bool active_ = false;
    bool started_ = false;
    double activeRate_ = 0.0;
    int activeMax_ = 0;
    int64_t steadyTime_ = 0;
    std::atomic<bool> layoutChanged_{false};

    std::vector<std::vector<int>> inMap_, outMap_;
    std::vector<clap_audio_buffer> inBufs_, outBufs_;
    std::vector<std::vector<float*>> inPtrs_, outPtrs_;
    std::vector<int> inScratchCh_, outScratchCh_;
    AudioBuffer<float> inScratch_, outScratch_;
    EventList inEvents_, outEvents_;
};

#if MINIHOST_CLAP_EDITOR

// The plugin's own view, parented to an NSView this component owns.
class ClapEditor final : public AudioProcessorEditor
{
public:
    explicit ClapEditor(ClapInstance& owner)
        : AudioProcessorEditor(owner), owner_(owner), gui_(owner.gui()), plugin_(owner.plugin())
    {
        created_ = gui_->create(plugin_, CLAP_WINDOW_API_COCOA, false);
        uint32_t w = 400, h = 300;
        if (created_)
            gui_->get_size(plugin_, &w, &h);
        w = jmax(1u, w);
        h = jmax(1u, h);

        view_ = mh_clap_nsview_create((int) w, (int) h);
        nsview_.setView(view_);
        addAndMakeVisible(nsview_);

        if (created_)
        {
            clap_window win{};
            win.api = CLAP_WINDOW_API_COCOA;
            win.cocoa = view_;
            parented_ = gui_->set_parent(plugin_, &win);
        }
        resizable_ = parented_ && gui_->can_resize(plugin_);
        setResizable(resizable_, false);
        setSizeFromPlugin(w, h);
        if (parented_)
            gui_->show(plugin_);
        owner_.editor = this;
    }

    ~ClapEditor() override
    {
        if (owner_.editor == this)
            owner_.editor = nullptr;
        if (parented_)
            gui_->hide(plugin_);
        if (created_)
            gui_->destroy(plugin_);
        nsview_.setView(nullptr);
        mh_clap_nsview_release(view_);
    }

    void resized() override
    {
        nsview_.setBounds(getLocalBounds());
        if (! resizable_ || fromPlugin_) return;
        uint32_t w = (uint32_t) jmax(1, getWidth()), h = (uint32_t) jmax(1, getHeight());
        if (gui_->adjust_size(plugin_, &w, &h)
            && ((int) w != getWidth() || (int) h != getHeight()))
        {
            setSizeFromPlugin(w, h);
        }
        gui_->set_size(plugin_, w, h);
    }

    // The plugin asked for this size: apply it without echoing set_size.
    void setSizeFromPlugin(uint32_t w, uint32_t h)
    {
        const ScopedValueSetter<bool> svs(fromPlugin_, true);
        setSize((int) w, (int) h);
    }

private:
    ClapInstance& owner_;
    const clap_plugin_gui* gui_;
    const clap_plugin* plugin_;
    NSViewComponent nsview_;
    void* view_ = nullptr;
    bool created_ = false, parented_ = false, resizable_ = false, fromPlugin_ = false;
};

AudioProcessorEditor* ClapInstance::createEditor()
{
    return hasEditor() ? new ClapEditor(*this) : nullptr;
}

// [thread-safe] in CLAP, but the editor exists only on the main thread, so
// a resize asked for from elsewhere is declined.
bool requestResize(const clap_host* h, uint32_t w, uint32_t height)
{
    auto* host = Host::from(h);
    if (host->inst == nullptr || std::this_thread::get_id() != host->mainThread)
        return false;
    auto* ed = host->inst->editor;
    if (ed == nullptr) return false;
    ed->setSizeFromPlugin(w, height);
    return true;
}

const clap_host_gui kGui{
    [](const clap_host*) {},
    requestResize,
    [](const clap_host*) { return false; },   // request_show: embedded only
    [](const clap_host*) { return false; },   // request_hide
    [](const clap_host*, bool) {},
};

#endif  // MINIHOST_CLAP_EDITOR

void ClapParam::setValue(float v)
{
    if ((flags.load() & CLAP_PARAM_IS_READONLY) != 0) return;
    plain.store(fromNorm(v));
    dirty.store(true, std::memory_order_release);
    owner_.markParamsDirty();
}

String ClapParam::getText(float v, int maxLen) const
{
    auto s = owner_.valueToText(*this, fromNorm(v));
    return maxLen > 0 ? s.substring(0, maxLen) : s;
}

float ClapParam::getValueForText(const String& text) const
{
    double v = 0.0;
    if (owner_.textToValue(*this, text, v))
        return toNorm(v);
    return toNorm(text.getDoubleValue());
}

// ---------------------------------------------------------------------------
// Host extensions
// ---------------------------------------------------------------------------

int Host::serviceLocked()
{
    if (plugin == nullptr) return 0;
    int n = 0;
    if (wantCallback.exchange(false))
    {
        plugin->on_main_thread(plugin);
        ++n;
    }
    if (inst != nullptr)
        n += inst->serviceMain();
    return n;
}

const clap_host_log kLog{
    [](const clap_host*, clap_log_severity sev, const char* msg)
    {
        if (sev < CLAP_LOG_ERROR || msg == nullptr) return;
        static const char* names[] = { "debug", "info", "warning", "error", "fatal",
                                       "host misbehaving", "plugin misbehaving" };
        const char* what = (sev >= 0 && sev <= CLAP_LOG_PLUGIN_MISBEHAVING) ? names[sev] : "log";
        std::fprintf(stderr, "minihost: CLAP %s: %s\n", what, msg);
    }
};

const clap_host_thread_check kThreadCheck{
    [](const clap_host* h) { return ! tlAudio && std::this_thread::get_id() == Host::from(h)->mainThread; },
    [](const clap_host*) { return tlAudio; },
};

const clap_host_params kParams{
    [](const clap_host* h, clap_param_rescan_flags flags)
    {
        if (auto* inst = Host::from(h)->inst) inst->rescanParams(flags);
    },
    [](const clap_host*, clap_id, clap_param_clear_flags) {},
    [](const clap_host* h) { Host::from(h)->request(Host::from(h)->wantFlush); },
};

const clap_host_latency kLatency{
    [](const clap_host* h) { Host::from(h)->request(Host::from(h)->wantLatency); },
};

const clap_host_tail kTail{
    [](const clap_host*) {},
};

const clap_host_state kState{
    [](const clap_host* h)
    {
        if (auto* inst = Host::from(h)->inst) inst->markStateDirty();
    },
};

const clap_host_audio_ports kAudioPorts{
    [](const clap_host*, uint32_t) { return false; },
    [](const clap_host*, uint32_t) {},
};

const clap_host_note_ports kNotePorts{
    [](const clap_host*) -> uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI; },
    [](const clap_host*, uint32_t) {},
};

const void* Host::getExtension(const clap_host*, const char* id)
{
    if (id == nullptr) return nullptr;
    const auto is = [id](const char* x) { return std::strcmp(id, x) == 0; };
    if (is(CLAP_EXT_LOG))          return &kLog;
    if (is(CLAP_EXT_THREAD_CHECK)) return &kThreadCheck;
    if (is(CLAP_EXT_PARAMS))       return &kParams;
    if (is(CLAP_EXT_LATENCY))      return &kLatency;
    if (is(CLAP_EXT_TAIL))         return &kTail;
    if (is(CLAP_EXT_STATE))        return &kState;
    if (is(CLAP_EXT_AUDIO_PORTS))  return &kAudioPorts;
    if (is(CLAP_EXT_NOTE_PORTS))   return &kNotePorts;
   #if MINIHOST_CLAP_EDITOR
    if (is(CLAP_EXT_GUI))          return &kGui;
   #endif
    return nullptr;
}

// ---------------------------------------------------------------------------
// Format
// ---------------------------------------------------------------------------

bool looksLikeClap(const String& path)
{
    const File f(path);
    return f.getFileExtension().equalsIgnoreCase(".clap") && f.exists();
}

// Main channel counts, read from a throwaway instance. Zero if it will not
// instantiate: the description is still listed so the open reports why.
std::pair<int, int> probeChannels(const clap_plugin_factory& factory, const char* id)
{
    Host host;
    const clap_plugin* plugin = factory.create_plugin(&factory, &host, id);
    if (plugin == nullptr) return { 0, 0 };
    host.plugin = plugin;
    std::pair<int, int> result{ 0, 0 };
    if (plugin->init(plugin))
    {
        const auto ports = readPorts(plugin);
        result = { mainChannels(ports.audioIn), mainChannels(ports.audioOut) };
    }
    host.unregister();
    plugin->destroy(plugin);
    return result;
}

class ClapFormat final : public AudioPluginFormat
{
public:
    String getName() const override { return kFormatName; }

    void findAllTypesForFile(OwnedArray<PluginDescription>& results,
                             const String& fileOrIdentifier) override
    {
        if (! looksLikeClap(fileOrIdentifier)) return;
        const auto path = File(fileOrIdentifier).getFullPathName();
        String err;
        auto mod = ModuleRef::acquire(path, err);
        if (! mod) return;
        auto* factory = mod.factory();
        if (factory == nullptr) return;

        const uint32_t count = factory->get_plugin_count(factory);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto* d = factory->get_plugin_descriptor(factory, i);
            if (d == nullptr || d->id == nullptr || ! clap_version_is_compatible(d->clap_version))
                continue;
            const auto ch = probeChannels(*factory, d->id);
            auto desc = std::make_unique<PluginDescription>();
            fillDescription(*desc, *d, path, ch.first, ch.second, count > 1);
            results.add(desc.release());
        }
    }

    bool fileMightContainThisPluginType(const String& f) override { return looksLikeClap(f); }

    String getNameOfPluginFromIdentifier(const String& f) override
    {
        return File(f).getFileNameWithoutExtension();
    }

    bool pluginNeedsRescanning(const PluginDescription& d) override
    {
        return File(d.fileOrIdentifier).getLastModificationTime() != d.lastFileModTime;
    }

    bool doesPluginStillExist(const PluginDescription& d) override
    {
        return File(d.fileOrIdentifier).exists();
    }

    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }

    StringArray searchPathsForPlugins(const FileSearchPath& dirs, bool recursive, bool) override
    {
        StringArray out;
        for (int i = 0; i < dirs.getNumPaths(); ++i)
            search(dirs[i], recursive, out);
        return out;
    }

    // Per the CLAP spec (entry.h), plus CLAP_PATH.
    FileSearchPath getDefaultLocationsToSearch() override
    {
        FileSearchPath paths;
        const auto home = File::getSpecialLocation(File::userHomeDirectory);
       #if JUCE_MAC
        paths.add(File("/Library/Audio/Plug-Ins/CLAP"));
        paths.add(home.getChildFile("Library/Audio/Plug-Ins/CLAP"));
       #elif JUCE_WINDOWS
        paths.add(File::getSpecialLocation(File::globalApplicationsDirectory)
                      .getChildFile("Common Files/CLAP"));
        if (auto* local = std::getenv("LOCALAPPDATA"))
            paths.add(File(String::fromUTF8(local)).getChildFile("Programs/Common/CLAP"));
       #else
        paths.add(home.getChildFile(".clap"));
        paths.add(File("/usr/lib/clap"));
       #endif
        if (auto* env = std::getenv("CLAP_PATH"))
        {
           #if JUCE_WINDOWS
            const char* sep = ";";
           #else
            const char* sep = ":";
           #endif
            for (const auto& p : StringArray::fromTokens(String::fromUTF8(env), sep, ""))
                if (p.isNotEmpty())
                    paths.add(File(p));
        }
        return paths;
    }

private:
    static void search(const File& dir, bool recursive, StringArray& out)
    {
        for (const auto& e : RangedDirectoryIterator(dir, false, "*", File::findFilesAndDirectories))
        {
            const auto f = e.getFile();
            if (f.getFileExtension().equalsIgnoreCase(".clap"))
                out.add(f.getFullPathName());
            else if (recursive && f.isDirectory())
                search(f, recursive, out);
        }
    }

    bool requiresUnblockedMessageThreadDuringCreation(const PluginDescription&) const override
    {
        return false;
    }

    void createPluginInstance(const PluginDescription& desc, double, int,
                              PluginCreationCallback callback) override
    {
        String err;
        auto inst = create(desc, err);
        callback(std::move(inst), err);
    }

    static std::unique_ptr<AudioPluginInstance> create(const PluginDescription& desc, String& err)
    {
        auto mod = ModuleRef::acquire(File(desc.fileOrIdentifier).getFullPathName(), err);
        if (! mod) return nullptr;
        auto* factory = mod.factory();
        if (factory == nullptr)
        {
            err = "CLAP module has no plugin factory: " + desc.fileOrIdentifier;
            return nullptr;
        }

        const uint32_t count = factory->get_plugin_count(factory);
        const clap_plugin_descriptor* match = nullptr;
        for (uint32_t i = 0; i < count && match == nullptr; ++i)
        {
            const auto* d = factory->get_plugin_descriptor(factory, i);
            if (d != nullptr && d->id != nullptr && str(d->id).hashCode() == desc.uniqueId)
                match = d;
        }
        if (match == nullptr)
        {
            err = "no plugin in " + desc.fileOrIdentifier + " matches the description";
            return nullptr;
        }
        auto inst = ClapInstance::create(std::move(mod), *match, desc.fileOrIdentifier, count > 1, err);
        return std::unique_ptr<AudioPluginInstance>(inst.release());
    }
};

}  // namespace

std::unique_ptr<juce::AudioPluginFormat> createFormat()
{
    return std::make_unique<ClapFormat>();
}

int service()
{
    std::lock_guard<std::mutex> lk(gHostMtx);
    int n = 0;
    // Only plugins whose main thread this is: CLAP's main-thread calls
    // must stay on the thread that created the plugin.
    const auto self = std::this_thread::get_id();
    for (size_t i = 0; i < gHosts.size(); ++i)
        if (gHosts[i]->mainThread == self)
            n += gHosts[i]->serviceLocked();
    return n;
}

}  // namespace minihost::clapfmt
