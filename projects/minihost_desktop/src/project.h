// project.h
//
// C++ port of src/minihost/project.py. Same JSON schema (v1). Parses
// a project file, opens plugins, reads input WAVs, builds a compiled
// minihost::PluginGraph, renders block-by-block, writes output WAVs.
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

#include <cmath>
#include <cstdint>
#include <cstring>

namespace minihost_desktop::project {

class ProjectError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// MH_ResampleQuality for "best", "medium" or "fastest"; -1 for anything else.
int resampleQualityFromName(const juce::String& name);

// A pre-recorded audio file (WAV/FLAC/MP3/Vorbis) read at project
// load time and fed into the graph per-block, both in file rendering
// and from the transport position during live playback.
struct InputNodeSpec {
    juce::String id;
    int          channels = 0;
    juce::File   source;     // resolved against project_dir
    // When true, a file whose sample rate differs from the project rate
    // is resampled to the project rate at load (shared mh_audio_resample).
    // When false (default) a mismatch is an error -- the renderer is
    // otherwise strict about input rates. Mirrors _InputNode.resample in
    // src/minihost/project.py so the two loaders stay in parity.
    bool         resample = false;
    // "best", "medium" or "fastest": the converter when resample is on.
    // Mirrors _InputNode.resample_quality.
    juce::String resample_quality = "best";
};

// A file-sink: receives audio from the graph and writes it to disk
// (WAV/FLAC) when the project is rendered, or while the live transport
// plays. Bit depth is per-output (16, 24, or 32 = float). It does NOT
// route to the live audio device; use DeviceOutputNodeSpec for that.
//
// Legacy back-compat: when a project has *no* DeviceOutputNodeSpec,
// LiveEngine falls back to routing the first OutputNodeSpec's buffer
// to the system audio device so projects authored before
// device_output existed still play. New projects should use an
// explicit DeviceOutputNodeSpec and keep OutputNodeSpec for files.
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

    // Serialized juce::PluginDescription (createXml() form), base64-encoded.
    // Set for plugins with no usable file path (AudioUnits, identified by an
    // AU id rather than a path). When present, the loader opens via
    // mh_open_desc instead of mh_open(path); path may be empty.
    juce::String descriptor;
    // Human-readable plugin name for the canvas label when there is no file
    // path to derive one from (AU nodes). Optional / cosmetic.
    juce::String display_name;
    // Cached at canvas-add time (-1 = unknown, e.g. for plugins
    // loaded from disk without a fresh probe). Used for connect-time
    // channel validation; the render-time loader re-derives these
    // from a fresh mh_open and is the authoritative source.
    int          probed_in_channels  = -1;
    int          probed_out_channels = -1;

    // Cached MIDI capability flags (best-effort, from the canvas-add
    // probe). Authoritative values come from mh_get_info at load time.
    bool         accepts_midi   = false;
    bool         produces_midi  = false;

    // DEPRECATED: legacy "fan out live MIDI to every plugin" flag from
    // the pre-MIDI-routing format. New projects use MidiInputNodeSpec
    // + MIDI edges instead. On load, this field drives a migration:
    // if a project has no MidiInputNodeSpec but some plugin has
    // receives_midi=true, a synthetic MIDI input is materialized and
    // wired to those plugins.
    bool         receives_midi  = true;
};

struct MidiInputNodeSpec {
    juce::String id;
    // Optional port name; empty means "let the LiveEngine decide"
    // (the engine's user-chosen MIDI input) during live playback.
    juce::String port_name;
    // Optional .mid file. Offline render streams it block by block,
    // mirroring the Python renderer; live mode plays it from the
    // transport position, merged with device MIDI. Empty = device only.
    juce::File   source;
};

struct MidiOutputNodeSpec {
    juce::String id;
    // Optional port name for live output. Empty means "no live MIDI
    // sink"; events are still collected for inspection.
    juce::String port_name;
};

