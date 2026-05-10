#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/optional.h>
#include <nanobind/ndarray.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <mutex>
#include <memory>

#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "minihost.h"
#include "minihost_chain.h"
#include "minihost_audio.h"
#include "minihost_audiofile.h"
#include "minihost_midi.h"
#include "minihost_vstpreset.h"
#include "MidiFile.h"

namespace nb = nanobind;
using namespace nb::literals;

// Thin wrapper around juce::AudioBuffer<float> exposed to Python as
// minihost.AudioBuffer. The wrapper enforces the contiguous-memory
// invariant by only ever calling the (channels, frames) constructor of
// juce::AudioBuffer (never setDataToReferTo). This guarantees the data
// pointer is one big block of channels*frames floats laid out planar
// (row-major: ch0..., ch1..., ...), which is what nb::ndarray<float,
// nb::shape<-1, -1>, nb::c_contig> expects.
class MhAudioBuffer {
public:
    MhAudioBuffer(int channels, int frames)
        : buf_(juce::jmax(1, channels), juce::jmax(0, frames))
    {
        // juce::AudioBuffer leaves memory uninitialized; explicit clear so
        // freshly constructed buffers are deterministic (zero-filled).
        buf_.clear();
    }

    int channels() const { return buf_.getNumChannels(); }
    int frames()   const { return buf_.getNumSamples(); }
    float*       data()       { return buf_.getWritePointer(0); }
    const float* data() const { return buf_.getReadPointer(0); }

    juce::AudioBuffer<float>&       juce()       { return buf_; }
    const juce::AudioBuffer<float>& juce() const { return buf_; }

private:
    juce::AudioBuffer<float> buf_;
};

// Normalize a (possibly negative) index to [0, size). Throws on out-of-range.
static int normalize_index(int idx, int size, const char* axis) {
    int result = idx;
    if (result < 0) result += size;
    if (result < 0 || result >= size) {
        throw nb::index_error(
            (std::string(axis) + " index " + std::to_string(idx) +
             " out of range for axis of length " + std::to_string(size)).c_str());
    }
    return result;
}

// Resolve a slice or int key on one axis. Returns (start, count). For an int
// key the count is 1. Throws TypeError on stride != 1, fancy indexing,
// Ellipsis, or other unsupported forms.
static std::pair<int, int> resolve_axis_key(nb::handle key, int size,
                                            const char* axis) {
    PyObject* p = key.ptr();
    if (PyLong_Check(p)) {
        int idx = nb::cast<int>(key);
        return {normalize_index(idx, size, axis), 1};
    }
    if (PySlice_Check(p)) {
        Py_ssize_t start, stop, step, length;
        if (PySlice_GetIndicesEx(p, size, &start, &stop, &step, &length) != 0) {
            throw nb::python_error();
        }
        if (step != 1) {
            throw nb::type_error(
                "AudioBuffer does not support strided slicing "
                "(step != 1). Use .numpy() for that.");
        }
        return {(int)start, (int)length};
    }
    if (p == Py_Ellipsis) {
        throw nb::type_error(
            "AudioBuffer does not support Ellipsis indexing. Use .numpy() for that.");
    }
    if (PyList_Check(p) || PyTuple_Check(p)
        || (Py_TYPE(p)->tp_iter != nullptr && !PyUnicode_Check(p))) {
        throw nb::type_error(
            "AudioBuffer does not support fancy / boolean indexing. "
            "Use .numpy() for that.");
    }
    throw nb::type_error(
        (std::string(axis) + " key must be an int or slice").c_str());
}

// Validate a 2-tuple key for AudioBuffer.__getitem__ / __setitem__.
// Returns the two key elements; throws TypeError on shape mismatch.
static std::pair<nb::object, nb::object> require_2tuple(nb::object key) {
    if (!PyTuple_Check(key.ptr())) {
        throw nb::type_error(
            "AudioBuffer requires 2-axis indexing: buf[ch, fr]. "
            "Single-axis access (buf[ch]) is ambiguous; use buf[ch, :] instead.");
    }
    auto t = nb::cast<nb::tuple>(key);
    if (t.size() != 2) {
        throw nb::type_error(
            "AudioBuffer requires exactly 2 indices (channel, frame).");
    }
    return {t[0], t[1]};
}

// Helper to convert numpy arrays to raw pointers
using AudioArray = nb::ndarray<float, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>;
using DoubleAudioArray = nb::ndarray<double, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>;

// Maximum number of MIDI output events per process call.
// Events beyond this limit are silently dropped by the C layer.
static constexpr int MIDI_OUT_CAPACITY = 256;

// Parse a Python MIDI event tuple (sample_offset, status, data1, data2) into MH_MidiEvent.
// Validates that the tuple has exactly 4 elements before indexing.
static MH_MidiEvent parse_midi_event(nb::handle item) {
    nb::tuple ev = nb::cast<nb::tuple>(item);
    if (nb::len(ev) < 4) {
        throw std::runtime_error(
            "MIDI event must be a tuple of 4 elements: "
            "(sample_offset, status, data1, data2)");
    }
    MH_MidiEvent e;
    e.sample_offset = nb::cast<int>(ev[0]);
    e.status = nb::cast<unsigned char>(ev[1]);
    e.data1 = nb::cast<unsigned char>(ev[2]);
    e.data2 = nb::cast<unsigned char>(ev[3]);
    return e;
}

// Convert planar float audio [ch0_s0,ch0_s1,...,ch1_s0,ch1_s1,...] to interleaved
// [s0_ch0,s0_ch1,...,s1_ch0,s1_ch1,...].
static void planar_to_interleaved(const float* planar, float* interleaved,
                                  size_t channels, size_t frames) {
    for (size_t f = 0; f < frames; f++)
        for (size_t ch = 0; ch < channels; ch++)
            interleaved[f * channels + ch] = planar[ch * frames + f];
}

// Convert interleaved float audio to planar layout.
static void interleaved_to_planar(const float* interleaved, float* planar,
                                  size_t channels, size_t frames) {
    for (size_t ch = 0; ch < channels; ch++)
        for (size_t f = 0; f < frames; f++)
            planar[ch * frames + f] = interleaved[f * channels + ch];
}

// Validate audio buffer shape for a process call. Throws on mismatch.
// User arrays must have >= the plugin's required channel count; extra
// channels are harmless (the C layer only references the first N).
static void validate_process_shape(int in_channels, int out_channels,
                                   int in_frames, int out_frames,
                                   int required_in, int required_out,
                                   int max_block_size) {
    if (in_frames != out_frames) {
        throw std::runtime_error(
            "Input and output frame counts must match (input=" +
            std::to_string(in_frames) + ", output=" + std::to_string(out_frames) + ")");
    }
    if (in_frames > max_block_size) {
        throw std::runtime_error(
            "Frame count " + std::to_string(in_frames) +
            " exceeds max block size " + std::to_string(max_block_size));
    }
    if (in_channels < required_in) {
        throw std::runtime_error(
            "Input has " + std::to_string(in_channels) +
            " channel(s) but plugin requires at least " +
            std::to_string(required_in));
    }
    if (out_channels < required_out) {
        throw std::runtime_error(
            "Output has " + std::to_string(out_channels) +
            " channel(s) but plugin requires at least " +
            std::to_string(required_out));
    }
}

// Build a Python dict from MH_PluginDesc fields.
static nb::dict plugin_desc_to_dict(const MH_PluginDesc& desc) {
    nb::dict d;
    d["name"] = std::string(desc.name);
    d["vendor"] = std::string(desc.vendor);
    d["version"] = std::string(desc.version);
    d["format"] = std::string(desc.format);
    d["unique_id"] = std::string(desc.unique_id);
    d["path"] = std::string(desc.path);
    d["accepts_midi"] = desc.accepts_midi != 0;
    d["produces_midi"] = desc.produces_midi != 0;
    d["num_inputs"] = desc.num_inputs;
    d["num_outputs"] = desc.num_outputs;
    return d;
}

// Deferred callback event -- pushed from any thread (including the audio thread)
// without acquiring the GIL, drained from Python via poll_callbacks().
struct CallbackEvent {
    enum Type : uint8_t { Change, ParamValue, GestureBegin, GestureEnd };
    Type type;
    int int_val;     // flags (Change) or param_index (ParamValue, Gesture*)
    float float_val; // new_value (ParamValue only)
};

// Python wrapper class for MH_Plugin
class Plugin {
public:
    Plugin(const std::string& path, double sample_rate, int max_block_size,
           int in_channels = 2, int out_channels = 2, int sidechain_channels = 0)
        : sample_rate_(sample_rate), max_block_size_(max_block_size)
    {
        char err[1024] = {0};
        if (sidechain_channels > 0) {
            // Use extended open with sidechain support
            plugin_ = mh_open_ex(path.c_str(), sample_rate, max_block_size,
                                 in_channels, out_channels, sidechain_channels,
                                 err, sizeof(err));
        } else {
            plugin_ = mh_open(path.c_str(), sample_rate, max_block_size,
                              in_channels, out_channels, err, sizeof(err));
        }
        if (!plugin_) {
            throw std::runtime_error(std::string("Failed to open plugin: ") + err);
        }
        // Pre-allocate the callback queues so the audio-thread trampoline
        // never has to allocate. capacity is preserved across poll_callbacks
        // by using clear() rather than swap() to drain.
        cb_queue_.reserve(CB_QUEUE_CAPACITY);
        dispatch_buffer_.reserve(CB_QUEUE_CAPACITY);
    }

    ~Plugin() {
        close();
    }

    // Explicit close. Idempotent. Subsequent operations on this Plugin
    // raise a clear RuntimeError via the underlying C API's null-checks.
    void close() {
        if (plugin_) {
            // Clear callbacks before closing to avoid dangling pointers
            mh_set_change_callback(plugin_, nullptr, nullptr);
            mh_set_param_value_callback(plugin_, nullptr, nullptr);
            mh_set_param_gesture_callback(plugin_, nullptr, nullptr);
            mh_close(plugin_);
            plugin_ = nullptr;
        }
    }

    Plugin& enter() { return *this; }
    void exit(nb::object, nb::object, nb::object) { close(); }

    // Disable copy
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    // Enable move
    Plugin(Plugin&& other) noexcept
        : plugin_(other.plugin_), sample_rate_(other.sample_rate_),
          max_block_size_(other.max_block_size_)
    {
        other.plugin_ = nullptr;
    }

    Plugin& operator=(Plugin&& other) noexcept {
        if (this != &other) {
            if (plugin_) mh_close(plugin_);
            plugin_ = other.plugin_;
            sample_rate_ = other.sample_rate_;
            max_block_size_ = other.max_block_size_;
            other.plugin_ = nullptr;
        }
        return *this;
    }

    // Properties
    std::string path() const {
        const char* p = mh_get_path(plugin_);
        return p ? std::string(p) : std::string();
    }

    int num_params() const {
        return mh_get_num_params(plugin_);
    }

    int num_input_channels() const {
        MH_Info info;
        if (mh_get_info(plugin_, &info))
            return info.num_input_ch;
        return 0;
    }

    int num_output_channels() const {
        MH_Info info;
        if (mh_get_info(plugin_, &info))
            return info.num_output_ch;
        return 0;
    }

    int latency_samples() const {
        return mh_get_latency_samples(plugin_);
    }

    double tail_seconds() const {
        return mh_get_tail_seconds(plugin_);
    }

    int sidechain_channels() const {
        return mh_get_sidechain_channels(plugin_);
    }

    bool accepts_midi() const {
        MH_Info info;
        if (mh_get_info(plugin_, &info))
            return info.accepts_midi != 0;
        return false;
    }

    bool produces_midi() const {
        MH_Info info;
        if (mh_get_info(plugin_, &info))
            return info.produces_midi != 0;
        return false;
    }

    bool is_midi_effect() const {
        MH_Info info;
        if (mh_get_info(plugin_, &info))
            return info.is_midi_effect != 0;
        return false;
    }

    bool supports_mpe() const {
        MH_Info info;
        if (mh_get_info(plugin_, &info))
            return info.supports_mpe != 0;
        return false;
    }

    // Sample rate
    double get_sample_rate() const {
        return mh_get_sample_rate(plugin_);
    }

    void set_sample_rate(double new_rate) {
        if (!mh_set_sample_rate(plugin_, new_rate)) {
            throw std::runtime_error("Failed to set sample rate");
        }
        sample_rate_ = new_rate;
    }

    // Bus layout queries
    int num_input_buses() const {
        return mh_get_num_buses(plugin_, 1);
    }

    int num_output_buses() const {
        return mh_get_num_buses(plugin_, 0);
    }

    nb::dict get_bus_info(bool is_input, int bus_index) const {
        MH_BusInfo info;
        if (!mh_get_bus_info(plugin_, is_input ? 1 : 0, bus_index, &info)) {
            throw std::runtime_error("Failed to get bus info");
        }

        nb::dict d;
        d["name"] = std::string(info.name);
        d["num_channels"] = info.num_channels;
        d["is_main"] = info.is_main != 0;
        d["is_enabled"] = info.is_enabled != 0;
        return d;
    }

