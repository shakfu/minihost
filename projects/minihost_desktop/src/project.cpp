// project.cpp -- see project.h for the design.

#include "project.h"

#include "minihost_audiofile.h"

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace minihost_desktop::project {

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

        if (kind == "input")
        {
            InputNodeSpec s;
            s.id       = id;
            s.channels = requireInt(n, "channels");
            const auto rel = requireString(n, "source");
            s.source   = project_dir.getChildFile(rel);
            out.inputs.push_back(std::move(s));
        }
        else if (kind == "output")
        {
            OutputNodeSpec s;
            s.id       = id;
            s.channels = requireInt(n, "channels");
            const auto rel = requireString(n, "sink");
            s.sink     = project_dir.getChildFile(rel);
            if (n.getDynamicObject()->hasProperty("bit_depth"))
                s.bit_depth = (int) n["bit_depth"];
            out.outputs.push_back(std::move(s));
        }
        else if (kind == "plugin")
        {
            PluginNodeSpec s;
            s.id   = id;
            s.path = juce::File(requireString(n, "path"));
            if (n.getDynamicObject()->hasProperty("state_b64"))
            {
                auto v = n["state_b64"];
                if (!v.isVoid() && v.isString())
                    s.state_b64 = v.toString();
            }
            if (n.getDynamicObject()->hasProperty("receives_midi"))
            {
                auto v = n["receives_midi"];
                if (v.isBool() || v.isInt() || v.isInt64())
                    s.receives_midi = ((bool) v);
            }
            out.plugins.push_back(std::move(s));
        }
        else if (kind == "mix")
        {
            MixNodeSpec s;
            s.id         = id;
            s.num_inputs = requireInt(n, "num_inputs");
            s.channels   = requireInt(n, "channels");
            s.gains.assign((size_t) s.num_inputs, 1.0f);
            if (n.getDynamicObject()->hasProperty("gains"))
            {
                auto v = n["gains"];
                if (v.isArray())
                {
                    const auto& arr = *v.getArray();
                    if ((int) arr.size() != s.num_inputs)
                        throwErr("mix " + id + ": gains length does not "
                                 "match num_inputs");
                    for (int i = 0; i < s.num_inputs; ++i)
                        s.gains[(size_t) i] = (float) (double) arr[i];
                }
            }
            out.mixes.push_back(std::move(s));
        }
        else
        {
            throwErr("unknown node kind: " + kind);
        }
    }

    if (out.outputs.empty())
        throwErr("project has no output nodes");

    auto edges = requireArray(doc, "edges");
    for (const auto& e : edges)
    {
        if (!e.isObject()) throwErr("edge entry must be an object");
        EdgeSpec es;
        es.src      = requireString(e, "src");
        es.dst      = requireString(e, "dst");
        if (e.getDynamicObject()->hasProperty("dst_port"))
            es.dst_port = (int) e["dst_port"];
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
            for (const auto& n : out.inputs)  known.insert(n.id.toStdString());
            for (const auto& n : out.plugins) known.insert(n.id.toStdString());
            for (const auto& n : out.mixes)   known.insert(n.id.toStdString());
            for (const auto& n : out.outputs) known.insert(n.id.toStdString());

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
    auto pushNode = [&](juce::DynamicObject* obj, const juce::String& kind,
                        const juce::String& id) {
        obj->setProperty("id",   id);
        obj->setProperty("kind", kind);
        nodes.add(juce::var(obj));
    };
    for (const auto& n : doc.inputs)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("channels", n.channels);
        o->setProperty("source",   n.source.getFullPathName());
        pushNode(o, "input", n.id);
    }
    for (const auto& n : doc.plugins)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("path", n.path.getFullPathName());
        if (n.state_b64.isNotEmpty())
            o->setProperty("state_b64", n.state_b64);
        if (!n.receives_midi)   // serialize the non-default explicitly
            o->setProperty("receives_midi", false);
        pushNode(o, "plugin", n.id);
    }
    for (const auto& n : doc.mixes)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("num_inputs", n.num_inputs);
        o->setProperty("channels",   n.channels);
        juce::Array<juce::var> gains;
        for (float g : n.gains) gains.add((double) g);
        o->setProperty("gains", gains);
        pushNode(o, "mix", n.id);
    }
    for (const auto& n : doc.outputs)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("channels",  n.channels);
        o->setProperty("sink",      n.sink.getFullPathName());
        o->setProperty("bit_depth", n.bit_depth);
        pushNode(o, "output", n.id);
    }
    root->setProperty("nodes", nodes);

    juce::Array<juce::var> edges;
    for (const auto& e : doc.edges)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("src", e.src);
        o->setProperty("dst", e.dst);
        o->setProperty("dst_port", e.dst_port);
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

        if ((int) ad->sample_rate != doc.sample_rate)
            throwErr("input " + in.id + ": file sample rate "
                     + juce::String((int) ad->sample_rate)
                     + " does not match project sample_rate "
                     + juce::String(doc.sample_rate));
        if ((int) ad->channels != in.channels)
            throwErr("input " + in.id + ": file has "
                     + juce::String((int) ad->channels)
                     + " channels, project declares "
                     + juce::String(in.channels));

        std::vector<float> planar;
        deinterleave(ad->data, in.channels, (int) ad->frames, planar);
        loaded->input_audio.push_back(std::move(planar));
        loaded->input_frames.push_back((int) ad->frames);
    }

    // Compute render length.
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
        throwErr("project has no input nodes and no duration_seconds; "
                 "cannot determine render length");
    }

    // Open plugins.
    for (auto& pl : doc.plugins)
    {
        if (!pl.path.exists())
            throwErr("plugin path not found: " + pl.path.getFullPathName());
        char err[512] = {0};
        MH_Plugin* p = mh_open(pl.path.getFullPathName().toRawUTF8(),
                               (double) doc.sample_rate,
                               doc.block_size,
                               /*req_in=*/0, /*req_out=*/0,
                               err, sizeof(err));
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
    loaded->graph = std::make_unique<minihost::GraphV2>(
        doc.block_size, (double) doc.sample_rate);
    auto& g = *loaded->graph;

    std::unordered_map<std::string, minihost::GraphV2::NodeId> id_to_node;
    for (const auto& in : doc.inputs)
        id_to_node[in.id.toStdString()] = g.addInput(in.channels);
    for (size_t i = 0; i < doc.plugins.size(); ++i)
    {
        const auto& pl = doc.plugins[i];
        id_to_node[pl.id.toStdString()] = g.addPlugin(loaded->plugins[i]);
    }
    for (const auto& mx : doc.mixes)
    {
        auto nid = g.addMix(mx.num_inputs, mx.channels);
        for (int i = 0; i < mx.num_inputs; ++i)
            g.setMixGain(nid, i, mx.gains[(size_t) i]);
        id_to_node[mx.id.toStdString()] = nid;
    }
    for (const auto& on : doc.outputs)
        id_to_node[on.id.toStdString()] = g.addOutput(on.channels);

    for (const auto& e : doc.edges)
    {
        auto src_it = id_to_node.find(e.src.toStdString());
        auto dst_it = id_to_node.find(e.dst.toStdString());
        if (src_it == id_to_node.end())
            throwErr("edge references unknown src id: " + e.src);
        if (dst_it == id_to_node.end())
            throwErr("edge references unknown dst id: " + e.dst);
        g.connect(src_it->second, dst_it->second, e.dst_port);
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
    const int block = doc.block_size;
    const int total = proj.duration_frames;

    // Pre-allocate scratch buffers (per input node, per output node).
    std::vector<std::vector<float>>  in_planar(proj.doc.inputs.size());
    std::vector<std::vector<const float*>> in_ch_ptrs(proj.doc.inputs.size());
    std::vector<const float* const*> in_top_ptrs(proj.doc.inputs.size());

    for (size_t i = 0; i < proj.doc.inputs.size(); ++i)
    {
        const int ch = proj.doc.inputs[i].channels;
        in_planar[i].assign((size_t) ch * (size_t) block, 0.0f);
        in_ch_ptrs[i].resize((size_t) ch);
        for (int c = 0; c < ch; ++c)
            in_ch_ptrs[i][(size_t) c]
                = in_planar[i].data() + (size_t) c * block;
        in_top_ptrs[i] = in_ch_ptrs[i].data();
    }

    std::vector<std::vector<float>>  out_planar(proj.doc.outputs.size());
    std::vector<std::vector<float*>> out_ch_ptrs(proj.doc.outputs.size());
    std::vector<float* const*>       out_top_ptrs(proj.doc.outputs.size());
    for (size_t i = 0; i < proj.doc.outputs.size(); ++i)
    {
        const int ch = proj.doc.outputs[i].channels;
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

    int frame = 0;
    while (frame < total)
    {
        if (cancel_flag.load(std::memory_order_relaxed))
        {
            error = "cancelled";
            return false;
        }
        const int n = std::min(block, total - frame);

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
                                    (int) proj.doc.inputs.size(),
                                    out_top_ptrs.data(),
                                    (int) proj.doc.outputs.size(),
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
