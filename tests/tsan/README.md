# ThreadSanitizer stress harness

`ringbuffer_stress.cpp` exercises minihost's lock-free single-producer /
single-consumer ring buffers (`projects/libminihost_audio/midi_ringbuffer.cpp`
and `audio_ringbuffer.cpp`) from two real threads under
[ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html).

These ring buffers are the project's only hand-rolled lock-free code: they
coordinate the audio thread with the app / MIDI threads through release/acquire
atomics and no lock. A missing fence there is invisible on x86 (strong memory
ordering) and in ordinary tests, but corrupts data on a weakly-ordered CPU --
i.e. the Apple-silicon and ARM-Linux wheels minihost ships. TSan makes those
races observable.

## Run

```bash
make tsan              # default workload (N = 200000 items/frames per test)
make tsan N=2000000    # heavier run
```

The target compiles the two ring-buffer sources plus the harness with
`-fsanitize=thread` (no JUCE, no Python) and runs them. It exits non-zero on a
functional failure, and -- because it runs with `TSAN_OPTIONS=halt_on_error=1`
-- aborts on the first data race.

Requirements: a `clang` or `gcc` with ThreadSanitizer (macOS or Linux). Windows
is not supported (no upstream TSan). Override the compiler with
`make tsan TSAN_CXX=clang++`.

## What it checks

Per buffer, beyond TSan's race detection, the harness asserts SPSC
*correctness*, so a wrong memory order that reorders data (but that TSan might
not classify as a race) is still caught:

- **MIDI ring buffer** (`pop` and `pop_all` paths): every event is delivered
  exactly once, strictly in order, with all fields intact (each field encodes
  the sequence number, so a torn write is detected).
- **Audio ring buffer**: channels within a frame agree (no interleave tearing)
  and real-frame values strictly increase (no reorder or duplication).

A clean run prints `all clean (no data races, SPSC correctness held)`.

## Scope / what it does NOT cover

Only the ring buffers. The other concurrency fix in this area -- the atomic
`input_callback` pointer in `minihost_audio.c` -- runs on miniaudio's audio
thread, which only exists once a real audio device is open, so it is not
reachable in a headless harness. That fix is a single atomic pointer with a
release/acquire publish and is validated by inspection.

## CI

Not wired into CI by default. To add it, run `make tsan` in a Linux job on a
clang/gcc image (it needs no audio device and finishes in seconds). Keep it a
separate job from the normal build -- the binary is TSan-instrumented and must
not be shipped.
