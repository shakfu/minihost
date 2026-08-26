// transport_ringbuffer.h
// Lock-free single-producer single-consumer command ring for the host playhead
//
// The audio thread owns the transport outright: it holds the authoritative
// MH_TransportInfo, advances the playhead each block, and is the only writer.
// Control threads therefore do not mutate it -- they post commands here and
// the audio thread applies them at the top of the next block.
//
// A command ring rather than a set of atomics because this project avoids C11
// <stdatomic.h> on purpose (MSVC gates it behind a flag the Visual Studio
// generator does not reliably pass -- see the note in minihost_audio.c), and
// because a transport is a state machine whose updates want to be ordered
// rather than independently torn. Setting tempo and position together should
// not be observable half-applied.
//
// Thread Safety:
//   - push(): one producer thread only
//   - pop_all(): consumer (audio) thread only
//
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum MH_TransportCommandType {
    MH_TRANSPORT_CMD_PLAY = 0,
    MH_TRANSPORT_CMD_STOP,
    MH_TRANSPORT_CMD_SET_BPM,
    MH_TRANSPORT_CMD_SET_TIME_SIG,
    MH_TRANSPORT_CMD_SET_POSITION_SAMPLES,
    MH_TRANSPORT_CMD_SET_LOOP,
    MH_TRANSPORT_CMD_SET_RECORDING
} MH_TransportCommandType;

typedef struct MH_TransportCommand {
    int type;               // MH_TransportCommandType
    double dvalue;          // bpm
    long long lvalue;       // position, loop start
    long long lvalue2;      // loop end
    int ivalue;             // numerator, loop enabled, recording
    int ivalue2;            // denominator
} MH_TransportCommand;

typedef struct MH_TransportRingBuffer MH_TransportRingBuffer;

MH_TransportRingBuffer* mh_transport_ringbuffer_create(int capacity);
void mh_transport_ringbuffer_free(MH_TransportRingBuffer* rb);

// Push a command (producer thread). Returns 1 on success, 0 if full.
int mh_transport_ringbuffer_push(MH_TransportRingBuffer* rb,
                                 const MH_TransportCommand* cmd);

// Pop everything pending (consumer/audio thread). Returns the count.
int mh_transport_ringbuffer_pop_all(MH_TransportRingBuffer* rb,
                                    MH_TransportCommand* out, int max_out);

#ifdef __cplusplus
}
#endif
