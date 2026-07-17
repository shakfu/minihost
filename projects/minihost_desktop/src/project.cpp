// project.cpp -- see project.h for the design.

#include "project.h"

#include "minihost_audiofile.h"
#include "node_registry.h"

#include "MidiFile.h"   // smf::MidiFile (projects/midifile)

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace minihost_desktop::project {

bool readMidiFileEvents(const juce::File& midi_file, double sample_rate,
                        std::vector<MH_MidiEvent>& out)
{
    out.clear();
    smf::MidiFile mf;
    if (! mf.read(midi_file.getFullPathName().toStdString()))
        return false;
    mf.doTimeAnalysis();   // populate MidiEvent::seconds from the tempo map

    for (int t = 0; t < mf.getTrackCount(); ++t)
    {
        auto& track = mf[t];
        for (int i = 0; i < track.getEventCount(); ++i)
        {
            auto& ev = track[i];
            if (ev.size() < 1) continue;
            const int status = ev[0];
            // Keep channel-voice messages (0x80..0xEF); drop meta / sysex /
            // system (0xF0..0xFF) and running-status stragglers.
            if ((status & 0x80) == 0 || (status & 0xF0) == 0xF0) continue;
            MH_MidiEvent e{};
            // Truncate to match Python's int(seconds * sample_rate).
            e.sample_offset = (int) (ev.seconds * sample_rate);
            e.status = (unsigned char) status;
            e.data1  = ev.size() > 1 ? (unsigned char) ev[1] : 0;
            e.data2  = ev.size() > 2 ? (unsigned char) ev[2] : 0;
            out.push_back(e);
        }
    }
    std::sort(out.begin(), out.end(),
              [](const MH_MidiEvent& a, const MH_MidiEvent& b) {
                  return a.sample_offset < b.sample_offset;
              });
    return true;
}


namespace {

constexpr int kSchemaVersion = 1;

[[noreturn]] void throwErr(const juce::String& msg)
{
    throw ProjectError(msg.toStdString());
}

juce::var requireField(const juce::var& obj, const juce::String& key)
{
    if (!obj.isObject() || !obj.getDynamicObject()->hasProperty(key))
        throwErr("missing required field: " + key);
    return obj.getDynamicObject()->getProperty(key);
}

int requireInt(const juce::var& obj, const juce::String& key)
{
    auto v = requireField(obj, key);
    if (!v.isInt() && !v.isInt64() && !v.isDouble())
        throwErr("field " + key + " must be a number");
    return (int) v;
}

double requireDouble(const juce::var& obj, const juce::String& key)
{
    auto v = requireField(obj, key);
    if (!v.isInt() && !v.isInt64() && !v.isDouble())
        throwErr("field " + key + " must be a number");
    return (double) v;
}

juce::String requireString(const juce::var& obj, const juce::String& key)
{
    auto v = requireField(obj, key);
    if (!v.isString()) throwErr("field " + key + " must be a string");
    return v.toString();
}

juce::Array<juce::var> requireArray(const juce::var& obj,
                                    const juce::String& key)
{
    auto v = requireField(obj, key);
    if (!v.isArray()) throwErr("field " + key + " must be an array");
    return *v.getArray();
}

bool decodeBase64(const juce::String& b64, juce::MemoryBlock& out)
{
    juce::MemoryOutputStream stream(out, /*appendToExistingBlockContents=*/false);
    return juce::Base64::convertFromBase64(stream, b64);
}

// De-interleave [frames*channels] into planar contiguous storage:
// channel 0's `frames` samples first, then channel 1's, ...
void deinterleave(const float* src, int channels, int frames,
                  std::vector<float>& dst)
{
    dst.assign((size_t) channels * (size_t) frames, 0.0f);
    for (int c = 0; c < channels; ++c)
    {
        float* d = dst.data() + (size_t) c * frames;
        const float* s = src + c;
        for (int i = 0; i < frames; ++i)
            d[i] = s[i * channels];
    }
}

// Interleave planar [channels][frames] into a contiguous [frames*channels]
// buffer suitable for mh_audio_write.
void interleave(const std::vector<std::vector<float>>& planar,
                int frames, std::vector<float>& dst)
{
    const int channels = (int) planar.size();
    dst.assign((size_t) channels * (size_t) frames, 0.0f);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
            dst[(size_t) i * channels + c] = planar[(size_t) c][(size_t) i];
}

} // namespace

void LoadedProject::updateMeters(float* const* const* out_buffers, int nframes)
{
    for (size_t m = 0; m < meter_states.size(); ++m)
    {
        const int buf_i = meter_buffer_indices[m];
        auto* state = meter_states[m].get();
        float* const* chans = out_buffers[(size_t) buf_i];
        const int n_ch = (int) state->peak.size();
        for (int c = 0; c < n_ch; ++c)
        {
            const float* src = chans[c];
            float peak = 0.0f;
            for (int i = 0; i < nframes; ++i)
            {
                const float a = std::fabs(src[i]);
                if (a > peak) peak = a;
            }
            state->peak[(size_t) c].store(peak, std::memory_order_relaxed);
        }
    }
}

void LoadedProject::renderMetronomes(std::vector<std::vector<float>>& planar_inputs,
                                     int block_size,
                                     int nframes,
                                     long long pos_samples,
                                     double sr, double bpm, bool playing)
{
    if (metronome_buffer_indices.empty()) return;
    const double samples_per_beat = (bpm > 0.0)
        ? sr * 60.0 / bpm : 0.0;

    for (size_t m = 0; m < metronome_buffer_indices.size(); ++m)
    {
        const int buf_i = metronome_buffer_indices[m];
        const auto& spec = doc.metronomes[m];
        auto& st = metronome_states[m];
        float* base = planar_inputs[(size_t) buf_i].data();
        auto chan = [&](int c) -> float* {
            return base + (size_t) c * (size_t) block_size;
        };

        // Always zero-fill first; clicks paint over silence.
        for (int c = 0; c < spec.channels; ++c)
            std::memset(chan(c), 0, (size_t) nframes * sizeof(float));

        if (!playing || samples_per_beat <= 0.0)
        {
            st.phase_samples = -1;
            continue;
        }

        // Click envelope parameters (sample-accurate).
        const double  decay_samples = (double) spec.decay_ms * 0.001 * sr;
        const double  w_rad         = 2.0 * 3.14159265358979323846
                                          * (double) spec.freq_hz / sr;
        const int     click_len     = (int) std::ceil(decay_samples * 6.0);
        const float   gain          = spec.gain;

        auto render_click = [&](int start_off, int sample_in_click_start) {
            const int end = std::min(nframes, start_off + click_len
                                              - sample_in_click_start);
            for (int i = start_off; i < end; ++i)
            {
                const int k = sample_in_click_start + (i - start_off);
                const float env = (float) std::exp(-(double) k / decay_samples);
                const float v   = gain * env
                    * (float) std::sin(w_rad * (double) k);
                for (int c = 0; c < spec.channels; ++c)
                    chan(c)[i] += v;
            }
            // Return how many samples of click remain after the block.
            const int produced = end - start_off;
            const int total_consumed = sample_in_click_start + produced;
            if (total_consumed >= click_len) return -1;
            // If we ran out of block before the click finished, carry
            // its phase forward.
            return total_consumed;
        };

        // 1. Continue any click in flight from a previous block.
        if (st.phase_samples >= 0)
            st.phase_samples = render_click(0, st.phase_samples);

        // 2. Find beat onsets that fall within this block and start
        //    clicks at them. Use 64-bit math throughout.
        const long long first_beat_idx = (long long) std::ceil(
            (double) pos_samples / samples_per_beat);
        for (long long b = first_beat_idx; ; ++b)
        {
            const long long beat_pos
                = (long long) std::llround((double) b * samples_per_beat);
            const long long off = beat_pos - pos_samples;
            if (off < 0)         continue;
            if (off >= nframes)  break;
            // Starting a new click overrides any in-flight one (rare
            // unless decay_ms approaches samples_per_beat).
            st.phase_samples = render_click((int) off, 0);
        }
    }
}

void LoadedProject::stageMidiClocks(MH_PluginGraph* g_handle,
                                    int nframes,
                                    long long pos_samples,
                                    double sr, double bpm, bool playing)
{
    if (midi_clock_node_ids.empty()) return;
    const double samples_per_tick = (bpm > 0.0)
        ? sr * 60.0 / (bpm * 24.0) : 0.0;

    for (size_t m = 0; m < midi_clock_node_ids.size(); ++m)
    {
        auto& buf  = midi_clock_event_buffers[m];
        auto& st   = midi_clock_states[m];
        buf.clear();

        // Transport-edge events: Start (0xFA) on rising edge,
        // Stop (0xFC) on falling edge. Sample offset 0.
        if (playing && !st.was_playing)
        {
            MH_MidiEvent e{}; e.sample_offset = 0; e.status = 0xFA;
            buf.push_back(e);
        }
        else if (!playing && st.was_playing)
        {
            MH_MidiEvent e{}; e.sample_offset = 0; e.status = 0xFC;
            buf.push_back(e);
        }
        st.was_playing = playing;

        // 24-PPQN clock ticks while playing.
        if (playing && samples_per_tick > 0.0)
        {
            const long long first_tick = (long long) std::ceil(
                (double) pos_samples / samples_per_tick);
            for (long long t = first_tick; ; ++t)
            {
                const long long tick_pos = (long long) std::llround(
                    (double) t * samples_per_tick);
                const long long off = tick_pos - pos_samples;
                if (off < 0)        continue;
                if (off >= nframes) break;
                MH_MidiEvent e{};
                e.sample_offset = (int) off;
                e.status        = 0xF8;
                buf.push_back(e);
            }
        }

        mh_graph_set_midi_input_events(
            g_handle, midi_clock_node_ids[m],
            buf.empty() ? nullptr : buf.data(),
            (int) buf.size());
    }
}

LoadedProject::~LoadedProject()
{
    // Destroy the graph first so its plugin-node references release
    // before the underlying MH_Plugin* instances get mh_close'd.
    graph.reset();
    for (MH_Plugin* p : plugins) if (p) mh_close(p);
    plugins.clear();
}

ProjectDocument parseProjectFile(const juce::File& path)
{
    if (!path.existsAsFile())
        throw ProjectError(("project file not found: "
                            + path.getFullPathName()).toStdString());

    juce::var doc;
    {
        auto parsed = juce::JSON::parse(path);
        if (parsed.isVoid())
            throwErr("invalid JSON in " + path.getFullPathName());
        doc = parsed;
    }
    if (!doc.isObject())
        throwErr("project root must be an object");

    const int version = requireInt(doc, "minihost_project_version");
    if (version != kSchemaVersion)
        throwErr("unsupported project version "
                 + juce::String(version)
                 + " (this build understands version "
                 + juce::String(kSchemaVersion) + ")");

    ProjectDocument out;
    out.sample_rate = (int) requireDouble(doc, "sample_rate");
    out.block_size  = requireInt(doc, "block_size");
    if (doc.getDynamicObject()->hasProperty("duration_seconds"))
    {
        auto v = doc["duration_seconds"];
        if (!v.isVoid())
            out.duration_seconds = (double) v;
    }

    const auto project_dir = path.getParentDirectory();

    std::unordered_set<std::string> ids;
    auto nodes = requireArray(doc, "nodes");
    for (const auto& n : nodes)
    {
        if (!n.isObject()) throwErr("node entry must be an object");
        const auto id   = requireString(n, "id");
        const auto kind = requireString(n, "kind");
        if (!ids.insert(id.toStdString()).second)
            throwErr("duplicate node id: " + id);

        const auto* entry = findKind(kind);
        if (entry != nullptr)
        {
            try {
                entry->parse(n, id, out, project_dir);
            } catch (const ProjectError& e) {
                // Re-prepend the kind so error messages remain
                // localizable to the failing node.
                throw ProjectError(
                    (kind + " " + id + ": " + e.what()).toStdString());
            }
        }
        else
        {
            throwErr("unknown node kind: " + kind);
        }
    }

    // Project must have at least one audio destination. File renders
    // require an OutputNodeSpec (file sink); live playback can use
    // a DeviceOutputNodeSpec instead. The stricter file-render
    // requirement is checked separately in renderProject.
    if (out.outputs.empty() && out.device_outputs.empty())
        throwErr("project has no output nodes "
                 "(needs at least one file 'output' or 'device_output')");

    auto edges = requireArray(doc, "edges");
    for (const auto& e : edges)
    {
        if (!e.isObject()) throwErr("edge entry must be an object");
        EdgeSpec es;
        es.src      = requireString(e, "src");
        es.dst      = requireString(e, "dst");
        if (e.getDynamicObject()->hasProperty("dst_port"))
            es.dst_port = (int) e["dst_port"];
        if (e.getDynamicObject()->hasProperty("kind"))
        {
            const auto k = e["kind"].toString();
            if      (k == "audio") es.kind = EdgeKind::Audio;
            else if (k == "midi")  es.kind = EdgeKind::Midi;
            else throwErr("edge kind must be \"audio\" or \"midi\": "
                           + k);
        }
        out.edges.push_back(std::move(es));
    }

    // Optional canvas layout. Entries for unknown ids are silently
    // dropped (defensive: hand-edited files, schema drift).
    if (doc.getDynamicObject()->hasProperty("layout"))
    {
        auto layout_var = doc["layout"];
        if (layout_var.isObject())
        {
            std::unordered_set<std::string> known;
            for (const auto& entry : nodeRegistry())
            {
                const int c = entry.count(out);
                for (int k = 0; k < c; ++k)
                    known.insert(entry.id_at(out, k).toStdString());
            }

            auto* dyn = layout_var.getDynamicObject();
            for (const auto& kv : dyn->getProperties())
            {
                const std::string nid = kv.name.toString().toStdString();
                if (known.find(nid) == known.end()) continue;
                if (!kv.value.isObject()) continue;
                auto pos = kv.value;
                if (!pos.getDynamicObject()->hasProperty("x")) continue;
                if (!pos.getDynamicObject()->hasProperty("y")) continue;
                NodePosition p;
                p.x = (float) (double) pos["x"];
                p.y = (float) (double) pos["y"];
                out.layout[nid] = p;
            }
        }
    }

    return out;
}

void saveProjectFile(const juce::File& path, const ProjectDocument& doc)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("minihost_project_version", kSchemaVersion);
    root->setProperty("sample_rate", doc.sample_rate);
    root->setProperty("block_size",  doc.block_size);
    if (doc.duration_seconds.has_value())
        root->setProperty("duration_seconds", *doc.duration_seconds);

    juce::Array<juce::var> nodes;
    NodeKindEntry::PushNodeFn pushNode =
        [&](juce::DynamicObject* obj, const juce::String& kind,
            const juce::String& id) {
            obj->setProperty("id",   id);
            obj->setProperty("kind", kind);
            nodes.add(juce::var(obj));
        };
    for (const auto& entry : nodeRegistry())
        entry.serialize_all(doc, pushNode);
    root->setProperty("nodes", nodes);

    juce::Array<juce::var> edges;
    for (const auto& e : doc.edges)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("src", e.src);
        o->setProperty("dst", e.dst);
        if (e.kind == EdgeKind::Midi)
        {
            o->setProperty("kind", juce::String("midi"));
            // dst_port is meaningful for MIDI edges only when the
            // dst is a midi_merge. Emit it whenever non-zero.
            if (e.dst_port != 0)
                o->setProperty("dst_port", e.dst_port);
        }
        else
        {
            // Audio is the default; emit dst_port only.
            o->setProperty("dst_port", e.dst_port);
        }
        edges.add(juce::var(o));
    }
    root->setProperty("edges", edges);

    if (!doc.layout.empty())
    {
        auto* layout = new juce::DynamicObject();
        for (const auto& kv : doc.layout)
        {
            auto* pos = new juce::DynamicObject();
            pos->setProperty("x", (double) kv.second.x);
            pos->setProperty("y", (double) kv.second.y);
            layout->setProperty(juce::String(kv.first), juce::var(pos));
        }
        root->setProperty("layout", juce::var(layout));
    }

    const juce::String text = juce::JSON::toString(juce::var(root),
        /*allOnOneLine=*/false);

    juce::File tmp = path.getSiblingFile(path.getFileName() + ".tmp");
    if (!tmp.replaceWithText(text + "\n"))
        throw ProjectError("failed to write " + tmp.getFullPathName().toStdString());
    if (!tmp.moveFileTo(path))
    {
        tmp.deleteFile();
        throw ProjectError("failed to rename tmp file to "
                           + path.getFullPathName().toStdString());
    }
}

