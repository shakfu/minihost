# minihost TODO

Tasks are ordered by **user-facing value**: things a user notices first or unblock workflows currently sit at the top; internal quality and style nits sit at the bottom.

Desktop app work is tracked separately in [docs/dev/desktop_app_todo.md](docs/dev/desktop_app_todo.md) (design: [docs/dev/desktop_app.md](docs/dev/desktop_app.md)).

Items tagged **(review Mn / Ln)** come from the 2026-08 code-review pass. That pass fixed every Critical and High finding plus M1 and M10 (see the 0.5.0 and Unreleased sections of [CHANGELOG.md](CHANGELOG.md)); what remains is carried below, slotted into the tiers by user-facing value rather than by original severity. Verification gaps left behind by the *fixed* items are listed too, since they are real outstanding work.

## Tier 1 - Correctness

All Tier 1 correctness items are resolved (see [Done](#done-recent)), including the `open_async` deadlock, which was fixed for real via a dedicated native plugin thread. Tier 1 is empty.

Possible follow-ups on the plugin-thread work (not correctness bugs):

- Verify the affine-op set is complete beyond what the suite exercises (e.g. `set_track_properties`/`updateTrackProperties`, param gestures): if any rarely-used control op turns out thread-affine, wrap it with `runOnMsg` (the pattern is established in `minihost.cpp`).

- The plugin thread currently runs for the process lifetime (detached; reclaimed at exit) -- a deliberate choice to avoid JUCE teardown-ordering hangs at interpreter shutdown. Add a clean stop only if a real need arises.

- Truly *parallel* loading (not just non-blocking) would need out-of-process hosting; still a separate future feature.

## Tier 2 - Medium user value

Real improvements but each affects a narrower slice of users, or is a "nice to have" on top of an already-working path. The 2026-07-07 implementation wave shipped six of the seven items that were here (see [Done](#done-recent)); the remaining one is deferred with reasoning below.

- [ ] **Parallel-branch latency compensation** (`MH_PluginBus`, `minihost_graph.cpp:209-262`). The bus sums branches sample-aligned and `mh_bus_get_latency_samples` returns only the max (`:346-355`), so branches with differing plugin latencies phase-misalign. **Deferred** in the 2026-07-07 wave, deliberately: a correct fix needs (a) a control-thread prepare step that reads each branch's latency and sizes per-branch delay lines, (b) RT-safe ring buffers in the process loop (no audio-thread allocation), (c) handling of dynamic latency changes (plugins report latency updates via callback), and (d) MIDI-output offset compensation for delayed branches. Crucially, meaningful end-to-end verification needs branches with *different, known* latencies -- the fixed-latency test plugins (Dexed) can't construct that scenario, so this wants a controllable-latency test fixture (or a checked-in latency plugin, cf. the Tier 3 "CI integration test plugin" item) before it can be shipped under the zero-tolerance testing bar. Lowest user value in the tier (niche parallel-routing nicety), highest correctness risk -- hence its own focused pass.

### From the code-review pass (user-facing)

- [ ] **Verify the MIDI back-end on Windows and Linux.** Enabling it was the 0.5.0 fix for a subsystem that was inert on every platform (libremidi was compiling its dummy back-end). The WinMM and ALSA macros mirror libremidi's own cmake but have **never been built or exercised** -- only macOS/CoreMIDI is verified. Neither platform has the run-loop hazard that drove the isolation work there, so behaviour may differ. Highest-value open item: a whole subsystem is unproven on two of three platforms.

- [ ] **Does sidechain work for AudioUnits?** (review, from H1). Sidechain routing is verified end to end for VST3 -- FabFilter Pro-C 3 ducks by -7.3 dB with an internal-source control at 0.00 dB. FabFilter Pro-C 2 (AU) showed no response across every value of its sidechain-source parameter, but that is one negative data point with a version confound, and no installed plugin both exposes a sidechain and demonstrably responds to it in AU form. Settle it with a same-version AU/VST3 pair: if AU sidechain really is inert that is a significant limitation independent of the channel accounting already fixed.

- [ ] **`mh_process_sidechain` has no MIDI input** (review, from H6). It is the only `process*` entry point without one, so a MIDI-driven plugin with a sidechain cannot be rendered offline. `process_audio` now rejects the combination with an explanation rather than silently dropping the events, but the gap stands. An additive `mh_process_sidechain_midi` would close it.

- [ ] **`save_vstpreset` class-id auto-detection needs VST3 SDK 3.7.5+** (review, from H2). It reads `Contents/Resources/moduleinfo.json`, which older plugins do not ship (2 of 9 VST3s on the development machine lack it). The fallback is "pass `class_id` explicitly". JUCE knows `holder->cidOfComponent` internally but does not expose it.

- [ ] **Callbacks require manual `poll_callbacks()` and are easy to miss** (review M2). `set_change_callback` / `set_param_value_callback` / `set_param_gesture_callback` only enqueue; nothing fires until polled (`_core.cpp`). The no-GIL-on-the-audio-thread rationale is sound, but a user who registers a callback and never polls sees total silence with no diagnostic, and `callback_events_dropped()` only becomes non-zero after 1024 undelivered events. Consider an opt-in background dispatcher, or at minimum a warning on first overflow.

- [ ] **Device MIDI is quantised to the block boundary** (review M4). `minihost_audio.c` and `live.cpp` set `sample_offset = 0` for every incoming event, discarding libremidi's timestamps. At a 512-frame buffer / 48 kHz that is ~10.7 ms of jitter -- audible on percussive material. Document as a known limitation even if not fixed.

- [ ] **No SysEx support anywhere** (review M5). `MH_MidiEvent` is a fixed `(offset, status, data1, data2)`, so there is no SysEx path in the C API, bindings, graph or device layer (`live.cpp` explicitly drops anything longer than 3 bytes). Patch dumps, MPE configuration and MIDI-CI all need it -- and Dexed, the project's own default test plugin, is SysEx-driven.

- [ ] **Resampling is linear-interpolation only** (review M6). `minihost_audiofile.c` uses `ma_resample_algorithm_linear` with a 4th-order low-pass; linear resampling has audible aliasing and HF loss. It is on by default in `process_audio_to_file` (`resample_to_plugin_rate=True`), so processing a 44.1 kHz file through a 48 kHz plugin silently degrades the audio. Also `mh_audio_resample` never checks that all input was consumed and never flushes the resampler tail, so frames can be dropped silently. At minimum document it; better, offer a higher-quality backend.

- [ ] **CLI: `--block-size` / `--sample-rate` must precede the subcommand** (review M8). They are on the top-level parser, so `minihost process plugin.vst3 --block-size 1024 ...` fails with "unrecognized arguments", and `minihost process --help` never mentions them -- yet the command prints `Block size:` in its summary. Additionally `-r/--sample-rate` is silently overridden by the input file's rate whenever audio input is present, so it only matters in MIDI-only mode. Make them per-subcommand (or use a parent parser) and document the override.

- [ ] **CLI: `--tail` is ignored for audio-input processing** (review M9). `minihost process reverb.vst3 -i dry.wav -o wet.wav --tail 4.0` silently truncates at the source length; the tail only applies in MIDI-only mode. The underlying `process_audio_to_file` supports it fine, so this reads as a missing feature rather than a deliberate restriction -- and it is the single most common effect-processing need.


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

### From the code-review pass (internal quality)

- [ ] **Centralise plugin gating in a `conftest.py` fixture** (review, Testing and CI). Gating is inconsistent: some files check `os.path.exists(PLUGIN)`, others only whether the env var is *set*, and `test_minihost.py`'s `plugin_path` fixture does neither. Pointing `MINIHOST_TEST_PLUGIN` at a stale path produces **7 failures and 64 errors instead of clean skips** (measured). One fixture that checks existence once would fix all of it.

- [ ] **CI runs no plugins, so audio correctness is untested there** (review, Testing and CI). `.github/workflows/build.yml` runs pytest under cibuildwheel on runners with no plugins installed, so ~31 of 57 test files skip entirely. None of the Critical or High findings from the review pass involved pure-Python logic, so a green CI says little about audio behaviour. Bundling a small permissively-licensed test plugin -- or building a trivial JUCE one as part of CI -- would change that materially. This is the multiplier on every other test item.

- [ ] **Integration assertions are mostly liveness checks** (review, Testing and CI). Several assert only "did not crash" or `isfinite`, which is why the sidechain routing bug survived: the existing `test_process_sidechain` fed all-zero buffers and asserted the call returned. The review pass added signal-level tests for sidechain, transport and GIL release; the older ones deserve the same treatment.

- [ ] **No automated coverage for the desktop `LiveEngine` audio callback** (review, from H7/H8). Two real bugs were fixed there blind -- the planar-buffer clear and the sample-rate mismatch -- but the app is GUI-driven and its headless self-test modes do not reach the audio callback. H7 in particular wants a listening check on a project with a 2+ channel input node and a device buffer smaller than the project block size. Also outstanding: wire `hasSampleRateMismatch()` into a user-visible warning in the app UI (see [docs/dev/desktop_app_todo.md](docs/dev/desktop_app_todo.md)).

- [ ] **Single-file `.vst3` scanning is unverified** (review, from H9). Scanning now searches files as well as bundle directories, but that only matters on Windows and Linux, and every VST3 on the development machine is a bundle -- a bare file named `.vst3` is not loadable on macOS, so it fails to probe either way. Needs a check on an affected platform.

- [ ] **Third-party `.vstpreset` interop is unverified** (review, from H2). Saving and loading are now spec-shaped and round-trip correctly, but a filesystem-wide search found no foreign `.vstpreset` on the development machine. The claim rests on the written chunk being byte-identical to what the plugin's own `IComponent::getState` produced, plus a synthesized foreign-shaped preset. One manual check against a DAW-saved preset would close it.

- [ ] **Sample-accurate automation takes locks on the audio thread** (review M7). `minihost.h` declares the `mh_process*` family "no locks, no allocations after warmup", but `mh_process_auto` calls `setValueNotifyingHost` (which dispatches to listeners, and minihost's own then takes a mutex) and `mh_chain_process_auto` calls `mh_set_param` (which takes `stateMutex` outright). Both are priority-inversion hazards that contradict the header's own contract. Use `AudioProcessorParameter::setValue`, the RT-safe variant, as `live.cpp` already does.

- [ ] **Audio-thread MIDI output blocks; the out-ring is dead code** (review M3). `minihost_audio.c` calls `mh_midi_out_send` -> libremidi directly from the miniaudio callback, once per event -- a syscall-bearing, potentially allocating call on the RT thread. Meanwhile `dev->midi_out_buffer` is created and freed but never used: the ring buffer that exists precisely to avoid this is dead.

- [ ] **`send_midi`'s ring is single-producer** (review, from H11). It no longer shares the libremidi input thread's ring, but its own is SPSC too, so concurrent `send_midi` from several threads would reintroduce the same corruption. The single-thread contract is documented in `minihost_audio.h` and the binding; make it MPSC if multi-thread sending ever becomes a supported pattern.

- [ ] **`mh_graph_get_midi_output_events` over-reports the event count** (review M11). It sets `*num_events_out` to the *untruncated* total but copies only `min(total, capacity)`. The Python wrapper uses that number both to size the buffer and to `resize()` afterwards, so a block producing more than 1024 events yields fabricated zero-filled tuples in the tail. Return the copied count and expose the truncated total separately -- the node already tracks both.

- [ ] **`plugincache` discovers formats minihost cannot load** (review M13). `PLUGIN_EXTS` includes `.dll`, `.so`, `.clap` and `.vst`; minihost supports VST3, AU and LV2 only. On Linux a scan of any directory containing shared objects probes every one and caches a permanent error entry. Also `_fingerprint` stats the bundle *directory*, so replacing the binary inside a macOS `.vst3` in place does not change the mtime and stale metadata is served indefinitely.

- [ ] **Redundant state serialisations** (review M14). `Plugin.get_state` calls `mh_get_state_size` then `mh_get_state`, each serialising the whole state -- two full dumps (megabytes, sometimes seconds) per save, and a hazard if anything mutates the plugin in between. The H2 fix added another snapshot on the `set_state` side, which is the price of being able to report failure at all. A `mh_get_state_alloc` (or caching the `MemoryBlock` behind the size query) would pay off on both paths.

- [ ] **MIDI open failures discard libremidi's returned error** (review, from M12). Thrown `stdx::error`s now report properly via `describe_current_exception`, but the *returned* error from `open_port` / `open_virtual_port` is still swallowed in favour of a fixed string. Feeding it through the same helper would finish the job.

- [ ] **`Plugin`'s move constructor leaves a stale trampoline pointer** (review L2). It moves `plugin_` / `sample_rate_` / `max_block_size_` but not `non_realtime_`, the callback holders or the callback queue -- and the C-layer trampolines were registered with the old `this`. Not reachable from Python today (nanobind holds `Plugin` by pointer) but a live trap for anyone extending the class. Fix the move or `= delete` it, as the copy constructor already is.

- [ ] **`mh_message_thread_init` cannot restart after shutdown** (review L3). `std::call_once` means a second `init` after `mh_message_thread_shutdown` silently does nothing, so every thread-affine operation reverts to running inline. A foot-gun for a library embedded in a longer-lived host. There is also a small race: `shutdown` clears `enabled_` before joining, so a concurrent `run()` executes inline while the thread is still alive.

- [ ] **Audio-thread diagnostics in the desktop app** (review L6). `live.cpp` scans every output buffer for a non-zero sample on *every* callback, and keeps doing so for as long as the project is silent; `fprintf` on the audio thread is acknowledged in comments but still an RT violation. Move both behind a lock-free flag consumed by the GUI timer.

- [ ] **Uninitialised pointer array passed as non-null** (review L7). `minihost_graph_v2.cpp` leaves `const float* in_ptrs[64]` uninitialised when a node has no input ports, then passes it to `mh_process*`, which branches on `if (inputs)` -- true for a stack array. Safe today only because such nodes always have zero input channels, so the copy loop never runs. The comment claims it passes null, which it does not. Pass `nullptr` explicitly.

- [ ] **Seqlock payload is a non-atomic struct copy** (review L11). `MH_PlayHead::read`/`write` copy `State` while the writer may be mid-write. The sequence counter makes the result correct on retry, but the copy is a formal data race (UB, and TSan will flag it). Practically fine on the target architectures; worth a comment or relaxed atomic fields.


## Tier 4 - Style / minor code quality

The two channel-count nits formerly here (`minihost.cpp` `jmax(1,...)` and `render.py` stereo minimum) were promoted and merged into the Tier 2 "Honest channel counts" item after the 2026-07-07 review.

- [ ] `minihost_audiofile.c:87-89,108`: `ma_encoder_write_pcm_frames` result is checked but `written != frames` is not, so a partial WAV write reports success. Trivial.

- [ ] `minihost_audiofile.c:412-421`: single-shot `ma_resampler_process_pcm_frames` never flushes the linear filter's internal delay, dropping a few trailing output frames. Small.

### From the code-review pass (style / docs)

- [ ] **`MidiMapper` documents a value range the plugin layer clamps away** (review L1). `control.py`'s docstring shows `map_cc(..., value_range=(-1.0, 1.0))`, but `mh_set_param` clamps to `[0, 1]`, so the bottom half of the fader travel maps to a constant 0. The documented example is actively misleading.

- [ ] **`--bit-depth` help does not match behaviour** (review L4). `cli.py` says "default: match input or 24"; the code uses a flat 24 and never inspects the input.

- [ ] **Chain channel truncation is silent** (review L5). `minihost_chain.cpp` zero-pads when the next plugin needs more channels but silently drops the extras when it needs fewer -- a 6-channel plugin feeding a stereo one loses channels 2-5 with no warning. Document it, or offer a downmix.

- [ ] **`write_wav` ignores the written-frame count** (review L8). `minihost_audiofile.c` discards `written`, so a short write is reported as success.

- [ ] **`_READ_EXTENSIONS` is dead** (review L9). `audio_io.py` defines it and nothing uses it; `read_audio` does no extension validation, so an unsupported file surfaces as a raw miniaudio error code.

- [ ] **Decide on `sdist.include = ["thirdparty/JUCE"]`** (review L10). The sdist ships the whole JUCE tree (24 MB compressed, 4377 files, verified self-contained). That is deliberate and load-bearing -- a source install builds without fetching JUCE -- but if the directory is absent at build time the sdist silently ships without it, which is the worse failure. Either document the intent or make its absence an error.

- [ ] **`mh_check_buses_layout` has a tautological guard** (review L12). `(input_channels && i < num_input_buses)` -- the second conjunct is the loop condition. Harmless, but it obscures intent.


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
