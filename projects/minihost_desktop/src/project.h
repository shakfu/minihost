// project.h
//
// C++ port of src/minihost/project.py. Same JSON schema (v1). Parses
// a project file, opens plugins, reads input WAVs, builds a compiled
// minihost::GraphV2, renders block-by-block, writes output WAVs.
//
// Pure non-GUI logic so it can be unit-tested headlessly and driven
// from a worker thread inside the desktop app. The UI integration
// (file chooser, juce::ThreadWithProgressWindow) lives in main.cpp.

#pragma once

#include "minihost.h"
#include "minihost_graph_v2.hpp"

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace minihost_desktop::project {

class ProjectError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct InputNodeSpec {
    juce::String id;
    int          channels = 0;
    juce::File   source;     // resolved against project_dir
};

struct OutputNodeSpec {
    juce::String id;
    int          channels  = 0;
    juce::File   sink;
    int          bit_depth = 24;
};

struct PluginNodeSpec {
    juce::String id;
    juce::File   path;
    juce::String state_b64;  // empty if none
    // Cached at canvas-add time (-1 = unknown, e.g. for plugins
    // loaded from disk without a fresh probe). Used for connect-time
    // channel validation; the render-time loader re-derives these
    // from a fresh mh_open and is the authoritative source.
    int          probed_in_channels  = -1;
    int          probed_out_channels = -1;
    // Whether live MIDI input is fanned out to this plugin. Default
    // true (matches the v1 "all plugins receive" routing).
    bool         receives_midi       = true;
};

struct MixNodeSpec {
    juce::String id;
    int                 num_inputs = 0;
    int                 channels   = 0;
    std::vector<float>  gains;
};

struct EdgeSpec {
    juce::String src;
    juce::String dst;
    int          dst_port = 0;
};

struct NodePosition { float x = 0.0f; float y = 0.0f; };

struct ProjectDocument {
    int                    sample_rate = 0;
    int                    block_size  = 0;
    std::optional<double>  duration_seconds;
    std::vector<InputNodeSpec>  inputs;
    std::vector<OutputNodeSpec> outputs;
    std::vector<PluginNodeSpec> plugins;
    std::vector<MixNodeSpec>    mixes;
    std::vector<EdgeSpec>       edges;

    // Optional canvas layout. Keyed by node id. Auto-layout fills in
    // any node missing from this map. The canvas writes back here on
    // user drags; saveProjectFile serializes the whole map.
    std::unordered_map<std::string, NodePosition> layout;
};

ProjectDocument parseProjectFile(const juce::File& path);

// Serialize a ProjectDocument back to disk. Writes to a `.tmp` file and
// renames; the destination is only replaced once the write succeeds.
// Throws ProjectError on I/O failure.
void saveProjectFile(const juce::File& path, const ProjectDocument& doc);

// Owns the built graph, opened plugin instances, and loaded input
// audio buffers. mh_close runs for every plugin in the destructor.
struct LoadedProject {
    LoadedProject() = default;
    LoadedProject(const LoadedProject&) = delete;
    LoadedProject& operator=(const LoadedProject&) = delete;
    ~LoadedProject();

    std::unique_ptr<minihost::GraphV2> graph;
    std::vector<MH_Plugin*>            plugins;          // owned
    ProjectDocument                    doc;
    int                                duration_frames = 0;

    // Per-input-node planar audio. input_audio[i] is a flat
    // (channels * frames) float32 buffer in planar layout
    // (channel 0 first frames frames, then channel 1, ...).
    std::vector<std::vector<float>>    input_audio;
    std::vector<int>                   input_frames;
};

std::unique_ptr<LoadedProject> loadProject(const juce::File& path);

// Per-render options. Set fields to override the schema defaults.
// bit_depth_override == 0 means "use the per-output bit_depth from
// the project file"; otherwise it applies to ALL output nodes.
// normalize_dbfs == 0.0 means "no normalization"; any non-zero value
// peak-normalizes the WHOLE render to that target before writing.
struct RenderOptions {
    int    bit_depth_override = 0;
    double normalize_dbfs     = 0.0;
};

// Renders the project block-by-block. progress_callback (if non-null)
// is called from the calling thread between blocks with
// (frames_done, frames_total). The function checks cancel_flag at the
// top of each block and aborts cleanly if set.
//
// Returns true on success, false on failure (sets `error`) or
// cancellation (sets error == "cancelled").
bool renderProject(LoadedProject& proj,
                   std::atomic<bool>& cancel_flag,
                   std::function<void(int, int)> progress_callback,
                   juce::String& error,
                   const RenderOptions& options = {});

} // namespace minihost_desktop::project
