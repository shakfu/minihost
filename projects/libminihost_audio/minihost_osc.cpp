// minihost_osc.cpp
// OSC input and output implementation using juce_osc

#include "minihost_osc.h"

#include <juce_osc/juce_osc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>
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
    // Strings, blobs and anything else report as NaN rather than being
    // dropped, so an argument index means the same thing to sender and
    // receiver. It was 0.0, which a bound fader took as a write to 0.
    return std::numeric_limits<float>::quiet_NaN();
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

    // After this no callback starts; one already running finishes.
    void begin_close() { closing_.store(true); }
    bool idle() const { return active_.load() == 0; }
    // True on the socket thread while it is inside this listener's callback.
    bool dispatching_here() const { return t_dispatching == this; }

private:
    void dispatch(const juce::OSCMessage& message) {
        if (!callback_) return;

        // Count first, then check: with close() setting the flag before it
        // reads the count, one side always sees the other.
        active_.fetch_add(1);
        struct Leave {
            std::atomic<int>& n;
            ~Leave() { n.fetch_sub(1); }
        } leave{active_};
        if (closing_.load()) return;

        args_.clear();
        for (const auto& arg : message)
            args_.push_back(arg_to_float(arg));

        const juce::String address = message.getAddressPattern().toString();
        const Listener* outer = t_dispatching;
        t_dispatching = this;
        callback_(address.toRawUTF8(),
                  args_.empty() ? nullptr : args_.data(),
                  static_cast<int>(args_.size()),
                  user_data_);
        t_dispatching = outer;
    }

    MH_OscCallback callback_;
    void* user_data_;
    std::vector<float> args_;
    std::atomic<bool> closing_{false};
    std::atomic<int> active_{0};
    static thread_local const Listener* t_dispatching;
};

thread_local const Listener* Listener::t_dispatching = nullptr;

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
    // Packets JUCE could not parse, including type tags it does not support
    // (d, T, F, h, ...); these used to vanish without a trace.
    std::atomic<int> format_errors{0};
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

// ---------------------------------------------------------------------------
// OSC address pattern matching.
//
// JUCE's matcher backtracks: each '*' retries every split point, so k stars
// against an n-character part cost about C(n+k, k). A 32-byte pattern with 12
// stars stalled an OscMapper for seconds per binding. This one keeps the set
// of target positions the pattern prefix can reach, so each pattern token
// costs O(n). It reproduces JUCE's semantics, quirks included, which
// tests/test_osc_pattern_parity.py checks against recorded JUCE results:
//   - parts between '/' match independently, empty parts dropped; '*' and
//     '?' never cross '/';
//   - '*' may consume the rest of a part only as the pattern's last token;
//   - "{a,b}" is a set of literal alternatives; "{}" matches the empty string;
//   - "[...]" takes '!' first for negation and "x-y" ranges; '-' last is
//     literal; an empty set matches zero characters; a malformed range
//     (after '{', ',', '}' or at the start) fails the whole pattern;
//   - an unterminated '{' or '[' fails the whole pattern.

