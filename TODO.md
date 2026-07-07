# minihost TODO

Tasks are ordered by **user-facing value**: things a user notices first or unblock workflows currently sit at the top; internal quality and style nits sit at the bottom.

Desktop app work is tracked separately in [docs/dev/desktop_app_todo.md](docs/dev/desktop_app_todo.md) (design: [docs/dev/desktop_app.md](docs/dev/desktop_app.md)).

## Tier 1 - Correctness

All Tier 1 correctness items are resolved (see [Done](#done-recent)), including the `open_async` deadlock, which was fixed for real via a dedicated native plugin thread. Tier 1 is empty.

Possible follow-ups on the plugin-thread work (not correctness bugs):

- Verify the affine-op set is complete beyond what the suite exercises (e.g. `set_track_properties`/`updateTrackProperties`, param gestures): if any rarely-used control op turns out thread-affine, wrap it with `runOnMsg` (the pattern is established in `minihost.cpp`).

- The plugin thread currently runs for the process lifetime (detached; reclaimed at exit) -- a deliberate choice to avoid JUCE teardown-ordering hangs at interpreter shutdown. Add a clean stop only if a real need arises.

- Truly *parallel* loading (not just non-blocking) would need out-of-process hosting; still a separate future feature.

## Tier 2 - Medium user value

Real improvements but each affects a narrower slice of users, or is a "nice to have" on top of an already-working path. The 2026-07-07 implementation wave shipped six of the seven items that were here (see [Done](#done-recent)); the remaining one is deferred with reasoning below.

- [ ] **Parallel-branch latency compensation** (`MH_PluginBus`, `minihost_graph.cpp:209-262`). The bus sums branches sample-aligned and `mh_bus_get_latency_samples` returns only the max (`:346-355`), so branches with differing plugin latencies phase-misalign. **Deferred** in the 2026-07-07 wave, deliberately: a correct fix needs (a) a control-thread prepare step that reads each branch's latency and sizes per-branch delay lines, (b) RT-safe ring buffers in the process loop (no audio-thread allocation), (c) handling of dynamic latency changes (plugins report latency updates via callback), and (d) MIDI-output offset compensation for delayed branches. Crucially, meaningful end-to-end verification needs branches with *different, known* latencies -- the fixed-latency test plugins (Dexed) can't construct that scenario, so this wants a controllable-latency test fixture (or a checked-in latency plugin, cf. the Tier 3 "CI integration test plugin" item) before it can be shipped under the zero-tolerance testing bar. Lowest user value in the tier (niche parallel-routing nicety), highest correctness risk -- hence its own focused pass.

## Tier 3 - Internal quality

Test coverage and developer-experience improvements. Important for keeping the project trustworthy, but no individual item is something an end user will perceive directly.

### Test gaps (ordered: untested public surface first, then performance/fuzz)

- [x] **`open_async()` tests** -- Future/error/timeout/kwargs/background-thread mechanics covered deterministically (monkeypatched) plus a real invalid-path error test in `tests/test_open_async.py`. The real-plugin *success* path is skipped because it deadlocks -- promoted to the Tier 1 `open_async` bug above.

- [ ] **`MidiIn` tests** -- only an existence/export check today (`tests/test_minihost.py:48-52`). No virtual-port or real-input coverage. Skip gracefully on unsupported platforms.

- [ ] **Callback integration tests** -- narrower than first written. Real plugin-initiated dispatch *is* tested: `test_concurrency.py:99-122` fires 50 param-value events and asserts in-order `poll_callbacks()` delivery (plus the overflow test at :126-147). Still missing: latency / param-info / program / non-param-state callback types, all plugin-gated.

- [ ] **Boundary/edge-case tests** -- frame-count edges (`nframes=0`, `nframes=max_block_size`, `nframes > max_block_size`) and zero-channel plugins. (Channel-mismatch is already in `tests/test_channel_validation.py`; empty-MIDI-list paths are covered in `test_audio_processing.py:286`, `test_render_internals.py:207`, `test_minihost.py:1431` -- so this entry is now narrower than originally written.)

- [ ] **Double-precision MIDI/auto/sidechain processing is unimplemented (feature gap, not a test gap).** Correction after inspection: the C API only has `mh_process_double` (plain audio, no MIDI); there is no `mh_process_midi_double`, `mh_process_auto_double`, or `mh_process_sidechain_double`, and no chain double-MIDI path. So `process_midi_double` / `process_auto_double` / sidechain-double can't be "tested" -- they don't exist. If double-precision + MIDI/automation/sidechain is wanted, it needs implementing in C (`minihost.cpp` / `minihost_chain.cpp`), binding, and testing. Niche (most plugins process float; double is rare), so low priority. The existing `process_double` (audio only) and `AudioBufferD` are well covered (`test_audio_buffer_double.py`, `test_minihost.py`, `test_rt_allocations.py`).

- [ ] **Fuzz testing for VST3 preset parser** -- `read_vstpreset` with malformed / truncated input.

- [ ] **Performance benchmarks** -- audio processing hot-path benchmarks to catch regressions.

### Developer experience

- [ ] **CI integration test plugin** -- a lightweight JUCE-built pass-through plugin checked into the repo so integration tests (~30% of suite, currently skipped in CI) can run everywhere.

- [ ] **Incremental build support** -- `make test` currently forces a full rebuild via `uv sync --reinstall-package`. Add a `test-only` target or file-based dependencies.

- [ ] **Cache JUCE in CI** -- JUCE is re-downloaded on every CI run (~30s). Cache via GitHub Actions cache.

### Internal consistency

- [ ] **`_tick_to_seconds` optimization** (`render.py:63`) -- use binary search or a running accumulator instead of linear scan for large MIDI files with many tempo changes (currently `O(n*m)`).

## Tier 4 - Style / minor code quality

The two channel-count nits formerly here (`minihost.cpp` `jmax(1,...)` and `render.py` stereo minimum) were promoted and merged into the Tier 2 "Honest channel counts" item after the 2026-07-07 review.

- [ ] `minihost_audiofile.c:87-89,108`: `ma_encoder_write_pcm_frames` result is checked but `written != frames` is not, so a partial WAV write reports success. Trivial.

- [ ] `minihost_audiofile.c:412-421`: single-shot `ma_resampler_process_pcm_frames` never flushes the linear filter's internal delay, dropping a few trailing output frames. Small.

## Done (recent)

This section tracks the current development wave. Older work (0.1.6-era: numpy-optional, MIDI CC mapping, extended DSP ops, batch path delegation, migration guide, etc.) lives in [CHANGELOG.md](CHANGELOG.md) -- this list is a working summary, not an archive.

- [x] **`open_async` fixed for real via a dedicated native plugin thread (and the library is now thread-safe for control ops).** The original `open_async` deadlocked because JUCE VST3/AU instances are thread-affine -- construction, destruction, and control-plane queries (state, parameter text, program names, reset, sample-rate, precision) must all run on one thread -- and it built the plugin on a short-lived daemon thread. The fix: a `MinihostMessageThread` singleton (`minihost.cpp`) owns one persistent background thread that becomes the JUCE message thread; every thread-affine control op is marshaled onto it via a plain condition-variable request queue (callers on any thread push a task + promise and block -- JUCE's own `callFunctionOnMessageThread`/`CallbackMessage` both proved unreliable on macOS). The real-time `process*()` path stays lock-free on the caller's thread. Enabled by default; opt out with `MINIHOST_MESSAGE_THREAD=0`. Because affinity is now handled in C, `open_async` is a simple daemon-thread loader returning a real `Plugin` usable/closable from any thread (no proxy, no warning), and the previously-skipped real-plugin load+use+close test passes. Full suite green with the plugin thread default-on (794 pass). Path to this fix: after the persistent-worker proxy and a first background-message-thread attempt both failed on macOS, the custom-queue design (proposed as "put requests in a queue") was the one that worked. Tests: `tests/test_open_async.py` (7).

- [x] **Honest channel counts** -- `minihost.cpp:1076-1077` now reports the plugin's true JUCE channel counts (a synth honestly reports 0 audio inputs) instead of inflating to a minimum of 1; the internal process buffer keeps a >=1-channel floor so a pure-MIDI plugin still gets a valid buffer. `render.py:453` output now uses `max(num_output_channels, 1)` so a genuine mono plugin renders one channel rather than a fake-stereo file. Required making the Python process pipeline synth-input-aware (`process.py` uses an effective input width `max(in_ch_required, 1)` for buffer sizing/slicing). Tests: `tests/test_tier2_features.py` (synth-reports-0 gated) plus the full `process_audio` suite still green.

- [x] **Expose `MIDI_OUT_CAPACITY` as a module constant** -- published as `minihost.MIDI_OUT_CAPACITY` (== 256) with the truncation signal documented. `tests/test_tier2_features.py`.

- [x] **Zero-copy channel-range slicing** -- `AudioBuffer.channel_view(start, count)` returns a new buffer aliasing a contiguous channel range (JUCE `setDataToReferTo`; parent pinned via nanobind `keep_alive`). Bidirectional aliasing, chained views, bounds checks. 8 tests in `tests/test_tier2_features.py`.

- [x] **Parameter preset morphing** -- new `minihost.morph` module (`capture` / `apply` / `lerp` / `morph`, re-exported as `capture_params` / `apply_params` / `lerp_params` / `morph_params`). Interpolates normalized per-parameter snapshots (scalar or per-parameter blend), clamped to [0, 1]. 9 tests in `tests/test_tier2_features.py`.

- [x] **Document the process-vs-control threading contract** -- the C header already classified this thoroughly; added a Python-facing class docstring to `Plugin` spelling out that the lock-free process methods must not overlap the reconfiguring setters (sample_rate/set_state/etc.).

- [x] **WAV metadata / BWF support** -- `mh_audio_write_bwf` (`minihost_audiofile.{h,c}`) appends an EBU Tech 3285 `bext` chunk (description, originator, originator_reference, origination date/time, time_reference) after the data chunk and fixes up the RIFF size; `mh_audio_write` is now a NULL-metadata wrapper. Exposed via `write_audio(..., bwf=dict)` (WAV only; FLAC raises). `smpl` sampler-loop chunks are intentionally out of scope (different feature). 6 tests in `tests/test_bwf_metadata.py` (field round-trip via raw RIFF parse + audio still decodes).

- [x] **Sample-accurate automation: dropped in-block parameter changes** -- `mh_process_auto` (`minihost.cpp`) and `mh_chain_process_auto` (`minihost_chain.cpp`) computed the chunk boundary from `param_changes[param_idx]` *before* the apply-loop advanced past changes due at the chunk start, so when two or more changes fell in one block the later ones were silently swallowed (never applied). Fixed by applying all due changes first, then setting `chunk_end` from the next still-pending change. Reproduced first: `tests/test_process_auto_automation.py` (3 tests) failed against the unfixed build (Dexed param read back the first value, not the last) and passes after the fix. Found in the 2026-07-07 review.

- [x] **DLPack export verified correct and zero-copy** -- the review's "may not return a capsule / torch+jax will error" concern was refuted empirically: `__dlpack__` (`_core.cpp:3057-3065`) returns a proper `"dltensor"` PyCapsule, shares the identical memory pointer as `as_ndarray()`, and buffer mutations are observed through the view (no hidden copy). No code change needed. Regression + interop coverage added in `tests/test_dlpack_interop.py` (numpy always; torch/jax via `importorskip`). Documented nuance: `numpy.from_dlpack` imports read-only (numpy 2.x default); use `as_ndarray()` for a writable zero-copy view. Follow-up (Tier 3): array-library interop docs paragraph; verify torch/jax where installed.

- [x] **Double-precision `AudioBufferD`** -- shipped as a separate `AudioBufferD` class (float64) alongside `AudioBuffer` (float32); bound at `_core.cpp:3126`, feeds `Plugin.process_double` directly via DLPack with no numpy dependency. Covered by `tests/test_audio_buffer_double.py`. (Confirmed already present in the 2026-07-07 review; the old Tier 2 "add AudioBufferD" item was stale.)

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

  - FLAC cover archival / intermediate / lossless. Compressed delivery is downstream of rendering -- pipe a WAV through `ffmpeg`, which ships better, more current encoders for every target format than anything we'd vendor.
