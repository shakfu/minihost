// minihost_osc.cpp
// OSC input and output implementation using juce_osc

#include "minihost_osc.h"

#include <juce_osc/juce_osc.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// Arguments are handed to the C callback as a flat float array. This is the
// staging buffer for one message, owned by the listener so the socket thread
// does not allocate per message once it has settled at its high-water mark.
constexpr int kInitialArgCapacity = 16;

// juce::OSCAddress and OSCAddressPattern validate in their constructors by
// throwing OSCFormatError; there is no static predicate to ask instead. So
// asking means attempting, which is why every entry point that takes an
// address funnels through here rather than repeating the try/catch.
bool is_valid_address(const char* address) {
    if (!address || *address == '\0') return false;
    try {
        juce::OSCAddressPattern pattern{juce::String(address)};
        juce::ignoreUnused(pattern);
        return true;
    } catch (const juce::OSCException&) {
        return false;
    }
}

float arg_to_float(const juce::OSCArgument& arg) {
    if (arg.isFloat32()) return arg.getFloat32();
    if (arg.isInt32())   return static_cast<float>(arg.getInt32());
    // Strings, blobs and anything else report as 0 rather than being dropped,
    // so an argument index means the same thing to sender and receiver.
    return 0.0f;
}

class Listener : public juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback> {
public:
    Listener(MH_OscCallback cb, void* user_data)
        : callback_(cb), user_data_(user_data) {
        args_.reserve(kInitialArgCapacity);
    }

    void oscMessageReceived(const juce::OSCMessage& message) override {
        dispatch(message);
    }

    void oscBundleReceived(const juce::OSCBundle& bundle) override {
        // Delivered immediately, time tag ignored -- see the header.
        for (const auto& element : bundle) {
            if (element.isMessage())
                dispatch(element.getMessage());
            else if (element.isBundle())
                oscBundleReceived(element.getBundle());
        }
    }

private:
    void dispatch(const juce::OSCMessage& message) {
        if (!callback_) return;

        args_.clear();
        for (const auto& arg : message)
            args_.push_back(arg_to_float(arg));

        const juce::String address = message.getAddressPattern().toString();
        callback_(address.toRawUTF8(),
                  args_.empty() ? nullptr : args_.data(),
                  static_cast<int>(args_.size()),
                  user_data_);
    }

    MH_OscCallback callback_;
    void* user_data_;
    std::vector<float> args_;
};

}  // namespace

struct MH_OscServer {
    // Bound here rather than by OSCReceiver::connect(port), which reports
    // only success and keeps the socket to itself -- so a server opened on
    // port 0 could never say which port the OS actually gave it. Binding it
    // ourselves and handing it over with connectToSocket() answers that, and
    // must outlive the receiver, hence the declaration order.
    juce::DatagramSocket socket;
    juce::OSCReceiver receiver{"minihost OSC"};
    std::unique_ptr<Listener> listener;
    int port = -1;
};

struct MH_OscClient {
    juce::OSCSender sender;
    std::string host;
    int port = 0;
};

// juce::OSCSender::send throws OSCFormatError on an address it cannot parse.
// An exception must not cross back into C, so every send funnels through here.
template <typename Fn>
static int send_guarded(MH_OscClient* client, const char* address, Fn&& fn) {
    if (!client || !address) return 0;
    if (!is_valid_address(address)) return 0;
    try {
        return fn() ? 1 : 0;
    } catch (const juce::OSCException&) {
        return 0;
    } catch (...) {
        return 0;
    }
}

static void set_error(char* err_buf, size_t err_buf_size, const char* msg) {
    if (err_buf && err_buf_size > 0) {
        std::snprintf(err_buf, err_buf_size, "%s", msg);
    }
}