    // Parameter access
    float get_param(int index) const {
        return mh_get_param(plugin_, index);
    }

    void set_param(int index, float value) {
        if (!mh_set_param(plugin_, index, value)) {
            throw std::runtime_error("Failed to set parameter");
        }
    }

    nb::dict get_param_info(int index) const {
        MH_ParamInfo info;
        if (!mh_get_param_info(plugin_, index, &info)) {
            throw std::runtime_error("Failed to get parameter info");
        }

        nb::dict d;
        d["name"] = std::string(info.name);
        d["id"] = std::string(info.id);
        d["label"] = std::string(info.label);
        d["current_value_str"] = std::string(info.current_value_str);
        d["min_value"] = info.min_value;
        d["max_value"] = info.max_value;
        d["default_value"] = info.default_value;
        d["num_steps"] = info.num_steps;
        d["is_automatable"] = info.is_automatable != 0;
        d["is_boolean"] = info.is_boolean != 0;
        d["category"] = info.category;
        return d;
    }

    // Find parameter index by name (case-insensitive)
    int find_param(const std::string& name) const {
        int n = mh_get_num_params(plugin_);
        // Convert search name to lowercase
        std::string name_lower = name;
        for (auto& c : name_lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        for (int i = 0; i < n; ++i) {
            MH_ParamInfo info;
            if (mh_get_param_info(plugin_, i, &info)) {
                std::string pname(info.name);
                for (auto& c : pname) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (pname == name_lower)
                    return i;
            }
        }
        throw std::runtime_error("Parameter not found: '" + name + "'");
    }

    // Get parameter value by name
    float get_param_by_name(const std::string& name) const {
        return mh_get_param(plugin_, find_param(name));
    }

    // Set parameter value by name
    void set_param_by_name(const std::string& name, float value) {
        int idx = find_param(name);
        if (!mh_set_param(plugin_, idx, value)) {
            throw std::runtime_error("Failed to set parameter");
        }
    }

    // Parameter text conversion
    std::string param_to_text(int index, float value) const {
        char buf[256] = {0};
        if (!mh_param_to_text(plugin_, index, value, buf, sizeof(buf))) {
            throw std::runtime_error("Failed to convert parameter to text");
        }
        return std::string(buf);
    }

    float param_from_text(int index, const std::string& text) const {
        float value = 0.0f;
        if (!mh_param_from_text(plugin_, index, text.c_str(), &value)) {
            throw std::runtime_error("Failed to convert text to parameter value");
        }
        return value;
    }

    // Factory presets (programs)
    int num_programs() const {
        return mh_get_num_programs(plugin_);
    }

    std::string get_program_name(int index) const {
        char buf[256] = {0};
        if (!mh_get_program_name(plugin_, index, buf, sizeof(buf))) {
            throw std::runtime_error("Failed to get program name");
        }
        return std::string(buf);
    }

    int get_program() const {
        return mh_get_program(plugin_);
    }

    void set_program(int index) {
        if (!mh_set_program(plugin_, index)) {
            throw std::runtime_error("Failed to set program");
        }
    }

    // State management
    nb::bytes get_state() const {
        int size = mh_get_state_size(plugin_);
        if (size <= 0) {
            return nb::bytes(nullptr, 0);
        }

        std::vector<char> buffer(size);
        if (!mh_get_state(plugin_, buffer.data(), size)) {
            throw std::runtime_error("Failed to get plugin state");
        }

        return nb::bytes(buffer.data(), size);
    }

    void set_state(nb::bytes data) {
        if (!mh_set_state(plugin_, data.c_str(), static_cast<int>(data.size()))) {
            throw std::runtime_error("Failed to set plugin state");
        }
    }

    // Bypass
    bool get_bypass() const {
        return mh_get_bypass(plugin_) != 0;
    }

    bool set_bypass(bool bypass) {
        return mh_set_bypass(plugin_, bypass ? 1 : 0) != 0;
    }

    // Transport
    void set_transport(double bpm, int time_sig_num, int time_sig_denom,
                       long long position_samples, double position_beats,
                       bool is_playing, bool is_recording = false,
                       bool is_looping = false,
                       long long loop_start = 0, long long loop_end = 0)
    {
        MH_TransportInfo transport;
        transport.bpm = bpm;
        transport.time_sig_numerator = time_sig_num;
        transport.time_sig_denominator = time_sig_denom;
        transport.position_samples = position_samples;
        transport.position_beats = position_beats;
        transport.is_playing = is_playing ? 1 : 0;
        transport.is_recording = is_recording ? 1 : 0;
        transport.is_looping = is_looping ? 1 : 0;
        transport.loop_start_samples = loop_start;
        transport.loop_end_samples = loop_end;
        mh_set_transport(plugin_, &transport);
    }

    void clear_transport() {
        mh_set_transport(plugin_, nullptr);
    }

    // Reset internal state
    void reset() {
        if (!mh_reset(plugin_)) {
            throw std::runtime_error("Failed to reset plugin");
        }
    }

    // Non-realtime mode
    bool get_non_realtime() const {
        // Note: JUCE doesn't provide a getter, so we track it ourselves
        return non_realtime_;
    }

    void set_non_realtime(bool non_realtime) {
        if (!mh_set_non_realtime(plugin_, non_realtime ? 1 : 0)) {
            throw std::runtime_error("Failed to set non-realtime mode");
        }
        non_realtime_ = non_realtime;
    }

    // Process audio (simple version - no MIDI)
    void process(AudioArray input, AudioArray output) {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        MH_Info info;
        mh_get_info(plugin_, &info);
        validate_process_shape(in_channels, out_channels, in_frames, out_frames,
                               info.num_input_ch, info.num_output_ch, max_block_size_);

        // Set up channel pointers
        std::vector<const float*> in_ptrs(in_channels);
        std::vector<float*> out_ptrs(out_channels);

        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = input.data() + ch * in_frames;
        }
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = output.data() + ch * out_frames;
        }

        if (!mh_process(plugin_, in_ptrs.data(), out_ptrs.data(), in_frames)) {
            throw std::runtime_error("Process failed");
        }
    }

    // Process with MIDI
    nb::list process_midi(AudioArray input, AudioArray output,
                          nb::list midi_in)
    {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        MH_Info info;
        mh_get_info(plugin_, &info);
        validate_process_shape(in_channels, out_channels, in_frames, out_frames,
                               info.num_input_ch, info.num_output_ch, max_block_size_);

        // Convert MIDI input
        std::vector<MH_MidiEvent> midi_events;
        for (size_t i = 0; i < nb::len(midi_in); ++i) {
            midi_events.push_back(parse_midi_event(midi_in[i]));
        }

        // Set up channel pointers
        std::vector<const float*> in_ptrs(in_channels);
        std::vector<float*> out_ptrs(out_channels);

        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = input.data() + ch * in_frames;
        }
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = output.data() + ch * out_frames;
        }

        // Output MIDI buffer (capped at MIDI_OUT_CAPACITY; excess events are dropped)
        std::vector<MH_MidiEvent> midi_out(MIDI_OUT_CAPACITY);
        int num_midi_out = 0;

        if (!mh_process_midi_io(plugin_, in_ptrs.data(), out_ptrs.data(), in_frames,
                                midi_events.data(), static_cast<int>(midi_events.size()),
                                midi_out.data(), MIDI_OUT_CAPACITY, &num_midi_out)) {
            throw std::runtime_error("Process failed");
        }

        // Convert MIDI output to Python list
        nb::list result;
        for (int i = 0; i < num_midi_out; ++i) {
            result.append(nb::make_tuple(
                midi_out[i].sample_offset,
                midi_out[i].status,
                midi_out[i].data1,
                midi_out[i].data2
            ));
        }
        return result;
    }

    // Process with sample-accurate automation
    nb::list process_auto(AudioArray input, AudioArray output,
                          nb::list midi_in, nb::list param_changes)
    {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        MH_Info info;
        mh_get_info(plugin_, &info);
        validate_process_shape(in_channels, out_channels, in_frames, out_frames,
                               info.num_input_ch, info.num_output_ch, max_block_size_);

        // Convert MIDI input
        std::vector<MH_MidiEvent> midi_events;
        for (size_t i = 0; i < nb::len(midi_in); ++i) {
            midi_events.push_back(parse_midi_event(midi_in[i]));
        }

        // Convert param changes
        std::vector<MH_ParamChange> changes;
        for (size_t i = 0; i < nb::len(param_changes); ++i) {
            nb::tuple pc = nb::cast<nb::tuple>(param_changes[i]);
            MH_ParamChange c;
            c.sample_offset = nb::cast<int>(pc[0]);
            c.param_index = nb::cast<int>(pc[1]);
            c.value = nb::cast<float>(pc[2]);
            changes.push_back(c);
        }

        // Set up channel pointers
        std::vector<const float*> in_ptrs(in_channels);
        std::vector<float*> out_ptrs(out_channels);

        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = input.data() + ch * in_frames;
        }
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = output.data() + ch * out_frames;
        }

        // Output MIDI buffer (capped at MIDI_OUT_CAPACITY; excess events are dropped)
        std::vector<MH_MidiEvent> midi_out(MIDI_OUT_CAPACITY);
        int num_midi_out = 0;

        if (!mh_process_auto(plugin_, in_ptrs.data(), out_ptrs.data(), in_frames,
                             midi_events.data(), static_cast<int>(midi_events.size()),
                             midi_out.data(), MIDI_OUT_CAPACITY, &num_midi_out,
                             changes.data(), static_cast<int>(changes.size()))) {
            throw std::runtime_error("Process failed");
        }

        // Convert MIDI output to Python list
        nb::list result;
        for (int i = 0; i < num_midi_out; ++i) {
            result.append(nb::make_tuple(
                midi_out[i].sample_offset,
                midi_out[i].status,
                midi_out[i].data1,
                midi_out[i].data2
            ));
        }
        return result;
    }

    // Process with sidechain input
    void process_sidechain(AudioArray main_in, AudioArray main_out,
                           nb::ndarray<float, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> sidechain_in)
    {
        int main_in_ch = static_cast<int>(main_in.shape(0));
        int main_out_ch = static_cast<int>(main_out.shape(0));
        int main_in_frames = static_cast<int>(main_in.shape(1));
        int main_out_frames = static_cast<int>(main_out.shape(1));
        int sc_ch = static_cast<int>(sidechain_in.shape(0));
        int sc_frames = static_cast<int>(sidechain_in.shape(1));

        if (main_in_frames != main_out_frames || main_in_frames != sc_frames) {
            throw std::runtime_error("All buffer frame counts must match");
        }
        if (main_in_frames > max_block_size_) {
            throw std::runtime_error("Frame count exceeds max block size");
        }

        MH_Info info;
        mh_get_info(plugin_, &info);
        int required_sc = mh_get_sidechain_channels(plugin_);
        if (main_in_ch < info.num_input_ch) {
            throw std::runtime_error(
                "Main input has " + std::to_string(main_in_ch) +
                " channel(s) but plugin requires at least " +
                std::to_string(info.num_input_ch));
        }
        if (main_out_ch < info.num_output_ch) {
            throw std::runtime_error(
                "Main output has " + std::to_string(main_out_ch) +
                " channel(s) but plugin requires at least " +
                std::to_string(info.num_output_ch));
        }
        if (sc_ch < required_sc) {
            throw std::runtime_error(
                "Sidechain input has " + std::to_string(sc_ch) +
                " channel(s) but plugin requires " +
                std::to_string(required_sc));
        }

        int nframes = main_in_frames;

        // Set up channel pointers for main input
        std::vector<const float*> main_in_ptrs(main_in_ch);
        for (int ch = 0; ch < main_in_ch; ++ch) {
            main_in_ptrs[ch] = main_in.data() + ch * nframes;
        }

        // Set up channel pointers for main output
        std::vector<float*> main_out_ptrs(main_out_ch);
        for (int ch = 0; ch < main_out_ch; ++ch) {
            main_out_ptrs[ch] = main_out.data() + ch * nframes;
        }

        // Set up channel pointers for sidechain
        std::vector<const float*> sc_ptrs(sc_ch);
        for (int ch = 0; ch < sc_ch; ++ch) {
            sc_ptrs[ch] = sidechain_in.data() + ch * nframes;
        }

        if (!mh_process_sidechain(plugin_,
                                  main_in_ptrs.data(),
                                  main_out_ptrs.data(),
                                  sc_ptrs.data(),
                                  nframes)) {
            throw std::runtime_error("Process with sidechain failed");
        }
    }

    // Double precision support
    bool supports_double() const {
        return mh_supports_double(plugin_) != 0;
    }

    // Process audio with double precision
    void process_double(DoubleAudioArray input, DoubleAudioArray output) {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        MH_Info info;
        mh_get_info(plugin_, &info);
        validate_process_shape(in_channels, out_channels, in_frames, out_frames,
                               info.num_input_ch, info.num_output_ch, max_block_size_);

        // Set up channel pointers
        std::vector<const double*> in_ptrs(in_channels);
        std::vector<double*> out_ptrs(out_channels);

        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = input.data() + ch * in_frames;
        }
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = output.data() + ch * out_frames;
        }

        if (!mh_process_double(plugin_, in_ptrs.data(), out_ptrs.data(), in_frames)) {
            throw std::runtime_error("Process (double) failed");
        }
    }

    // Processing precision
    int get_processing_precision() const {
        return mh_get_processing_precision(plugin_);
    }

    void set_processing_precision(int precision) {
        if (!mh_set_processing_precision(plugin_, precision)) {
            if (precision == MH_PRECISION_DOUBLE)
                throw std::runtime_error("Failed to set double precision (plugin may not support it)");
            else
                throw std::runtime_error("Failed to set processing precision");
        }
    }

    // Track properties
    void set_track_properties(std::optional<std::string> name, std::optional<unsigned int> colour) {
        const char* name_ptr = name.has_value() ? name->c_str() : nullptr;
        int has_colour = colour.has_value() ? 1 : 0;
        unsigned int colour_val = colour.value_or(0);
        if (!mh_set_track_properties(plugin_, name_ptr, has_colour, colour_val)) {
            throw std::runtime_error("Failed to set track properties");
        }
    }

    // Bus layout validation
    bool check_buses_layout(std::vector<int> input_channels, std::vector<int> output_channels) const {
        return mh_check_buses_layout(
            plugin_,
            input_channels.data(), static_cast<int>(input_channels.size()),
            output_channels.data(), static_cast<int>(output_channels.size())) != 0;
    }

    // Parameter gestures (host -> plugin)
    void begin_param_gesture(int index) {
        if (!mh_begin_param_gesture(plugin_, index)) {
            throw std::runtime_error("Failed to begin parameter gesture");
        }
    }

    void end_param_gesture(int index) {
        if (!mh_end_param_gesture(plugin_, index)) {
            throw std::runtime_error("Failed to end parameter gesture");
        }
    }

    // Current program state (lighter-weight per-program state)
    nb::bytes get_program_state() const {
        int size = mh_get_program_state_size(plugin_);
        if (size <= 0) {
            return nb::bytes(nullptr, 0);
        }

        std::vector<char> buffer(size);
        if (!mh_get_program_state(plugin_, buffer.data(), size)) {
            throw std::runtime_error("Failed to get program state");
        }

        return nb::bytes(buffer.data(), size);
    }

    void set_program_state(nb::bytes data) {
        if (!mh_set_program_state(plugin_, data.c_str(), static_cast<int>(data.size()))) {
            throw std::runtime_error("Failed to set program state");
        }
    }

    // Change notification callbacks
    void set_change_callback(nb::handle cb) {
        if (cb.is_none()) {
            change_callback_ = nb::object();
            mh_set_change_callback(plugin_, nullptr, nullptr);
        } else {
            change_callback_ = nb::borrow<nb::object>(cb);
            mh_set_change_callback(plugin_, &Plugin::change_callback_trampoline, this);
        }
    }

    void set_param_value_callback(nb::handle cb) {
        if (cb.is_none()) {
            param_value_callback_ = nb::object();
            mh_set_param_value_callback(plugin_, nullptr, nullptr);
        } else {
            param_value_callback_ = nb::borrow<nb::object>(cb);
            mh_set_param_value_callback(plugin_, &Plugin::param_value_callback_trampoline, this);
        }
    }

    void set_param_gesture_callback(nb::handle cb) {
        if (cb.is_none()) {
            param_gesture_callback_ = nb::object();
            mh_set_param_gesture_callback(plugin_, nullptr, nullptr);
        } else {
            param_gesture_callback_ = nb::borrow<nb::object>(cb);
            mh_set_param_gesture_callback(plugin_, &Plugin::param_gesture_callback_trampoline, this);
        }
    }

    // Drain pending callback events and dispatch to registered Python callbacks.
    // Call this from the main thread (or any non-audio thread) to receive
    // notifications that the plugin queued since the last poll.
    // Returns the number of events dispatched.
    int poll_callbacks() {
        // Copy out events under the lock; clear() preserves capacity so the
        // producer side never has to reallocate inside the trampoline. The
        // local `dispatch_buffer_` is reused across calls for the same
        // reason on the consumer side.
        {
            std::lock_guard<std::mutex> lock(cb_queue_mutex_);
            // Use assign-from-iterators so dispatch_buffer_ keeps its own
            // capacity across calls and only grows once.
            dispatch_buffer_.assign(cb_queue_.begin(), cb_queue_.end());
            cb_queue_.clear();
        }

        for (const auto& ev : dispatch_buffer_) {
            switch (ev.type) {
                case CallbackEvent::Change:
                    if (change_callback_.is_valid() && !change_callback_.is_none())
                        change_callback_(ev.int_val);
                    break;
                case CallbackEvent::ParamValue:
                    if (param_value_callback_.is_valid() && !param_value_callback_.is_none())
                        param_value_callback_(ev.int_val, ev.float_val);
                    break;
                case CallbackEvent::GestureBegin:
                    if (param_gesture_callback_.is_valid() && !param_gesture_callback_.is_none())
                        param_gesture_callback_(ev.int_val, true);
                    break;
                case CallbackEvent::GestureEnd:
                    if (param_gesture_callback_.is_valid() && !param_gesture_callback_.is_none())
                        param_gesture_callback_(ev.int_val, false);
                    break;
            }
        }
        return static_cast<int>(dispatch_buffer_.size());
    }

    // Number of callback events dropped because the bounded queue was full.
    // Resets to zero on read; useful for diagnostics if poll_callbacks() is
    // not being drained frequently enough.
    int callback_events_dropped() {
        return cb_queue_dropped_.exchange(0, std::memory_order_relaxed);
    }