// Routes audio to the system audio output device during live playback
// (speakers, headphones, etc.). At file-render time the node still
// receives samples, but they are discarded -- only OutputNodeSpec
// writes to disk. Channels are summed across multiple device_output
// nodes if a project has more than one.
//
// device_name is informational in v1: the LiveEngine routes to the
// AudioDeviceManager's currently selected output device regardless.
// A future extension may use the name to pick a specific device.
struct DeviceOutputNodeSpec {
    juce::String id;
    int          channels    = 2;
    juce::String device_name;     // empty = engine's current device
};

// Routes the system audio input device (microphone, line-in, audio
// interface input) into the graph during live playback. Symmetric
// with DeviceOutputNodeSpec.
//
// At file-render time the buffers are silence-filled -- live audio
// input is a live-only concept. device_name is informational in v1:
// the LiveEngine reads from the AudioDeviceManager's currently
// selected input device regardless.
struct DeviceInputNodeSpec {
    juce::String id;
    int          channels    = 2;
    juce::String device_name;     // empty = engine's current device
};

// A read-only audio sink that captures per-channel peak level each
// block for canvas visualization. Graph-wise it's a regular audio
// output (the same shape as OutputNodeSpec / DeviceOutputNodeSpec);
// its samples are computed by the graph but never written to disk
// and never routed to the audio device -- only the peak values are
// surfaced via MeterState (see LoadedProject below).
struct MeterNodeSpec {
    juce::String id;
    int          channels = 2;
};

// Single-input, single-output linear gain stage. Maps to a
// 1-input mix node at the graph level; the project-format kind
// exists for clarity (a fader / monitor knob isn't a mix).
struct GainNodeSpec {
    juce::String id;
    int          channels = 2;
    float        gain     = 1.0f;
};

// N-channel labeled passthrough. Pure cosmetic / readability:
// graph-wise it's identical to a unity-gain GainNodeSpec, but the
// project format keeps it distinct so the canvas can present it
// differently (no gain knob in the UI).
struct BusNodeSpec {
    juce::String id;
    int          channels = 2;
};

// Extracts a single channel from an `in_channels` source into a
// 1-channel output. Useful for routing one side of a stereo source
// into a mono-only plugin without an upstream channel reducer.
struct PickChannelNodeSpec {
    juce::String id;
    int          in_channels   = 2;
    int          channel_index = 0;
};

// Interleaves `out_channels` separate 1-channel inputs into one
// `out_channels`-channel output. Inverse of pick_channel: lets a
// pair of mono signal paths reconverge into a stereo output.
struct MergeChannelsNodeSpec {
    juce::String id;
    int          out_channels = 2;
};

// Emits an audio click on each beat driven by LiveEngine's transport
// (bpm + playing state + transport position). Behaves as an audio
// source: graph-wise it's an MH_NODE_INPUT and LiveEngine fills its
// buffer each block. Silent during file rendering (file render has
// no transport tick).
//
// freq_hz / decay_ms control the click tone: a freq_hz sine windowed
// by an exponential envelope with the specified half-life. Defaults
// give a short bright click suitable for monitoring.
struct MetronomeNodeSpec {
    juce::String id;
    int          channels  = 1;
    float        gain      = 0.5f;
    float        freq_hz   = 1000.0f;
    float        decay_ms  = 20.0f;
};

// Emits MIDI Clock (0xF8) at 24 PPQN plus Start (0xFA) / Stop (0xFC)
// on transport edges. Graph-wise it's an MH_NODE_MIDI_INPUT;
// LiveEngine stages events each block. Connect to a MIDI output
// node to drive external gear, or to plugins that consume MIDI
// clock. Silent during file rendering.
struct MidiClockNodeSpec {
    juce::String id;
};

// Generative step sequencer: a MIDI source that plays a random melody in
// a scale on the transport's step grid. Every note is a pure function of
// these settings and the step index (see sequencer.h), so live playback,
// loops and offline renders produce the same notes. Trivially copyable so
// LiveEngine can take edits on the audio thread.
enum class Scale : int {
    Major = 0, Minor, Dorian, MajorPentatonic, MinorPentatonic, Chromatic
};

