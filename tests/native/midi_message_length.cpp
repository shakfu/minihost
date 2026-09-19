// The MIDI message-length table used by the audio device's MIDI output path.
//
// There is no headless way to assert what reaches a MIDI port -- it needs a
// real device -- so the length itself is tested here and the callback in
// minihost_audio.c is a single call to it. Built and run by
// tests/test_native.py.

#include <cstdio>

#include "midi_message_length.h"

namespace {

int failures = 0;

void expect(unsigned char status, int want, const char* what) {
    const int got = mh_midi_message_length(status);
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s (status 0x%02X): want %d, got %d\n",
                     what, status, want, got);
        ++failures;
    }
}

// Every channel voice message, on every one of the 16 channels.
void test_channel_voice() {
    for (int ch = 0; ch < 16; ++ch) {
        const unsigned char c = (unsigned char)ch;
        expect(0x80 | c, 3, "Note Off");
        expect(0x90 | c, 3, "Note On");
        expect(0xA0 | c, 3, "Polyphonic Key Pressure");
        expect(0xB0 | c, 3, "Control Change");
        expect(0xC0 | c, 2, "Program Change");
        expect(0xD0 | c, 2, "Channel Pressure");
        expect(0xE0 | c, 3, "Pitch Bend");
    }
}

void test_system_common() {
    expect(0xF0, 0, "System Exclusive is not representable in 3 bytes");
    expect(0xF1, 2, "MIDI Time Code quarter frame");
    expect(0xF2, 3, "Song Position Pointer");
    expect(0xF3, 2, "Song Select");
    expect(0xF4, 0, "undefined");
    expect(0xF5, 0, "undefined");
    expect(0xF6, 1, "Tune Request");
    expect(0xF7, 1, "End of Exclusive");
}

void test_real_time() {
    expect(0xF8, 1, "Timing Clock");
    expect(0xF9, 1, "undefined real-time");
    expect(0xFA, 1, "Start");
    expect(0xFB, 1, "Continue");
    expect(0xFC, 1, "Stop");
    expect(0xFD, 1, "undefined real-time");
    expect(0xFE, 1, "Active Sensing");
    expect(0xFF, 1, "System Reset");
}

// A data byte in the status position means running status, which MH_MidiEvent
// does not carry. Sending those three bytes would emit a stray note.
void test_data_bytes_are_not_status() {
    for (int b = 0x00; b < 0x80; ++b) {
        expect((unsigned char)b, 0, "data byte is not a status byte");
    }
}

} // namespace

int main() {
    test_channel_voice();
    test_system_common();
    test_real_time();
    test_data_bytes_are_not_status();

    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("midi message length: all clean\n");
    return 0;
}
