// minihost_midi.cpp
// MIDI port enumeration and I/O implementation using libremidi

#include "minihost_midi.h"

#include <libremidi/libremidi.hpp>
#include <cstring>
#include <vector>
#include <mutex>
#include <memory>

// Global observer for port enumeration (lazy initialized)
static std::unique_ptr<libremidi::observer> g_observer;
static std::mutex g_observer_mutex;

static libremidi::observer& get_observer() {
    std::lock_guard<std::mutex> lock(g_observer_mutex);
    if (!g_observer) {
        g_observer = std::make_unique<libremidi::observer>();
    }
    return *g_observer;
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
        auto& obs = get_observer();
        auto ports = obs.get_input_ports();

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
        auto& obs = get_observer();
        auto ports = obs.get_output_ports();

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
        auto& obs = get_observer();
        return static_cast<int>(obs.get_input_ports().size());
    } catch (...) {
        return 0;
    }
}

int mh_midi_get_num_outputs(void) {
    try {
        auto& obs = get_observer();
        return static_cast<int>(obs.get_output_ports().size());
    } catch (...) {
        return 0;
    }
}

int mh_midi_get_input_name(int index, char* buf, size_t buf_size) {
    if (!buf || buf_size == 0 || index < 0) return 0;

    try {
        auto& obs = get_observer();
        auto ports = obs.get_input_ports();

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
        auto& obs = get_observer();
        auto ports = obs.get_output_ports();

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
        auto& obs = get_observer();
        auto ports = obs.get_input_ports();

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

        midi_in->midi_in = std::make_unique<libremidi::midi_in>(config);

        auto err = midi_in->midi_in->open_port(ports[port_index]);
        if (err != stdx::error{}) {
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size, "Failed to open MIDI input port");
            }
            delete midi_in;
            return nullptr;
        }

        return midi_in;
    } catch (const std::exception& e) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, e.what(), err_buf_size - 1);
        }
        return nullptr;
    } catch (...) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Unknown error", err_buf_size - 1);
        }
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

        midi_in->midi_in = std::make_unique<libremidi::midi_in>(config);

        auto err = midi_in->midi_in->open_virtual_port(port_name);
        if (err != stdx::error{}) {
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size, "Failed to open virtual MIDI input port (may not be supported on this platform)");
            }
            delete midi_in;
            return nullptr;
        }

        return midi_in;
    } catch (const std::exception& e) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, e.what(), err_buf_size - 1);
        }
        return nullptr;
    } catch (...) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Unknown error", err_buf_size - 1);
        }
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
        auto& obs = get_observer();
        auto ports = obs.get_output_ports();

        if (port_index < 0 || port_index >= static_cast<int>(ports.size())) {
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size, "Invalid port index: %d", port_index);
            }
            return nullptr;
        }

        auto* midi_out = new MH_MidiOut();
        midi_out->midi_out = std::make_unique<libremidi::midi_out>();

        auto err = midi_out->midi_out->open_port(ports[port_index]);
        if (err != stdx::error{}) {
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size, "Failed to open MIDI output port");
            }
            delete midi_out;
            return nullptr;
        }

        return midi_out;
    } catch (const std::exception& e) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, e.what(), err_buf_size - 1);
        }
        return nullptr;
    } catch (...) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Unknown error", err_buf_size - 1);
        }
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
        midi_out->midi_out = std::make_unique<libremidi::midi_out>();

        auto err = midi_out->midi_out->open_virtual_port(port_name);
        if (err != stdx::error{}) {
            if (err_buf && err_buf_size > 0) {
                std::snprintf(err_buf, err_buf_size, "Failed to open virtual MIDI output port (may not be supported on this platform)");
            }
            delete midi_out;
            return nullptr;
        }

        return midi_out;
    } catch (const std::exception& e) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, e.what(), err_buf_size - 1);
        }
        return nullptr;
    } catch (...) {
        if (err_buf && err_buf_size > 0) {
            std::strncpy(err_buf, "Unknown error", err_buf_size - 1);
        }
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

}  // extern "C"
