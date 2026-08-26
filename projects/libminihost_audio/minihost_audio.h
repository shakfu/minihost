// minihost_audio.h
// Real-time audio playback using miniaudio
//
// Thread Safety:
//   - mh_audio_open/close: Call from any thread, not thread-safe with each other
//   - mh_audio_start/stop: Call from any thread, thread-safe
//   - mh_audio_is_playing: Call from any thread, thread-safe
//   - mh_audio_get_*: Call from any thread, thread-safe after open
//   - The audio callback runs on the audio thread and only calls mh_process
//
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations - require minihost.h/minihost_chain.h for actual use
typedef struct MH_Plugin MH_Plugin;
typedef struct MH_PluginChain MH_PluginChain;

typedef struct MH_AudioDevice MH_AudioDevice;

// Distinct parameters the audio thread will apply in one block.
//
// The bound is on distinct parameters, not on writes: the drain coalesces, so
// a fader emitting hundreds of values inside one block occupies one slot. A
// block that changes more than this many *different* parameters is a preset
// load rather than a gesture, and the excess is applied on the next block.
#define MH_AUDIO_MAX_PARAM_CHANGES 64

typedef struct MH_AudioConfig {
    double sample_rate;      // 0 = use device default
    int buffer_frames;       // 0 = auto (~256-512 depending on platform)
    int output_channels;     // 0 = use plugin's output channel count
    int midi_input_port;     // -1 = none, >= 0 = MIDI input port index
    int midi_output_port;    // -1 = none, >= 0 = MIDI output port index
    int capture;             // 0 = playback only, 1 = duplex (capture + playback)
    int playback_device_index; // -1 = system default, >= 0 = index into mh_audio_enumerate_playback_devices()
    int capture_device_index;  // -1 = system default, >= 0 = index into mh_audio_enumerate_capture_devices()
} MH_AudioConfig;

// Audio device descriptor returned by enumeration functions.
typedef struct MH_AudioDeviceInfo {
    char name[256];
    int is_default;  // 1 if this is the system default device
} MH_AudioDeviceInfo;

// Enumerate available audio playback (output) devices.
// out_devices: optional buffer to receive device info (may be NULL to count only)
// max_devices: capacity of out_devices buffer (0 to count only)
// Returns total number of devices available (may exceed max_devices), or -1 on error.
int mh_audio_enumerate_playback_devices(MH_AudioDeviceInfo* out_devices, int max_devices);

// Enumerate available audio capture (input) devices.
// See mh_audio_enumerate_playback_devices for semantics.
int mh_audio_enumerate_capture_devices(MH_AudioDeviceInfo* out_devices, int max_devices);

// Input callback for effects (called from audio thread)
// Provides input audio to be processed by the plugin
// buffer: non-interleaved audio buffers [channel][frame]
// nframes: number of frames to fill
// user_data: user-provided context pointer
typedef void (*MH_AudioInputCallback)(float* const* buffer, int nframes, void* user_data);

// Open an audio device for real-time playback through a plugin
// plugin: the plugin to process audio (must remain valid while device is open)
// config: optional configuration (NULL for defaults)
// err_buf: buffer to receive error message on failure
// err_buf_size: size of error buffer
// Returns NULL on failure
MH_AudioDevice* mh_audio_open(MH_Plugin* plugin, const MH_AudioConfig* config,
                               char* err_buf, size_t err_buf_size);

// Open an audio device for real-time playback through a plugin chain
// chain: the plugin chain to process audio (must remain valid while device is open)
// config: optional configuration (NULL for defaults)
// err_buf: buffer to receive error message on failure
// err_buf_size: size of error buffer
// Returns NULL on failure
MH_AudioDevice* mh_audio_open_chain(MH_PluginChain* chain, const MH_AudioConfig* config,
                                     char* err_buf, size_t err_buf_size);

// Close the audio device
// Automatically stops playback if running
void mh_audio_close(MH_AudioDevice* dev);

// Start audio playback
// Returns 1 on success, 0 on failure
int mh_audio_start(MH_AudioDevice* dev);

// Stop audio playback
// Returns 1 on success, 0 on failure
int mh_audio_stop(MH_AudioDevice* dev);

// Check if audio is currently playing
// Returns 1 if playing, 0 if stopped
int mh_audio_is_playing(MH_AudioDevice* dev);

// Set input callback for effect plugins
// The callback will be called from the audio thread to get input audio
// Pass NULL to clear the callback (silence input)
void mh_audio_set_input_callback(MH_AudioDevice* dev, MH_AudioInputCallback cb, void* user_data);

// Get the actual sample rate (may differ from requested)
double mh_audio_get_sample_rate(MH_AudioDevice* dev);

// Get the actual buffer size in frames
int mh_audio_get_buffer_frames(MH_AudioDevice* dev);

// Get the number of output channels
int mh_audio_get_channels(MH_AudioDevice* dev);

// Connect MIDI input port (can be called while running)
// port_index: MIDI input port index (use mh_midi_enumerate_inputs to list)
// Returns 1 on success, 0 on failure
int mh_audio_connect_midi_input(MH_AudioDevice* dev, int port_index);

// Connect MIDI output port (can be called while running)
// port_index: MIDI output port index (use mh_midi_enumerate_outputs to list)
// Returns 1 on success, 0 on failure
int mh_audio_connect_midi_output(MH_AudioDevice* dev, int port_index);

// Disconnect MIDI input (can be called while running)
// Returns 1 on success, 0 on failure
int mh_audio_disconnect_midi_input(MH_AudioDevice* dev);

// Disconnect MIDI output (can be called while running)
// Returns 1 on success, 0 on failure
int mh_audio_disconnect_midi_output(MH_AudioDevice* dev);

