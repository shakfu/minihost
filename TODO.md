# minihost TODO

Tasks are ordered by **user-facing value**: things a user notices first or unblock workflows currently sit at the top; internal quality and style nits sit at the bottom.

## Critical

## High

## Medium

- [ ] Verify the affine-op set is complete beyond what the suite exercises (e.g. `set_track_properties`/`updateTrackProperties`, param gestures): if any rarely-used control op turns out thread-affine, wrap it with `runOnMsg` (the pattern is established in `minihost.cpp`).

- [ ] The plugin thread currently runs for the process lifetime (detached; reclaimed at exit) -- a deliberate choice to avoid JUCE teardown-ordering hangs at interpreter shutdown. Add a clean stop only if a real need arises.

- [ ] Truly *parallel* loading (not just non-blocking) would need out-of-process hosting; still a separate future feature.

- [ ] **Parallel-branch latency compensation** (`MH_PluginBus`, `minihost_graph.cpp:209-262`). The bus sums branches sample-aligned and `mh_bus_get_latency_samples` returns only the max (`:346-355`), so branches with differing plugin latencies phase-misalign. **Deferred** in the 2026-07-07 wave, deliberately: a correct fix needs (a) a control-thread prepare step that reads each branch's latency and sizes per-branch delay lines, (b) RT-safe ring buffers in the process loop (no audio-thread allocation), (c) handling of dynamic latency changes (plugins report latency updates via callback), and (d) MIDI-output offset compensation for delayed branches. Crucially, meaningful end-to-end verification needs branches with *different, known* latencies -- the fixed-latency test plugins (Dexed) can't construct that scenario, so this wants a controllable-latency test fixture (or a checked-in latency plugin, cf. the Tier 3 "CI integration test plugin" item) before it can be shipped under the zero-tolerance testing bar. Lowest user value in the tier (niche parallel-routing nicety), highest correctness risk -- hence its own focused pass.

- [ ] **Adopt `ui_json` schema 3 in `minihost touch` once py2tosc releases it.** Schema 3 lets a choice stand among *bindings*, not just in place of a node, and lets a branch hold a list -- including an empty one. That is exactly what Phase 6 worked around: a parameter past the 128th has no CC to bind, `each` could not conditionally omit a message, so `touch.py::_branch_table` doubles every widget kind into a `...NoCc` twin. Six complete templates for two independent questions; schema 3 nests them and makes it five, and the growth is additive rather than multiplicative (a third question: seven against twelve). py2tosc's own schema-3 fixture is this case verbatim and its changelog names minihost as the caller that prompted the change.

  **Blocked on a release.** py2tosc's working tree has `SCHEMA = 3` but `__version__` is still `0.5.2` and that is the newest tag, so emitting schema 3 now would produce descriptions that `pip install 'minihost[touch]'` refuses with a `SchemaError`. When it lands: move the `touch` extra floor and `CIBW_TEST_REQUIRES` off `>=0.5.2`, re-copy `tests/check_json.py` (the vendored one is from 0.5.2 and does not know schema 3), and simplify the branch table. `build_layout` already stamps what `required_schema` computes rather than a constant, so the envelope needs no change. Rows could also drop the `cc` key entirely instead of carrying a sentinel, since only the taken branch is substituted into. Full notes in [docs/dev/osc_and_touch.md](docs/dev/osc_and_touch.md) section 6.

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

## Low

- [ ] `minihost_audiofile.c:87-89,108`: `ma_encoder_write_pcm_frames` result is checked but `written != frames` is not, so a partial WAV write reports success. Trivial.

- [ ] `minihost_audiofile.c:412-421`: single-shot `ma_resampler_process_pcm_frames` never flushes the linear filter's internal delay, dropping a few trailing output frames. Small.

- [ ] **`MidiMapper` documents a value range the plugin layer clamps away** (review L1). `control.py`'s docstring shows `map_cc(..., value_range=(-1.0, 1.0))`, but `mh_set_param` clamps to `[0, 1]`, so the bottom half of the fader travel maps to a constant 0. The documented example is actively misleading.

- [ ] **`--bit-depth` help does not match behaviour** (review L4). `cli.py` says "default: match input or 24"; the code uses a flat 24 and never inspects the input.

- [ ] **Chain channel truncation is silent** (review L5). `minihost_chain.cpp` zero-pads when the next plugin needs more channels but silently drops the extras when it needs fewer -- a 6-channel plugin feeding a stereo one loses channels 2-5 with no warning. Document it, or offer a downmix.

- [ ] **`write_wav` ignores the written-frame count** (review L8). `minihost_audiofile.c` discards `written`, so a short write is reported as success.

- [ ] **`_READ_EXTENSIONS` is dead** (review L9). `audio_io.py` defines it and nothing uses it; `read_audio` does no extension validation, so an unsupported file surfaces as a raw miniaudio error code.

- [ ] **Decide on `sdist.include = ["thirdparty/JUCE"]`** (review L10). The sdist ships the whole JUCE tree (24 MB compressed, 4377 files, verified self-contained). That is deliberate and structural -- a source install builds without fetching JUCE -- but if the directory is absent at build time the sdist silently ships without it, which is the worse failure. Either document the intent or make its absence an error.

- [ ] **`mh_check_buses_layout` has a tautological guard** (review L12). `(input_channels && i < num_input_buses)` -- the second conjunct is the loop condition. Harmless, but it obscures intent.
