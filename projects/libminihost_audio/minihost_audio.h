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
int mh_audio_send_midi(MH_AudioDevice* dev, unsigned char status, unsigned char data1, unsigned char data2);

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