// Get connected MIDI input port index (-1 if not connected or virtual)
int mh_audio_get_midi_input_port(MH_AudioDevice* dev);

// Get connected MIDI output port index (-1 if not connected or virtual)
int mh_audio_get_midi_output_port(MH_AudioDevice* dev);

// Create a virtual MIDI input port (can be called while running)
// Other applications can send MIDI to this port
// Disconnects any existing MIDI input connection
// Returns 1 on success, 0 on failure (or if platform doesn't support virtual ports)
int mh_audio_create_virtual_midi_input(MH_AudioDevice* dev, const char* port_name);

// Create a virtual MIDI output port (can be called while running)
// Other applications can receive MIDI from this port
// Disconnects any existing MIDI output connection
// Returns 1 on success, 0 on failure (or if platform doesn't support virtual ports)
int mh_audio_create_virtual_midi_output(MH_AudioDevice* dev, const char* port_name);

// Check if MIDI input is a virtual port
// Returns 1 if virtual, 0 if physical port or not connected
int mh_audio_is_midi_input_virtual(MH_AudioDevice* dev);

// Check if MIDI output is a virtual port
// Returns 1 if virtual, 0 if physical port or not connected
int mh_audio_is_midi_output_virtual(MH_AudioDevice* dev);

// Send MIDI event to the plugin (thread-safe, can be called while playing)
// Events are queued and processed at the start of the next audio buffer
// status: MIDI status byte (e.g., 0x90 for note on, 0x80 for note off)
// data1: first data byte (e.g., note number)
// data2: second data byte (e.g., velocity)
// Returns 1 on success, 0 on failure (e.g., queue full)
// Queue a MIDI event for the plugin from application code.
//
// Call from a single thread. The event goes onto a lock-free
// single-producer/single-consumer ring that is separate from the one the MIDI
// input port feeds -- sharing that ring made the input thread and this function
// two producers on an SPSC structure, corrupting its indices and losing or
// duplicating events. Returns 1 if queued, 0 if the ring is full.
int mh_audio_send_midi(MH_AudioDevice* dev, unsigned char status, unsigned char data1, unsigned char data2);

// Listen for OSC on a UDP port and drive parameters from it directly.
//
// Addresses recognised, with one float argument in 0..1:
//
//   /mh/param/<index>              parameter <index> of the plugin
//   /mh/<slot>/param/<index>       parameter <index> of chain slot <slot>
//
// Anything else is ignored. This is deliberately the numeric-only subset:
// resolving a parameter *name* means holding a name table and a lock, and the
// socket thread must do neither. Name-addressed control belongs in the mapping
// layer above this, which can resolve at bind time and send numerically.
//
// The value goes straight onto the control parameter ring, so the socket
// thread never blocks and never takes the GIL -- which is the advantage of
// this over receiving in Python and calling mh_audio_send_param_control. A
// Python callback pays a GIL acquisition per message.
//
// port: the UDP port to bind, or 0 to let the OS choose one (see
//   mh_audio_get_osc_port). Disconnects any existing OSC connection first.
// Returns 1 on success, 0 on failure (port in use, or not permitted).
int mh_audio_connect_osc(MH_AudioDevice* dev, int port);

// Stop listening for OSC. Returns 1 on success, 0 if nothing was connected.
// Blocks until the socket thread has stopped.
int mh_audio_disconnect_osc(MH_AudioDevice* dev);

// The UDP port OSC is bound to, or -1 if not connected.
int mh_audio_get_osc_port(MH_AudioDevice* dev);

// Queue a parameter change for the plugin from application code.
//
// Call from a single thread, exactly as mh_audio_send_midi requires and for
// the same reason: the change goes onto a lock-free single-producer ring, and
// a second concurrent producer would corrupt its indices. A control surface
// driving parameters from its own thread uses mh_audio_send_param_control
// instead, which has a ring of its own.
//
// plugin_index selects the chain slot on a device opened with
// mh_audio_open_chain; pass 0 on a device opened with mh_audio_open, where any
// other value is discarded.
//
// value is normalized 0..1 and clamped when applied, matching mh_set_param.
//
// The change is applied by the audio thread at the start of the next block,
// through the _auto process entry point, rather than by writing the parameter
// underneath a running processBlock. Returns 1 if queued, 0 if the ring is
// full.
int mh_audio_send_param(MH_AudioDevice* dev, int plugin_index, int param_index, float value);

// Queue a parameter change from a control-surface thread.
//
// Identical to mh_audio_send_param but writes the ring reserved for control
// input -- a MIDI input callback, or an OSC socket thread -- so that a surface
// and application code are not two producers on one ring. Call from a single
// thread.
int mh_audio_send_param_control(MH_AudioDevice* dev, int plugin_index, int param_index, float value);

// Enable ring-buffer-based audio input for effect processing.
// Creates an internal ring buffer and installs an input callback that reads from it.
// Call mh_audio_write_input() from any thread to push audio data.
// capacity_frames: ring buffer capacity in frames (rounded up to power of 2)
// Returns 1 on success, 0 on failure.
int mh_audio_enable_input(MH_AudioDevice* dev, int capacity_frames);

// Disable ring-buffer-based audio input and revert to silence.
void mh_audio_disable_input(MH_AudioDevice* dev);

// Write interleaved audio frames into the input ring buffer (thread-safe).
// data: interleaved float audio [frame0_ch0, frame0_ch1, ..., frame1_ch0, ...]
// nframes: number of frames to write
// Returns number of frames actually written (may be less if buffer is full).
int mh_audio_write_input(MH_AudioDevice* dev, const float* data, int nframes);

// Get number of frames available in the input ring buffer for reading.
// Returns 0 if input ring buffer is not enabled.
int mh_audio_input_available(MH_AudioDevice* dev);

#ifdef __cplusplus
}
#endif