struct SequencerParams {
    int      steps    = 16;     // pattern length; it repeats unless mutated
    double   rate     = 0.25;   // step length in beats (0.25 = 1/16)
    int      root     = 48;     // MIDI note of scale degree 0
    Scale    scale    = Scale::Minor;
    int      octaves  = 2;      // pitch range above root
    double   density  = 0.7;    // probability a step plays
    double   gate     = 0.5;    // note length as a fraction of a step
    int      vel_min  = 70;
    int      vel_max  = 110;
    double   mutate   = 0.0;    // probability a step differs from its pattern
    uint32_t seed     = 1;
    int      channel  = 1;      // 1-16
};

inline bool operator==(const SequencerParams& a, const SequencerParams& b)
{
    return a.steps == b.steps && a.rate == b.rate && a.root == b.root
        && a.scale == b.scale && a.octaves == b.octaves && a.density == b.density
        && a.gate == b.gate && a.vel_min == b.vel_min && a.vel_max == b.vel_max
        && a.mutate == b.mutate && a.seed == b.seed && a.channel == b.channel;
}

struct SequencerNodeSpec {
    juce::String    id;
    SequencerParams params;
};

// MIDI filter: passes events whose channel is in `channel_mask` (bit
// i = channel i). Note On/Off events also require note in
// [min_note, max_note]. System messages (status >= 0xF0) always pass.
struct MidiFilterNodeSpec {
    juce::String id;
    int          min_note     = 0;
    int          max_note     = 127;
    int          channel_mask = 0xFFFF;
};

// MIDI transpose: adds `semitones` to data1 of Note On/Off events.
// Out-of-range results drop the event; other event kinds pass
// unchanged.
struct MidiTransposeNodeSpec {
    juce::String id;
    int          semitones = 0;
};

// MIDI velocity curve: remaps Note On velocity via gamma curve.
// `gamma` = 1.0 is identity; <1 compresses dynamics, >1 expands.
// vel_out = pow(vel_in/127, gamma) * 127, clamped to [1, 127].
// Note On with velocity=0 (a Note Off in MIDI's wire format) is
// preserved as-is so downstream consumers still see note-off.
struct MidiVelocityCurveNodeSpec {
    juce::String id;
    float        gamma = 1.0f;
};

// MIDI merge: fans `num_inputs` MIDI source streams into one
// downstream consumer, sorted by sample_offset (stable across
// ports). The canonical fan-in primitive for MIDI; pairs with the
// "one MIDI edge per (dst, port)" rule that the graph normally
// enforces for single-port consumers.
struct MidiMergeNodeSpec {
    juce::String id;
    int          num_inputs = 2;
};

struct MixNodeSpec {
    juce::String id;
    int                 num_inputs = 0;
    int                 channels   = 0;
    std::vector<float>  gains;
};

enum class EdgeKind {
    Audio = 0,
    Midi  = 1,
};

struct EdgeSpec {
    juce::String src;
    juce::String dst;
    int          dst_port = 0;     // ignored for Midi edges
    EdgeKind     kind     = EdgeKind::Audio;
};

struct NodePosition { float x = 0.0f; float y = 0.0f; };

struct ProjectDocument {
    int                    sample_rate = 0;
    int                    block_size  = 0;
    std::optional<double>  duration_seconds;
    // Tempo for offline renders and the initial live tempo. Unset = 120.
    std::optional<double>  bpm;
    double effectiveBpm() const { return bpm.value_or(120.0); }
    std::vector<InputNodeSpec>        inputs;
    std::vector<OutputNodeSpec>       outputs;
    std::vector<PluginNodeSpec>       plugins;
    std::vector<MixNodeSpec>          mixes;
    std::vector<MidiInputNodeSpec>    midi_inputs;
    std::vector<MidiOutputNodeSpec>   midi_outputs;
    std::vector<DeviceInputNodeSpec>  device_inputs;
    std::vector<DeviceOutputNodeSpec> device_outputs;
    std::vector<MeterNodeSpec>        meters;
    std::vector<GainNodeSpec>            gains;
    std::vector<BusNodeSpec>             buses;
    std::vector<PickChannelNodeSpec>     pick_channels;
    std::vector<MergeChannelsNodeSpec>   merge_channels;
    std::vector<MetronomeNodeSpec>       metronomes;
    std::vector<MidiClockNodeSpec>       midi_clocks;
    std::vector<SequencerNodeSpec>       sequencers;
    std::vector<MidiFilterNodeSpec>        midi_filters;
    std::vector<MidiTransposeNodeSpec>     midi_transposes;
    std::vector<MidiVelocityCurveNodeSpec> midi_velocity_curves;
    std::vector<MidiMergeNodeSpec>         midi_merges;
    std::vector<EdgeSpec>             edges;

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

