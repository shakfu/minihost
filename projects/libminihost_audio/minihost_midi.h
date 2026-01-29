// minihost_midi.h
// MIDI port enumeration and I/O using libremidi
//
// Thread Safety:
//   - Enumeration functions are thread-safe
//   - MIDI handles should be used from a single thread
//
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// MIDI port information
typedef struct MH_MidiPortInfo {
    char name[256];
    int index;
} MH_MidiPortInfo;

// Callback for port enumeration
typedef void (*MH_MidiPortCallback)(const MH_MidiPortInfo* port, void* user_data);

// Enumerate available MIDI input ports
// Returns number of ports found, or -1 on error
int mh_midi_enumerate_inputs(MH_MidiPortCallback callback, void* user_data);

// Enumerate available MIDI output ports
// Returns number of ports found, or -1 on error
int mh_midi_enumerate_outputs(MH_MidiPortCallback callback, void* user_data);

// Get number of MIDI input ports
int mh_midi_get_num_inputs(void);

// Get number of MIDI output ports
int mh_midi_get_num_outputs(void);

// Get MIDI input port name by index
// Returns 1 on success, 0 on failure
int mh_midi_get_input_name(int index, char* buf, size_t buf_size);

// Get MIDI output port name by index
// Returns 1 on success, 0 on failure
int mh_midi_get_output_name(int index, char* buf, size_t buf_size);

// Opaque MIDI input handle
typedef struct MH_MidiIn MH_MidiIn;

// Opaque MIDI output handle
typedef struct MH_MidiOut MH_MidiOut;

// MIDI message callback (called from MIDI thread)
// data: raw MIDI bytes, len: number of bytes (typically 1-3 for channel messages)
typedef void (*MH_MidiCallback)(const unsigned char* data, size_t len, void* user_data);

// Open MIDI input port
// callback: called when MIDI messages arrive (from MIDI thread)
// Returns NULL on failure
MH_MidiIn* mh_midi_in_open(int port_index, MH_MidiCallback callback, void* user_data,
                            char* err_buf, size_t err_buf_size);

// Open virtual MIDI input port
// Creates a named port that other applications can send MIDI to
// callback: called when MIDI messages arrive (from MIDI thread)
// Returns NULL on failure (or if platform doesn't support virtual ports)
MH_MidiIn* mh_midi_in_open_virtual(const char* port_name, MH_MidiCallback callback, void* user_data,
                                    char* err_buf, size_t err_buf_size);

// Close MIDI input
void mh_midi_in_close(MH_MidiIn* midi_in);

// Open MIDI output port
// Returns NULL on failure
MH_MidiOut* mh_midi_out_open(int port_index, char* err_buf, size_t err_buf_size);

// Open virtual MIDI output port
// Creates a named port that other applications can receive MIDI from
// Returns NULL on failure (or if platform doesn't support virtual ports)
MH_MidiOut* mh_midi_out_open_virtual(const char* port_name, char* err_buf, size_t err_buf_size);

// Close MIDI output
void mh_midi_out_close(MH_MidiOut* midi_out);

// Send MIDI message
// Returns 1 on success, 0 on failure
int mh_midi_out_send(MH_MidiOut* midi_out, const unsigned char* data, size_t len);

#ifdef __cplusplus
}
#endif