private:
    MH_Plugin* plugin_ = nullptr;
    double sample_rate_;
    int max_block_size_;
    bool non_realtime_ = false;

    // Python callback holders (prevent GC)
    nb::object change_callback_;
    nb::object param_value_callback_;
    nb::object param_gesture_callback_;

    // Bounded, capacity-preserving queue for deferred callback dispatch.
    // Trampolines push events here from any thread (including the audio
    // thread) without acquiring the GIL. Python drains via poll_callbacks().
    //
    // RT-safety on the producer side rests on three properties:
    //   1. cb_queue_ is reserve()'d to CB_QUEUE_CAPACITY at construction.
    //   2. push_back on a vector with sufficient capacity does not allocate.
    //   3. poll_callbacks() uses clear() (not swap) so capacity is preserved
    //      across drains.
    // The mutex hold is microseconds (one push_back, no allocation). When
    // the queue is full, events are dropped and counted in
    // cb_queue_dropped_; the producer never blocks waiting for the consumer.
    static constexpr size_t CB_QUEUE_CAPACITY = 1024;
    std::mutex cb_queue_mutex_;
    std::vector<CallbackEvent> cb_queue_;
    std::vector<CallbackEvent> dispatch_buffer_;   // owned by poll_callbacks
    std::atomic<int> cb_queue_dropped_{0};

    // Push helper: returns true if pushed, false if dropped (queue full).
    bool push_callback_event(const CallbackEvent& ev) {
        std::lock_guard<std::mutex> lock(cb_queue_mutex_);
        if (cb_queue_.size() >= CB_QUEUE_CAPACITY) {
            cb_queue_dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        cb_queue_.push_back(ev);
        return true;
    }

    // Static trampolines -- enqueue events without touching the GIL.
    static void change_callback_trampoline(MH_Plugin*, int flags, void* user_data) {
        auto* self = static_cast<Plugin*>(user_data);
        self->push_callback_event({CallbackEvent::Change, flags, 0.0f});
    }

    static void param_value_callback_trampoline(MH_Plugin*, int param_index, float new_value, void* user_data) {
        auto* self = static_cast<Plugin*>(user_data);
        self->push_callback_event({CallbackEvent::ParamValue, param_index, new_value});
    }

    static void param_gesture_callback_trampoline(MH_Plugin*, int param_index, int gesture_starting, void* user_data) {
        auto* self = static_cast<Plugin*>(user_data);
        self->push_callback_event({gesture_starting ? CallbackEvent::GestureBegin : CallbackEvent::GestureEnd,
                                   param_index, 0.0f});
    }

    // Allow AudioDevice and PluginChain to access raw plugin pointer
    friend class AudioDevice;
    friend class PluginChain;
};


// Python wrapper class for MH_PluginChain
class PluginChain {
public:
    PluginChain(nb::list plugins)
    {
        if (nb::len(plugins) == 0) {
            throw std::runtime_error("Plugin chain must contain at least one plugin");
        }

        // Extract raw plugin pointers and keep references to prevent GC
        std::vector<MH_Plugin*> raw_ptrs;
        for (size_t i = 0; i < nb::len(plugins); ++i) {
            Plugin& p = nb::cast<Plugin&>(plugins[i]);
            if (!p.plugin_) {
                throw std::runtime_error(
                    "Plugin at index " + std::to_string(i) +
                    " is invalid (null internal pointer -- was it moved from?)");
            }
            raw_ptrs.push_back(p.plugin_);
            plugin_refs_.push_back(&p);
        }

        char err[1024] = {0};
        chain_ = mh_chain_create(raw_ptrs.data(), static_cast<int>(raw_ptrs.size()),
                                  err, sizeof(err));
        if (!chain_) {
            throw std::runtime_error(std::string("Failed to create plugin chain: ") + err);
        }
    }

    ~PluginChain() {
        close();
    }

    void close() {
        if (chain_) {
            mh_chain_close(chain_);
            chain_ = nullptr;
        }
    }

    PluginChain& enter() { return *this; }
    void exit(nb::object, nb::object, nb::object) { close(); }

    // Disable copy
    PluginChain(const PluginChain&) = delete;
    PluginChain& operator=(const PluginChain&) = delete;

    // Enable move
    PluginChain(PluginChain&& other) noexcept
        : chain_(other.chain_), plugin_refs_(std::move(other.plugin_refs_))
    {
        other.chain_ = nullptr;
    }

    PluginChain& operator=(PluginChain&& other) noexcept {
        if (this != &other) {
            if (chain_) mh_chain_close(chain_);
            chain_ = other.chain_;
            plugin_refs_ = std::move(other.plugin_refs_);
            other.chain_ = nullptr;
        }
        return *this;
    }

    // Properties
    int num_plugins() const {
        return mh_chain_get_num_plugins(chain_);
    }

    int latency_samples() const {
        return mh_chain_get_latency_samples(chain_);
    }

    int num_input_channels() const {
        return mh_chain_get_num_input_channels(chain_);
    }

    int num_output_channels() const {
        return mh_chain_get_num_output_channels(chain_);
    }

    double get_sample_rate() const {
        return mh_chain_get_sample_rate(chain_);
    }

    double tail_seconds() const {
        return mh_chain_get_tail_seconds(chain_);
    }

    // Reset all plugins
    void reset() {
        if (!mh_chain_reset(chain_)) {
            throw std::runtime_error("Failed to reset plugin chain");
        }
    }

    // Non-realtime mode
    void set_non_realtime(bool non_realtime) {
        if (!mh_chain_set_non_realtime(chain_, non_realtime ? 1 : 0)) {
            throw std::runtime_error("Failed to set non-realtime mode");
        }
    }

    // Get a plugin from the chain by index
    Plugin* get_plugin(int index) {
        if (index < 0 || index >= static_cast<int>(plugin_refs_.size())) {
            throw std::runtime_error("Plugin index out of range");
        }
        return plugin_refs_[index];
    }