extern "C" {

MH_OscServer* mh_osc_server_open(int port, MH_OscCallback callback, void* user_data,
                                 char* err_buf, size_t err_buf_size) {
    if (port < 0 || port > 65535) {
        set_error(err_buf, err_buf_size, "OSC port must be 0-65535");
        return nullptr;
    }

    auto server = std::make_unique<MH_OscServer>();
    server->listener = std::make_unique<Listener>(callback, user_data);

    // Registered before connecting so no message can arrive unobserved.
    // RealtimeCallback: delivered straight from the socket thread, bypassing
    // the JUCE message loop, which the Python wheel does not run.
    server->receiver.addListener(server->listener.get());

    // JUCE sets SO_REUSEADDR on every DatagramSocket at construction. On Linux
    // and Windows that implies port re-use, so a second bind to a port already
    // in use succeeds and the two servers split incoming messages between them.
    // Cleared before binding so an occupied port is an error on every platform.
    server->socket.setEnablePortReuse(false);

    if (!server->socket.bindToPort(port)) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
                      "Failed to bind OSC port %d (in use, or not permitted)", port);
        set_error(err_buf, err_buf_size, msg);
        return nullptr;
    }

    // Whatever the OS gave us, which is the point of binding it ourselves.
    server->port = server->socket.getBoundPort();

    if (!server->receiver.connectToSocket(server->socket)) {
        set_error(err_buf, err_buf_size, "Failed to start the OSC receive thread");
        return nullptr;
    }

    return server.release();
}

void mh_osc_server_close(MH_OscServer* server) {
    if (!server) return;
    // disconnect() joins the socket thread, so the listener cannot be running
    // by the time it returns and is safe to destroy.
    server->receiver.disconnect();
    if (server->listener)
        server->receiver.removeListener(server->listener.get());
    delete server;
}

int mh_osc_server_get_port(MH_OscServer* server) {
    return server ? server->port : -1;
}

MH_OscClient* mh_osc_client_open(const char* host, int port,
                                 char* err_buf, size_t err_buf_size) {
    if (!host || *host == '\0') {
        set_error(err_buf, err_buf_size, "OSC host must not be empty");
        return nullptr;
    }
    if (port < 1 || port > 65535) {
        set_error(err_buf, err_buf_size, "OSC port must be 1-65535");
        return nullptr;
    }

    auto client = std::make_unique<MH_OscClient>();
    if (!client->sender.connect(juce::String(host), port)) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
                      "Failed to open OSC sender to %s:%d", host, port);
        set_error(err_buf, err_buf_size, msg);
        return nullptr;
    }
    client->host = host;
    client->port = port;
    return client.release();
}

void mh_osc_client_close(MH_OscClient* client) {
    if (!client) return;
    client->sender.disconnect();
    delete client;
}

int mh_osc_send_float(MH_OscClient* client, const char* address, float value) {
    return send_guarded(client, address, [&] {
        return client->sender.send(juce::OSCAddressPattern(address), value);
    });
}

int mh_osc_send_int(MH_OscClient* client, const char* address, int value) {
    return send_guarded(client, address, [&] {
        return client->sender.send(juce::OSCAddressPattern(address), value);
    });
}

int mh_osc_send_string(MH_OscClient* client, const char* address, const char* value) {
    if (!value) return 0;
    return send_guarded(client, address, [&] {
        return client->sender.send(juce::OSCAddressPattern(address),
                                   juce::String::fromUTF8(value));
    });
}

int mh_osc_send_bang(MH_OscClient* client, const char* address) {
    return send_guarded(client, address, [&] {
        return client->sender.send(juce::OSCMessage(juce::OSCAddressPattern(address)));
    });
}

int mh_osc_send_floats(MH_OscClient* client, const char* address,
                       const float* values, int num_values) {
    if (num_values < 0) return 0;
    if (num_values > 0 && !values) return 0;
    return send_guarded(client, address, [&] {
        juce::OSCMessage message{juce::OSCAddressPattern(address)};
        for (int i = 0; i < num_values; ++i)
            message.addFloat32(values[i]);
        return client->sender.send(message);
    });
}

int mh_osc_address_matches(const char* pattern, const char* address) {
    if (!pattern || !address) return 0;
    try {
        juce::OSCAddressPattern p{juce::String(pattern)};
        juce::OSCAddress a{juce::String(address)};
        return p.matches(a) ? 1 : 0;
    } catch (const juce::OSCException&) {
        // A malformed pattern or a concrete address that is not one (an
        // address may not contain wildcards) matches nothing.
        return 0;
    }
}

int mh_osc_is_valid_address(const char* address) {
    return is_valid_address(address) ? 1 : 0;
}

}  // extern "C"