std::unique_ptr<LoadedProject> loadProject(const juce::File& path)
{
    auto loaded = std::make_unique<LoadedProject>();
    loaded->doc = parseProjectFile(path);
    auto& doc = loaded->doc;

    // Read input audio.
    for (auto& in : doc.inputs)
    {
        if (!in.source.existsAsFile())
            throwErr("input source not found: " + in.source.getFullPathName());
        char err[256] = {0};
        MH_AudioData* ad = mh_audio_read(in.source.getFullPathName().toRawUTF8(),
                                         err, sizeof(err));
        if (!ad)
            throwErr("failed to read " + in.source.getFullPathName()
                     + ": " + juce::String(static_cast<const char*>(err)));
        struct Guard { MH_AudioData* p; ~Guard() { if (p) mh_audio_data_free(p); } } g{ad};

        // `use` points at whichever buffer feeds the graph: the decoded
        // file, or a resampled copy when the rate mismatches and the
        // input opted in. resampled (if created) is freed by g2.
        MH_AudioData* use = ad;
        struct Guard2 { MH_AudioData* p; ~Guard2() { if (p) mh_audio_data_free(p); } } g2{nullptr};

        if ((int) ad->sample_rate != doc.sample_rate)
        {
            if (!in.resample)
                throwErr("input " + in.id + ": file sample rate "
                         + juce::String((int) ad->sample_rate)
                         + " does not match project sample_rate "
                         + juce::String(doc.sample_rate)
                         + " (enable resample on the input to convert)");
            char rerr[256] = {0};
            MH_AudioData* rs = mh_audio_resample(
                ad->data, ad->channels, (unsigned int) ad->frames,
                ad->sample_rate, (unsigned int) doc.sample_rate,
                rerr, sizeof(rerr));
            if (!rs)
                throwErr("input " + in.id + ": resample "
                         + juce::String((int) ad->sample_rate) + " -> "
                         + juce::String(doc.sample_rate) + " failed: "
                         + juce::String(static_cast<const char*>(rerr)));
            g2.p = rs;
            use  = rs;
        }

        if ((int) use->channels != in.channels)
            throwErr("input " + in.id + ": file has "
                     + juce::String((int) use->channels)
                     + " channels, project declares "
                     + juce::String(in.channels));

        std::vector<float> planar;
        deinterleave(use->data, in.channels, (int) use->frames, planar);
        loaded->input_audio.push_back(std::move(planar));
        loaded->input_frames.push_back((int) use->frames);
    }

    // Compute render length. Only meaningful for file rendering;
    // live playback runs until the user stops it and ignores
    // duration_frames. If we can't derive a length (no file inputs
    // and no explicit duration_seconds), leave it at 0 -- live mode
    // will work fine and renderProject enforces its own non-zero
    // requirement before writing files.
    if (doc.duration_seconds.has_value())
    {
        loaded->duration_frames = (int) std::lround(
            *doc.duration_seconds * (double) doc.sample_rate);
    }
    else if (!loaded->input_frames.empty())
    {
        int maxF = 0;
        for (int f : loaded->input_frames) maxF = std::max(maxF, f);
        loaded->duration_frames = maxF;
    }
    else
    {
        loaded->duration_frames = 0;
    }

    // Open plugins. Prefer the descriptor (AudioUnits, which have no file
    // path) via mh_open_desc; fall back to the path via mh_open.
    for (auto& pl : doc.plugins)
    {
        char err[512] = {0};
        MH_Plugin* p = nullptr;
        if (pl.descriptor.isNotEmpty())
        {
            juce::MemoryBlock db;
            if (!decodeBase64(pl.descriptor, db))
                throwErr("plugin " + pl.id + ": malformed descriptor");
            const juce::String pd_xml = juce::String::fromUTF8(
                static_cast<const char*>(db.getData()), (int) db.getSize());
            p = mh_open_desc(pd_xml.toRawUTF8(),
                             (double) doc.sample_rate, doc.block_size,
                             /*req_in=*/0, /*req_out=*/0, err, sizeof(err));
        }
        else
        {
            if (!pl.path.exists())
                throwErr("plugin path not found: " + pl.path.getFullPathName());
            p = mh_open(pl.path.getFullPathName().toRawUTF8(),
                        (double) doc.sample_rate, doc.block_size,
                        /*req_in=*/0, /*req_out=*/0, err, sizeof(err));
        }
        if (!p)
            throwErr("plugin " + pl.id + " failed to open: "
                     + juce::String(static_cast<const char*>(err)));
        loaded->plugins.push_back(p);

        if (pl.state_b64.isNotEmpty())
        {
            juce::MemoryBlock mb;
            if (!decodeBase64(pl.state_b64, mb))
                throwErr("plugin " + pl.id + ": malformed state_b64");
            mh_set_state(p, mb.getData(), (int) mb.getSize());
        }
    }

    // Build the graph.
    loaded->graph = std::make_unique<minihost::PluginGraph>(
        doc.block_size, (double) doc.sample_rate);
    auto& g = *loaded->graph;

    std::unordered_map<std::string, minihost::PluginGraph::NodeId> id_to_node;
    // Single dispatch: walk every kind in canonical registry order
    // and let the entry's load_one translate its specs into graph
    // nodes (and record any side effects on LoadedProject). The
    // registry order is what defines input_buffers / output_buffers
    // ordering for the resulting graph -- see node_registry.cpp.
    for (const auto& entry : nodeRegistry())
    {
        const int c = entry.count(doc);
        for (int i = 0; i < c; ++i)
            entry.load_one(doc, i, g, id_to_node, *loaded);
    }

    for (const auto& e : doc.edges)
    {
        auto src_it = id_to_node.find(e.src.toStdString());
        auto dst_it = id_to_node.find(e.dst.toStdString());
        if (src_it == id_to_node.end())
            throwErr("edge references unknown src id: " + e.src);
        if (dst_it == id_to_node.end())
            throwErr("edge references unknown dst id: " + e.dst);
        if (e.kind == EdgeKind::Midi)
            // dst_port is reused for MIDI edges so that merges with
            // multiple inputs can be addressed. Single-port consumers
            // accept only dst_port == 0; the graph compiler enforces
            // this at compile time.
            g.connectMidiPort(src_it->second, dst_it->second,
                              e.dst_port);
        else
            g.connect(src_it->second, dst_it->second, e.dst_port);
    }

    // Legacy migration: pre-MIDI-routing projects expressed live MIDI
    // fan-out via PluginNodeSpec::receives_midi. If the loaded document
    // has no midi_inputs but at least one plugin opts in, synthesize a
    // MIDI_INPUT node and connect it to those plugins so live MIDI
    // continues to drive them through the new routing path. The
    // ProjectDocument is left unchanged on disk; the migration only
    // affects the in-memory graph.
    //
    // Only wire plugins that actually accept MIDI. receives_midi defaults
    // to true, so an effect-only graph would otherwise try to connect MIDI
    // to a plugin the graph compiler rejects (connectMidi throws), aborting
    // the whole load. The opened instance is authoritative -- the cached
    // accepts_midi flag on the spec may be stale or absent for plugins
    // loaded from disk without a fresh probe.
    if (doc.midi_inputs.empty())
    {
        std::vector<MH_NodeId> recv_nodes;
        for (size_t i = 0; i < doc.plugins.size(); ++i)
        {
            const auto& pl = doc.plugins[i];
            if (!pl.receives_midi) continue;
            MH_Info info{};
            if (mh_get_info(loaded->plugins[i], &info) && info.accepts_midi)
                recv_nodes.push_back(id_to_node[pl.id.toStdString()]);
        }
        if (!recv_nodes.empty())
        {
            auto mi_node = g.addMidiInput();
            loaded->midi_input_node_ids.push_back(mi_node);
            for (auto n : recv_nodes)
                g.connectMidi(mi_node, n);
        }
    }

    g.compile();
    return loaded;
}