    // Process audio (no MIDI)
    void process(AudioArray input, AudioArray output) {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        validate_process_shape(in_channels, out_channels, in_frames, out_frames,
                               mh_chain_get_num_input_channels(chain_),
                               mh_chain_get_num_output_channels(chain_),
                               mh_chain_get_max_block_size(chain_));

        std::vector<const float*> in_ptrs(in_channels);
        std::vector<float*> out_ptrs(out_channels);

        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = input.data() + ch * in_frames;
        }
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = output.data() + ch * out_frames;
        }

        if (!mh_chain_process(chain_, in_ptrs.data(), out_ptrs.data(), in_frames)) {
            throw std::runtime_error("Chain process failed");
        }
    }

    // Process with MIDI
    nb::list process_midi(AudioArray input, AudioArray output, nb::list midi_in)
    {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        validate_process_shape(in_channels, out_channels, in_frames, out_frames,
                               mh_chain_get_num_input_channels(chain_),
                               mh_chain_get_num_output_channels(chain_),
                               mh_chain_get_max_block_size(chain_));

        // Convert MIDI input
        std::vector<MH_MidiEvent> midi_events;
        for (size_t i = 0; i < nb::len(midi_in); ++i) {
            midi_events.push_back(parse_midi_event(midi_in[i]));
        }

        std::vector<const float*> in_ptrs(in_channels);
        std::vector<float*> out_ptrs(out_channels);

        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = input.data() + ch * in_frames;
        }
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = output.data() + ch * out_frames;
        }

        // Output MIDI buffer (capped at MIDI_OUT_CAPACITY; excess events are dropped)
        std::vector<MH_MidiEvent> midi_out(MIDI_OUT_CAPACITY);
        int num_midi_out = 0;

        if (!mh_chain_process_midi_io(chain_, in_ptrs.data(), out_ptrs.data(), in_frames,
                                       midi_events.data(), static_cast<int>(midi_events.size()),
                                       midi_out.data(), MIDI_OUT_CAPACITY, &num_midi_out)) {
            throw std::runtime_error("Chain process failed");
        }

        // Convert MIDI output to Python list
        nb::list result;
        for (int i = 0; i < num_midi_out; ++i) {
            result.append(nb::make_tuple(
                midi_out[i].sample_offset,
                midi_out[i].status,
                midi_out[i].data1,
                midi_out[i].data2
            ));
        }
        return result;
    }

    // Process with sample-accurate automation
    nb::list process_auto(AudioArray input, AudioArray output,
                          nb::list midi_in, nb::list param_changes)
    {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        validate_process_shape(in_channels, out_channels, in_frames, out_frames,
                               mh_chain_get_num_input_channels(chain_),
                               mh_chain_get_num_output_channels(chain_),
                               mh_chain_get_max_block_size(chain_));

        // Convert MIDI input
        std::vector<MH_MidiEvent> midi_events;
        for (size_t i = 0; i < nb::len(midi_in); ++i) {
            midi_events.push_back(parse_midi_event(midi_in[i]));
        }

        // Convert param changes (4-tuples: sample_offset, plugin_index, param_index, value)
        std::vector<MH_ChainParamChange> changes;
        for (size_t i = 0; i < nb::len(param_changes); ++i) {
            nb::tuple pc = nb::cast<nb::tuple>(param_changes[i]);
            MH_ChainParamChange c;
            c.sample_offset = nb::cast<int>(pc[0]);
            c.plugin_index = nb::cast<int>(pc[1]);
            c.param_index = nb::cast<int>(pc[2]);
            c.value = nb::cast<float>(pc[3]);
            changes.push_back(c);
        }

        // Set up channel pointers
        std::vector<const float*> in_ptrs(in_channels);
        std::vector<float*> out_ptrs(out_channels);

        for (int ch = 0; ch < in_channels; ++ch) {
            in_ptrs[ch] = input.data() + ch * in_frames;
        }
        for (int ch = 0; ch < out_channels; ++ch) {
            out_ptrs[ch] = output.data() + ch * out_frames;
        }

        // Output MIDI buffer (capped at MIDI_OUT_CAPACITY; excess events are dropped)
        std::vector<MH_MidiEvent> midi_out(MIDI_OUT_CAPACITY);
        int num_midi_out = 0;

        if (!mh_chain_process_auto(chain_, in_ptrs.data(), out_ptrs.data(), in_frames,
                                    midi_events.data(), static_cast<int>(midi_events.size()),
                                    midi_out.data(), MIDI_OUT_CAPACITY, &num_midi_out,
                                    changes.data(), static_cast<int>(changes.size()))) {
            throw std::runtime_error("Chain process_auto failed");
        }

        // Convert MIDI output to Python list
        nb::list result;
        for (int i = 0; i < num_midi_out; ++i) {
            result.append(nb::make_tuple(
                midi_out[i].sample_offset,
                midi_out[i].status,
                midi_out[i].data1,
                midi_out[i].data2
            ));
        }
        return result;
    }

private:
    MH_PluginChain* chain_ = nullptr;
    std::vector<Plugin*> plugin_refs_;  // Keep references to prevent plugins from being GC'd

    // Allow AudioDevice to access raw chain pointer
    friend class AudioDevice;
};


// Python wrapper class for MH_AudioDevice
class AudioDevice {
public:
    // Constructor for single plugin
    AudioDevice(Plugin& plugin, double sample_rate = 0, int buffer_frames = 0,
                int output_channels = 0, int midi_input_port = -1, int midi_output_port = -1,
                bool capture = false, int playback_device_index = -1,
                int capture_device_index = -1)
        : plugin_ref_(&plugin), chain_ref_(nullptr)
    {
        MH_AudioConfig config;
        config.sample_rate = sample_rate;
        config.buffer_frames = buffer_frames;
        config.output_channels = output_channels;
        config.midi_input_port = midi_input_port;
        config.midi_output_port = midi_output_port;
        config.capture = capture ? 1 : 0;
        config.playback_device_index = playback_device_index;
        config.capture_device_index = capture_device_index;

        char err[1024] = {0};
        device_ = mh_audio_open(plugin.plugin_, &config, err, sizeof(err));
        if (!device_) {
            throw std::runtime_error(std::string("Failed to open audio device: ") + err);
        }
    }

    // Constructor for plugin chain
    AudioDevice(PluginChain& chain, double sample_rate = 0, int buffer_frames = 0,
                int output_channels = 0, int midi_input_port = -1, int midi_output_port = -1,
                bool capture = false, int playback_device_index = -1,
                int capture_device_index = -1)
        : plugin_ref_(nullptr), chain_ref_(&chain)
    {
        MH_AudioConfig config;
        config.sample_rate = sample_rate;
        config.buffer_frames = buffer_frames;
        config.output_channels = output_channels;
        config.midi_input_port = midi_input_port;
        config.midi_output_port = midi_output_port;
        config.capture = capture ? 1 : 0;
        config.playback_device_index = playback_device_index;
        config.capture_device_index = capture_device_index;

        char err[1024] = {0};
        device_ = mh_audio_open_chain(chain.chain_, &config, err, sizeof(err));
        if (!device_) {
            throw std::runtime_error(std::string("Failed to open audio device with chain: ") + err);
        }
    }

    ~AudioDevice() {
        if (device_) {
            mh_audio_close(device_);
            device_ = nullptr;
        }
    }

    // Disable copy
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Enable move
    AudioDevice(AudioDevice&& other) noexcept
        : device_(other.device_), plugin_ref_(other.plugin_ref_), chain_ref_(other.chain_ref_)
    {
        other.device_ = nullptr;
        other.plugin_ref_ = nullptr;
        other.chain_ref_ = nullptr;
    }

    AudioDevice& operator=(AudioDevice&& other) noexcept {
        if (this != &other) {
            if (device_) mh_audio_close(device_);
            device_ = other.device_;
            plugin_ref_ = other.plugin_ref_;
            chain_ref_ = other.chain_ref_;
            other.device_ = nullptr;
            other.plugin_ref_ = nullptr;
            other.chain_ref_ = nullptr;
        }
        return *this;
    }

    void start() {
        if (!mh_audio_start(device_)) {
            throw std::runtime_error("Failed to start audio");
        }
    }

    void stop() {
        if (!mh_audio_stop(device_)) {
            throw std::runtime_error("Failed to stop audio");
        }
    }

    bool is_playing() const {
        return mh_audio_is_playing(device_) != 0;
    }

    double get_sample_rate() const {
        return mh_audio_get_sample_rate(device_);
    }

    int get_buffer_frames() const {
        return mh_audio_get_buffer_frames(device_);
    }

    int get_channels() const {
        return mh_audio_get_channels(device_);
    }

    // MIDI connection
    void connect_midi_input(int port_index) {
        if (!mh_audio_connect_midi_input(device_, port_index)) {
            throw std::runtime_error("Failed to connect MIDI input");
        }
    }

    void connect_midi_output(int port_index) {
        if (!mh_audio_connect_midi_output(device_, port_index)) {
            throw std::runtime_error("Failed to connect MIDI output");
        }
    }

    void disconnect_midi_input() {
        mh_audio_disconnect_midi_input(device_);
    }

    void disconnect_midi_output() {
        mh_audio_disconnect_midi_output(device_);
    }

    int get_midi_input_port() const {
        return mh_audio_get_midi_input_port(device_);
    }

    int get_midi_output_port() const {
        return mh_audio_get_midi_output_port(device_);
    }

    // Virtual MIDI ports
    void create_virtual_midi_input(const std::string& port_name) {
        if (!mh_audio_create_virtual_midi_input(device_, port_name.c_str())) {
            throw std::runtime_error("Failed to create virtual MIDI input (may not be supported on this platform)");
        }
    }

    void create_virtual_midi_output(const std::string& port_name) {
        if (!mh_audio_create_virtual_midi_output(device_, port_name.c_str())) {
            throw std::runtime_error("Failed to create virtual MIDI output (may not be supported on this platform)");
        }
    }

    bool is_midi_input_virtual() const {
        return mh_audio_is_midi_input_virtual(device_) != 0;
    }

    bool is_midi_output_virtual() const {
        return mh_audio_is_midi_output_virtual(device_) != 0;
    }

    // Send MIDI event programmatically
    void send_midi(int status, int data1, int data2) {
        if (!mh_audio_send_midi(device_,
                                static_cast<unsigned char>(status),
                                static_cast<unsigned char>(data1),
                                static_cast<unsigned char>(data2))) {
            throw std::runtime_error("Failed to send MIDI (queue may be full)");
        }
    }

    // Audio input via lock-free ring buffer (no GIL on audio thread)
    void enable_input(int capacity_frames = 0) {
        if (capacity_frames <= 0) {
            // Default: ~0.5s at device sample rate
            double sr = mh_audio_get_sample_rate(device_);
            capacity_frames = static_cast<int>(sr > 0 ? sr * 0.5 : 24000);
        }
        if (!mh_audio_enable_input(device_, capacity_frames)) {
            throw std::runtime_error("Failed to enable audio input ring buffer");
        }
    }

    void disable_input() {
        mh_audio_disable_input(device_);
    }

    int write_input(AudioArray data) {
        int channels = static_cast<int>(data.shape(0));
        int frames = static_cast<int>(data.shape(1));
        int dev_channels = mh_audio_get_channels(device_);

        // Interleave: numpy is [channels, frames] row-major, ring buffer wants interleaved
        std::vector<float> interleaved(frames * dev_channels, 0.0f);
        const float* src = data.data();
        int copy_ch = std::min(channels, dev_channels);
        for (int f = 0; f < frames; f++) {
            for (int c = 0; c < copy_ch; c++) {
                interleaved[f * dev_channels + c] = src[c * frames + f];
            }
        }

        return mh_audio_write_input(device_, interleaved.data(), frames);
    }

    int input_available() const {
        return mh_audio_input_available(device_);
    }

    // Context manager support
    AudioDevice& enter() {
        start();
        return *this;
    }

    void exit(nb::object, nb::object, nb::object) {
        stop();
    }

private:
    MH_AudioDevice* device_ = nullptr;
    Plugin* plugin_ref_ = nullptr;        // Keep reference to prevent plugin from being GC'd
    PluginChain* chain_ref_ = nullptr;    // Keep reference to prevent chain from being GC'd
};


// Module-level probe function
nb::dict probe_plugin(const std::string& path) {
    MH_PluginDesc desc;
    char err[1024] = {0};

    if (!mh_probe(path.c_str(), &desc, err, sizeof(err))) {
        throw std::runtime_error(std::string("Failed to probe plugin: ") + err);
    }

    return plugin_desc_to_dict(desc);
}

// Callback context for scan_directory
struct ScanContext {
    std::vector<nb::dict>* results;
};

static void scan_callback(const MH_PluginDesc* desc, void* user_data) {
    auto* ctx = static_cast<ScanContext*>(user_data);
    ctx->results->push_back(plugin_desc_to_dict(*desc));
}

// Module-level scan_directory function
nb::list scan_directory(const std::string& directory_path) {
    std::vector<nb::dict> results;
    ScanContext ctx{&results};

    int count = mh_scan_directory(directory_path.c_str(), scan_callback, &ctx);

    if (count < 0) {
        throw std::runtime_error("Failed to scan directory: " + directory_path);
    }

    nb::list result_list;
    for (const auto& d : results) {
        result_list.append(d);
    }
    return result_list;
}

// MIDI port enumeration callback context
struct MidiPortContext {
    std::vector<nb::dict>* results;
};

static void midi_port_callback(const MH_MidiPortInfo* port, void* user_data) {
    auto* ctx = static_cast<MidiPortContext*>(user_data);
    nb::dict d;
    d["name"] = std::string(port->name);
    d["index"] = port->index;
    ctx->results->push_back(d);
}

// Module-level MIDI port enumeration
nb::list midi_get_input_ports() {
    std::vector<nb::dict> results;
    MidiPortContext ctx{&results};

    mh_midi_enumerate_inputs(midi_port_callback, &ctx);

    nb::list result_list;
    for (const auto& d : results) {
        result_list.append(d);
    }
    return result_list;
}

nb::list midi_get_output_ports() {
    std::vector<nb::dict> results;
    MidiPortContext ctx{&results};

    mh_midi_enumerate_outputs(midi_port_callback, &ctx);

    nb::list result_list;
    for (const auto& d : results) {
        result_list.append(d);
    }
    return result_list;
}

