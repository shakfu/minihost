// minihost_midi.cpp
// MIDI port enumeration and I/O implementation using libremidi

#include "minihost_midi.h"
#include "midi_ringbuffer.h"
#include "midi_message_length.h"

#include <atomic>
#include <new>
#include <chrono>
#include <libremidi/libremidi.hpp>
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <memory>
#include <thread>
#include <exception>
#include <typeinfo>
#if defined(__GNUC__) || defined(__clang__)
#include <cxxabi.h>
#endif

// Global observer for port enumeration (lazy initialized)
static std::unique_ptr<libremidi::observer> g_observer;
static std::mutex g_observer_mutex;

// Observer configuration.
//
// libremidi's defaults are track_hardware=true, track_virtual=false,
// track_any=false. Its per-backend classifier treats an endpoint with no
// hardware entity as "virtual" (see backends/coremidi/observer.hpp
// to_port_info), which covers exactly the ports a host most needs to see:
// macOS IAC buses, ALSA/JACK software ports, and the virtual endpoints
// published by other applications (DAWs, Max/MSP, ...). With the defaults
// those are all filtered out and enumeration returns an empty list even
// though the platform reports the ports. Track everything and let the caller
// decide.
//
// No port-added/removed callbacks are installed, so observer_configuration::
// has_callbacks() stays false and the backends skip creating a notification
// client entirely -- enumeration is a direct platform query each call.
static libremidi::observer_configuration make_observer_config() {
    libremidi::observer_configuration cfg;
    cfg.track_hardware = true;
    cfg.track_virtual = true;
    cfg.track_any = true;
    cfg.notify_in_constructor = false;
    return cfg;
}

static libremidi::observer& get_observer() {
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    if (!g_observer) {
        g_observer = std::make_unique<libremidi::observer>(make_observer_config());
    }
    return *g_observer;
}

// ---------------------------------------------------------------------------
// libremidi calls, isolated from the caller's run loop.
//
// Several libremidi CoreMIDI entry points pump the *calling thread's*
// CoreFoundation run loop before doing their work:
//
//     CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
//
// It appears in observer::get_input_ports / get_output_ports, and in
// midi_in::open_port and midi_out::open_port (plus their UMP variants).
//
// On an application's main thread -- which in a plugin host is also driving
// JUCE, CoreAudio and plugin code -- that single pass dispatches whatever those
// subsystems have queued, including work whose owner has already been torn
// down. Two failure modes were observed, both only after a process had been
// running long enough to accumulate queued work:
//
//   * enumeration crashed the process (SIGABRT / SIGBUS). Uncatchable, because
//     get_input_ports() is noexcept, so a throw inside it reaches
//     std::terminate before any handler here can unwind.
//   * open_port threw std::bad_function_call -- some unrelated queued callback
//     holding an empty std::function -- surfacing as a spurious
//     "failed to open MIDI port" long after the port itself was fine.
//
// Running these calls on a fresh thread sidesteps both: that thread's run loop
// has nothing queued, so the pump is a no-op and only the CoreMIDI work runs.
// Message delivery is unaffected -- CoreMIDI invokes MIDIReadProc on its own
// thread, not on the run loop of whichever thread created the port.
//
// These are all control-plane operations (never the audio thread), so a
// short-lived thread per call is cheap enough, and -- unlike a persistent
// worker -- adds no process-exit teardown ordering to get wrong.
//
// On ALSA/WinMM the pump does not exist and this is simply a thread hop.
// ---------------------------------------------------------------------------
template <typename Fn>
static auto run_isolated(Fn&& fn) -> decltype(fn()) {
    using R = decltype(fn());
    R result{};
    std::exception_ptr thrown;
    std::thread worker([&] {
        try {
            result = fn();
        } catch (...) {
            // Carry it back to the caller rather than letting it escape the
            // thread (which would call std::terminate).
            thrown = std::current_exception();
        }
    });
    worker.join();
    if (thrown)
        std::rethrow_exception(thrown);
    return result;
}

