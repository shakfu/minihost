// minihost_osc.h
// OSC (Open Sound Control) input and output
//
// Sits beside minihost_midi.h because it is the same kind of thing: a
// transport for control data arriving from outside the process. A touch
// surface, a phone, another host on the network.
//
// Built on JUCE's juce_osc module rather than a vendored OSC library. JUCE is
// already a dependency, juce_osc needs only juce_events (which
// juce_audio_processors_headless already pulls in), and it carries the same
// licence as every other JUCE module minihost links -- so it adds no
// dependency at all. See docs/dev/osc_and_touch.md for the comparison against
// embedding liblo.
//
// Scope, stated rather than discovered later:
//   - UDP only. No TCP, no SLIP. This is what TouchOSC and every other tablet
//     surface uses; a serial OSC device is not served.
//   - Bundle time tags are parsed but not scheduled. JUCE delivers bundle
//     contents immediately, which is right for control and wrong for
//     sequencing.
//
// Thread Safety:
//   - Open/close: call from any thread, not thread-safe against each other
//     for the same handle.
//   - The receive callback runs on the OSC socket thread, not the audio
//     thread and not a JUCE message thread.
//   - mh_osc_send_*: call from a single thread per client.
//
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handles
typedef struct MH_OscServer MH_OscServer;
typedef struct MH_OscClient MH_OscClient;

// Called on the OSC socket thread when a message arrives.
//
// address: the full OSC address pattern, NUL-terminated, valid for the
//   duration of the call only.
// args: the message's numeric arguments. float32 arrives as itself and int32
//   is converted; any other type (string, blob, time tag, ...) is reported as
//   0.0f rather than skipped, so argument positions stay aligned with what the
//   sender wrote. num_args is the message's full argument count.
//
// Must not block: the socket thread is the only reader, so a slow callback
// costs incoming messages. The intended shape is a push onto a lock-free ring
// that some other thread drains.
typedef void (*MH_OscCallback)(const char* address, const float* args,
                               int num_args, void* user_data);

// Open a UDP port and start listening.
//
// port: the port to bind. Pass 0 to let the OS choose a free one, which is
//   what a test wants; mh_osc_server_get_port reports what was chosen.
// Returns NULL on failure (port in use, permission denied), with a message in
// err_buf when one is supplied.
MH_OscServer* mh_osc_server_open(int port, MH_OscCallback callback, void* user_data,
                                 char* err_buf, size_t err_buf_size);

// Stop listening and free the server. Blocks until the socket thread has
// stopped, so the callback is guaranteed not to be running on return.
void mh_osc_server_close(MH_OscServer* server);

// The port the server is actually bound to. Useful after opening on port 0.
// Returns -1 if unknown.
int mh_osc_server_get_port(MH_OscServer* server);

// Open a sender aimed at host:port.
//
// No connection is established -- UDP has none -- so this fails only on a
// name that will not resolve. A send to a host that is not listening is
// silently discarded by the network, which is the protocol's nature and not
// something this layer can report.
MH_OscClient* mh_osc_client_open(const char* host, int port,
                                 char* err_buf, size_t err_buf_size);

// Close and free the client.
void mh_osc_client_close(MH_OscClient* client);

// Send a message with a single argument. Returns 1 on success, 0 on failure
// (malformed address, socket error).
int mh_osc_send_float(MH_OscClient* client, const char* address, float value);
int mh_osc_send_int(MH_OscClient* client, const char* address, int value);
int mh_osc_send_string(MH_OscClient* client, const char* address, const char* value);

// Send a message with no arguments -- a trigger, such as /mh/transport/play.
int mh_osc_send_bang(MH_OscClient* client, const char* address);

// Send a message with an array of float arguments.
// num_values may be 0, which is equivalent to mh_osc_send_bang.
int mh_osc_send_floats(MH_OscClient* client, const char* address,
                       const float* values, int num_values);

// Does an OSC address pattern match a concrete address?
//
// OSC senders may address with wildcards -- `?`, `*`, `[a-z]`, `{a,b}` -- so a
// receiver holding concrete addresses has to match rather than look up. This
// delegates to juce::OSCAddressPattern::matches rather than reimplementing the
// spec, so both ends of a minihost connection agree by construction.
//
// pattern: the address pattern as received.
// address: a concrete address, with no wildcards.
// Returns 1 on a match, 0 on no match or if either side is malformed.
int mh_osc_address_matches(const char* pattern, const char* address);

// Check whether an address is a valid OSC address pattern.
//
// Exposed because the failure it prevents is otherwise silent: a control bound
// to an address the host will not accept simply never arrives, with nothing
// logged at either end. Returns 1 if valid, 0 if not.
int mh_osc_is_valid_address(const char* address);

#ifdef __cplusplus
}
#endif