// Audio device enumeration
static nb::list audio_enumerate(bool capture) {
    // First pass: count
    int total = capture
        ? mh_audio_enumerate_capture_devices(nullptr, 0)
        : mh_audio_enumerate_playback_devices(nullptr, 0);

    nb::list result_list;
    if (total <= 0) {
        return result_list;
    }

    std::vector<MH_AudioDeviceInfo> infos(total);
    int n = capture
        ? mh_audio_enumerate_capture_devices(infos.data(), total)
        : mh_audio_enumerate_playback_devices(infos.data(), total);
    if (n < 0) n = 0;

    for (int i = 0; i < n; i++) {
        nb::dict d;
        d["name"] = std::string(infos[i].name);
        d["index"] = i;
        d["is_default"] = infos[i].is_default != 0;
        result_list.append(d);
    }
    return result_list;
}

nb::list audio_get_playback_devices() { return audio_enumerate(false); }
nb::list audio_get_capture_devices() { return audio_enumerate(true); }

// MIDI file wrapper class
class MidiFile {
public:
    MidiFile() = default;

    // Load from file
    bool load(const std::string& path) {
        if (!file_.read(path)) {
            return false;
        }
        file_.doTimeAnalysis();
        file_.linkNotePairs();
        return true;
    }

    // Save to file
    bool save(const std::string& path) {
        return file_.write(path);
    }

    // Get number of tracks
    int num_tracks() const {
        return file_.getTrackCount();
    }

    // Get ticks per quarter note
    int ticks_per_quarter() const {
        return file_.getTicksPerQuarterNote();
    }

    // Set ticks per quarter note
    void set_ticks_per_quarter(int tpq) {
        file_.setTicksPerQuarterNote(tpq);
    }

    // Get total duration in seconds
    double duration_seconds() {
        file_.doTimeAnalysis();
        return file_.getFileDurationInSeconds();
    }

    // Add a track
    int add_track() {
        return file_.addTrack();
    }

    // Add a tempo event (BPM)
    void add_tempo(int track, int tick, double bpm) {
        file_.addTempo(track, tick, bpm);
    }

    // Add a note on event
    void add_note_on(int track, int tick, int channel, int pitch, int velocity) {
        file_.addNoteOn(track, tick, channel, pitch, velocity);
    }

    // Add a note off event
    void add_note_off(int track, int tick, int channel, int pitch, int velocity = 0) {
        file_.addNoteOff(track, tick, channel, pitch, velocity);
    }

    // Add a control change event
    void add_control_change(int track, int tick, int channel, int controller, int value) {
        file_.addController(track, tick, channel, controller, value);
    }

    // Add a program change event
    void add_program_change(int track, int tick, int channel, int program) {
        file_.addPatchChange(track, tick, channel, program);
    }

    // Add a pitch bend event
    void add_pitch_bend(int track, int tick, int channel, int value) {
        file_.addPitchBend(track, tick, channel, value);
    }

    // Get all events from a track as a list of dicts
    nb::list get_events(int track) const {
        nb::list events;

        if (track < 0 || track >= file_.getTrackCount()) {
            return events;
        }

        const auto& track_events = file_[track];
        for (int i = 0; i < track_events.getEventCount(); i++) {
            const auto& event = track_events[i];

            nb::dict d;
            d["tick"] = event.tick;
            d["seconds"] = event.seconds;

            if (event.isNoteOn()) {
                d["type"] = "note_on";
                d["channel"] = event.getChannel();
                d["pitch"] = event.getKeyNumber();
                d["velocity"] = event.getVelocity();
            } else if (event.isNoteOff()) {
                d["type"] = "note_off";
                d["channel"] = event.getChannel();
                d["pitch"] = event.getKeyNumber();
                d["velocity"] = event.getVelocity();
            } else if (event.isController()) {
                d["type"] = "control_change";
                d["channel"] = event.getChannel();
                d["controller"] = event.getP1();
                d["value"] = event.getP2();
            } else if (event.isTimbre()) {
                d["type"] = "program_change";
                d["channel"] = event.getChannel();
                d["program"] = event.getP1();
            } else if (event.isPitchbend()) {
                d["type"] = "pitch_bend";
                d["channel"] = event.getChannel();
                d["value"] = event.getP1() | (event.getP2() << 7);
            } else if (event.isTempo()) {
                d["type"] = "tempo";
                d["bpm"] = event.getTempoBPM();
            } else if (event.isMeta()) {
                d["type"] = "meta";
                d["meta_type"] = static_cast<int>(event[1]);
            } else {
                d["type"] = "other";
                d["status"] = static_cast<int>(event[0]);
            }

            events.append(d);
        }

        return events;
    }

    // Convert to absolute ticks
    void make_absolute_ticks() {
        file_.makeAbsoluteTicks();
    }

    // Convert to delta ticks
    void make_delta_ticks() {
        file_.makeDeltaTicks();
    }

    // Join all tracks into track 0 (Type 0 format)
    void join_tracks() {
        file_.joinTracks();
    }

    // Split tracks (Type 1 format)
    void split_tracks() {
        file_.splitTracks();
    }

private:
    smf::MidiFile file_;
};


// Python wrapper class for MH_MidiIn (standalone MIDI input without a plugin)
class MidiIn {
public:
    // Private constructor - use static factory methods
    MidiIn() : handle_(nullptr) {}

    ~MidiIn() {
        close();
    }

    // Disable copy
    MidiIn(const MidiIn&) = delete;
    MidiIn& operator=(const MidiIn&) = delete;

    // Enable move
    MidiIn(MidiIn&& other) noexcept
        : handle_(other.handle_), callback_(std::move(other.callback_))
    {
        other.handle_ = nullptr;
    }

    MidiIn& operator=(MidiIn&& other) noexcept {
        if (this != &other) {
            close();
            handle_ = other.handle_;
            callback_ = std::move(other.callback_);
            other.handle_ = nullptr;
        }
        return *this;
    }

    static MidiIn open(int port_index, nb::callable callback) {
        MidiIn m;
        m.callback_ = std::move(callback);

        char err[1024] = {0};
        m.handle_ = mh_midi_in_open(port_index, &MidiIn::midi_callback, &m,
                                     err, sizeof(err));
        if (!m.handle_) {
            throw std::runtime_error(std::string("Failed to open MIDI input: ") + err);
        }
        return m;
    }

    static MidiIn open_virtual(const std::string& name, nb::callable callback) {
        MidiIn m;
        m.callback_ = std::move(callback);

        char err[1024] = {0};
        m.handle_ = mh_midi_in_open_virtual(name.c_str(), &MidiIn::midi_callback, &m,
                                             err, sizeof(err));
        if (!m.handle_) {
            throw std::runtime_error(std::string("Failed to open virtual MIDI input: ") + err);
        }
        return m;
    }

    void close() {
        if (handle_) {
            mh_midi_in_close(handle_);
            handle_ = nullptr;
        }
    }

    MidiIn& enter() {
        return *this;
    }

    void exit(nb::object, nb::object, nb::object) {
        close();
    }

private:
    MH_MidiIn* handle_;
    nb::callable callback_;

    // NOTE: Called from the MIDI I/O thread. Acquires the GIL; callback must
    // return quickly to avoid missed MIDI events.
    static void midi_callback(const unsigned char* data, size_t len, void* user_data) {
        nb::gil_scoped_acquire gil;
        auto* self = static_cast<MidiIn*>(user_data);
        if (self->callback_.is_valid() && !self->callback_.is_none()) {
            self->callback_(nb::bytes(reinterpret_cast<const char*>(data), len));
        }
    }
};


// Note: Async plugin loading in Python is best done using Python's threading module:
//
//   import threading
//   import minihost
//
//   def load_plugin_async(path, callback):
//       def loader():
//           try:
//               plugin = minihost.Plugin(path, sample_rate=48000)
//               callback(plugin, None)
//           except Exception as e:
//               callback(None, str(e))
//       thread = threading.Thread(target=loader, daemon=True)
//       thread.start()
//       return thread
//
// The C API provides mh_open_async() for C/C++ applications needing async loading.