static std::vector<libremidi::input_port> enumerate_input_ports() {
    return run_isolated([] { return get_observer().get_input_ports(); });
}

static std::vector<libremidi::output_port> enumerate_output_ports() {
    return run_isolated([] { return get_observer().get_output_ports(); });
}

// Copy a message into a caller-supplied error buffer, always NUL-terminated.
static void set_err(char* buf, size_t n, const char* msg) {
    if (!buf || n == 0) return;
    std::snprintf(buf, n, "%s", msg ? msg : "");
}

// Describe the in-flight exception into `buf`.
//
// libremidi reports failures with stdx::error (its bundled system_error2),
// which deliberately does NOT derive from std::exception -- so a plain
// catch(const std::exception&) misses it entirely and the catch(...) fallback
// used to discard the reason as "Unknown error". That made every MIDI open
// failure undiagnosable. Handle stdx::error explicitly, and for anything else
// at least name the dynamic type so an unexpected throw is traceable.
//
// Call only from inside a catch block.
static void describe_current_exception(char* buf, size_t n) {
    try {
        throw;
    } catch (const stdx::error& e) {
        const auto msg = e.message();
        if (buf && n > 0)
            std::snprintf(buf, n, "%.*s", static_cast<int>(msg.size()), msg.data());
    } catch (const std::exception& e) {
        set_err(buf, n, e.what());
    } catch (...) {
#if defined(__GNUC__) || defined(__clang__)
        if (const std::type_info* t = abi::__cxa_current_exception_type()) {
            if (buf && n > 0)
                std::snprintf(buf, n, "unrecognized exception of type '%s'", t->name());
            return;
        }
#endif
        set_err(buf, n, "Unknown error");
    }
}

// MIDI input wrapper
struct MH_MidiIn {
    std::unique_ptr<libremidi::midi_in> midi_in;
    MH_MidiCallback callback;
    void* user_data;
};

// MIDI output wrapper
struct MH_MidiOut {
    std::unique_ptr<libremidi::midi_out> midi_out;
};

extern "C" {

int mh_midi_enumerate_inputs(MH_MidiPortCallback callback, void* user_data) {
    if (!callback) return -1;

    try {
        auto ports = enumerate_input_ports();

        int index = 0;
        for (const auto& port : ports) {
            MH_MidiPortInfo info;
            std::memset(&info, 0, sizeof(info));
            std::strncpy(info.name, port.port_name.c_str(), sizeof(info.name) - 1);
            info.index = index;
            callback(&info, user_data);
            index++;
        }
        return static_cast<int>(ports.size());
    } catch (...) {
        return -1;
    }
}

int mh_midi_enumerate_outputs(MH_MidiPortCallback callback, void* user_data) {
    if (!callback) return -1;

    try {
        auto ports = enumerate_output_ports();

        int index = 0;
        for (const auto& port : ports) {
            MH_MidiPortInfo info;
            std::memset(&info, 0, sizeof(info));
            std::strncpy(info.name, port.port_name.c_str(), sizeof(info.name) - 1);
            info.index = index;
            callback(&info, user_data);
            index++;
        }
        return static_cast<int>(ports.size());
    } catch (...) {
        return -1;
    }
}

int mh_midi_get_num_inputs(void) {
    try {
        return static_cast<int>(enumerate_input_ports().size());
    } catch (...) {
        return 0;
    }
}

int mh_midi_get_num_outputs(void) {
    try {
        return static_cast<int>(enumerate_output_ports().size());
    } catch (...) {
        return 0;
    }
}

int mh_midi_get_input_name(int index, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0 || index < 0) return 0;

    try {
        auto ports = enumerate_input_ports();

        if (index >= static_cast<int>(ports.size())) return 0;

        std::strncpy(buf, ports[index].port_name.c_str(), buf_size - 1);
        buf[buf_size - 1] = '\0';
        return 1;
    } catch (...) {
        return 0;
    }
}

