#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/optional.h>
#include <nanobind/ndarray.h>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstring>

#include "minihost.h"

namespace nb = nanobind;

// Helper to convert numpy arrays to raw pointers
using AudioArray = nb::ndarray<float, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>;

// Python wrapper class for MH_Plugin
class Plugin {
public:
    Plugin(const std::string& path, double sample_rate, int max_block_size,
           int in_channels = 2, int out_channels = 2)
        : sample_rate_(sample_rate), max_block_size_(max_block_size)
    {
        char err[1024] = {0};
        plugin_ = mh_open(path.c_str(), sample_rate, max_block_size,
                          in_channels, out_channels, err, sizeof(err));
        if (!plugin_) {
            throw std::runtime_error(std::string("Failed to open plugin: ") + err);
        }
    }

    ~Plugin() {
        if (plugin_) {
            mh_close(plugin_);
            plugin_ = nullptr;
        }
    }

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
        d["label"] = std::string(info.label);
        d["current_value_str"] = std::string(info.current_value_str);
        d["min_value"] = info.min_value;
        d["max_value"] = info.max_value;
        d["default_value"] = info.default_value;
        d["num_steps"] = info.num_steps;
        d["is_automatable"] = info.is_automatable != 0;
        d["is_boolean"] = info.is_boolean != 0;
        return d;
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

    // Process audio (simple version - no MIDI)
    void process(AudioArray input, AudioArray output) {
        int in_channels = static_cast<int>(input.shape(0));
        int out_channels = static_cast<int>(output.shape(0));
        int in_frames = static_cast<int>(input.shape(1));
        int out_frames = static_cast<int>(output.shape(1));

        if (in_frames != out_frames) {
            throw std::runtime_error("Input and output frame counts must match");
        }
        if (in_frames > max_block_size_) {
            throw std::runtime_error("Frame count exceeds max block size");
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

        if (in_frames != out_frames) {
            throw std::runtime_error("Input and output frame counts must match");
        }
        if (in_frames > max_block_size_) {
            throw std::runtime_error("Frame count exceeds max block size");
        }

        // Convert MIDI input
        std::vector<MH_MidiEvent> midi_events;
        for (size_t i = 0; i < nb::len(midi_in); ++i) {
            nb::tuple ev = nb::cast<nb::tuple>(midi_in[i]);
            MH_MidiEvent e;
            e.sample_offset = nb::cast<int>(ev[0]);
            e.status = nb::cast<unsigned char>(ev[1]);
            e.data1 = nb::cast<unsigned char>(ev[2]);
            e.data2 = nb::cast<unsigned char>(ev[3]);
            midi_events.push_back(e);
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

        // Output MIDI buffer
        std::vector<MH_MidiEvent> midi_out(256);
        int num_midi_out = 0;

        if (!mh_process_midi_io(plugin_, in_ptrs.data(), out_ptrs.data(), in_frames,
                                midi_events.data(), static_cast<int>(midi_events.size()),
                                midi_out.data(), 256, &num_midi_out)) {
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

        if (in_frames != out_frames) {
            throw std::runtime_error("Input and output frame counts must match");
        }
        if (in_frames > max_block_size_) {
            throw std::runtime_error("Frame count exceeds max block size");
        }

        // Convert MIDI input
        std::vector<MH_MidiEvent> midi_events;
        for (size_t i = 0; i < nb::len(midi_in); ++i) {
            nb::tuple ev = nb::cast<nb::tuple>(midi_in[i]);
            MH_MidiEvent e;
            e.sample_offset = nb::cast<int>(ev[0]);
            e.status = nb::cast<unsigned char>(ev[1]);
            e.data1 = nb::cast<unsigned char>(ev[2]);
            e.data2 = nb::cast<unsigned char>(ev[3]);
            midi_events.push_back(e);
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

        // Output MIDI buffer
        std::vector<MH_MidiEvent> midi_out(256);
        int num_midi_out = 0;

        if (!mh_process_auto(plugin_, in_ptrs.data(), out_ptrs.data(), in_frames,
                             midi_events.data(), static_cast<int>(midi_events.size()),
                             midi_out.data(), 256, &num_midi_out,
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

private:
    MH_Plugin* plugin_ = nullptr;
    double sample_rate_;
    int max_block_size_;
};


NB_MODULE(_core, m) {
    m.doc() = "minihost - Python bindings for audio plugin hosting";

    nb::class_<Plugin>(m, "Plugin")
        .def(nb::init<const std::string&, double, int, int, int>(),
             nb::arg("path"),
             nb::arg("sample_rate") = 48000.0,
             nb::arg("max_block_size") = 512,
             nb::arg("in_channels") = 2,
             nb::arg("out_channels") = 2,
             "Open an audio plugin (VST3 or AudioUnit)")

        // Properties
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

        // State
        .def("get_state", &Plugin::get_state,
             "Get plugin state as bytes")
        .def("set_state", &Plugin::set_state,
             nb::arg("data"),
             "Restore plugin state from bytes")

        // Bypass
        .def_prop_rw("bypass", &Plugin::get_bypass, &Plugin::set_bypass,
                     "Bypass state")

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
             "Process audio with MIDI. midi_in: list of (sample_offset, status, data1, data2)")
        .def("process_auto", &Plugin::process_auto,
             nb::arg("input"), nb::arg("output"), nb::arg("midi_in"), nb::arg("param_changes"),
             "Process with sample-accurate automation. param_changes: list of (sample_offset, param_index, value)");
}
