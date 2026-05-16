# minihost TODO

Tasks are ordered by **user-facing value**: things a user notices first or
unblock workflows currently sit at the top; internal quality and style nits
sit at the bottom.

## Tier 1 - High user value

All Tier 1 items from earlier passes have shipped: README rewrite,
`--progress` / `--normalize` / `--chain`, the `process_audio_to_file`
extension, dry/wet mix on `PluginChain`, and `PluginGraph`. See
[Done](#done-recent). New high-impact ideas welcome.

## Tier 2 - Medium user value

Real improvements but each affects a narrower slice of users, or is a
"nice to have" on top of an already-working path. Ordered roughly by
my read of effort-to-impact (highest leverage first). Tier 1 is
empty for now; the top of Tier 2 is the natural promotion candidate
if a Tier 1 slot needs filling.


- [ ] **WAV metadata / BWF support** -- write timestamps, descriptions, and originator metadata into WAV `bext` / `smpl` chunks for film / broadcast workflows. miniaudio reads these (`projects/miniaudio/miniaudio.h:62011`); writing them is the gap. Niche audience but well-scoped and additive.

- [ ] **Double-precision `AudioBufferD`** -- the v1 `AudioBuffer` is float32 only. `Plugin.process_double` still requires numpy float64. Add a parallel `MhAudioBufferD` wrapper around `juce::AudioBuffer<double>` and wire it in. Decide between `AudioBufferD` (separate class) vs. `AudioBuffer(channels, frames, dtype="float64")`. Completes the numpy-optional story for the double-precision processing path.


- [ ] **Zero-copy channel-range slicing** -- `AudioBuffer.channel_view(start, count)` returning a new `AudioBuffer` that aliases (rather than copies) a contiguous channel range. Frame slicing stays copy-only (would need strided views). Niche optimization for patterns that repeatedly hand subsets of channels to processors.

- [ ] **Parameter preset morphing** -- higher-level Python utility for interpolating between two parameter snapshots (A/B morph) using `get_state` / `set_state` and per-parameter access. Small utility; useful for sound-design exploration but not a workflow blocker today.

- [ ] **Dynamic MIDI output buffer** -- make the 256-event MIDI output cap (`MIDI_OUT_CAPACITY` in `_core.cpp:127`) configurable at the Python level, or use a dynamically-sized buffer. Edge case: dense MIDI streams (e.g. MPE polyphonic aftertouch) can blow the cap; routine usage doesn't.

- [ ] **DLPack interop verification with PyTorch / JAX** -- `MhAudioBuffer.__dlpack__` exists (`_core.cpp:2493`) but is unverified against the two consumers users actually care about. Add a smoke test (skipped via `importorskip` when frameworks absent), assert zero-copy aliasing for torch (mutate-on-one-side, observe-on-the-other), and add a one-paragraph "interop with other array libraries" section to the docs. ~2-3 hours; high visibility for ML-adjacent users; surfaces a real correctness question (does the stride layout actually round-trip without a hidden copy?).

## Tier 3 - Internal quality

Test coverage and developer-experience improvements. Important for keeping
the project trustworthy, but no individual item is something an end user
will perceive directly.

### Test gaps (ordered: untested public surface first, then performance/fuzz)

- [ ] **`open_async()` tests** -- success path, error path, and timeout. Currently untested on a public API (confirmed: zero grep hits in `tests/`).
- [ ] **`MidiIn` tests** -- only an existence/export check today (`tests/test_minihost.py:48-52`). No virtual-port or real-input coverage. Skip gracefully on unsupported platforms.
- [ ] **Callback integration tests** -- `poll_callbacks()` is exercised only indirectly via `test_concurrency.py`'s overflow scenario. Add real plugin-initiated callback tests (latency / param-info / program / non-param-state) when `MINIHOST_TEST_PLUGIN` is set.
- [ ] **Boundary/edge-case tests** -- frame-count edges (`nframes=0`, `nframes=max_block_size`, `nframes > max_block_size`) and zero-channel plugins. (Channel-mismatch is already in `tests/test_channel_validation.py`; empty-MIDI-list paths are covered in `test_audio_processing.py:286`, `test_render_internals.py:207`, `test_minihost.py:1431` -- so this entry is now narrower than originally written.)
- [ ] **Expand double-precision coverage** -- `process_double` has ~5 tests today (2 in `test_minihost.py`, 3 in `test_rt_allocations.py`); they cover the bare process call and RT-allocations. Missing: `process_midi_double`, `process_auto_double`, sidechain-double, and chain-process-double if applicable.
- [ ] **Fuzz testing for VST3 preset parser** -- `read_vstpreset` with malformed / truncated input.
- [ ] **Performance benchmarks** -- audio processing hot-path benchmarks to catch regressions.

### Developer experience

- [ ] **CI integration test plugin** -- a lightweight JUCE-built pass-through plugin checked into the repo so integration tests (~30% of suite, currently skipped in CI) can run everywhere.
- [ ] **Incremental build support** -- `make test` currently forces a full rebuild via `uv sync --reinstall-package`. Add a `test-only` target or file-based dependencies.
- [ ] **Cache JUCE in CI** -- JUCE is re-downloaded on every CI run (~30s). Cache via GitHub Actions cache.

### Internal consistency

- [ ] **`_tick_to_seconds` optimization** (`render.py:63`) -- use binary search or a running accumulator instead of linear scan for large MIDI files with many tempo changes (currently `O(n*m)`).

## Tier 4 - Style / minor code quality

- [ ] `projects/libminihost/minihost.cpp:1055-1056`: `jmax(1, inst->getTotal{In,Out}putChannels())` forces minimum 1 input *and* output channel. Synthesizers with 0 inputs get unnecessary buffer allocation; effects with 0 outputs (rare) get the same.
- [ ] `src/minihost/render.py:421`: `self._out_channels = max(plugin.num_output_channels, 2)` hardcodes stereo minimum output. Should use the plugin's actual channel count for mono plugins.

## Done (recent)

This section tracks the current development wave. Older work
(0.1.6-era: numpy-optional, MIDI CC mapping, extended DSP ops, batch
path delegation, migration guide, etc.) lives in
[CHANGELOG.md](CHANGELOG.md) -- this list is a working summary, not an
archive.

- [x] **`process_audio` in-place mode** -- `process_audio(plugin, audio, in_place=True)` writes output into the input buffer instead of allocating a new one (for the stereo-in / stereo-out case). Requires AudioBuffer input, matching I/O channel counts, no tail. Existing loop is already safe because each input block is snapshotted into a scratch buffer before any output write. Returns the same buffer object as `audio`. 6 tests in `tests/test_in_place_and_session.py`.
- [x] **`minihost.Session`** -- shared `AudioPluginFormatManager` across loads/probes/scans. New C API in `projects/libminihost/minihost.{h,cpp}`: `mh_session_create` / `mh_session_close` / `mh_session_open` / `mh_session_probe` / `mh_session_scan_directory`. Python: `minihost.Session()` with `open()` / `probe()` / `scan_directory()` methods. Refactor: removed the per-plugin `AudioPluginFormatManager fm` field from `MH_Plugin` (the manager was only used at construction); `mh_open_ex` now constructs a local manager; session-bound entries reuse the session's. Plugins survive the session that created them (AudioPluginInstance is self-contained post-creation). 8 tests in `tests/test_in_place_and_session.py`.
- [x] **`process_audio_stream(plugin_or_chain, audio, ...)` generator** -- mirrors `render_midi_stream` for the audio-in case. Yields user-visible blocks (post-latency-comp, post-trim) so concatenating every yielded block reproduces `process_audio`'s return value. Same kwargs (`midi=`, `sidechain=`, `param_changes=`, `bpm=`, synth-mode `audio=None`); `normalize=` is intentionally absent (peak normalization needs the full output). `as_=numpy.ndarray` selector matches `render_midi_stream`. Implementation factored both `process_audio` and the streamer onto a shared `_prepare_render` + `_iter_blocks` (yields independent copies for the streaming case via `copy=True`; `process_audio` passes `copy=False` since it memcpys into a pre-allocated buffer). 9 tests in `tests/test_process_audio_stream.py`.
- [x] **Dry/wet mix on `PluginChain`** -- `chain.set_mix(plugin_index, mix)` / `get_mix(plugin_index)`; `mix` in `[0, 1]` with 1.0=full wet (default), 0.0=full dry, 0.5=equal blend. Plugin's input and output channel counts must match (else `set_mix` raises). Applied to all chain process variants. Allocation-free on the audio thread.
- [x] **`PluginGraph` parallel-branches-summed** -- new type for parallel routing. Fans input to N branches, sums their outputs with per-branch gain. Muted branches (gain=0) skip processing entirely. C API in `projects/libminihost/minihost_graph.{h,cpp}`; Python `minihost.PluginGraph`. 20 tests in `tests/test_chain_mix_and_graph.py`.
- [x] **`process_audio_to_file` absorbs the rest of `cmd_process`** -- new kwargs `midi=`, `sidechain=`, `param_changes=`, `bpm=`, `audio=None` for synth mode. `cmd_process` collapsed from ~410 to ~200 lines and delegates the block loop, MIDI/sidechain/automation routing, latency comp, normalize, and write to the library. 18 new tests.
- [x] **CLI `--progress` / `--normalize` / `--chain`** -- progress bar on stderr, peak normalization with dBFS target, and declarative chain loading from JSON/YAML. Library hooks on `process_audio` / `process_audio_to_file` / `render_midi_to_file`. New `src/minihost/chain.py` with `_OwningPluginChain` subclass to keep plugin refs alive. 12 new tests.
- [x] **README leads with new API** -- Quick Start uses `process_audio_to_file`; manual block-loop demoted to a "Lower-level processing" subsection.
- [x] **MidiRenderer internal buffers are `AudioBuffer`** (was Tier 3; confirmed already shipped at `render.py:487-488`).
- [x] **Concurrent-access smoke test** -- already in `tests/test_concurrency.py::test_set_param_does_not_crash_concurrent_process` (was Tier 3; confirmed already shipped).
- [x] **vstpreset.py debug-stripped assert** -- the original line is gone (was Tier 4; confirmed already shipped).

## Non-goals

Intentionally omitted for headless / server use:

- Editor window management
- GUI hosting
- Preset browser UI
- MIDI learn
- Plugin shell / multi-instrument handling
- Compressed-output writers (AIFF, OGG/Vorbis, MP3, Opus, AAC). WAV
  + FLAC cover archival / intermediate / lossless. Compressed delivery
  is downstream of rendering -- pipe a WAV through `ffmpeg`, which
  ships better, more current encoders for every target format than
  anything we'd vendor.