    std::unique_ptr<minihost::PluginGraph> graph;
    std::vector<MH_Plugin*>            plugins;          // owned
    ProjectDocument                    doc;
    int                                duration_frames = 0;

    // Per-input-node planar audio. input_audio[i] is a flat
    // (channels * frames) float32 buffer in planar layout
    // (channel 0 first frames frames, then channel 1, ...).
    std::vector<std::vector<float>>    input_audio;
    std::vector<int>                   input_frames;

    // Graph node ids for MIDI_INPUT nodes, in the order they were
    // added (declared MidiInputNodeSpec first, then any migrated/
    // synthesized node for legacy receives_midi). LiveEngine stages
    // incoming device MIDI into every entry so a single device feeds
    // all MIDI_INPUT nodes in the project.
    std::vector<MH_NodeId>             midi_input_node_ids;

    // File-sourced MIDI inputs. One entry per midi_input node that has a
    // `source` .mid file: its spec id, graph node id and the file's events
    // flattened to absolute sample offsets (sorted). renderProject streams
    // these per block; LiveEngine plays them from the transport position.
    struct FileMidiInput {
        juce::String               id;
        MH_NodeId                  node_id;
        std::vector<MH_MidiEvent>  events;
    };
    std::vector<FileMidiInput>         file_midi_inputs;

    // Graph node ids of midi_output nodes, in doc.midi_outputs order.
    std::vector<MH_NodeId>             midi_output_node_ids;

    // For each DeviceOutputNodeSpec, the index into the graph's audio
    // output_buffers[] (i.e. its position in add-order among audio
    // output nodes -- doc.outputs come first, then device_outputs).
    // LiveEngine sums these buffers into the system audio output
    // device's channels each block. Empty for legacy projects, in
    // which case LiveEngine falls back to routing output_buffers[0].
    std::vector<int>                   device_output_buffer_indices;

    // For each DeviceInputNodeSpec, the index into the graph's audio
    // input_buffers[] (doc.inputs come first, then device_inputs).
    // LiveEngine copies live device input channels into these buffers
    // each block.
    std::vector<int>                   device_input_buffer_indices;

    // Per-meter live state: lock-free per-channel peak amplitude
    // (0.0 .. 1.0+, clip-safe display is the GUI's responsibility).
    // Written from the audio thread by updateMeters() after each
    // render_block; read from the GUI thread by the canvas's repaint
    // timer. Indexed by position in doc.meters; channels match
    // doc.meters[i].channels.
    struct MeterState {
        std::vector<std::atomic<float>> peak;
        explicit MeterState(int channels) : peak((size_t) channels)
        {
            for (auto& p : peak) p.store(0.0f, std::memory_order_relaxed);
        }
    };
    std::vector<std::unique_ptr<MeterState>> meter_states;

    // For each MeterNodeSpec, the index into the graph's audio
    // output_buffers[] (doc.outputs, then device_outputs, then
    // meters). Used by updateMeters() to find the freshly-rendered
    // samples for each meter.
    std::vector<int>                   meter_buffer_indices;

    // Computes per-channel peak across `nframes` for every meter
    // node and stores them in meter_states. out_buffers[i][c] must
    // point to `nframes` valid float samples for each meter buffer
    // index. Safe to call from the audio thread (no locks, no
    // allocations; std::atomic<float> on the target platforms here
    // is lock-free).
    void updateMeters(float* const* const* out_buffers, int nframes);

