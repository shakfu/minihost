# ThreadSanitizer stress harness

`ringbuffer_stress.cpp` exercises minihost's lock-free structures -- the
single-producer / single-consumer ring buffers in
`projects/libminihost_audio/` and the transport seqlock in
`projects/libminihost/transport_seqlock.h` -- from real threads under
[ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html).

These are the project's only hand-rolled lock-free code: they coordinate the
audio thread with the app / MIDI threads through atomics and no lock. A missing
fence there is invisible on x86 (strong memory ordering) and in ordinary tests,
but corrupts data on a weakly-ordered CPU -- i.e. the Apple-silicon and
ARM-Linux wheels minihost ships. TSan makes those races observable.

## Run

```bash
make tsan              # default workload (N = 200000 items/frames per test)
make tsan N=2000000    # heavier run
```

The target compiles the ring-buffer sources plus the harness with
`-fsanitize=thread` (no JUCE, no Python) and runs them. It exits non-zero on a
functional failure, and -- because it runs with `TSAN_OPTIONS=halt_on_error=1`
-- aborts on the first data race.

Requirements: a `clang` or `gcc` with ThreadSanitizer (macOS or Linux). Windows
is not supported (no upstream TSan). Override the compiler with
`make tsan TSAN_CXX=clang++`.

On a kernel whose mmap layout GCC's TSan runtime does not recognise, the binary
aborts before `main` with `unexpected memory mapping`. Run it with ASLR off:
`setarch -R ./build/tsan_ringbuffer_stress`.

## What it checks

Per buffer, beyond TSan's race detection, the harness asserts SPSC
*correctness*, so a wrong memory order that reorders data (but that TSan might
not classify as a race) is still caught:

- **MIDI ring buffer** (`pop` and `pop_all` paths): every event is delivered
  exactly once, strictly in order, with all fields intact (each field encodes
  the sequence number, so a torn write is detected).
- **Audio ring buffer**: channels within a frame agree (no interleave tearing)
  and real-frame values strictly increase (no reorder or duplication).
- **Transport seqlock** (one writer, two readers): every field of a snapshot
  comes from the same write, and the position a reader sees never goes
  backwards. The payload is individual atomics rather than a plain struct,
  because a seqlock over a plain struct is a data race however the counter is
  ordered -- TSan flags that version on the first read.
- **Transport snapshot** (`mh_transport_snapshot_*`, the C API the audio
  device publishes its playhead through): the same checks through the C
  struct, plus nothing is readable before the first write.

A clean run prints `all clean (no data races, SPSC correctness held)`.

## Scope / what it does NOT cover

Only the ring buffers and the seqlock. The other concurrency fix in this area
-- the atomic `input_callback` pointer in `minihost_audio.c` -- runs on
miniaudio's audio thread, which only exists once a real audio device is open,
so it is not
reachable in a headless harness. That fix is a single atomic pointer with a
release/acquire publish and is validated by inspection.

## CI

Wired up in `.github/workflows/tsan.yml`, deliberately as its own workflow
rather than a job in `build.yml` -- the binary is TSan-instrumented and must
not be shipped, and a timing-sensitive sanitizer run should not be able to
block an unrelated release.

It runs weekly, on manual dispatch (where the workload `N` is an input), and
on pushes/PRs that touch the ring buffers or this harness. A clean run takes
seconds, so the path-filtered trigger is effectively free.
