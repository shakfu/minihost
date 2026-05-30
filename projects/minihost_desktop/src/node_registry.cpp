// node_registry.cpp -- see node_registry.h for design.

#include "node_registry.h"
#include "minihost.h"

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cstring>

namespace minihost_desktop {

namespace {

using project::ProjectDocument;
using project::LoadedProject;

//============================================================
// Reusable accessors keyed by pointer-to-member of a doc vector
//
// Each NodeKindEntry needs count / id_at / erase_by_id functions
// that all do the same thing modulo which doc vector they hit.
// Templating on the pointer-to-member collapses the boilerplate.
//============================================================
template <auto VecPtr>
int count_of(const ProjectDocument& d)
{
    return (int) (d.*VecPtr).size();
}

template <auto VecPtr>
juce::String id_of(const ProjectDocument& d, int idx)
{
    return (d.*VecPtr)[(size_t) idx].id;
}

template <auto VecPtr>
void erase_of(ProjectDocument& d, const juce::String& id)
{
    auto& v = d.*VecPtr;
    v.erase(std::remove_if(v.begin(), v.end(),
        [&](const auto& s){ return s.id == id; }), v.end());
}

// Common pattern: setProperty("kind", kind), setProperty("id", id).
// pushNode is provided by the caller as a closure that adds the
// finished DynamicObject to the nodes array.
//
//============================================================
// Per-kind serialize_all helpers
//============================================================

// Generic shape for kinds where we just emit a small set of fields.
// Each kind needs its own lambda; this is only sugar.

//============================================================
// JSON property helpers
//============================================================
static int prop_int(const juce::var& v, const juce::String& key, int dflt)
{
    const juce::Identifier id(key);
    if (v.getDynamicObject()->hasProperty(id))
        return (int) v[id];
    return dflt;
}
static double prop_double(const juce::var& v, const juce::String& key, double dflt)
{
    const juce::Identifier id(key);
    if (v.getDynamicObject()->hasProperty(id))
        return (double) v[id];
    return dflt;
}
static juce::String prop_string(const juce::var& v, const juce::String& key,
                                 const juce::String& dflt)
{
    const juce::Identifier id(key);
    if (v.getDynamicObject()->hasProperty(id))
    {
        auto x = v[id];
        if (x.isString()) return x.toString();
    }
    return dflt;
}

// Required-field accessors that throw ProjectError when missing or
// the wrong type, matching the pre-registry behavior of the
// requireXxx helpers in project.cpp.
static int require_int(const juce::var& v, const juce::String& key)
{
    const juce::Identifier id(key);
    auto* obj = v.getDynamicObject();
    if (!obj || !obj->hasProperty(id))
        throw project::ProjectError(
            ("missing required field: " + key).toStdString());
    return (int) v[id];
}
static juce::String require_string(const juce::var& v, const juce::String& key)
{
    const juce::Identifier id(key);
    auto* obj = v.getDynamicObject();
    if (!obj || !obj->hasProperty(id))
        throw project::ProjectError(
            ("missing required field: " + key).toStdString());
    auto x = v[id];
    if (!x.isString())
        throw project::ProjectError(
            ("field " + key + " must be a string").toStdString());
    return x.toString();
}

// Unique id-generator that doesn't collide with anything already in
// any of the doc's spec vectors. Used by menu_add helpers.
static juce::String uniqueId(const ProjectDocument& d,
                             const juce::String& prefix)
{
    int n = 1;
    for (;; ++n)
    {
        juce::String candidate = prefix + juce::String(n);
        bool used = false;
        for (const auto& e : nodeRegistry())
        {
            const int c = e.count ? e.count(d) : 0;
            for (int i = 0; i < c; ++i)
                if (e.id_at(d, i) == candidate) { used = true; break; }
            if (used) break;
        }
        if (!used) return candidate;
    }
}

// Read a text editor field as int / float with a fallback when the
// field is empty or unparseable.
static int read_int(juce::AlertWindow& aw, const juce::String& field,
                     int dflt)
{
    auto t = aw.getTextEditorContents(field);
    if (t.isEmpty()) return dflt;
    return t.getIntValue();
}
static float read_float(juce::AlertWindow& aw, const juce::String& field,
                         float dflt)
{
    auto t = aw.getTextEditorContents(field);
    if (t.isEmpty()) return dflt;
    return t.getFloatValue();
}

//============================================================
// Per-kind entry builders
//
// One function per node kind, returning a fully-populated
// NodeKindEntry. The functions stay private to this translation
// unit; the registry vector is the public surface.
//============================================================

//---- input ----
NodeKindEntry makeInput()
{
    NodeKindEntry e;
    e.kind_string = "input";
    e.colour      = juce::Colour(0xff3a6f99);
    e.count       = count_of  <&ProjectDocument::inputs>;
    e.id_at       = id_of     <&ProjectDocument::inputs>;
    e.erase_by_id = erase_of  <&ProjectDocument::inputs>;
    e.menu_label  = {};  // file chooser handled by canvas directly

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::InputNodeSpec s;
        s.id       = id;
        s.channels = require_int(n, "channels");
        s.source   = project_dir.getChildFile(require_string(n, "source"));
        d.inputs.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.inputs)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels", n.channels);
            o->setProperty("source",   n.source.getFullPathName());
            push(o, "input", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.inputs[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(input, " + juce::String(s.channels) + "ch)",
            0, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.inputs[(size_t) i].channels;
    };

    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Input node");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.inputs[(size_t) i];
        aw.addTextEditor("channels", juce::String(s.channels), "channels");
        aw.addTextEditor("source",   s.source.getFullPathName(), "source path");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.inputs[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels = read_int(aw, "channels", s.channels);
        s.source   = juce::File(aw.getTextEditorContents("source"));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.inputs[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addInput(s.channels);
    };
    return e;
}

//---- plugin ----
NodeKindEntry makePlugin()
{
    NodeKindEntry e;
    e.kind_string = "plugin";
    e.colour      = juce::Colour(0xff707070);
    e.count       = count_of  <&ProjectDocument::plugins>;
    e.id_at       = id_of     <&ProjectDocument::plugins>;
    e.erase_by_id = erase_of  <&ProjectDocument::plugins>;
    e.menu_label  = {};   // canvas's addPluginNode launches a file chooser

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        (void) project_dir;
        project::PluginNodeSpec s;
        s.id   = id;
        s.path = juce::File(require_string(n, "path"));
        s.state_b64 = prop_string(n, "state_b64", {});
        const juce::Identifier kReceivesMidi("receives_midi");
        if (n.getDynamicObject()->hasProperty(kReceivesMidi))
        {
            auto v = n[kReceivesMidi];
            if (v.isBool() || v.isInt() || v.isInt64())
                s.receives_midi = ((bool) v);
        }
        d.plugins.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.plugins)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("path", n.path.getFullPathName());
            if (n.state_b64.isNotEmpty())
                o->setProperty("state_b64", n.state_b64);
            if (!n.receives_midi)
                o->setProperty("receives_midi", false);
            push(o, "plugin", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.plugins[(size_t) i];
        // Instruments (probed_in_channels == 0) have no audio input
        // port. -1 means unknown; assume effect (1 in port) until
        // re-probed.
        const int in_ports = (s.probed_in_channels == 0) ? 0 : 1;
        return CanvasNodeInfo{
            s.id + "\n" + s.path.getFileNameWithoutExtension(),
            in_ports, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool out) {
        const auto& s = d.plugins[(size_t) i];
        return out ? s.probed_out_channels : s.probed_in_channels;
    };

    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Plugin node (path read-only; delete + re-add to change)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.plugins[(size_t) i];
        aw.addTextEditor("path", s.path.getFullPathName(), "plugin path");
        aw.getTextEditor("path")->setReadOnly(true);
        aw.addTextEditor("state_b64_size",
                         juce::String((int) s.state_b64.length()),
                         "state_b64 length");
        aw.getTextEditor("state_b64_size")->setReadOnly(true);
        aw.addTextEditor("receives_midi", s.receives_midi ? "1" : "0",
                         "receives MIDI input (1/0; legacy)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.plugins[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.receives_midi = read_int(aw, "receives_midi",
                                    s.receives_midi ? 1 : 0) != 0;
    };

    // Plugin add happens after loadProject has already opened the
    // MH_Plugin* (so it can state-load + probe channels). load_one
    // attaches the already-opened plugin to the graph.
    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject& lp) {
        const auto& s = d.plugins[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addPlugin(lp.plugins[(size_t) i]);
    };
    return e;
}

//---- mix ----
NodeKindEntry makeMix()
{
    NodeKindEntry e;
    e.kind_string = "mix";
    e.colour      = juce::Colour(0xff5c7a3a);
    e.count       = count_of  <&ProjectDocument::mixes>;
    e.id_at       = id_of     <&ProjectDocument::mixes>;
    e.erase_by_id = erase_of  <&ProjectDocument::mixes>;
    e.menu_label  = "Add Mix (2 inputs, stereo)";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        (void) project_dir;
        project::MixNodeSpec s;
        s.id         = id;
        s.num_inputs = require_int(n, "num_inputs");
        s.channels   = require_int(n, "channels");
        s.gains.assign((size_t) s.num_inputs, 1.0f);
        const juce::Identifier kGains("gains");
        if (n.getDynamicObject()->hasProperty(kGains))
        {
            auto v = n[kGains];
            if (v.isArray())
            {
                const auto& arr = *v.getArray();
                if ((int) arr.size() != s.num_inputs)
                    throw project::ProjectError(
                        "gains length does not match num_inputs");
                for (int i = 0; i < s.num_inputs; ++i)
                    s.gains[(size_t) i] = (float) (double) arr[i];
            }
        }
        d.mixes.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.mixes)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("num_inputs", n.num_inputs);
            o->setProperty("channels",   n.channels);
            juce::Array<juce::var> gains;
            for (float g : n.gains) gains.add((double) g);
            o->setProperty("gains", gains);
            push(o, "mix", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.mixes[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(mix " + juce::String(s.num_inputs)
              + " x " + juce::String(s.channels) + "ch)",
            s.num_inputs, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.mixes[(size_t) i].channels;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::MixNodeSpec s;
        s.id         = uniqueId(d, "mix");
        s.num_inputs = 2;
        s.channels   = 2;
        s.gains.assign(2, 1.0f);
        d.mixes.push_back(std::move(s));
    };

    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Mix node");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.mixes[(size_t) i];
        aw.addTextEditor("num_inputs", juce::String(s.num_inputs), "num_inputs");
        aw.addTextEditor("channels",   juce::String(s.channels),   "channels");
        juce::String gain_csv;
        for (size_t k = 0; k < s.gains.size(); ++k)
        {
            if (k) gain_csv += ",";
            gain_csv += juce::String(s.gains[k]);
        }
        aw.addTextEditor("gains", gain_csv, "gains (comma-separated)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.mixes[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        const int new_n = std::max(1, read_int(aw, "num_inputs", s.num_inputs));
        s.channels      = std::max(1, read_int(aw, "channels",   s.channels));
        juce::StringArray gain_strs;
        gain_strs.addTokens(aw.getTextEditorContents("gains"), ",", "");
        std::vector<float> new_gains((size_t) new_n, 1.0f);
        for (int k = 0; k < std::min(new_n, gain_strs.size()); ++k)
            new_gains[(size_t) k] = gain_strs[k].trim().getFloatValue();
        s.gains = std::move(new_gains);
        if (new_n < s.num_inputs)
        {
            // Drop edges into removed mix input ports.
            const juce::String mix_id = s.id;
            d.edges.erase(std::remove_if(d.edges.begin(), d.edges.end(),
                [&](const project::EdgeSpec& e2) {
                    return e2.dst == mix_id && e2.dst_port >= new_n
                        && e2.kind == project::EdgeKind::Audio;
                }), d.edges.end());
        }
        s.num_inputs = new_n;
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.mixes[(size_t) i];
        auto nid = g.addMix(s.num_inputs, s.channels);
        for (int k = 0; k < s.num_inputs; ++k)
            g.setMixGain(nid, k, s.gains[(size_t) k]);
        id_to_node[s.id.toStdString()] = nid;
    };
    return e;
}

//---- output ----
NodeKindEntry makeOutput()
{
    NodeKindEntry e;
    e.kind_string = "output";
    e.colour      = juce::Colour(0xff8e4a3a);
    e.count       = count_of  <&ProjectDocument::outputs>;
    e.id_at       = id_of     <&ProjectDocument::outputs>;
    e.erase_by_id = erase_of  <&ProjectDocument::outputs>;
    e.menu_label  = {};  // file chooser

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::OutputNodeSpec s;
        s.id        = id;
        s.channels  = require_int(n, "channels");
        s.sink      = project_dir.getChildFile(require_string(n, "sink"));
        s.bit_depth = prop_int(n, "bit_depth", 24);
        d.outputs.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.outputs)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels",  n.channels);
            o->setProperty("sink",      n.sink.getFullPathName());
            o->setProperty("bit_depth", n.bit_depth);
            push(o, "output", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.outputs[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(output, " + juce::String(s.channels) + "ch)",
            1, 0
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.outputs[(size_t) i].channels;
    };

    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Output node");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.outputs[(size_t) i];
        aw.addTextEditor("channels",  juce::String(s.channels),  "channels");
        aw.addTextEditor("bit_depth", juce::String(s.bit_depth), "bit depth (16/24/32)");
        aw.addTextEditor("sink",      s.sink.getFullPathName(),  "sink path");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.outputs[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels  = read_int(aw, "channels",  s.channels);
        s.bit_depth = read_int(aw, "bit_depth", s.bit_depth);
        s.sink      = juce::File(aw.getTextEditorContents("sink"));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.outputs[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addOutput(s.channels);
    };
    return e;
}

//============================================================
// MIDI source kinds
//============================================================

NodeKindEntry makeMidiInput()
{
    NodeKindEntry e;
    e.kind_string = "midi_input";
    e.colour      = juce::Colour(0xff7a4ea0);
    e.count       = count_of  <&ProjectDocument::midi_inputs>;
    e.id_at       = id_of     <&ProjectDocument::midi_inputs>;
    e.erase_by_id = erase_of  <&ProjectDocument::midi_inputs>;
    e.is_midi_source = true;
    e.menu_label  = "Add MIDI Input";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MidiInputNodeSpec s;
        s.id        = id;
        s.port_name = prop_string(n, "port_name", {});
        d.midi_inputs.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.midi_inputs)
        {
            auto* o = new juce::DynamicObject();
            if (n.port_name.isNotEmpty()) o->setProperty("port_name", n.port_name);
            push(o, "midi_input", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.midi_inputs[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(midi in"
              + (s.port_name.isNotEmpty()
                  ? juce::String(", ") + s.port_name : juce::String())
              + ")",
            0, 1
        };
    };
    e.channels_for = [](const ProjectDocument&, int, bool){ return -1; };

    e.menu_add = [](ProjectDocument& d) {
        project::MidiInputNodeSpec s;
        s.id = uniqueId(d, "mi");
        d.midi_inputs.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("MIDI input node");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.midi_inputs[(size_t) i];
        aw.addTextEditor("port_name", s.port_name,
                         "MIDI port name (empty = engine default)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.midi_inputs[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.port_name = aw.getTextEditorContents("port_name").trim();
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject& lp) {
        const auto& s = d.midi_inputs[(size_t) i];
        const auto nid = g.addMidiInput();
        id_to_node[s.id.toStdString()] = nid;
        lp.midi_input_node_ids.push_back(nid);
    };
    return e;
}

NodeKindEntry makeMidiOutput()
{
    NodeKindEntry e;
    e.kind_string = "midi_output";
    e.colour      = juce::Colour(0xffa05a99);
    e.count       = count_of  <&ProjectDocument::midi_outputs>;
    e.id_at       = id_of     <&ProjectDocument::midi_outputs>;
    e.erase_by_id = erase_of  <&ProjectDocument::midi_outputs>;
    e.is_midi_sink = true;
    e.menu_label   = "Add MIDI Output";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MidiOutputNodeSpec s;
        s.id        = id;
        s.port_name = prop_string(n, "port_name", {});
        d.midi_outputs.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.midi_outputs)
        {
            auto* o = new juce::DynamicObject();
            if (n.port_name.isNotEmpty()) o->setProperty("port_name", n.port_name);
            push(o, "midi_output", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.midi_outputs[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(midi out"
              + (s.port_name.isNotEmpty()
                  ? juce::String(", ") + s.port_name : juce::String())
              + ")",
            1, 0
        };
    };
    e.channels_for = [](const ProjectDocument&, int, bool){ return -1; };

    e.menu_add = [](ProjectDocument& d) {
        project::MidiOutputNodeSpec s;
        s.id = uniqueId(d, "mo");
        d.midi_outputs.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("MIDI output node");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.midi_outputs[(size_t) i];
        aw.addTextEditor("port_name", s.port_name,
                         "MIDI port name (empty = no live sink)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.midi_outputs[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.port_name = aw.getTextEditorContents("port_name").trim();
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.midi_outputs[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addMidiOutput();
    };
    return e;
}

//============================================================
// Device kinds (live audio device I/O)
//============================================================

NodeKindEntry makeDeviceOutput()
{
    NodeKindEntry e;
    e.kind_string = "device_output";
    e.colour      = juce::Colour(0xffc97a2c);
    e.count       = count_of  <&ProjectDocument::device_outputs>;
    e.id_at       = id_of     <&ProjectDocument::device_outputs>;
    e.erase_by_id = erase_of  <&ProjectDocument::device_outputs>;
    e.menu_label  = "Add Device Output (speakers)";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::DeviceOutputNodeSpec s;
        s.id          = id;
        s.channels    = prop_int(n, "channels", 2);
        s.device_name = prop_string(n, "device_name", {});
        d.device_outputs.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.device_outputs)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels", n.channels);
            if (n.device_name.isNotEmpty())
                o->setProperty("device_name", n.device_name);
            push(o, "device_output", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.device_outputs[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(speakers, " + juce::String(s.channels) + "ch"
              + (s.device_name.isNotEmpty()
                  ? juce::String(", ") + s.device_name : juce::String())
              + ")",
            1, 0
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.device_outputs[(size_t) i].channels;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::DeviceOutputNodeSpec s;
        s.id       = uniqueId(d, "speakers");
        s.channels = 2;
        d.device_outputs.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Device output node (speakers)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.device_outputs[(size_t) i];
        aw.addTextEditor("channels",    juce::String(s.channels), "channels");
        aw.addTextEditor("device_name", s.device_name,
                         "device name (empty = engine default)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.device_outputs[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels    = std::max(1, read_int(aw, "channels", s.channels));
        s.device_name = aw.getTextEditorContents("device_name").trim();
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject& lp) {
        const auto& s = d.device_outputs[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addOutput(s.channels);
        lp.device_output_buffer_indices.push_back(
            (int) (d.outputs.size() + (size_t) i));
    };
    return e;
}

NodeKindEntry makeDeviceInput()
{
    NodeKindEntry e;
    e.kind_string = "device_input";
    e.colour      = juce::Colour(0xff2c8ec9);
    e.count       = count_of  <&ProjectDocument::device_inputs>;
    e.id_at       = id_of     <&ProjectDocument::device_inputs>;
    e.erase_by_id = erase_of  <&ProjectDocument::device_inputs>;
    e.menu_label  = "Add Device Input (mic/line-in)";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::DeviceInputNodeSpec s;
        s.id          = id;
        s.channels    = prop_int(n, "channels", 2);
        s.device_name = prop_string(n, "device_name", {});
        d.device_inputs.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.device_inputs)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels", n.channels);
            if (n.device_name.isNotEmpty())
                o->setProperty("device_name", n.device_name);
            push(o, "device_input", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.device_inputs[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(mic/line, " + juce::String(s.channels) + "ch"
              + (s.device_name.isNotEmpty()
                  ? juce::String(", ") + s.device_name : juce::String())
              + ")",
            0, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.device_inputs[(size_t) i].channels;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::DeviceInputNodeSpec s;
        s.id       = uniqueId(d, "mic");
        s.channels = 2;
        d.device_inputs.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Device input node (mic/line-in)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.device_inputs[(size_t) i];
        aw.addTextEditor("channels",    juce::String(s.channels), "channels");
        aw.addTextEditor("device_name", s.device_name,
                         "device name (empty = engine default)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.device_inputs[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels    = std::max(1, read_int(aw, "channels", s.channels));
        s.device_name = aw.getTextEditorContents("device_name").trim();
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject& lp) {
        const auto& s = d.device_inputs[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addInput(s.channels);
        lp.device_input_buffer_indices.push_back(
            (int) (d.inputs.size() + (size_t) i));
    };
    return e;
}

//---- meter ----
NodeKindEntry makeMeter()
{
    NodeKindEntry e;
    e.kind_string = "meter";
    e.colour      = juce::Colour(0xff444a55);
    e.count       = count_of  <&ProjectDocument::meters>;
    e.id_at       = id_of     <&ProjectDocument::meters>;
    e.erase_by_id = erase_of  <&ProjectDocument::meters>;
    e.menu_label  = "Add Meter";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MeterNodeSpec s;
        s.id       = id;
        s.channels = prop_int(n, "channels", 2);
        d.meters.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.meters) {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels", n.channels);
            push(o, "meter", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.meters[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(meter, " + juce::String(s.channels) + "ch)",
            1, 0
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.meters[(size_t) i].channels;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::MeterNodeSpec s;
        s.id = uniqueId(d, "meter");
        s.channels = 2;
        d.meters.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Meter node (visual peak)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.meters[(size_t) i];
        aw.addTextEditor("channels", juce::String(s.channels), "channels");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.meters[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels = std::max(1, read_int(aw, "channels", s.channels));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject& lp) {
        const auto& s = d.meters[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addOutput(s.channels);
        lp.meter_buffer_indices.push_back(
            (int) (d.outputs.size() + d.device_outputs.size() + (size_t) i));
        lp.meter_states.push_back(
            std::make_unique<LoadedProject::MeterState>(s.channels));
    };
    return e;
}

//============================================================
// Routing kinds: gain, bus, pick_channel, merge_channels
//============================================================

NodeKindEntry makeGain()
{
    NodeKindEntry e;
    e.kind_string = "gain";
    e.colour      = juce::Colour(0xff5c7a99);
    e.count       = count_of  <&ProjectDocument::gains>;
    e.id_at       = id_of     <&ProjectDocument::gains>;
    e.erase_by_id = erase_of  <&ProjectDocument::gains>;
    e.menu_label  = "Add Gain (stereo)";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::GainNodeSpec s;
        s.id       = id;
        s.channels = prop_int   (n, "channels", 2);
        s.gain     = (float) prop_double(n, "gain", 1.0);
        d.gains.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.gains) {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels", n.channels);
            o->setProperty("gain",     (double) n.gain);
            push(o, "gain", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.gains[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(gain " + juce::String(s.gain, 2)
              + ", " + juce::String(s.channels) + "ch)",
            1, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.gains[(size_t) i].channels;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::GainNodeSpec s;
        s.id = uniqueId(d, "gain");
        s.channels = 2; s.gain = 1.0f;
        d.gains.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Gain node (fader)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.gains[(size_t) i];
        aw.addTextEditor("channels", juce::String(s.channels), "channels");
        aw.addTextEditor("gain",     juce::String(s.gain, 4),  "linear gain (1.0 = unity)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.gains[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels = std::max(1, read_int  (aw, "channels", s.channels));
        s.gain     =           read_float(aw, "gain",     s.gain);
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.gains[(size_t) i];
        auto nid = g.addMix(1, s.channels);
        g.setMixGain(nid, 0, s.gain);
        id_to_node[s.id.toStdString()] = nid;
    };
    return e;
}

NodeKindEntry makeBus()
{
    NodeKindEntry e;
    e.kind_string = "bus";
    e.colour      = juce::Colour(0xff6a6a6a);
    e.count       = count_of  <&ProjectDocument::buses>;
    e.id_at       = id_of     <&ProjectDocument::buses>;
    e.erase_by_id = erase_of  <&ProjectDocument::buses>;
    e.menu_label  = "Add Bus (stereo)";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::BusNodeSpec s;
        s.id       = id;
        s.channels = prop_int(n, "channels", 2);
        d.buses.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.buses) {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels", n.channels);
            push(o, "bus", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.buses[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(bus, " + juce::String(s.channels) + "ch)",
            1, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.buses[(size_t) i].channels;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::BusNodeSpec s;
        s.id = uniqueId(d, "bus");
        s.channels = 2;
        d.buses.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Bus node (labeled passthrough)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.buses[(size_t) i];
        aw.addTextEditor("channels", juce::String(s.channels), "channels");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.buses[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels = std::max(1, read_int(aw, "channels", s.channels));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.buses[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addMix(1, s.channels);
    };
    return e;
}

NodeKindEntry makePickChannel()
{
    NodeKindEntry e;
    e.kind_string = "pick_channel";
    e.colour      = juce::Colour(0xff8a6f3a);
    e.count       = count_of  <&ProjectDocument::pick_channels>;
    e.id_at       = id_of     <&ProjectDocument::pick_channels>;
    e.erase_by_id = erase_of  <&ProjectDocument::pick_channels>;
    e.menu_label  = {};  // exposed via "Add Channel Split (stereo)" elsewhere

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::PickChannelNodeSpec s;
        s.id            = id;
        s.in_channels   = prop_int(n, "in_channels",   2);
        s.channel_index = prop_int(n, "channel_index", 0);
        d.pick_channels.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.pick_channels) {
            auto* o = new juce::DynamicObject();
            o->setProperty("in_channels",   n.in_channels);
            o->setProperty("channel_index", n.channel_index);
            push(o, "pick_channel", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.pick_channels[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(pick ch " + juce::String(s.channel_index)
              + "/" + juce::String(s.in_channels) + ")",
            1, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool out) {
        const auto& s = d.pick_channels[(size_t) i];
        return out ? 1 : s.in_channels;
    };

    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Pick channel node");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.pick_channels[(size_t) i];
        aw.addTextEditor("in_channels",   juce::String(s.in_channels),   "input channel count");
        aw.addTextEditor("channel_index", juce::String(s.channel_index), "0-based channel to extract");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.pick_channels[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.in_channels   = std::max(1, read_int(aw, "in_channels", s.in_channels));
        s.channel_index = std::max(0, std::min(s.in_channels - 1,
                                    read_int(aw, "channel_index", s.channel_index)));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.pick_channels[(size_t) i];
        char err[256] = {0};
        auto nid = mh_graph_v2_add_pick_channel(
            g.handle(), s.in_channels, s.channel_index, err, sizeof(err));
        if (nid < 0)
            throw project::ProjectError(("pick_channel " + s.id + ": "
                + juce::String(static_cast<const char*>(err))).toStdString());
        id_to_node[s.id.toStdString()] = nid;
    };
    return e;
}

NodeKindEntry makeMergeChannels()
{
    NodeKindEntry e;
    e.kind_string = "merge_channels";
    e.colour      = juce::Colour(0xff8a3a6f);
    e.count       = count_of  <&ProjectDocument::merge_channels>;
    e.id_at       = id_of     <&ProjectDocument::merge_channels>;
    e.erase_by_id = erase_of  <&ProjectDocument::merge_channels>;
    e.menu_label  = "Add Channel Merge (2 mono -> stereo)";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MergeChannelsNodeSpec s;
        s.id           = id;
        s.out_channels = prop_int(n, "out_channels", 2);
        d.merge_channels.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.merge_channels) {
            auto* o = new juce::DynamicObject();
            o->setProperty("out_channels", n.out_channels);
            push(o, "merge_channels", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.merge_channels[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(merge -> " + juce::String(s.out_channels) + "ch)",
            s.out_channels, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool out) {
        const auto& s = d.merge_channels[(size_t) i];
        return out ? s.out_channels : 1;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::MergeChannelsNodeSpec s;
        s.id = uniqueId(d, "merge");
        s.out_channels = 2;
        d.merge_channels.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Merge channels node");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.merge_channels[(size_t) i];
        aw.addTextEditor("out_channels", juce::String(s.out_channels),
                         "output channel count (number of input ports)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.merge_channels[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.out_channels = std::max(1, read_int(aw, "out_channels", s.out_channels));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.merge_channels[(size_t) i];
        char err[256] = {0};
        auto nid = mh_graph_v2_add_merge_channels(
            g.handle(), s.out_channels, err, sizeof(err));
        if (nid < 0)
            throw project::ProjectError(("merge_channels " + s.id + ": "
                + juce::String(static_cast<const char*>(err))).toStdString());
        id_to_node[s.id.toStdString()] = nid;
    };
    return e;
}

//============================================================
// Transport-driven kinds: metronome, midi_clock
//============================================================

NodeKindEntry makeMetronome()
{
    NodeKindEntry e;
    e.kind_string = "metronome";
    e.colour      = juce::Colour(0xff3a8aa0);
    e.count       = count_of  <&ProjectDocument::metronomes>;
    e.id_at       = id_of     <&ProjectDocument::metronomes>;
    e.erase_by_id = erase_of  <&ProjectDocument::metronomes>;
    e.menu_label  = "Add Metronome";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MetronomeNodeSpec s;
        s.id       = id;
        s.channels = prop_int(n, "channels", s.channels);
        s.gain     = (float) prop_double(n, "gain",     s.gain);
        s.freq_hz  = (float) prop_double(n, "freq_hz",  s.freq_hz);
        s.decay_ms = (float) prop_double(n, "decay_ms", s.decay_ms);
        d.metronomes.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.metronomes) {
            auto* o = new juce::DynamicObject();
            o->setProperty("channels", n.channels);
            o->setProperty("gain",     (double) n.gain);
            o->setProperty("freq_hz",  (double) n.freq_hz);
            o->setProperty("decay_ms", (double) n.decay_ms);
            push(o, "metronome", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.metronomes[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(metronome, " + juce::String(s.channels) + "ch)",
            0, 1
        };
    };
    e.channels_for = [](const ProjectDocument& d, int i, bool) {
        return d.metronomes[(size_t) i].channels;
    };

    e.menu_add = [](ProjectDocument& d) {
        project::MetronomeNodeSpec s;
        s.id = uniqueId(d, "metronome");
        d.metronomes.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("Metronome (audio click on transport beat)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.metronomes[(size_t) i];
        aw.addTextEditor("channels", juce::String(s.channels),    "channels");
        aw.addTextEditor("gain",     juce::String(s.gain, 4),     "linear gain");
        aw.addTextEditor("freq_hz",  juce::String(s.freq_hz, 2),  "click frequency (Hz)");
        aw.addTextEditor("decay_ms", juce::String(s.decay_ms, 2), "click decay (ms)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.metronomes[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.channels = std::max(1, read_int(aw, "channels", s.channels));
        s.gain     = read_float(aw, "gain",     s.gain);
        s.freq_hz  = read_float(aw, "freq_hz",  s.freq_hz);
        s.decay_ms = read_float(aw, "decay_ms", s.decay_ms);
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject& lp) {
        const auto& s = d.metronomes[(size_t) i];
        id_to_node[s.id.toStdString()] = g.addInput(s.channels);
        lp.metronome_buffer_indices.push_back(
            (int) (d.inputs.size() + d.device_inputs.size() + (size_t) i));
        lp.metronome_states.emplace_back();
    };
    return e;
}

NodeKindEntry makeMidiClock()
{
    NodeKindEntry e;
    e.kind_string = "midi_clock";
    e.colour      = juce::Colour(0xffa05a3a);
    e.count       = count_of  <&ProjectDocument::midi_clocks>;
    e.id_at       = id_of     <&ProjectDocument::midi_clocks>;
    e.erase_by_id = erase_of  <&ProjectDocument::midi_clocks>;
    e.is_midi_source = true;
    e.menu_label  = "Add MIDI Clock";

    e.parse = [](const juce::var&, const juce::String& id,
                 ProjectDocument& d, const juce::File&) {
        project::MidiClockNodeSpec s;
        s.id = id;
        d.midi_clocks.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.midi_clocks) {
            auto* o = new juce::DynamicObject();
            push(o, "midi_clock", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.midi_clocks[(size_t) i];
        return CanvasNodeInfo{ s.id + "\n(midi clock)", 0, 1 };
    };
    e.channels_for = [](const ProjectDocument&, int, bool){ return -1; };

    e.menu_add = [](ProjectDocument& d) {
        project::MidiClockNodeSpec s;
        s.id = uniqueId(d, "clock");
        d.midi_clocks.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("MIDI clock (24 PPQN, start/stop on transport)");
    };
    e.dialog_emit = [](const ProjectDocument&, int, juce::AlertWindow&){};
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow&,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.midi_clocks[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject& lp) {
        const auto& s = d.midi_clocks[(size_t) i];
        const auto nid = g.addMidiInput();
        id_to_node[s.id.toStdString()] = nid;
        lp.midi_clock_node_ids.push_back(nid);
        lp.midi_clock_event_buffers.emplace_back();
        lp.midi_clock_event_buffers.back().reserve(64);
        lp.midi_clock_states.emplace_back();
    };
    return e;
}

//============================================================
// MIDI processor kinds
//============================================================

// Shared loader for the three MIDI_PROCESSOR variants.
static MH_NodeId addProcessor(minihost::GraphV2& g, const juce::String& id,
                              MH_MidiProcessorParams params)
{
    char err[256] = {0};
    auto nid = mh_graph_v2_add_midi_processor(
        g.handle(), params, err, sizeof(err));
    if (nid < 0)
        throw project::ProjectError(("midi processor " + id + ": "
            + juce::String(static_cast<const char*>(err))).toStdString());
    return nid;
}

NodeKindEntry makeMidiFilter()
{
    NodeKindEntry e;
    e.kind_string = "midi_filter";
    e.colour      = juce::Colour(0xff5c4a7a);
    e.count       = count_of  <&ProjectDocument::midi_filters>;
    e.id_at       = id_of     <&ProjectDocument::midi_filters>;
    e.erase_by_id = erase_of  <&ProjectDocument::midi_filters>;
    e.is_midi_source = true;
    e.is_midi_sink   = true;
    e.menu_label  = "Add MIDI Filter";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MidiFilterNodeSpec s;
        s.id           = id;
        s.min_note     = prop_int(n, "min_note",     s.min_note);
        s.max_note     = prop_int(n, "max_note",     s.max_note);
        s.channel_mask = prop_int(n, "channel_mask", s.channel_mask);
        d.midi_filters.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.midi_filters) {
            auto* o = new juce::DynamicObject();
            o->setProperty("min_note",     n.min_note);
            o->setProperty("max_note",     n.max_note);
            o->setProperty("channel_mask", n.channel_mask);
            push(o, "midi_filter", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.midi_filters[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(filter " + juce::String(s.min_note) + "-"
              + juce::String(s.max_note) + ")",
            1, 1
        };
    };
    e.channels_for = [](const ProjectDocument&, int, bool){ return -1; };

    e.menu_add = [](ProjectDocument& d) {
        project::MidiFilterNodeSpec s;
        s.id = uniqueId(d, "mfilter");
        d.midi_filters.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("MIDI filter (channel mask + note range)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.midi_filters[(size_t) i];
        aw.addTextEditor("min_note",     juce::String(s.min_note),     "min note (0-127)");
        aw.addTextEditor("max_note",     juce::String(s.max_note),     "max note (0-127)");
        aw.addTextEditor("channel_mask", juce::String(s.channel_mask), "channel mask");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.midi_filters[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.min_note     = std::clamp(read_int(aw, "min_note",     s.min_note), 0, 127);
        s.max_note     = std::clamp(read_int(aw, "max_note",     s.max_note), 0, 127);
        s.channel_mask = read_int(aw, "channel_mask", s.channel_mask);
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.midi_filters[(size_t) i];
        MH_MidiProcessorParams p{};
        p.op           = MH_MIDI_OP_FILTER;
        p.min_note     = s.min_note;
        p.max_note     = s.max_note;
        p.channel_mask = s.channel_mask;
        id_to_node[s.id.toStdString()] = addProcessor(g, s.id, p);
    };
    return e;
}

NodeKindEntry makeMidiTranspose()
{
    NodeKindEntry e;
    e.kind_string = "midi_transpose";
    e.colour      = juce::Colour(0xff5c5a7a);
    e.count       = count_of  <&ProjectDocument::midi_transposes>;
    e.id_at       = id_of     <&ProjectDocument::midi_transposes>;
    e.erase_by_id = erase_of  <&ProjectDocument::midi_transposes>;
    e.is_midi_source = true;
    e.is_midi_sink   = true;
    e.menu_label  = "Add MIDI Transpose";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MidiTransposeNodeSpec s;
        s.id        = id;
        s.semitones = prop_int(n, "semitones", 0);
        d.midi_transposes.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.midi_transposes) {
            auto* o = new juce::DynamicObject();
            o->setProperty("semitones", n.semitones);
            push(o, "midi_transpose", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.midi_transposes[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(transpose " + juce::String(s.semitones) + ")",
            1, 1
        };
    };
    e.channels_for = [](const ProjectDocument&, int, bool){ return -1; };

    e.menu_add = [](ProjectDocument& d) {
        project::MidiTransposeNodeSpec s;
        s.id = uniqueId(d, "mtrans");
        d.midi_transposes.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("MIDI transpose (note delta)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.midi_transposes[(size_t) i];
        aw.addTextEditor("semitones", juce::String(s.semitones), "semitones (signed)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.midi_transposes[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.semitones = read_int(aw, "semitones", s.semitones);
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.midi_transposes[(size_t) i];
        MH_MidiProcessorParams p{};
        p.op                  = MH_MIDI_OP_TRANSPOSE;
        p.transpose_semitones = s.semitones;
        id_to_node[s.id.toStdString()] = addProcessor(g, s.id, p);
    };
    return e;
}

NodeKindEntry makeMidiVelocityCurve()
{
    NodeKindEntry e;
    e.kind_string = "midi_velocity_curve";
    e.colour      = juce::Colour(0xff5c6a7a);
    e.count       = count_of  <&ProjectDocument::midi_velocity_curves>;
    e.id_at       = id_of     <&ProjectDocument::midi_velocity_curves>;
    e.erase_by_id = erase_of  <&ProjectDocument::midi_velocity_curves>;
    e.is_midi_source = true;
    e.is_midi_sink   = true;
    e.menu_label  = "Add MIDI Velocity Curve";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MidiVelocityCurveNodeSpec s;
        s.id    = id;
        s.gamma = (float) prop_double(n, "gamma", 1.0);
        d.midi_velocity_curves.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.midi_velocity_curves) {
            auto* o = new juce::DynamicObject();
            o->setProperty("gamma", (double) n.gamma);
            push(o, "midi_velocity_curve", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.midi_velocity_curves[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(vel gamma " + juce::String(s.gamma, 2) + ")",
            1, 1
        };
    };
    e.channels_for = [](const ProjectDocument&, int, bool){ return -1; };

    e.menu_add = [](ProjectDocument& d) {
        project::MidiVelocityCurveNodeSpec s;
        s.id = uniqueId(d, "mvel");
        d.midi_velocity_curves.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("MIDI velocity curve (gamma)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.midi_velocity_curves[(size_t) i];
        aw.addTextEditor("gamma", juce::String(s.gamma, 4),
                         "gamma (1.0 = identity, <1 = compress)");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.midi_velocity_curves[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.gamma = std::max(0.01f, read_float(aw, "gamma", s.gamma));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.midi_velocity_curves[(size_t) i];
        MH_MidiProcessorParams p{};
        p.op             = MH_MIDI_OP_VELOCITY_CURVE;
        p.velocity_gamma = s.gamma;
        id_to_node[s.id.toStdString()] = addProcessor(g, s.id, p);
    };
    return e;
}

NodeKindEntry makeMidiMerge()
{
    NodeKindEntry e;
    e.kind_string = "midi_merge";
    e.colour      = juce::Colour(0xff7a5a99);
    e.count       = count_of  <&ProjectDocument::midi_merges>;
    e.id_at       = id_of     <&ProjectDocument::midi_merges>;
    e.erase_by_id = erase_of  <&ProjectDocument::midi_merges>;
    e.is_midi_source = true;
    e.is_midi_sink   = true;
    e.menu_label  = "Add MIDI Merge (2 in)";

    e.parse = [](const juce::var& n, const juce::String& id,
                 ProjectDocument& d, const juce::File& project_dir) {
        project::MidiMergeNodeSpec s;
        s.id         = id;
        s.num_inputs = prop_int(n, "num_inputs", 2);
        d.midi_merges.push_back(std::move(s));
    };
    e.serialize_all = [](const ProjectDocument& d,
                         NodeKindEntry::PushNodeFn& push) {
        for (const auto& n : d.midi_merges) {
            auto* o = new juce::DynamicObject();
            o->setProperty("num_inputs", n.num_inputs);
            push(o, "midi_merge", n.id);
        }
    };

    e.canvas_info = [](const ProjectDocument& d, int i) {
        const auto& s = d.midi_merges[(size_t) i];
        return CanvasNodeInfo{
            s.id + "\n(merge " + juce::String(s.num_inputs) + ")",
            s.num_inputs, 1
        };
    };
    e.channels_for = [](const ProjectDocument&, int, bool){ return -1; };

    e.menu_add = [](ProjectDocument& d) {
        project::MidiMergeNodeSpec s;
        s.id         = uniqueId(d, "mmerge");
        s.num_inputs = 2;
        d.midi_merges.push_back(std::move(s));
    };
    e.dialog_title = [](const ProjectDocument&, int) {
        return juce::String("MIDI merge (multi-source fan-in)");
    };
    e.dialog_emit = [](const ProjectDocument& d, int i,
                       juce::AlertWindow& aw) {
        const auto& s = d.midi_merges[(size_t) i];
        aw.addTextEditor("num_inputs", juce::String(s.num_inputs),
                         "number of MIDI input ports");
    };
    e.dialog_apply = [](ProjectDocument& d, int i, juce::AlertWindow& aw,
                        const juce::String& new_id,
                        NodeKindEntry::RenameFn& rename) {
        auto& s = d.midi_merges[(size_t) i];
        if (s.id != new_id) { rename(s.id, new_id); s.id = new_id; }
        s.num_inputs = std::max(2, read_int(aw, "num_inputs", s.num_inputs));
    };

    e.load_one = [](const ProjectDocument& d, int i,
                    minihost::GraphV2& g,
                    std::unordered_map<std::string, MH_NodeId>& id_to_node,
                    LoadedProject&) {
        const auto& s = d.midi_merges[(size_t) i];
        char err[256] = {0};
        auto nid = mh_graph_v2_add_midi_merge(
            g.handle(), s.num_inputs, err, sizeof(err));
        if (nid < 0)
            throw project::ProjectError(("midi_merge " + s.id + ": "
                + juce::String(static_cast<const char*>(err))).toStdString());
        id_to_node[s.id.toStdString()] = nid;
    };
    return e;
}

} // namespace

//============================================================
// Registry
//
// The order of this list defines the canonical iteration order
// shared by parse, serialize, rebuildLayout, mapCanvasIndex, etc.
//
// Notes on ordering:
//   - Audio inputs (input, device_input, metronome) come before
//     plugins so input_buffers[] is laid out predictably.
//   - Audio outputs (output, device_output, meter) come after
//     plugins for the same reason on output_buffers[].
//   - The relative order of MIDI / routing / processor kinds is
//     arbitrary, but is canonicalized here so save / load
//     round-trips are byte-stable.
//============================================================

const std::vector<NodeKindEntry>& nodeRegistry()
{
    static const std::vector<NodeKindEntry> kRegistry = [] {
        std::vector<NodeKindEntry> v;
        v.push_back(makeInput());
        v.push_back(makePlugin());
        v.push_back(makeMix());
        v.push_back(makeOutput());
        v.push_back(makeMidiInput());
        v.push_back(makeMidiOutput());
        v.push_back(makeDeviceOutput());
        v.push_back(makeDeviceInput());
        v.push_back(makeMeter());
        v.push_back(makeGain());
        v.push_back(makeBus());
        v.push_back(makePickChannel());
        v.push_back(makeMergeChannels());
        v.push_back(makeMetronome());
        v.push_back(makeMidiClock());
        v.push_back(makeMidiFilter());
        v.push_back(makeMidiTranspose());
        v.push_back(makeMidiVelocityCurve());
        v.push_back(makeMidiMerge());
        return v;
    }();
    return kRegistry;
}

const NodeKindEntry* findKind(const juce::String& kind_string)
{
    for (const auto& e : nodeRegistry())
        if (e.kind_string == kind_string) return &e;
    return nullptr;
}

int totalNodeCount(const project::ProjectDocument& doc)
{
    int total = 0;
    for (const auto& e : nodeRegistry())
        total += e.count(doc);
    return total;
}

CanvasNodeLookup mapCanvasIndex(const project::ProjectDocument& doc,
                                int canvas_node_index)
{
    int cursor = 0;
    for (const auto& e : nodeRegistry())
    {
        const int n = e.count(doc);
        if (canvas_node_index < cursor + n)
            return { &e, canvas_node_index - cursor };
        cursor += n;
    }
    return {};
}

} // namespace minihost_desktop
