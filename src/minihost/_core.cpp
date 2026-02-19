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
#include "minihost_chain.h"
#include "minihost_audio.h"
#include "minihost_audiofile.h"
#include "minihost_midi.h"
#include "MidiFile.h"

namespace nb = nanobind;

// Helper to convert numpy arrays to raw pointers
using AudioArray = nb::ndarray<float, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>;
using DoubleAudioArray = nb::ndarray<double, nb::shape<-1, -1>, nb::c_contig, nb::device::cpu>;

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
            // Clear callbacks before closing to avoid dangling pointers
            mh_set_change_callback(plugin_, nullptr, nullptr);
            mh_set_param_value_callback(plugin_, nullptr, nullptr);
            mh_set_param_gesture_callback(plugin_, nullptr, nullptr);
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

        if (in_frames != out_frames) {
            throw std::runtime_error("Input and output frame counts must match");
        }
        if (in_frames > max_block_size_) {
            throw std::runtime_error("Frame count exceeds max block size");
        }

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

private:
    MH_Plugin* plugin_ = nullptr;
    double sample_rate_;
    int max_block_size_;
    bool non_realtime_ = false;

    // Python callback holders (prevent GC)
    nb::object change_callback_;
    nb::object param_value_callback_;
    nb::object param_gesture_callback_;

    // Static trampoline functions for C callbacks
    static void change_callback_trampoline(MH_Plugin*, int flags, void* user_data) {
        auto* self = static_cast<Plugin*>(user_data);
        nb::gil_scoped_acquire gil;
        if (self->change_callback_.is_valid() && !self->change_callback_.is_none())
            self->change_callback_(flags);
    }

    static void param_value_callback_trampoline(MH_Plugin*, int param_index, float new_value, void* user_data) {
        auto* self = static_cast<Plugin*>(user_data);
        nb::gil_scoped_acquire gil;
        if (self->param_value_callback_.is_valid() && !self->param_value_callback_.is_none())
            self->param_value_callback_(param_index, new_value);
    }

    static void param_gesture_callback_trampoline(MH_Plugin*, int param_index, int gesture_starting, void* user_data) {
        auto* self = static_cast<Plugin*>(user_data);
        nb::gil_scoped_acquire gil;
        if (self->param_gesture_callback_.is_valid() && !self->param_gesture_callback_.is_none())
            self->param_gesture_callback_(param_index, gesture_starting != 0);
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
        if (chain_) {
            mh_chain_close(chain_);
            chain_ = nullptr;
        }
    }

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

        if (in_frames != out_frames) {
            throw std::runtime_error("Input and output frame counts must match");
        }

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

        if (in_frames != out_frames) {
            throw std::runtime_error("Input and output frame counts must match");
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

        if (!mh_chain_process_midi_io(chain_, in_ptrs.data(), out_ptrs.data(), in_frames,
                                       midi_events.data(), static_cast<int>(midi_events.size()),
                                       midi_out.data(), 256, &num_midi_out)) {
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
                int output_channels = 0, int midi_input_port = -1, int midi_output_port = -1)
        : plugin_ref_(&plugin), chain_ref_(nullptr)
    {
        MH_AudioConfig config;
        config.sample_rate = sample_rate;
        config.buffer_frames = buffer_frames;
        config.output_channels = output_channels;
        config.midi_input_port = midi_input_port;
        config.midi_output_port = midi_output_port;

        char err[1024] = {0};
        device_ = mh_audio_open(plugin.plugin_, &config, err, sizeof(err));
        if (!device_) {
            throw std::runtime_error(std::string("Failed to open audio device: ") + err);
        }
    }

    // Constructor for plugin chain
    AudioDevice(PluginChain& chain, double sample_rate = 0, int buffer_frames = 0,
                int output_channels = 0, int midi_input_port = -1, int midi_output_port = -1)
        : plugin_ref_(nullptr), chain_ref_(&chain)
    {
        MH_AudioConfig config;
        config.sample_rate = sample_rate;
        config.buffer_frames = buffer_frames;
        config.output_channels = output_channels;
        config.midi_input_port = midi_input_port;
        config.midi_output_port = midi_output_port;

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

// Callback context for scan_directory
struct ScanContext {
    std::vector<nb::dict>* results;
};

static void scan_callback(const MH_PluginDesc* desc, void* user_data) {
    auto* ctx = static_cast<ScanContext*>(user_data);

    nb::dict d;
    d["name"] = std::string(desc->name);
    d["vendor"] = std::string(desc->vendor);
    d["version"] = std::string(desc->version);
    d["format"] = std::string(desc->format);
    d["unique_id"] = std::string(desc->unique_id);
    d["path"] = std::string(desc->path);
    d["accepts_midi"] = desc->accepts_midi != 0;
    d["produces_midi"] = desc->produces_midi != 0;
    d["num_inputs"] = desc->num_inputs;
    d["num_outputs"] = desc->num_outputs;

    ctx->results->push_back(d);
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

    // Change notification flag constants
    m.attr("MH_CHANGE_LATENCY")         = MH_CHANGE_LATENCY;
    m.attr("MH_CHANGE_PARAM_INFO")      = MH_CHANGE_PARAM_INFO;
    m.attr("MH_CHANGE_PROGRAM")         = MH_CHANGE_PROGRAM;
    m.attr("MH_CHANGE_NON_PARAM_STATE") = MH_CHANGE_NON_PARAM_STATE;

    // Processing precision constants
    m.attr("MH_PRECISION_SINGLE") = MH_PRECISION_SINGLE;
    m.attr("MH_PRECISION_DOUBLE") = MH_PRECISION_DOUBLE;

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

        // Change notification callbacks
        .def("set_change_callback", &Plugin::set_change_callback,
             nb::arg("callback").none(),
             "Register callback for processor-level changes (latency, param info, program). "
             "Callback receives (flags: int). Pass None to clear.")
        .def("set_param_value_callback", &Plugin::set_param_value_callback,
             nb::arg("callback").none(),
             "Register callback for plugin-initiated parameter changes. "
             "Callback receives (param_index: int, new_value: float). Pass None to clear.")
        .def("set_param_gesture_callback", &Plugin::set_param_gesture_callback,
             nb::arg("callback").none(),
             "Register callback for parameter gesture begin/end from plugin UI. "
             "Callback receives (param_index: int, gesture_starting: bool). Pass None to clear.");

    // PluginChain class for chaining multiple plugins
    nb::class_<PluginChain>(m, "PluginChain")
        .def(nb::init<nb::list>(),
             nb::arg("plugins"),
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
             "Process audio with MIDI (to first plugin). midi_in: list of (sample_offset, status, data1, data2)");

    // AudioDevice class for real-time playback
    nb::class_<AudioDevice>(m, "AudioDevice")
        .def(nb::init<Plugin&, double, int, int, int, int>(),
             nb::arg("plugin"),
             nb::arg("sample_rate") = 0.0,
             nb::arg("buffer_frames") = 0,
             nb::arg("output_channels") = 0,
             nb::arg("midi_input_port") = -1,
             nb::arg("midi_output_port") = -1,
             "Open an audio device for real-time playback with a single plugin. "
             "sample_rate=0 uses device default. "
             "buffer_frames=0 uses auto (~256-512). "
             "output_channels=0 uses plugin's output channels. "
             "midi_input_port=-1 means no MIDI input. "
             "midi_output_port=-1 means no MIDI output.")
        .def(nb::init<PluginChain&, double, int, int, int, int>(),
             nb::arg("chain"),
             nb::arg("sample_rate") = 0.0,
             nb::arg("buffer_frames") = 0,
             nb::arg("output_channels") = 0,
             nb::arg("midi_input_port") = -1,
             nb::arg("midi_output_port") = -1,
             "Open an audio device for real-time playback with a plugin chain. "
             "sample_rate=0 uses device default. "
             "buffer_frames=0 uses auto (~256-512). "
             "output_channels=0 uses chain's output channels. "
             "midi_input_port=-1 means no MIDI input. "
             "midi_output_port=-1 means no MIDI output.")

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

        // Create numpy array shape (channels, frames) from interleaved data
        size_t total_samples = (size_t)channels * frames;
        float* buf = new float[total_samples];

        // De-interleave: interleaved [L0,R0,L1,R1,...] -> planar [L0,L1,...,R0,R1,...]
        for (unsigned int ch = 0; ch < channels; ch++) {
            for (unsigned int f = 0; f < frames; f++) {
                buf[ch * frames + f] = data->data[f * channels + ch];
            }
        }

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

        // Interleave: planar [L0,L1,...,R0,R1,...] -> interleaved [L0,R0,L1,R1,...]
        size_t total_samples = channels * frames;
        std::vector<float> interleaved(total_samples);
        const float* src = data.data();

        for (size_t f = 0; f < frames; f++) {
            for (size_t ch = 0; ch < channels; ch++) {
                interleaved[f * channels + ch] = src[ch * frames + f];
            }
        }

        char err[1024] = {0};
        int ok = mh_audio_write(path.c_str(), interleaved.data(),
                                (unsigned int)channels, (unsigned int)frames,
                                sample_rate, bit_depth, err, sizeof(err));
        if (!ok) {
            throw std::runtime_error(std::string(err));
        }
    }, nb::arg("path"), nb::arg("data"), nb::arg("sample_rate"), nb::arg("bit_depth") = 24,
       "Write audio data to a WAV file. Data shape: (channels, frames).");

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