    // -- Metronome support -- //
    // For each MetronomeNodeSpec, the index into the graph's audio
    // input_buffers[] where LiveEngine should write the click
    // signal each block.
    std::vector<int>                   metronome_buffer_indices;

    // Per-metronome render state. `phase_samples` = how far into the
    // current click envelope we are (-1 = no active click).
    struct MetronomeState {
        int phase_samples = -1;
    };
    std::vector<MetronomeState>        metronome_states;

    // Fills every metronome's input buffer with the click waveform
    // for the next `nframes` starting at transport sample `pos`.
    // Writes silence when `playing` is false. `sr` is the project
    // sample rate; `bpm` is the current tempo.
    //
    // `planar_inputs[i]` is the flat (channels * block_size) float32
    // buffer for input node `i`, in planar layout (channel 0 first,
    // then channel 1, ...). `block_size` is the stride between
    // channels. Audio-thread safe (no allocations; only updates
    // internal phase counters).
    void renderMetronomes(std::vector<std::vector<float>>& planar_inputs,
                          int block_size,
                          int nframes,
                          long long pos_samples,
                          double sr, double bpm, bool playing);

    // -- MIDI clock support -- //
    // For each MidiClockNodeSpec, the graph node id of its dedicated
    // MH_NODE_MIDI_INPUT and a scratch buffer for the events that
    // LiveEngine builds per block. Pointers into these buffers are
    // staged via mh_graph_set_midi_input_events.
    std::vector<MH_NodeId>             midi_clock_node_ids;
    std::vector<std::vector<MH_MidiEvent>> midi_clock_event_buffers;

    // Per-midi-clock running state for transport-edge detection.
    struct MidiClockState {
        bool was_playing = false;
    };
    std::vector<MidiClockState>        midi_clock_states;

    // Graph MIDI_INPUT node of each SequencerNodeSpec, in doc order.
    std::vector<MH_NodeId>             sequencer_node_ids;

    // Builds the next block's MIDI clock events (Start / Stop /
    // 24-PPQN Clock ticks) per midi_clock node, stages them on the
    // graph via mh_graph_set_midi_input_events, and updates the
    // per-node "was_playing" edge-detection state.
    void stageMidiClocks(struct MH_PluginGraph* graph,
                         int nframes,
                         long long pos_samples,
                         double sr, double bpm, bool playing);
};

std::unique_ptr<LoadedProject> loadProject(const juce::File& path);

// Copies channel counts and MIDI capability from `loaded`'s opened plugins
// into `doc` plugin specs that have no probe yet (older project files).
// `loaded` must have been built from `doc`. Returns true if any changed.
bool fillMissingProbes(ProjectDocument& doc, const LoadedProject& loaded);
// Same, from an in-memory document (the canvas's working copy, which may be
// unsaved or untitled). File paths in `doc` must already be absolute.
std::unique_ptr<LoadedProject> loadProject(ProjectDocument doc);

// Read a .mid file and flatten it to absolute sample-offset MIDI events
// (sorted), using the file's tempo map at `sample_rate`. Keeps channel-voice
// messages, drops meta/sysex. Mirrors Python's midi_file_to_events. Returns
// false if the file cannot be read.
bool readMidiFileEvents(const juce::File& midi_file, double sample_rate,
                        std::vector<MH_MidiEvent>& out);

// Per-render options. Set fields to override the schema defaults.
// bit_depth_override == 0 means "use the per-output bit_depth from
// the project file"; otherwise it applies to ALL output nodes.
// normalize_dbfs == 0.0 means "no normalization"; any non-zero value
// peak-normalizes the WHOLE render to that target before writing.
// tail_seconds extends the render past the project length so effect
// tails (reverb, delay) are not cut off.
// after_block, if set, runs after each block renders, with the block's
// first frame and length (e.g. to drain midi_output nodes).
struct RenderOptions {
    int    bit_depth_override = 0;
    double normalize_dbfs     = 0.0;
    double tail_seconds       = 0.0;
    std::function<void(int, int)> after_block;
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
