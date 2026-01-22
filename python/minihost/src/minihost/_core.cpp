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

    int sidechain_channels() const {
        return mh_get_sidechain_channels(plugin_);
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

private:
    MH_Plugin* plugin_ = nullptr;
    double sample_rate_;
    int max_block_size_;
    bool non_realtime_ = false;
};


// Module-level probe function
nb::dict probe_plugin(const std::string& path) {
    MH_PluginDesc desc;
    char err[1024] = {0};

    if (!mh_probe(path.c_str(), &desc, err, sizeof(err))) {
        throw std::runtime_error(std::string("Failed to probe plugin: ") + err);
    }

    nb::dict d;
    d["name"] = std::string(desc.name);
    d["vendor"] = std::string(desc.vendor);
    d["version"] = std::string(desc.version);
    d["format"] = std::string(desc.format);
    d["unique_id"] = std::string(desc.unique_id);
    d["accepts_midi"] = desc.accepts_midi != 0;
    d["produces_midi"] = desc.produces_midi != 0;
    d["num_inputs"] = desc.num_inputs;
    d["num_outputs"] = desc.num_outputs;
    return d;
}

NB_MODULE(_core, m) {
    m.doc() = "minihost - Python bindings for audio plugin hosting";

    // Module-level function
    m.def("probe", &probe_plugin,
          nb::arg("path"),
          "Get plugin metadata without full instantiation");

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
             "Process audio with MIDI. midi_in: list of (sample_offset, status, data1, data2)")
        .def("process_auto", &Plugin::process_auto,
             nb::arg("input"), nb::arg("output"), nb::arg("midi_in"), nb::arg("param_changes"),
             "Process with sample-accurate automation. param_changes: list of (sample_offset, param_index, value)")
        .def("process_sidechain", &Plugin::process_sidechain,
             nb::arg("main_in"), nb::arg("main_out"), nb::arg("sidechain_in"),
             "Process audio with sidechain input (all arrays shape: [channels, frames])");
}