int mh_midi_get_output_name(int index, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0 || index < 0) return 0;

    try {
        auto ports = enumerate_output_ports();

        if (index >= static_cast<int>(ports.size())) return 0;

        std::strncpy(buf, ports[index].port_name.c_str(), buf_size - 1);
        buf[buf_size - 1] = '\0';
        return 1;
    } catch (...) {
        return 0;
    }
}

MH_MidiIn* mh_midi_in_open(int port_index, MH_MidiCallback callback, void* user_data,
                           char* err_buf, size_t err_buf_size) {
    if (!callback) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Callback is required", err_buf_size - 1);
        }
        return nullptr;
    }

    try {
        auto ports = enumerate_input_ports();

        if (port_index < 0 || port_index >= static_cast<int>(ports.size())) {
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size, "Invalid port index: %d", port_index);
            }
            return nullptr;
        }

        auto* midi_in = new MH_MidiIn();
        midi_in->callback = callback;
        midi_in->user_data = user_data;

        libremidi::input_configuration config;
        config.on_message = [midi_in](const libremidi::message& msg) {
            if (midi_in->callback && !msg.bytes.empty()) {
                midi_in->callback(msg.bytes.data(), msg.bytes.size(), midi_in->user_data);
            }
        };

        // Construct + open on an isolated thread -- open_port() pumps the
        // caller's run loop (see run_isolated).
        const bool opened = run_isolated([&]() -> bool {
            midi_in->midi_in = std::make_unique<libremidi::midi_in>(config);
            return midi_in->midi_in->open_port(ports[port_index]) == stdx::error{};
        });
        if (!opened) {
            set_err(err_buf, err_buf_size, "Failed to open MIDI input port");
            delete midi_in;
            return nullptr;
        }

        return midi_in;
    } catch (...) {
        describe_current_exception(err_buf, err_buf_size);
        return nullptr;
    }
}

MH_MidiIn* mh_midi_in_open_virtual(const char* port_name, MH_MidiCallback callback, void* user_data,
                                    char* err_buf, size_t err_buf_size) {
    if (!callback) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Callback is required", err_buf_size - 1);
        }
        return nullptr;
    }

    if (!port_name || port_name[0] == '\0') {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Port name is required", err_buf_size - 1);
        }
        return nullptr;
    }

    try {
        auto* midi_in = new MH_MidiIn();
        midi_in->callback = callback;
        midi_in->user_data = user_data;

        libremidi::input_configuration config;
        config.on_message = [midi_in](const libremidi::message& msg) {
            if (midi_in->callback && !msg.bytes.empty()) {
                midi_in->callback(msg.bytes.data(), msg.bytes.size(), midi_in->user_data);
            }
        };

        const bool opened = run_isolated([&]() -> bool {
            midi_in->midi_in = std::make_unique<libremidi::midi_in>(config);
            return midi_in->midi_in->open_virtual_port(port_name) == stdx::error{};
        });
        if (!opened) {
            set_err(err_buf, err_buf_size,
                    "Failed to open virtual MIDI input port "
                    "(may not be supported on this platform)");
            delete midi_in;
            return nullptr;
        }

        return midi_in;
    } catch (...) {
        describe_current_exception(err_buf, err_buf_size);
        return nullptr;
    }
}

void mh_midi_in_close(MH_MidiIn* midi_in) {
    if (!midi_in) return;

    if (midi_in->midi_in) {
        midi_in->midi_in->close_port();
    }
    delete midi_in;
}

MH_MidiOut* mh_midi_out_open(int port_index, char* err_buf, size_t err_buf_size) {
    try {
        auto ports = enumerate_output_ports();

        if (port_index < 0 || port_index >= static_cast<int>(ports.size())) {
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size, "Invalid port index: %d", port_index);
            }
            return nullptr;
        }

        auto* midi_out = new MH_MidiOut();
        const bool opened = run_isolated([&]() -> bool {
            midi_out->midi_out = std::make_unique<libremidi::midi_out>();
            return midi_out->midi_out->open_port(ports[port_index]) == stdx::error{};
        });
        if (!opened) {
            set_err(err_buf, err_buf_size, "Failed to open MIDI output port");
            delete midi_out;
            return nullptr;
        }

        return midi_out;
    } catch (...) {
        describe_current_exception(err_buf, err_buf_size);
        return nullptr;
    }
}