NB_MODULE(_core, m) {
    m.doc() = "minihost - Python bindings for audio plugin hosting";

    // ABI version of the linked C library. Header constants
    // MH_API_VERSION_{MAJOR,MINOR,PATCH} are exposed so a wheel built against
    // one header version can detect a mismatch with a separately-installed
    // libminihost. Layout: MAJOR*10000 + MINOR*100 + PATCH.
    m.attr("MH_API_VERSION_MAJOR") = MH_API_VERSION_MAJOR;
    m.attr("MH_API_VERSION_MINOR") = MH_API_VERSION_MINOR;
    m.attr("MH_API_VERSION_PATCH") = MH_API_VERSION_PATCH;
    m.attr("MH_API_VERSION_NUMBER") = MH_API_VERSION_NUMBER;
    m.attr("MH_API_VERSION_STRING") = MH_API_VERSION_STRING;
    m.def("api_version", &mh_api_version,
          "Return the ABI version the linked C library was compiled against, "
          "as MAJOR*10000 + MINOR*100 + PATCH.");
    m.def("api_version_string", &mh_api_version_string,
          "Return the linked C library's ABI version as 'MAJOR.MINOR.PATCH'.");

    // Module-level functions
    m.def("probe", &probe_plugin,
          nb::arg("path"),
          "Get plugin metadata without full instantiation");

    m.def("scan_directory", &scan_directory,
          nb::arg("directory_path"),
          "Scan a directory for plugins (VST3, AudioUnit). Returns list of plugin metadata dicts.");

    // MIDI port enumeration
    m.def("midi_get_input_ports", &midi_get_input_ports,
          "Get list of available MIDI input ports. Returns list of dicts with 'name' and 'index'.");
    m.def("midi_get_output_ports", &midi_get_output_ports,
          "Get list of available MIDI output ports. Returns list of dicts with 'name' and 'index'.");

    // Audio device enumeration
    m.def("audio_get_playback_devices", &audio_get_playback_devices,
          "Get list of available audio playback (output) devices. "
          "Returns list of dicts with 'name', 'index', and 'is_default'.");
    m.def("audio_get_capture_devices", &audio_get_capture_devices,
          "Get list of available audio capture (input) devices. "
          "Returns list of dicts with 'name', 'index', and 'is_default'.");

    // VST3 .vstpreset helpers
    m.def("vstpreset_read_class_id_from_bundle",
          [](const std::string& vst3_path) {
              char class_id[MH_VSTPRESET_CLASS_ID_LEN + 1] = {0};
              char err[512] = {0};
              if (!mh_vstpreset_read_class_id_from_bundle(
                      vst3_path.c_str(), class_id, err, sizeof(err))) {
                  throw std::runtime_error(std::string("vstpreset_read_class_id_from_bundle: ") + err);
              }
              return std::string(class_id);
          },
          nb::arg("vst3_path"),
          "Read the processor class ID (32-char uppercase hex FUID) from a "
          "VST3 bundle's Contents/Resources/moduleinfo.json. Raises RuntimeError "
          "if the file is missing (plugin predates VST3 SDK 3.7.5), malformed, "
          "or contains no Audio Module Class entry.");

    // Read a .vstpreset via the C parser; return (class_id, component_state, controller_state).
    // controller_state is None if not present in the file.
    m.def("vstpreset_read",
          [](const std::string& path) {
              MH_VstPreset preset{};
              char err[512] = {0};
              if (!mh_vstpreset_read(path.c_str(), &preset, err, sizeof(err))) {
                  throw std::runtime_error(std::string("vstpreset_read: ") + err);
              }
              std::string class_id(preset.class_id);
              nb::object comp = preset.component_state
                  ? nb::cast<nb::object>(nb::bytes(static_cast<const char*>(preset.component_state),
                                                    preset.component_size))
                  : nb::none();
              nb::object cont = preset.controller_state
                  ? nb::cast<nb::object>(nb::bytes(static_cast<const char*>(preset.controller_state),
                                                    preset.controller_size))
                  : nb::none();
              mh_vstpreset_free(&preset);
              return nb::make_tuple(class_id, comp, cont);
          },
          nb::arg("path"),
          "Read a .vstpreset file via the C parser. Returns "
          "(class_id, component_state, controller_state) where the latter two "
          "are bytes objects (or None if the chunk is absent).");

    // Write a .vstpreset via the C writer.
    m.def("vstpreset_write",
          [](const std::string& path, const std::string& class_id,
             nb::bytes component_state, std::optional<nb::bytes> controller_state) {
              char err[512] = {0};
              const void* cont_data = nullptr;
              int cont_size = 0;
              if (controller_state.has_value()) {
                  cont_data = controller_state->c_str();
                  cont_size = static_cast<int>(controller_state->size());
              }
              if (!mh_vstpreset_write(
                      path.c_str(), class_id.c_str(),
                      component_state.c_str(),
                      static_cast<int>(component_state.size()),
                      cont_data, cont_size,
                      err, sizeof(err))) {
                  throw std::runtime_error(std::string("vstpreset_write: ") + err);
              }
          },
          nb::arg("path"), nb::arg("class_id"), nb::arg("component_state"),
          nb::arg("controller_state") = nb::none(),
          "Write a .vstpreset file via the C writer. controller_state is "
          "optional; pass None to write only the 'Comp' chunk.");

    // Change notification flag constants
    m.attr("MH_CHANGE_LATENCY")         = MH_CHANGE_LATENCY;
    m.attr("MH_CHANGE_PARAM_INFO")      = MH_CHANGE_PARAM_INFO;
    m.attr("MH_CHANGE_PROGRAM")         = MH_CHANGE_PROGRAM;
    m.attr("MH_CHANGE_NON_PARAM_STATE") = MH_CHANGE_NON_PARAM_STATE;

    // Processing precision constants
    m.attr("MH_PRECISION_SINGLE") = MH_PRECISION_SINGLE;
    m.attr("MH_PRECISION_DOUBLE") = MH_PRECISION_DOUBLE;

    // ----------------------------------------------------------------------
    // AudioBuffer (wrapper around juce::AudioBuffer<float>)
    // ----------------------------------------------------------------------
    //
    // Layout: planar float32, row-major (channels x frames). Data is one
    // contiguous block; the wrapper enforces this by only allowing the
    // (channels, frames) constructor of juce::AudioBuffer.
    //
    // Buffer-protocol exposure: __dlpack__ returns an nb::ndarray that
    // aliases the underlying memory. nanobind's nb::ndarray<...> parameter
    // converter consults __dlpack__, so AudioBuffer instances can be passed
    // directly to Plugin.process / chain.process / etc. on Python 3.10+
    // without an explicit .array property or memoryview cast.
    //
    // Slicing: numpy-shaped 2-axis indexing, with documented limits (no
    // strided slices, no fancy indexing, no Ellipsis -- raise TypeError
    // pointing the user at .numpy() for those).
    nb::class_<MhAudioBuffer>(m, "AudioBuffer",
        "Planar float32 audio buffer (stdlib-only; backed by juce::AudioBuffer).\n\n"
        "Layout is (channels x frames) row-major. The buffer is always\n"
        "contiguous in memory and exposes the DLPack buffer protocol, so it\n"
        "can be passed directly to Plugin.process / PluginChain.process /\n"
        "minihost.write_audio without an explicit conversion.\n\n"
        "2-axis indexing follows numpy conventions with deliberate limits:\n"
        "  buf[ch, fr]                -> float\n"
        "  buf[ch_slice, fr_slice]    -> AudioBuffer (copy, not view)\n"
        "Strided slices (step != 1), fancy indexing, boolean indexing, and\n"
        "Ellipsis raise TypeError; use .numpy() if you need those.")
        .def(nb::init<int, int>(), "channels"_a, "frames"_a,
             "Allocate a new AudioBuffer of (channels, frames) float32 samples, "
             "zero-initialized.")

        .def_prop_ro("channels", &MhAudioBuffer::channels,
                     "Number of channels.")
        .def_prop_ro("frames", &MhAudioBuffer::frames,
                     "Number of frames per channel.")
        .def_prop_ro("shape",
                     [](const MhAudioBuffer& self) {
                         return nb::make_tuple(self.channels(), self.frames());
                     },
                     "(channels, frames). Matches numpy's .shape on 2D arrays.")
        .def_prop_ro_static("dtype",
                            [](nb::handle) { return std::string("float32"); },
                            "Element dtype string. Always 'float32'.")

        .def("__len__", &MhAudioBuffer::channels,
             "Number of channels (matches numpy's len() on 2D arrays).")
        .def("__repr__",
             [](const MhAudioBuffer& self) {
                 return "AudioBuffer(channels=" + std::to_string(self.channels()) +
                        ", frames=" + std::to_string(self.frames()) + ")";
             })

        // ---- 2-axis indexing ----
        .def("__getitem__",
             [](MhAudioBuffer& self, nb::object key) -> nb::object {
                 auto [ch_key, fr_key] = require_2tuple(key);

                 // Scalar/scalar fast path -> Python float
                 if (PyLong_Check(ch_key.ptr()) && PyLong_Check(fr_key.ptr())) {
                     int ch = normalize_index(nb::cast<int>(ch_key),
                                              self.channels(), "channel");
                     int fr = normalize_index(nb::cast<int>(fr_key),
                                              self.frames(), "frame");
                     return nb::cast(self.data()[(size_t)ch * self.frames() + fr]);
                 }

                 // Slice or scalar mixed -> new AudioBuffer (copy).
                 // Scalar axis becomes 1-element (so buf[0, 100:200] returns
                 // a 1xN buffer, never a flat 1D array -- consistent shape).
                 auto [ch_start, ch_count] = resolve_axis_key(ch_key,
                                                              self.channels(),
                                                              "channel");
                 auto [fr_start, fr_count] = resolve_axis_key(fr_key,
                                                              self.frames(),
                                                              "frame");
                 auto* out = new MhAudioBuffer(ch_count, fr_count);
                 const float* src = self.data();
                 float* dst = out->data();
                 const size_t src_stride = (size_t)self.frames();
                 const size_t dst_stride = (size_t)fr_count;
                 const size_t bytes = (size_t)fr_count * sizeof(float);
                 for (int i = 0; i < ch_count; ++i) {
                     std::memcpy(dst + (size_t)i * dst_stride,
                                 src + ((size_t)ch_start + i) * src_stride + fr_start,
                                 bytes);
                 }
                 // Hand ownership of the new instance to Python.
                 return nb::cast(out, nb::rv_policy::take_ownership);
             })
        .def("__setitem__",
             [](MhAudioBuffer& self, nb::object key, nb::object value) {
                 auto [ch_key, fr_key] = require_2tuple(key);
                 auto [ch_start, ch_count] = resolve_axis_key(ch_key,
                                                              self.channels(),
                                                              "channel");
                 auto [fr_start, fr_count] = resolve_axis_key(fr_key,
                                                              self.frames(),
                                                              "frame");

                 float* dst = self.data();
                 const size_t dst_stride = (size_t)self.frames();
                 const size_t bytes = (size_t)fr_count * sizeof(float);

                 // Scalar broadcast.
                 if (PyFloat_Check(value.ptr()) || PyLong_Check(value.ptr())) {
                     float v = nb::cast<float>(value);
                     for (int i = 0; i < ch_count; ++i) {
                         float* row = dst + ((size_t)ch_start + i) * dst_stride
                                      + fr_start;
                         for (int j = 0; j < fr_count; ++j) row[j] = v;
                     }
                     return;
                 }

                 // 2D buffer-protocol source (numpy ndarray, AudioBuffer
                 // via DLPack, etc.). Shape must match exactly.
                 nb::ndarray<const float, nb::shape<-1, -1>, nb::c_contig,
                             nb::device::cpu> src;
                 try {
                     src = nb::cast<nb::ndarray<const float, nb::shape<-1, -1>,
                                                nb::c_contig, nb::device::cpu>>(value);
                 } catch (const nb::cast_error&) {
                     throw nb::type_error(
                         "AudioBuffer.__setitem__ value must be a Python "
                         "scalar (broadcast) or a 2D float32 c-contiguous "
                         "buffer (AudioBuffer / numpy ndarray / similar).");
                 }
                 if ((int)src.shape(0) != ch_count
                     || (int)src.shape(1) != fr_count) {
                     throw nb::value_error(
                         ("Source shape " + std::to_string(src.shape(0)) + "x"
                          + std::to_string(src.shape(1)) +
                          " does not match destination slice " +
                          std::to_string(ch_count) + "x" +
                          std::to_string(fr_count)).c_str());
                 }
                 const float* src_data = src.data();
                 const size_t src_stride = (size_t)src.shape(1);
                 for (int i = 0; i < ch_count; ++i) {
                     std::memcpy(dst + ((size_t)ch_start + i) * dst_stride
                                       + fr_start,
                                 src_data + (size_t)i * src_stride,
                                 bytes);
                 }
             })

        // ---- DSP ops borrowed from juce::AudioBuffer ----
        .def("clear",
             [](MhAudioBuffer& self,
                std::optional<int> start, std::optional<int> count) {
                 int s = start.value_or(0);
                 int n = count.value_or(self.frames() - s);
                 if (s < 0 || n < 0 || s + n > self.frames()) {
                     throw nb::value_error("clear range out of bounds");
                 }
                 self.juce().clear(s, n);
             },
             "start"_a = nb::none(), "count"_a = nb::none(),
             "Zero-fill all channels in the range [start, start+count). "
             "Defaults to the whole buffer.")
        .def("apply_gain",
             [](MhAudioBuffer& self, float gain) {
                 self.juce().applyGain(gain);
             }, "gain"_a,
             "Multiply every sample by `gain` in place.")
        .def("magnitude",
             [](MhAudioBuffer& self,
                std::optional<int> start, std::optional<int> count) {
                 int s = start.value_or(0);
                 int n = count.value_or(self.frames() - s);
                 if (s < 0 || n < 0 || s + n > self.frames()) {
                     throw nb::value_error("magnitude range out of bounds");
                 }
                 return self.juce().getMagnitude(s, n);
             },
             "start"_a = nb::none(), "count"_a = nb::none(),
             "Return the peak absolute sample value across all channels in "
             "the range [start, start+count). Defaults to the whole buffer.")
        .def("copy",
             [](const MhAudioBuffer& self) {
                 auto* out = new MhAudioBuffer(self.channels(),
                                               self.frames());
                 std::memcpy(out->data(), self.data(),
                             (size_t)self.channels() * self.frames()
                                 * sizeof(float));
                 return nb::cast(out, nb::rv_policy::take_ownership);
             },
             "Return a deep copy of this buffer.")

        // ---- Buffer-protocol export (DLPack) ----
        // Allows Plugin.process(buf, out) etc. to consume AudioBuffer
        // directly without an explicit .numpy() / memoryview conversion.
        .def("__dlpack__",
             [](nb::handle self_h, nb::handle /*stream*/) {
                 auto& self = nb::cast<MhAudioBuffer&>(self_h);
                 size_t shape[2] = { (size_t)self.channels(),
                                     (size_t)self.frames() };
                 return nb::ndarray<float, nb::shape<-1, -1>, nb::c_contig,
                                    nb::device::cpu>(
                     self.data(), 2, shape, self_h);
             },
             "stream"_a = nb::none(),
             "DLPack export. Consumers like nanobind's nb::ndarray and "
             "numpy.asarray call this to obtain a zero-copy view.")
        .def("__dlpack_device__",
             [](const MhAudioBuffer&) {
                 return nb::make_tuple(1, 0);  // (kDLCPU, device_id=0)
             },
             "DLPack device descriptor. Always (kDLCPU=1, 0).")

        // ---- numpy interop (lazy import) ----
        // For both .numpy() and .__array__(), we receive the bound object
        // as nb::handle (the Python self) so we can pass it as the owner
        // of the returned nb::ndarray. The owner is what keeps the
        // AudioBuffer alive while the consuming numpy array (or DLPack
        // capsule) is in use; without it the underlying memory is freed
        // out from under any view that outlives the local reference.
        .def("numpy",
             [](nb::handle self_h) {
                 auto& self = nb::cast<MhAudioBuffer&>(self_h);
                 size_t shape[2] = { (size_t)self.channels(),
                                     (size_t)self.frames() };
                 return nb::ndarray<nb::numpy, float, nb::shape<-1, -1>,
                                    nb::c_contig>(
                     self.data(), 2, shape, self_h);
             },
             "Return a numpy ndarray view (zero-copy). Requires numpy.")
        // numpy's asarray()/array() consult __array__ before __dlpack__.
        // Without this hook, numpy falls back to iterating __getitem__,
        // which our 2-axis-only indexing rejects.
        .def("__array__",
             [](nb::handle self_h,
                nb::handle /*dtype*/, nb::handle /*copy*/) {
                 auto& self = nb::cast<MhAudioBuffer&>(self_h);
                 size_t shape[2] = { (size_t)self.channels(),
                                     (size_t)self.frames() };
                 return nb::ndarray<nb::numpy, float, nb::shape<-1, -1>,
                                    nb::c_contig>(
                     self.data(), 2, shape, self_h);
             },
             "dtype"_a = nb::none(), "copy"_a = nb::none(),
             "numpy interop hook. Returns a numpy ndarray view (zero-copy). "
             "dtype and copy arguments are accepted for numpy 2.x compatibility "
             "but ignored (the buffer is always float32; numpy may copy).")
        .def_static("from_numpy",
             [](nb::ndarray<const float, nb::shape<-1, -1>, nb::c_contig,
                            nb::device::cpu> arr) {
                 int channels = (int)arr.shape(0);
                 int frames = (int)arr.shape(1);
                 auto* buf = new MhAudioBuffer(channels, frames);
                 std::memcpy(buf->data(), arr.data(),
                             (size_t)channels * frames * sizeof(float));
                 return nb::cast(buf, nb::rv_policy::take_ownership);
             }, "array"_a,
             "Construct a new AudioBuffer by copying from a 2D float32 "
             "c-contiguous array (numpy ndarray, another AudioBuffer, etc.).");

    nb::class_<Plugin>(m, "Plugin")
        .def(nb::init<const std::string&, double, int, int, int, int>(),
             nb::arg("path"),
             nb::arg("sample_rate") = 48000.0,
             nb::arg("max_block_size") = 512,
             nb::arg("in_channels") = 2,
             nb::arg("out_channels") = 2,
             nb::arg("sidechain_channels") = 0,
             "Open an audio plugin (VST3 or AudioUnit). Use sidechain_channels > 0 for sidechain support.")

        // Properties
        .def_prop_ro("path", &Plugin::path,
                     "Plugin file path passed to the constructor")
        .def_prop_ro("num_params", &Plugin::num_params,
                     "Number of parameters")
        .def_prop_ro("num_input_channels", &Plugin::num_input_channels,
                     "Number of input channels")
        .def_prop_ro("num_output_channels", &Plugin::num_output_channels,
                     "Number of output channels")
        .def_prop_ro("latency_samples", &Plugin::latency_samples,
                     "Plugin latency in samples")
        .def_prop_ro("tail_seconds", &Plugin::tail_seconds,
                     "Plugin tail length in seconds")
        .def_prop_ro("sidechain_channels", &Plugin::sidechain_channels,
                     "Number of sidechain input channels (0 if none)")
        .def_prop_ro("accepts_midi", &Plugin::accepts_midi,
                     "True if plugin accepts MIDI input")
        .def_prop_ro("produces_midi", &Plugin::produces_midi,
                     "True if plugin produces MIDI output")
        .def_prop_ro("is_midi_effect", &Plugin::is_midi_effect,
                     "True if plugin is a pure MIDI effect (no audio)")
        .def_prop_ro("supports_mpe", &Plugin::supports_mpe,
                     "True if plugin supports MIDI Polyphonic Expression")
        .def_prop_ro("num_input_buses", &Plugin::num_input_buses,
                     "Number of input buses")
        .def_prop_ro("num_output_buses", &Plugin::num_output_buses,
                     "Number of output buses")
        .def_prop_rw("sample_rate", &Plugin::get_sample_rate, &Plugin::set_sample_rate,
                     "Current sample rate (can be changed without reloading)")

        // Bus layout
        .def("get_bus_info", &Plugin::get_bus_info,
             nb::arg("is_input"), nb::arg("bus_index"),
             "Get bus info as dict (name, num_channels, is_main, is_enabled)")

        // Parameter access
        .def("get_param", &Plugin::get_param,
             nb::arg("index"),
             "Get parameter value (normalized 0-1)")
        .def("set_param", &Plugin::set_param,
             nb::arg("index"), nb::arg("value"),
             "Set parameter value (normalized 0-1)")
        .def("get_param_info", &Plugin::get_param_info,
             nb::arg("index"),
             "Get parameter metadata as dict")
        .def("find_param", &Plugin::find_param,
             nb::arg("name"),
             "Find parameter index by name (case-insensitive). Raises RuntimeError if not found.")
        .def("get_param_by_name", &Plugin::get_param_by_name,
             nb::arg("name"),
             "Get parameter value by name (case-insensitive)")
        .def("set_param_by_name", &Plugin::set_param_by_name,
             nb::arg("name"), nb::arg("value"),
             "Set parameter value by name (case-insensitive)")
        .def("param_to_text", &Plugin::param_to_text,
             nb::arg("index"), nb::arg("value"),
             "Convert normalized value (0-1) to display string (e.g., '2500 Hz')")
        .def("param_from_text", &Plugin::param_from_text,
             nb::arg("index"), nb::arg("text"),
             "Convert display string to normalized value (0-1)")

        // Factory presets (programs)
        .def_prop_ro("num_programs", &Plugin::num_programs,
                     "Number of factory presets")
        .def("get_program_name", &Plugin::get_program_name,
             nb::arg("index"),
             "Get name of factory preset at index")
        .def_prop_rw("program", &Plugin::get_program, &Plugin::set_program,
                     "Current factory preset index")

        // State
        .def("get_state", &Plugin::get_state,
             "Get plugin state as bytes")
        .def("set_state", &Plugin::set_state,
             nb::arg("data"),
             "Restore plugin state from bytes")

        // Bypass
        .def_prop_rw("bypass", &Plugin::get_bypass, &Plugin::set_bypass,
                     "Bypass state")

        // Reset
        .def("reset", &Plugin::reset,
             "Reset internal state (clears delay lines, reverb tails, etc.)")

        // Non-realtime mode
        .def_prop_rw("non_realtime", &Plugin::get_non_realtime, &Plugin::set_non_realtime,
                     "Non-realtime mode (enables higher-quality algorithms for offline processing)")

        // Transport
        .def("set_transport", &Plugin::set_transport,
             nb::arg("bpm"),
             nb::arg("time_sig_num") = 4,
             nb::arg("time_sig_denom") = 4,
             nb::arg("position_samples") = 0LL,
             nb::arg("position_beats") = 0.0,
             nb::arg("is_playing") = true,
             nb::arg("is_recording") = false,
             nb::arg("is_looping") = false,
             nb::arg("loop_start") = 0LL,
             nb::arg("loop_end") = 0LL,
             "Set transport info for tempo-synced plugins")
        .def("clear_transport", &Plugin::clear_transport,
             "Clear transport info")

        // Process
        .def("process", &Plugin::process,
             nb::arg("input"), nb::arg("output"),
             "Process audio (shape: [channels, frames])")
        .def("process_midi", &Plugin::process_midi,
             nb::arg("input"), nb::arg("output"), nb::arg("midi_in"),
             "Process audio with MIDI. midi_in: list of (sample_offset, status, data1, data2). "
             "Returns list of output MIDI events (max 256 per call; excess events are dropped).")
        .def("process_auto", &Plugin::process_auto,
             nb::arg("input"), nb::arg("output"), nb::arg("midi_in"), nb::arg("param_changes"),
             "Process with sample-accurate automation. param_changes: list of (sample_offset, param_index, value). "
             "Returns list of output MIDI events (max 256 per call; excess events are dropped).")
        .def("process_sidechain", &Plugin::process_sidechain,
             nb::arg("main_in"), nb::arg("main_out"), nb::arg("sidechain_in"),
             "Process audio with sidechain input (all arrays shape: [channels, frames])")

        // Double precision processing
        .def_prop_ro("supports_double", &Plugin::supports_double,
                     "True if plugin supports native double precision processing")
        .def("process_double", &Plugin::process_double,
             nb::arg("input"), nb::arg("output"),
             "Process audio with double precision (float64). Shape: [channels, frames]")

        // Processing precision
        .def_prop_rw("processing_precision",
                     &Plugin::get_processing_precision, &Plugin::set_processing_precision,
                     "Processing precision (MH_PRECISION_SINGLE=0 or MH_PRECISION_DOUBLE=1)")

        // Track properties
        .def("set_track_properties", &Plugin::set_track_properties,
             nb::arg("name") = nb::none(),
             nb::arg("colour") = nb::none(),
             "Set track name and/or colour (ARGB as int). Pass None to clear.")

        // Bus layout validation
        .def("check_buses_layout", &Plugin::check_buses_layout,
             nb::arg("input_channels"), nb::arg("output_channels"),
             "Check if a bus layout is supported. Takes lists of channel counts per bus.")

        // Parameter gestures (host -> plugin)
        .def("begin_param_gesture", &Plugin::begin_param_gesture,
             nb::arg("index"),
             "Signal start of a parameter change gesture")
        .def("end_param_gesture", &Plugin::end_param_gesture,
             nb::arg("index"),
             "Signal end of a parameter change gesture")

        // Current program state
        .def("get_program_state", &Plugin::get_program_state,
             "Get current program state as bytes (lighter than get_state)")
        .def("set_program_state", &Plugin::set_program_state,
             nb::arg("data"),
             "Restore current program state from bytes")

        // Change notification callbacks (deferred -- never called on audio thread)
        .def("set_change_callback", &Plugin::set_change_callback,
             nb::arg("callback").none(),
             "Register callback for processor-level changes (latency, param info, program). "
             "Callback receives (flags: int). Pass None to clear. "
             "Events are queued and dispatched when poll_callbacks() is called.")
        .def("set_param_value_callback", &Plugin::set_param_value_callback,
             nb::arg("callback").none(),
             "Register callback for plugin-initiated parameter changes. "
             "Callback receives (param_index: int, new_value: float). Pass None to clear. "
             "Events are queued and dispatched when poll_callbacks() is called.")
        .def("set_param_gesture_callback", &Plugin::set_param_gesture_callback,
             nb::arg("callback").none(),
             "Register callback for parameter gesture begin/end from plugin UI. "
             "Callback receives (param_index: int, gesture_starting: bool). Pass None to clear. "
             "Events are queued and dispatched when poll_callbacks() is called.")
        .def("poll_callbacks", &Plugin::poll_callbacks,
             "Drain pending callback events and dispatch to registered Python callbacks. "
             "Call this periodically from your main/UI thread to receive change, "
             "parameter value, and gesture notifications. Returns the number of events dispatched.")
        .def("callback_events_dropped", &Plugin::callback_events_dropped,
             "Return (and reset) the count of callback events dropped because "
             "the bounded queue (capacity 1024) was full. Non-zero values "
             "indicate poll_callbacks() is not being called frequently enough.")

        // Explicit close + context-manager support
        .def("close", &Plugin::close,
             "Release plugin resources immediately. Idempotent. Subsequent "
             "operations on this Plugin raise RuntimeError. Equivalent to "
             "letting the Plugin go out of scope, but deterministic.")
        .def("__enter__", &Plugin::enter, nb::rv_policy::reference)
        .def("__exit__", [](Plugin& self, const nb::args&) {
            self.close();
        });

    // PluginChain class for chaining multiple plugins
    nb::class_<PluginChain>(m, "PluginChain")
        .def(nb::init<nb::list>(),
             nb::arg("plugins"),
             // keep_alive<1, 2>: tie the passed list's Python lifetime to the
             // chain. The list holds references to its Plugin elements, so the
             // raw Plugin* pointers stored in plugin_refs_ stay valid as long
             // as the chain exists, even if the caller does not retain the list.
             nb::keep_alive<1, 2>(),
             "Create a plugin chain from a list of Plugin instances. "
             "Audio flows sequentially through plugins (e.g., synth -> reverb -> limiter). "
             "All plugins must have the same sample rate. "
             "MIDI is sent to the first plugin only.")

        // Properties
        .def_prop_ro("num_plugins", &PluginChain::num_plugins,
                     "Number of plugins in the chain")
        .def_prop_ro("latency_samples", &PluginChain::latency_samples,
                     "Total latency in samples (sum of all plugin latencies)")
        .def_prop_ro("num_input_channels", &PluginChain::num_input_channels,
                     "Number of input channels (from first plugin)")
        .def_prop_ro("num_output_channels", &PluginChain::num_output_channels,
                     "Number of output channels (from last plugin)")
        .def_prop_ro("sample_rate", &PluginChain::get_sample_rate,
                     "Sample rate (all plugins have the same rate)")
        .def_prop_ro("tail_seconds", &PluginChain::tail_seconds,
                     "Maximum tail length in seconds (for reverbs, delays)")

        // Get plugin by index
        .def("get_plugin", &PluginChain::get_plugin,
             nb::arg("index"),
             nb::rv_policy::reference_internal,
             "Get a plugin from the chain by index")

        // Reset
        .def("reset", &PluginChain::reset,
             "Reset all plugins (clears delay lines, reverb tails, etc.)")

        // Non-realtime mode
        .def("set_non_realtime", &PluginChain::set_non_realtime,
             nb::arg("non_realtime"),
             "Set non-realtime mode for all plugins in the chain")

        // Process
        .def("process", &PluginChain::process,
             nb::arg("input"), nb::arg("output"),
             "Process audio through the chain (shape: [channels, frames])")
        .def("process_midi", &PluginChain::process_midi,
             nb::arg("input"), nb::arg("output"), nb::arg("midi_in"),
             "Process audio with MIDI (to first plugin). midi_in: list of (sample_offset, status, data1, data2). "
             "Returns list of output MIDI events (max 256 per call; excess events are dropped).")
        .def("process_auto", &PluginChain::process_auto,
             nb::arg("input"), nb::arg("output"), nb::arg("midi_in"), nb::arg("param_changes"),
             "Process with sample-accurate automation. param_changes: list of (sample_offset, plugin_index, param_index, value). "
             "Returns list of output MIDI events (max 256 per call; excess events are dropped).")

        // Explicit close + context-manager support
        .def("close", &PluginChain::close,
             "Release the chain's internal resources. Idempotent. The "
             "underlying Plugin objects are not closed by this call; they "
             "remain owned by the caller (or by their own context managers).")
        .def("__enter__", &PluginChain::enter, nb::rv_policy::reference)
        .def("__exit__", [](PluginChain& self, const nb::args&) {
            self.close();
        });

    // AudioDevice class for real-time playback
    nb::class_<AudioDevice>(m, "AudioDevice")
        .def(nb::init<Plugin&, double, int, int, int, int, bool, int, int>(),
             nb::arg("plugin"),
             nb::arg("sample_rate") = 0.0,
             nb::arg("buffer_frames") = 0,
             nb::arg("output_channels") = 0,
             nb::arg("midi_input_port") = -1,
             nb::arg("midi_output_port") = -1,
             nb::arg("capture") = false,
             nb::arg("playback_device_index") = -1,
             nb::arg("capture_device_index") = -1,
             // Pin the Plugin's Python lifetime to the AudioDevice so the
             // device's stored raw Plugin* (plugin_ref_) cannot dangle.
             nb::keep_alive<1, 2>(),
             "Open an audio device for real-time playback with a single plugin. "
             "sample_rate=0 uses device default. "
             "buffer_frames=0 uses auto (~256-512). "
             "output_channels=0 uses plugin's output channels. "
             "midi_input_port=-1 means no MIDI input. "
             "midi_output_port=-1 means no MIDI output. "
             "capture=True enables duplex mode (system audio input through plugin). "
             "playback_device_index=-1 uses the system default output device; "
             "pass an index from audio_get_playback_devices() to select a specific one. "
             "capture_device_index=-1 uses the system default input device (only consulted when capture=True); "
             "pass an index from audio_get_capture_devices() to select a specific one.")
        .def(nb::init<PluginChain&, double, int, int, int, int, bool, int, int>(),
             nb::arg("chain"),
             nb::arg("sample_rate") = 0.0,
             nb::arg("buffer_frames") = 0,
             nb::arg("output_channels") = 0,
             nb::arg("midi_input_port") = -1,
             nb::arg("midi_output_port") = -1,
             nb::arg("capture") = false,
             nb::arg("playback_device_index") = -1,
             nb::arg("capture_device_index") = -1,
             // Pin the PluginChain's Python lifetime to the AudioDevice.
             nb::keep_alive<1, 2>(),
             "Open an audio device for real-time playback with a plugin chain. "
             "sample_rate=0 uses device default. "
             "buffer_frames=0 uses auto (~256-512). "
             "output_channels=0 uses chain's output channels. "
             "midi_input_port=-1 means no MIDI input. "
             "midi_output_port=-1 means no MIDI output. "
             "capture=True enables duplex mode (system audio input through chain). "
             "playback_device_index / capture_device_index: see single-plugin constructor.")

        .def("start", &AudioDevice::start,
             "Start audio playback")
        .def("stop", &AudioDevice::stop,
             "Stop audio playback")

        .def_prop_ro("is_playing", &AudioDevice::is_playing,
                     "True if audio is currently playing")
        .def_prop_ro("sample_rate", &AudioDevice::get_sample_rate,
                     "Actual sample rate (may differ from requested)")
        .def_prop_ro("buffer_frames", &AudioDevice::get_buffer_frames,
                     "Actual buffer size in frames")
        .def_prop_ro("channels", &AudioDevice::get_channels,
                     "Number of output channels")

        // MIDI
        .def("connect_midi_input", &AudioDevice::connect_midi_input,
             nb::arg("port_index"),
             "Connect to a MIDI input port")
        .def("connect_midi_output", &AudioDevice::connect_midi_output,
             nb::arg("port_index"),
             "Connect to a MIDI output port")
        .def("disconnect_midi_input", &AudioDevice::disconnect_midi_input,
             "Disconnect MIDI input")
        .def("disconnect_midi_output", &AudioDevice::disconnect_midi_output,
             "Disconnect MIDI output")
        .def_prop_ro("midi_input_port", &AudioDevice::get_midi_input_port,
                     "Connected MIDI input port index (-1 if not connected or virtual)")
        .def_prop_ro("midi_output_port", &AudioDevice::get_midi_output_port,
                     "Connected MIDI output port index (-1 if not connected or virtual)")

        // Virtual MIDI ports
        .def("create_virtual_midi_input", &AudioDevice::create_virtual_midi_input,
             nb::arg("port_name"),
             "Create a virtual MIDI input port that other applications can send MIDI to")
        .def("create_virtual_midi_output", &AudioDevice::create_virtual_midi_output,
             nb::arg("port_name"),
             "Create a virtual MIDI output port that other applications can receive MIDI from")
        .def_prop_ro("is_midi_input_virtual", &AudioDevice::is_midi_input_virtual,
                     "True if MIDI input is a virtual port")
        .def_prop_ro("is_midi_output_virtual", &AudioDevice::is_midi_output_virtual,
                     "True if MIDI output is a virtual port")

        // Programmatic MIDI
        .def("send_midi", &AudioDevice::send_midi,
             nb::arg("status"), nb::arg("data1"), nb::arg("data2"),
             "Send a MIDI event to the plugin (e.g., send_midi(0x90, 60, 100) for note on)")

        // Audio input for effect processing (lock-free ring buffer)
        .def("enable_input", &AudioDevice::enable_input,
             nb::arg("capacity_frames") = 0,
             "Enable audio input for effect processing via a lock-free ring buffer. "
             "Use write_input() to push audio data. capacity_frames=0 uses ~0.5s default.")
        .def("disable_input", &AudioDevice::disable_input,
             "Disable audio input and revert to silence")
        .def("write_input", &AudioDevice::write_input,
             nb::arg("data"),
             "Write audio frames into the input ring buffer. "
             "data: numpy array of shape (channels, frames), dtype float32. "
             "Returns number of frames actually written (may be less if buffer is full). "
             "Thread-safe, can be called while playing.")
        .def_prop_ro("input_available", &AudioDevice::input_available,
             "Number of audio frames available in the input ring buffer")

        // Context manager support
        .def("__enter__", &AudioDevice::enter, nb::rv_policy::reference)
        .def("__exit__", [](AudioDevice& self, const nb::args&) {
            self.stop();
        });

    // MidiIn class for standalone MIDI input monitoring
    nb::class_<MidiIn>(m, "MidiIn")
        .def_static("open", &MidiIn::open,
             nb::arg("port_index"), nb::arg("callback"),
             "Open a MIDI input port. callback receives bytes for each message.")
        .def_static("open_virtual", &MidiIn::open_virtual,
             nb::arg("name"), nb::arg("callback"),
             "Open a virtual MIDI input port. callback receives bytes for each message.")
        .def("close", &MidiIn::close,
             "Close the MIDI input")
        .def("__enter__", &MidiIn::enter, nb::rv_policy::reference)
        .def("__exit__", [](MidiIn& self, const nb::args&) {
            self.close();
        });

    // MidiFile class for MIDI file read/write
    nb::class_<MidiFile>(m, "MidiFile")
        .def(nb::init<>(),
             "Create a new empty MIDI file")

        // File I/O
        .def("load", &MidiFile::load,
             nb::arg("path"),
             "Load a MIDI file. Returns True on success.")
        .def("save", &MidiFile::save,
             nb::arg("path"),
             "Save to a MIDI file. Returns True on success.")

        // Properties
        .def_prop_ro("num_tracks", &MidiFile::num_tracks,
                     "Number of tracks")
        .def_prop_rw("ticks_per_quarter", &MidiFile::ticks_per_quarter, &MidiFile::set_ticks_per_quarter,
                     "Ticks per quarter note (resolution)")
        .def_prop_ro("duration_seconds", &MidiFile::duration_seconds,
                     "Total duration in seconds")

        // Track management
        .def("add_track", &MidiFile::add_track,
             "Add a new track. Returns the track index.")

        // Adding events
        .def("add_tempo", &MidiFile::add_tempo,
             nb::arg("track"), nb::arg("tick"), nb::arg("bpm"),
             "Add a tempo change event")
        .def("add_note_on", &MidiFile::add_note_on,
             nb::arg("track"), nb::arg("tick"), nb::arg("channel"), nb::arg("pitch"), nb::arg("velocity"),
             "Add a note on event")
        .def("add_note_off", &MidiFile::add_note_off,
             nb::arg("track"), nb::arg("tick"), nb::arg("channel"), nb::arg("pitch"), nb::arg("velocity") = 0,
             "Add a note off event")
        .def("add_control_change", &MidiFile::add_control_change,
             nb::arg("track"), nb::arg("tick"), nb::arg("channel"), nb::arg("controller"), nb::arg("value"),
             "Add a control change (CC) event")
        .def("add_program_change", &MidiFile::add_program_change,
             nb::arg("track"), nb::arg("tick"), nb::arg("channel"), nb::arg("program"),
             "Add a program change event")
        .def("add_pitch_bend", &MidiFile::add_pitch_bend,
             nb::arg("track"), nb::arg("tick"), nb::arg("channel"), nb::arg("value"),
             "Add a pitch bend event (value: 0-16383, center=8192)")

        // Reading events
        .def("get_events", &MidiFile::get_events,
             nb::arg("track"),
             "Get all events from a track as a list of dicts")

        // Track format conversion
        .def("make_absolute_ticks", &MidiFile::make_absolute_ticks,
             "Convert all events to absolute tick times")
        .def("make_delta_ticks", &MidiFile::make_delta_ticks,
             "Convert all events to delta tick times")
        .def("join_tracks", &MidiFile::join_tracks,
             "Merge all tracks into track 0 (Type 0 format)")
        .def("split_tracks", &MidiFile::split_tracks,
             "Split by channel into separate tracks (Type 1 format)");

    // Audio file I/O functions
    m.def("audio_read", [](const std::string& path) {
        char err[1024] = {0};
        MH_AudioData* data = mh_audio_read(path.c_str(), err, sizeof(err));
        if (!data) {
            throw std::runtime_error(std::string(err));
        }

        unsigned int channels = data->channels;
        unsigned int frames = data->frames;
        unsigned int sample_rate = data->sample_rate;

        // De-interleave to planar layout for numpy
        size_t total_samples = (size_t)channels * frames;
        float* buf = new float[total_samples];
        interleaved_to_planar(data->data, buf, channels, frames);
        mh_audio_data_free(data);

        // Create numpy array with capsule for ownership
        nb::capsule owner(buf, [](void* p) noexcept { delete[] static_cast<float*>(p); });
        size_t shape[2] = {channels, frames};
        auto array = nb::ndarray<float, nb::numpy, nb::shape<-1, -1>>(
            buf, 2, shape, owner);

        return nb::make_tuple(array, sample_rate);
    }, nb::arg("path"),
       "Read an audio file. Returns (data, sample_rate) where data has shape (channels, frames).");

    m.def("audio_write", [](const std::string& path,
                            nb::ndarray<const float, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> data,
                            unsigned int sample_rate,
                            int bit_depth) {
        size_t channels = data.shape(0);
        size_t frames = data.shape(1);

        // Interleave planar numpy data for the C API
        std::vector<float> interleaved(channels * frames);
        planar_to_interleaved(data.data(), interleaved.data(), channels, frames);

        char err[1024] = {0};
        int ok = mh_audio_write(path.c_str(), interleaved.data(),
                                (unsigned int)channels, (unsigned int)frames,
                                sample_rate, bit_depth, err, sizeof(err));
        if (!ok) {
            throw std::runtime_error(std::string(err));
        }
    }, nb::arg("path"), nb::arg("data"), nb::arg("sample_rate"), nb::arg("bit_depth") = 24,
       "Write audio data to a WAV file. Data shape: (channels, frames).");

    m.def("audio_resample", [](
                nb::ndarray<const float, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu> data,
                unsigned int sample_rate_in,
                unsigned int sample_rate_out) {
        size_t channels = data.shape(0);
        size_t frames_in = data.shape(1);

        // Interleave planar numpy data for the C API
        std::vector<float> interleaved(channels * frames_in);
        planar_to_interleaved(data.data(), interleaved.data(), channels, frames_in);

        char err[1024] = {0};
        MH_AudioData* result = mh_audio_resample(
            interleaved.data(),
            static_cast<unsigned int>(channels),
            static_cast<unsigned int>(frames_in),
            sample_rate_in, sample_rate_out,
            err, sizeof(err));
        if (!result) {
            throw std::runtime_error(std::string(err));
        }

        unsigned int out_ch = result->channels;
        unsigned int out_frames = result->frames;

        // De-interleave resampled output to planar layout
        size_t total_out = (size_t)out_ch * out_frames;
        float* buf = new float[total_out];
        interleaved_to_planar(result->data, buf, out_ch, out_frames);
        mh_audio_data_free(result);

        nb::capsule owner(buf, [](void* p) noexcept { delete[] static_cast<float*>(p); });
        size_t shape[2] = {out_ch, out_frames};
        auto array = nb::ndarray<float, nb::numpy, nb::shape<-1, -1>>(
            buf, 2, shape, owner);

        return array;
    }, nb::arg("data"), nb::arg("sample_rate_in"), nb::arg("sample_rate_out"),
       "Resample audio data. Input shape: (channels, frames). Returns resampled array at sample_rate_out.");

    m.def("audio_get_file_info", [](const std::string& path) {
        char err[1024] = {0};
        MH_AudioFileInfo info;
        int ok = mh_audio_get_file_info(path.c_str(), &info, err, sizeof(err));
        if (!ok) {
            throw std::runtime_error(std::string(err));
        }

        nb::dict result;
        result["channels"] = info.channels;
        result["sample_rate"] = info.sample_rate;
        result["frames"] = info.frames;
        result["duration"] = info.duration;
        return result;
    }, nb::arg("path"),
       "Get audio file metadata without decoding.");
}
