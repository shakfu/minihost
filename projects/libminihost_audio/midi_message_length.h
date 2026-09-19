// midi_message_length.h
// Byte count of a MIDI message, from its status byte.
//
// MH_MidiEvent carries a status byte and two data bytes because that covers
// every channel voice message, but three is not the length of every message:
// Program Change and Channel Pressure are two bytes, and the real-time
// messages are one. Sending a fixed three bytes appends whatever data2 holds,
// which a receiver reads as the start of the next message.
//
// Internal to libminihost_audio; not part of the C API in docs/api_c.md.

#ifndef MINIHOST_MIDI_MESSAGE_LENGTH_H
#define MINIHOST_MIDI_MESSAGE_LENGTH_H

#ifdef __cplusplus
extern "C" {
#endif

// Returns 1, 2 or 3 for a message that fits in an MH_MidiEvent, or 0 for one
// that does not and must not be sent from those three bytes:
//
//   0x00-0x7F  a data byte, not a status byte (running status is not carried)
//   0xF0       System Exclusive -- variable length, truncated to 3 here
//   0xF4, 0xF5 undefined
static inline int mh_midi_message_length(unsigned char status) {
    if (status < 0x80) return 0;

    if (status < 0xF0) {
        switch (status & 0xF0) {
            case 0xC0:  // Program Change
            case 0xD0:  // Channel Pressure
                return 2;
            default:    // Note Off/On, Poly Pressure, Control Change, Pitch Bend
                return 3;
        }
    }

    switch (status) {
        case 0xF1: return 2;   // MIDI Time Code quarter frame
        case 0xF2: return 3;   // Song Position Pointer
        case 0xF3: return 2;   // Song Select
        case 0xF6: return 1;   // Tune Request
        case 0xF7: return 1;   // End of Exclusive
        case 0xF0:             // System Exclusive: variable length
        case 0xF4:             // undefined
        case 0xF5: return 0;   // undefined
        default:   return 1;   // 0xF8-0xFF real-time
    }
}

#ifdef __cplusplus
}
#endif

#endif // MINIHOST_MIDI_MESSAGE_LENGTH_H