MH_MidiOut* mh_midi_out_open_virtual(const char* port_name, char* err_buf, size_t err_buf_size) {
    if (!port_name || port_name[0] == '\0') {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Port name is required", err_buf_size - 1);
        }
        return nullptr;
    }

    try {
        auto* midi_out = new MH_MidiOut();
        const bool opened = run_isolated([&]() -> bool {
            midi_out->midi_out = std::make_unique<libremidi::midi_out>();
            return midi_out->midi_out->open_virtual_port(port_name) == stdx::error{};
        });
        if (!opened) {
            set_err(err_buf, err_buf_size,
                    "Failed to open virtual MIDI output port "
                    "(may not be supported on this platform)");
            delete midi_out;
            return nullptr;
        }

        return midi_out;
    } catch (...) {
        describe_current_exception(err_buf, err_buf_size);
        return nullptr;
    }
}

void mh_midi_out_close(MH_MidiOut* midi_out) {
    if (!midi_out) return;

    if (midi_out->midi_out) {
        midi_out->midi_out->close_port();
    }
    delete midi_out;
}

int mh_midi_out_send(MH_MidiOut* midi_out, const unsigned char* data, size_t len) {
    if (!midi_out || !midi_out->midi_out || !data || len == 0) return 0;

    try {
        auto err = midi_out->midi_out->send_message(data, len);
        return err == stdx::error{} ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

// -- MIDI output pump ---------------------------------------------------
//
// The audio callback used to call mh_midi_out_send() per event, which enters
// libremidi (locks, allocation, a write() on some backends) on the thread
// that must never block. The callback now pushes into an SPSC ring and this
// thread does the sending.

struct MH_MidiOutPump {
    MH_MidiOut*        out  = nullptr;
    MH_MidiRingBuffer* ring = nullptr;
    std::atomic<bool>  running{ false };
    std::thread        thread;
};

static void midi_out_pump_drain(MH_MidiOutPump* pump) {
    MH_MidiEvent events[64];
    for (;;) {
        int n = mh_midi_ringbuffer_pop_all(pump->ring, events, 64);
        if (n <= 0) return;
        for (int i = 0; i < n; ++i) {
            const unsigned char msg[3] = { events[i].status,
                                           events[i].data1,
                                           events[i].data2 };
            // Length comes from the status byte: a fixed 3 appends data2 to a
            // Program Change or Channel Pressure, and two zero bytes to a
            // real-time message, which the receiver reads as the start of the
            // next message. A status with no 3-byte form (System Exclusive,
            // which MH_MidiEvent cannot carry) is dropped, not truncated.
            const int len = mh_midi_message_length(msg[0]);
            if (len > 0)
                mh_midi_out_send(pump->out, msg, (size_t)len);
        }
    }
}

MH_MidiOutPump* mh_midi_out_pump_start(MH_MidiOut* out, struct MH_MidiRingBuffer* ring) {
    if (!out || !ring) return nullptr;

    auto* pump = new (std::nothrow) MH_MidiOutPump();
    if (!pump) return nullptr;
    pump->out  = out;
    pump->ring = ring;
    pump->running.store(true, std::memory_order_release);

    try {
        pump->thread = std::thread([pump]() {
            while (pump->running.load(std::memory_order_acquire)) {
                midi_out_pump_drain(pump);
                std::this_thread::sleep_for(
                    std::chrono::microseconds(MH_MIDI_PUMP_POLL_US));
            }
            // Whatever the audio thread queued before the stop still goes out.
            midi_out_pump_drain(pump);
        });
    } catch (...) {
        delete pump;
        return nullptr;
    }
    return pump;
}

void mh_midi_out_pump_stop(MH_MidiOutPump* pump) {
    if (!pump) return;
    pump->running.store(false, std::memory_order_release);
    if (pump->thread.joinable())
        pump->thread.join();
    delete pump;
}

}  // extern "C"