static bool match_part(const std::string& pat, const std::string& tgt) {
    const size_t n = tgt.size();
    std::vector<char> cur(n + 1, 0), next(n + 1, 0);
    cur[0] = 1;
    size_t i = 0;
    while (i < pat.size()) {
        std::fill(next.begin(), next.end(), 0);
        const char c = pat[i++];
        if (c == '?') {
            for (size_t p = 0; p < n; ++p)
                if (cur[p]) next[p + 1] = 1;
        } else if (c == '*') {
            const bool last = i == pat.size();
            char any = 0;
            for (size_t p = 0; p <= n; ++p) {
                any |= cur[p];
                if (any && (p < n || last)) next[p] = 1;
            }
        } else if (c == '{') {
            std::vector<std::string> alts;
            std::string elem;
            bool closed = false;
            while (i < pat.size()) {
                const char d = pat[i++];
                if (d == '}') { alts.push_back(elem); closed = true; break; }
                if (d == ',') { alts.push_back(elem); elem.clear(); continue; }
                elem += d;
            }
            if (!closed) return false;
            for (size_t p = 0; p <= n; ++p) {
                if (!cur[p]) continue;
                for (const auto& a : alts)
                    if (tgt.compare(p, a.size(), a) == 0 && p + a.size() <= n)
                        next[p + a.size()] = 1;
            }
        } else if (c == '[') {
            std::vector<char> set;
            bool negated = false, closed = false;
            while (i < pat.size()) {
                const char d = pat[i++];
                if (d == ']') { closed = true; break; }
                if (d == '-') {
                    // JUCE reads the range end without consuming it, so the
                    // loop adds that character again on its next pass.
                    if (i >= pat.size()) return false;
                    const char end = pat[i];
                    if (end == ']') { set.push_back('-'); continue; }
                    if (end == ',' || end == '{' || end == '}' || set.empty())
                        return false;
                    for (char r = set.back(); r < end;) set.push_back(++r);
                    continue;
                }
                if (d == '!' && set.empty() && !negated) { negated = true; continue; }
                set.push_back(d);
            }
            if (!closed) return false;
            if (set.empty()) {
                next = cur;  // an empty set consumes nothing
            } else {
                for (size_t p = 0; p < n; ++p) {
                    if (!cur[p]) continue;
                    const bool in = std::find(set.begin(), set.end(), tgt[p]) != set.end();
                    if (in != negated) next[p + 1] = 1;
                }
            }
        } else {
            for (size_t p = 0; p < n; ++p)
                if (cur[p] && tgt[p] == c) next[p + 1] = 1;
        }
        cur.swap(next);
        if (std::find(cur.begin(), cur.end(), 1) == cur.end()) return false;
    }
    return cur[n] != 0;
}

// Empty parts are dropped, as JUCE's tokeniser does: "//a/" is "/a".
static std::vector<std::string> split_parts(const char* s) {
    std::vector<std::string> parts;
    std::string part;
    for (const char* q = s;; ++q) {
        if (*q == '/' || *q == '\0') {
            if (!part.empty()) parts.push_back(part);
            part.clear();
            if (*q == '\0') break;
        } else {
            part += *q;
        }
    }
    return parts;
}

static bool match_address(const char* pattern, const char* address) {
    const auto pp = split_parts(pattern);
    const auto ap = split_parts(address);
    if (pp.size() != ap.size()) return false;
    for (size_t k = 0; k < pp.size(); ++k)
        if (!match_part(pp[k], ap[k])) return false;
    return true;
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

    MH_OscServer* raw = server.get();
    server->receiver.registerFormatErrorHandler(
        [raw](const char*, int) { raw->format_errors.fetch_add(1); });

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

// JUCE's disconnect() waits only 10 s for the socket thread, then drops the
// socket while the thread may still be in a callback: when it returned it
// ran on with the listener freed and a null socket. So the thread is made
// idle first -- no callback running -- and only then disconnected, which it
// then notices within its 100 ms poll.
static void finish_close(MH_OscServer* server) {
    while (!server->listener->idle())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    server->receiver.disconnect();
    server->receiver.removeListener(server->listener.get());
    delete server;
}

void mh_osc_server_close(MH_OscServer* server) {
    if (!server) return;
    server->listener->begin_close();
    if (server->listener->dispatching_here()) {
        // Called from this server's own callback. Its socket thread cannot
        // wait for itself (this used to hang forever and leak the server),
        // so the teardown finishes elsewhere once the callback returns.
        std::thread([server] { finish_close(server); }).detach();
        return;
    }
    finish_close(server);
}

int mh_osc_server_get_port(MH_OscServer* server) {
    return server ? server->port : -1;
}

int mh_osc_server_get_format_errors(MH_OscServer* server) {
    return server ? server->format_errors.load() : 0;
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
        // Constructed only to validate; JUCE's matches() backtracks.
        juce::OSCAddressPattern p{juce::String(pattern)};
        juce::OSCAddress a{juce::String(address)};
        // Without wildcards JUCE compares its stored form (trailing '/'
        // trimmed), which is cheap, so defer to it.
        if (!p.containsWildcards())
            return p.toString() == a.toString() ? 1 : 0;
        return match_address(pattern, address) ? 1 : 0;
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