bool renderProject(LoadedProject& proj,
                   std::atomic<bool>& cancel_flag,
                   std::function<void(int, int)> progress_callback,
                   juce::String& error,
                   const RenderOptions& options)
{
    const auto& doc = proj.doc;
    if (doc.outputs.empty())
    {
        error = "project has no file 'output' nodes -- file rendering "
                "needs at least one OutputNodeSpec (a device_output "
                "alone is for live playback, not file writing)";
        return false;
    }
    if (proj.duration_frames <= 0)
    {
        error = "cannot determine render length -- the project needs "
                "either an `input` node (length derived from the file) "
                "or an explicit `duration_seconds` field at the top level";
        return false;
    }
    const int block = doc.block_size;
    const int total = proj.duration_frames;

    // Input buffer table covers file-source inputs (staged from
    // decoded WAV/FLAC data per block), device_input nodes (live
    // only; file render keeps them silent), and metronome nodes
    // (live only; same). Ordering matches the loader: file inputs
    // first, then device_inputs, then metronomes.
    const size_t n_file_in  = proj.doc.inputs.size();
    const size_t n_dev_in   = proj.doc.device_inputs.size();
    const size_t n_met_in   = proj.doc.metronomes.size();
    const size_t n_audio_in = n_file_in + n_dev_in + n_met_in;
    std::vector<std::vector<float>>  in_planar(n_audio_in);
    std::vector<std::vector<const float*>> in_ch_ptrs(n_audio_in);
    std::vector<const float* const*> in_top_ptrs(n_audio_in);
    auto in_ch_at = [&](size_t i) -> int {
        if (i < n_file_in) return proj.doc.inputs[i].channels;
        if (i < n_file_in + n_dev_in)
            return proj.doc.device_inputs[i - n_file_in].channels;
        return proj.doc.metronomes[i - n_file_in - n_dev_in].channels;
    };
    for (size_t i = 0; i < n_audio_in; ++i)
    {
        const int ch = in_ch_at(i);
        in_planar[i].assign((size_t) ch * (size_t) block, 0.0f);
        in_ch_ptrs[i].resize((size_t) ch);
        for (int c = 0; c < ch; ++c)
            in_ch_ptrs[i][(size_t) c]
                = in_planar[i].data() + (size_t) c * block;
        in_top_ptrs[i] = in_ch_ptrs[i].data();
    }

    // Output buffer table covers file-sink outputs (written to disk
    // via the accumulator below), device_output nodes (scratch only;
    // samples discarded in file render), and meter nodes (scratch
    // only; live peak capture is skipped during file render). Order
    // matches the loader.
    const size_t n_file_out   = proj.doc.outputs.size();
    const size_t n_dev_out    = proj.doc.device_outputs.size();
    const size_t n_meters_out = proj.doc.meters.size();
    const size_t n_audio_out  = n_file_out + n_dev_out + n_meters_out;
    std::vector<std::vector<float>>  out_planar(n_audio_out);
    std::vector<std::vector<float*>> out_ch_ptrs(n_audio_out);
    std::vector<float* const*>       out_top_ptrs(n_audio_out);
    auto out_ch_at = [&](size_t i) -> int {
        if (i < n_file_out) return proj.doc.outputs[i].channels;
        if (i < n_file_out + n_dev_out)
            return proj.doc.device_outputs[i - n_file_out].channels;
        return proj.doc.meters[i - n_file_out - n_dev_out].channels;
    };
    for (size_t i = 0; i < n_audio_out; ++i)
    {
        const int ch = out_ch_at(i);
        out_planar[i].assign((size_t) ch * (size_t) block, 0.0f);
        out_ch_ptrs[i].resize((size_t) ch);
        for (int c = 0; c < ch; ++c)
            out_ch_ptrs[i][(size_t) c]
                = out_planar[i].data() + (size_t) c * block;
        out_top_ptrs[i] = out_ch_ptrs[i].data();
    }

    // Per-output accumulator (planar) covering the full render.
    std::vector<std::vector<std::vector<float>>> accum(proj.doc.outputs.size());
    for (size_t i = 0; i < proj.doc.outputs.size(); ++i)
    {
        const int ch = proj.doc.outputs[i].channels;
        accum[i].assign((size_t) ch, std::vector<float>((size_t) total, 0.0f));
    }

    // Per-file-input cursors into each MIDI source's sorted event list, so
    // staging a block is a forward walk rather than a rescan.
    std::vector<size_t> midi_cursors(proj.file_midi_inputs.size(), 0);
    std::vector<MH_MidiEvent> block_midi;   // reused scratch per block

    int frame = 0;
    while (frame < total)
    {
        if (cancel_flag.load(std::memory_order_relaxed))
        {
            error = "cancelled";
            return false;
        }
        const int n = std::min(block, total - frame);

        // Stage MIDI inputs: events in [frame, frame + n), rebased to a
        // block-local sample offset (mirrors the Python renderer). Must be
        // re-staged every block; the graph clears pending events after each
        // render_block.
        const int block_end = frame + n;
        for (size_t mi = 0; mi < proj.file_midi_inputs.size(); ++mi)
        {
            auto& fmi = proj.file_midi_inputs[mi];
            size_t& cur = midi_cursors[mi];
            block_midi.clear();
            while (cur < fmi.events.size()
                   && fmi.events[cur].sample_offset < block_end)
            {
                MH_MidiEvent e = fmi.events[cur];
                e.sample_offset -= frame;
                block_midi.push_back(e);
                ++cur;
            }
            mh_graph_set_midi_input_events(
                proj.graph->handle(), fmi.node_id,
                block_midi.empty() ? nullptr : block_midi.data(),
                (int) block_midi.size());
        }

        // Stage inputs.
        for (size_t i = 0; i < proj.doc.inputs.size(); ++i)
        {
            const int ch = proj.doc.inputs[i].channels;
            const int avail = std::max(0, std::min(n, proj.input_frames[i] - frame));
            const float* src_base = proj.input_audio[i].data();
            const int src_frames  = proj.input_frames[i];
            for (int c = 0; c < ch; ++c)
            {
                float* dst = in_planar[i].data() + (size_t) c * block;
                const float* src = src_base + (size_t) c * src_frames + frame;
                if (avail > 0)
                    std::memcpy(dst, src, (size_t) avail * sizeof(float));
                if (avail < n)
                    std::memset(dst + avail, 0,
                                (size_t)(n - avail) * sizeof(float));
            }
        }

        try {
            proj.graph->renderBlock(in_top_ptrs.data(),
                                    (int) n_audio_in,
                                    out_top_ptrs.data(),
                                    (int) n_audio_out,
                                    n);
        } catch (const std::exception& e) {
            error = juce::String("render_block failed: ") + e.what();
            return false;
        }

        // Capture outputs into accumulator.
        for (size_t i = 0; i < proj.doc.outputs.size(); ++i)
        {
            const int ch = proj.doc.outputs[i].channels;
            for (int c = 0; c < ch; ++c)
            {
                const float* src = out_planar[i].data()
                                 + (size_t) c * block;
                std::memcpy(accum[i][(size_t) c].data() + frame,
                            src, (size_t) n * sizeof(float));
            }
        }

        frame += n;
        if (progress_callback) progress_callback(frame, total);
    }

    // Optional normalization: find the peak across all outputs and
    // scale so the peak == 10^(dbfs/20). Silence is left alone.
    if (options.normalize_dbfs != 0.0)
    {
        float peak = 0.0f;
        for (const auto& planar : accum)
            for (const auto& ch : planar)
                for (float s : ch) peak = std::max(peak, std::fabs(s));
        if (peak > 1e-9f)
        {
            const double target = std::pow(10.0, options.normalize_dbfs / 20.0);
            const float  gain   = (float) (target / peak);
            for (auto& planar : accum)
                for (auto& ch : planar)
                    for (float& s : ch) s *= gain;
        }
    }

    // Write sinks.
    for (size_t i = 0; i < proj.doc.outputs.size(); ++i)
    {
        const auto& on = proj.doc.outputs[i];
        on.sink.getParentDirectory().createDirectory();
        std::vector<float> interleaved;
        interleave(accum[i], total, interleaved);
        const int bit_depth = options.bit_depth_override > 0
                            ? options.bit_depth_override
                            : on.bit_depth;
        char err[256] = {0};
        const int ok = mh_audio_write(
            on.sink.getFullPathName().toRawUTF8(),
            interleaved.data(),
            (unsigned) on.channels,
            (unsigned) total,
            (unsigned) doc.sample_rate,
            bit_depth,
            err, sizeof(err));
        if (!ok)
        {
            error = "mh_audio_write failed for "
                  + on.sink.getFullPathName() + ": "
                  + juce::String(static_cast<const char*>(err));
            return false;
        }
    }
    return true;
}

} // namespace minihost_desktop::project
