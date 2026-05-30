// node_registry.h
//
// Single-source-of-truth dispatch table for the project's node kinds.
//
// Background: minihost_desktop has ~19 distinct node kinds (input,
// output, plugin, mix, midi_input, midi_output, device_input,
// device_output, meter, gain, bus, pick_channel, merge_channels,
// metronome, midi_clock, midi_filter, midi_transpose,
// midi_velocity_curve, midi_merge). Each previously appeared in
// per-kind switch arms / iteration loops scattered across
// project.cpp, canvas.cpp, and live.cpp, making "add a new node"
// touch ~17 sites in 4 files.
//
// This registry collapses the data-driven dispatch into one
// NodeKindEntry table. Adding a new node kind now requires only
// declaring its spec struct (in project.h) + adding one entry to
// the kRegistry list (in node_registry.cpp). The renderer's typed
// access to specific vectors (doc.outputs, doc.meters, ...) is left
// alone -- that's data access, not dispatch.

#pragma once

#include "project.h"
#include "minihost_graph_v2.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <unordered_map>
#include <vector>

namespace minihost_desktop {

// Display info for one spec entry, used by the canvas to render the
// node box: label text, input port count, output port count.
struct CanvasNodeInfo {
    juce::String label;
    int          num_input_ports  = 0;
    int          num_output_ports = 0;
};

// Bundles every per-kind dispatch site into one struct. Lambdas
// hold references to the doc; callers pass the doc + the spec index
// (position within the kind's vector, e.g. doc.inputs[idx]).
struct NodeKindEntry {
    juce::String kind_string;       // JSON "kind" field
    juce::Colour colour;            // canvas node background

    // -- iteration --
    // doc.<this kind>.size().
    std::function<int(const project::ProjectDocument&)> count;
    // id of the i-th entry (well-defined for 0 <= idx < count()).
    std::function<juce::String(const project::ProjectDocument&,
                               int idx)> id_at;

    // Erase any entry with this id from doc.<this kind>. No-op if id
    // doesn't match anything here.
    std::function<void(project::ProjectDocument&,
                       const juce::String& id)> erase_by_id;

    // -- parser / serializer --
    // Parse a single node entry from JSON and append it to the doc.
    // `id` is the already-extracted "id" field.
    // `project_dir` is the directory containing the project file;
    // file-path fields are resolved relative to it.
    std::function<void(const juce::var&,
                       const juce::String& id,
                       project::ProjectDocument&,
                       const juce::File& project_dir)> parse;
    // Push every entry of this kind into the nodes array (via
    // push_node). The kind string is supplied by the caller.
    using PushNodeFn = std::function<void(juce::DynamicObject*,
                                          const juce::String& kind,
                                          const juce::String& id)>;
    std::function<void(const project::ProjectDocument&,
                       PushNodeFn&)> serialize_all;

    // -- canvas display + connection --
    std::function<CanvasNodeInfo(const project::ProjectDocument&,
                                  int idx)> canvas_info;
    // Channel count for connect-time validation. `is_output_port`:
    // true when validating the src side, false for dst. Returns -1
    // for MIDI-only kinds (channel-mismatch check skipped).
    std::function<int(const project::ProjectDocument&,
                       int idx,
                       bool is_output_port)> channels_for;
    bool is_midi_source = false;
    bool is_midi_sink   = false;

    // -- context menu --
    // Empty `menu_label` means no entry in the right-click menu
    // (typically because the kind needs a file chooser or other
    // out-of-band setup that the canvas handles directly).
    juce::String menu_label;
    std::function<void(project::ProjectDocument&)> menu_add;

    // -- property dialog --
    std::function<juce::String(const project::ProjectDocument&,
                                int idx)> dialog_title;
    std::function<void(const project::ProjectDocument&,
                       int idx,
                       juce::AlertWindow&)> dialog_emit;
    // The caller is responsible for handling id renames (rewriting
    // edges + layout map); pass that work in via `rename`.
    using RenameFn = std::function<void(const juce::String& old_id,
                                         const juce::String& new_id)>;
    std::function<void(project::ProjectDocument&,
                       int idx,
                       juce::AlertWindow&,
                       const juce::String& new_id,
                       RenameFn&)> dialog_apply;

    // -- loader --
    // Translates one spec entry into one or more graph nodes,
    // recording any side effects on LoadedProject (buffer-index
    // tables, MIDI input node lists, meter states, ...).
    std::function<void(const project::ProjectDocument&,
                       int idx,
                       minihost::GraphV2&,
                       std::unordered_map<std::string, MH_NodeId>&,
                       project::LoadedProject&)> load_one;
};

// Canonical kind order. The canvas, parser, serializer, and loader
// all walk this list in order so kind sections appear consistently.
// Inserting a new entry here is the only change required to add a
// new node kind to the desktop app.
const std::vector<NodeKindEntry>& nodeRegistry();

// Find an entry by its kind_string, or nullptr if unknown.
const NodeKindEntry* findKind(const juce::String& kind_string);

// Total number of canvas nodes across all kinds.
int totalNodeCount(const project::ProjectDocument& doc);

// Map a canvas node index (the position in CanvasComponent::nodes_)
// to its registry entry plus the spec index within that kind's
// vector. Returns {nullptr, -1} on out-of-range.
struct CanvasNodeLookup {
    const NodeKindEntry* entry      = nullptr;
    int                  spec_index = -1;
};
CanvasNodeLookup mapCanvasIndex(const project::ProjectDocument& doc,
                                int canvas_node_index);

} // namespace minihost_desktop
