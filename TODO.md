# TODO

## Critical

## High

## Medium

- [x] Verify the affine-op set is complete beyond what the suite exercises. Done 2026-10-04 for VST3: `MinihostTestFx` logs control callbacks made off its constructing thread, and `tests/test_thread_affinity.py` drives every `Plugin` control op from the Python thread. It found `mh_get_tail_seconds` and `mh_supports_double` unmarshaled, plus a per-block support query in `mh_process_double`; all fixed (see CHANGELOG). AU is not covered: the fixture is VST3-only. Original note: `set_track_properties` turned out to be affine and now goes through `runOnMsg`; param gestures and the remaining rarely-used control ops are still unchecked. The deterministic fixture makes this testable.

- [ ] The plugin thread currently runs for the process lifetime (detached; reclaimed at exit) -- a deliberate choice to avoid JUCE teardown-ordering hangs at interpreter shutdown. Add a clean stop only if a real need arises.

- [ ] Truly *parallel* loading (not just non-blocking) would need out-of-process hosting; still a separate future feature.

- [ ] **Parallel-branch latency compensation** (`MH_PluginBus`, `minihost_graph.cpp:209-262`). The bus sums branches sample-aligned and `mh_bus_get_latency_samples` returns only the max (`:346-355`), so branches with differing plugin latencies phase-misalign. **Deferred** in the 2026-07-07 wave, deliberately: a correct fix needs (a) a control-thread prepare step that reads each branch's latency and sizes per-branch delay lines, (b) RT-safe ring buffers in the process loop (no audio-thread allocation), (c) handling of dynamic latency changes (plugins report latency updates via callback), and (d) MIDI-output offset compensation for delayed branches. **The fixture blocker is gone**: `MinihostTestFx` (`projects/test_plugin`) has a `latency` parameter that both reports and applies an exact sample delay, so two branches with different, known latencies can now be built and the misalignment measured -- `tests/test_fixture_signal.py` shows the pattern. Lowest user value in the tier (niche parallel-routing nicety), highest correctness risk -- hence its own focused pass. A plugin changing its latency mid-session now does reach the host, so (c) has what it needs.

- [ ] **Adopt `ui_json` schema 3 in `minihost touch` once py2tosc releases it.** Schema 3 lets a choice stand among *bindings*, not just in place of a node, and lets a branch hold a list -- including an empty one. That is exactly what Phase 6 worked around: a parameter past the 128th has no CC to bind, `each` could not conditionally omit a message, so `touch.py::_branch_table` doubles every widget kind into a `...NoCc` twin. Six complete templates for two independent questions; schema 3 nests them and makes it five, and the growth is additive rather than multiplicative (a third question: seven against twelve). py2tosc's own schema-3 fixture is this case verbatim and its changelog names minihost as the caller that prompted the change.

  **Blocked on a release.** py2tosc's working tree has `SCHEMA = 3` but `__version__` is still `0.5.2` and that is the newest tag, so emitting schema 3 now would produce descriptions that `pip install 'minihost[touch]'` refuses with a `SchemaError`. When it lands: move the `touch` extra floor and `CIBW_TEST_REQUIRES` off `>=0.5.2`, re-copy `tests/check_json.py` (the vendored one is from 0.5.2 and does not know schema 3), and simplify the branch table. `build_layout` already stamps what `required_schema` computes rather than a constant, so the envelope needs no change. Rows could also drop the `cc` key entirely instead of carrying a sentinel, since only the taken branch is substituted into. Full notes in [docs/dev/osc_and_touch.md](docs/dev/osc_and_touch.md) section 6.

- [ ] **Verify the MIDI back-end on Windows and Linux.** Enabling it was the 0.5.0 fix for a subsystem that was inert on every platform (libremidi was compiling its dummy back-end). The WinMM and ALSA macros mirror libremidi's own cmake but have **never been built or exercised** -- only macOS/CoreMIDI is verified. Neither platform has the run-loop hazard that drove the isolation work there, so behaviour may differ. Highest-value open item: a whole subsystem is unproven on two of three platforms.

- [ ] **Does sidechain work for AudioUnits?** (review, from H1). Sidechain routing is verified end to end for VST3 -- FabFilter Pro-C 3 ducks by -7.3 dB with an internal-source control at 0.00 dB. FabFilter Pro-C 2 (AU) showed no response across every value of its sidechain-source parameter, but that is one negative data point with a version confound, and no installed plugin both exposes a sidechain and demonstrably responds to it in AU form. Settle it with a same-version AU/VST3 pair: if AU sidechain really is inert that is a significant limitation independent of the channel accounting already fixed.

- [x] **`mh_process_sidechain` has no MIDI input** Fixed 2026-10-04: `mh_process_sidechain_midi_io` / `Plugin.process_sidechain_midi`; `process_audio` accepts both. The CLIs dropped MIDI on this path too; fixed. Original note: (review, from H6). It is the only `process*` entry point without one, so a MIDI-driven plugin with a sidechain cannot be rendered offline. `process_audio` now rejects the combination with an explanation rather than silently dropping the events, but the gap stands. An additive `mh_process_sidechain_midi` would close it.

- [ ] **`save_vstpreset` class-id auto-detection needs VST3 SDK 3.7.5+** (review, from H2). It reads `Contents/Resources/moduleinfo.json`, which older plugins do not ship (2 of 9 VST3s on the development machine lack it). The fallback is "pass `class_id` explicitly". JUCE knows `holder->cidOfComponent` internally but does not expose it.

- [x] **Callbacks require manual `poll_callbacks()` and are easy to miss** Fixed 2026-10-04: a `RuntimeWarning` on the first dropped event. A background dispatcher was rejected: on macOS `poll_callbacks` must run the main run loop. Original note: (review M2). `set_change_callback` / `set_param_value_callback` / `set_param_gesture_callback` only enqueue; nothing fires until polled (`_core.cpp`). The no-GIL-on-the-audio-thread rationale is sound, but a user who registers a callback and never polls sees total silence with no diagnostic, and `callback_events_dropped()` only becomes non-zero after 1024 undelivered events. Consider an opt-in background dispatcher, or at minimum a warning on first overflow.

- [x] **Device MIDI is quantised to the block boundary** Fixed 2026-10-04: events are stamped on arrival and placed one block later at their relative position, in `minihost_audio.c` and the desktop `live.cpp`. The mapping is unit-tested (`tests/native/midi_timestamp.cpp`); no test drives a real device end to end. Original note: (review M4). `minihost_audio.c` and `live.cpp` set `sample_offset = 0` for every incoming event, discarding libremidi's timestamps. At a 512-frame buffer / 48 kHz that is ~10.7 ms of jitter -- audible on percussive material. Document as a known limitation even if not fixed.

- [ ] **No SysEx support anywhere** (review M5). `MH_MidiEvent` is a fixed `(offset, status, data1, data2)`, so there is no SysEx path in the C API, bindings, graph or device layer (`live.cpp` explicitly drops anything longer than 3 bytes). Patch dumps, MPE configuration and MIDI-CI all need it -- and Dexed, the project's own default test plugin, is SysEx-driven.

- [x] **Resampling is linear-interpolation only** Fixed 2026-10-04: libsamplerate sinc converters, `best` by default; see CHANGELOG. Original note: (review M6). `minihost_audiofile.c` uses `ma_resample_algorithm_linear`; that is the only algorithm miniaudio ships, and its one quality knob (the anti-alias filter order) is now at the maximum, so the remaining aliasing and HF loss are inherent to linear interpolation. It is on by default in `process_audio_to_file` (`resample_to_plugin_rate=True`), so processing a 44.1 kHz file through a 48 kHz plugin silently degrades the audio. A real fix means a second backend (sinc/polyphase) behind a quality argument. At minimum document it.

- [x] **CLI: `--block-size` / `--sample-rate` must precede the subcommand** (review M8). Fixed 2026-09-25: accepted on either side of every plugin-loading subcommand; see CHANGELOG. Original note: They are on the top-level parser, so `minihost process plugin.vst3 --block-size 1024 ...` fails with "unrecognized arguments", and `minihost process --help` never mentions them -- yet the command prints `Block size:` in its summary. Additionally `-r/--sample-rate` is silently overridden by the input file's rate whenever audio input is present, so it only matters in MIDI-only mode. Make them per-subcommand (or use a parent parser) and document the override.

- [x] **`MidiIn` tests** Stale, closed 2026-10-04: `tests/test_midiin_lifetime.py` drives real traffic through a virtual port via `tests/coremidi_loopback.py` on macOS. Linux/Windows coverage belongs to the MIDI back-end item above. Original note: -- only an existence/export check today (`tests/test_minihost.py:48-52`). No virtual-port or real-input coverage. Skip gracefully on unsupported platforms.

- [ ] **Callback integration tests** -- narrower than first written. Real plugin-initiated dispatch *is* tested: `test_concurrency.py:99-122` fires 50 param-value events and asserts in-order `poll_callbacks()` delivery (plus the overflow test at :126-147). Still missing: latency / param-info / program / non-param-state callback types, all plugin-gated.

- [ ] **Boundary/edge-case tests** Frame counts done 2026-10-04: `tests/test_frame_boundaries.py` covers 0, `max_block_size` and one past it for all six `Plugin.process*` methods. Zero-channel plugins remain: neither fixture has zero outputs. Original note: -- frame-count edges (`nframes=0`, `nframes=max_block_size`, `nframes > max_block_size`) and zero-channel plugins. (Channel-mismatch is already in `tests/test_channel_validation.py`; empty-MIDI-list paths are covered in `test_audio_processing.py:286`, `test_render_internals.py:207`, `test_minihost.py:1431` -- so this entry is now narrower than originally written.)

- [ ] **Double-precision MIDI/auto/sidechain processing is unimplemented (feature gap, not a test gap).** Correction after inspection: the C API only has `mh_process_double` (plain audio, no MIDI); there is no `mh_process_midi_double`, `mh_process_auto_double`, or `mh_process_sidechain_double`, and no chain double-MIDI path. So `process_midi_double` / `process_auto_double` / sidechain-double can't be "tested" -- they don't exist. If double-precision + MIDI/automation/sidechain is wanted, it needs implementing in C (`minihost.cpp` / `minihost_chain.cpp`), binding, and testing. Niche (most plugins process float; double is rare), so low priority. The existing `process_double` (audio only) and `AudioBufferD` are well covered (`test_audio_buffer_double.py`, `test_minihost.py`, `test_rt_allocations.py`).

- [x] **Fuzz testing for VST3 preset parser** Done 2026-10-04: `TestFuzzReadVstPreset`, 3000 seeded mutations, reaches all nine parser error paths. No crash found. It found a non-UTF-8 class ID raising `UnicodeDecodeError`; fixed in the C reader. Original note: -- `read_vstpreset` with malformed / truncated input.

- [ ] **Performance benchmarks** -- audio processing hot-path benchmarks to catch regressions.

### Developer experience

- [x] **CI integration test plugin** -- done. `projects/test_plugin` builds two deterministic VST3 fixtures (`MinihostTestFx`: pass-through / exact gain / exact sample delay / sidechain / MIDI passthrough; `MinihostTestSynth`: monophonic sine instrument), behind `-DMINIHOST_BUILD_TEST_PLUGIN=ON`. The `integration` job in `build.yml` builds them on Linux and macOS and runs the suite with `MINIHOST_TEST_PLUGIN` (the synth, which is what the plugin-gated tests have always meant) and `MINIHOST_TEST_PLUGIN_FX`. Skips went 405 -> 116.

- [ ] **Extend the fixture where the suite still cannot reach.** The two builds cover the paths the review named, but not: a plugin that rejects a foreign state chunk (a JUCE wrapper accepts any bytes, so `test_vstpreset_interop.py::test_loading_a_corrupt_preset_raises_rather_than_silently_doing_nothing` skips against it), asymmetric channel counts, or double-precision processing. Windows is not in the `integration` job either -- the job costs a second full compile and the fixture is platform-independent by construction, but the hosting paths are not proven there.

- [ ] **Incremental build support** -- `make test` currently forces a full rebuild via `uv sync --reinstall-package`. Add a `test-only` target or file-based dependencies.

- [ ] **Cache JUCE in CI** -- JUCE is re-downloaded on every CI run (~30s). Cache via GitHub Actions cache.

### Internal consistency

- [x] **`_tick_to_seconds` optimization** Done 2026-10-04: `_tick_converter`, bisect over prefix sums; bit-identical. Original note: (`render.py:63`) -- use binary search or a running accumulator instead of linear scan for large MIDI files with many tempo changes (currently `O(n*m)`).

### From the code-review pass (internal quality)

- [x] **Centralise plugin gating in a `conftest.py` fixture** Done 2026-10-04, differently: a `MINIHOST_TEST_PLUGIN*` variable naming a missing path is now a usage error at startup, not a skip. A clean skip would let CI pass with the plugin tests silently off. Original note: (review, Testing and CI). Gating is inconsistent: some files check `os.path.exists(PLUGIN)`, others only whether the env var is *set*, and `test_minihost.py`'s `plugin_path` fixture does neither. Pointing `MINIHOST_TEST_PLUGIN` at a stale path produces **7 failures and 64 errors instead of clean skips** (measured). One fixture that checks existence once would fix all of it.

- [ ] **Integration assertions are mostly liveness checks** (review, Testing and CI). Several assert only "did not crash" or `isfinite`, which is why the sidechain routing bug survived: the existing `test_process_sidechain` fed all-zero buffers and asserted the call returned. The review pass added signal-level tests for sidechain, transport and GIL release; the older ones deserve the same treatment.

- [ ] **No automated coverage for the desktop `LiveEngine` audio callback** (review, from H7/H8). Two real bugs were fixed there blind -- the planar-buffer clear and the sample-rate mismatch -- but the app is GUI-driven and its headless self-test modes do not reach the audio callback. H7 in particular wants a listening check on a project with a 2+ channel input node and a device buffer smaller than the project block size. Also outstanding: wire `hasSampleRateMismatch()` into a user-visible warning in the app UI (see [docs/dev/desktop_app_todo.md](docs/dev/desktop_app_todo.md)).

- [ ] **Single-file `.vst3` scanning is unverified** (review, from H9). Scanning now searches files as well as bundle directories, but that only matters on Windows and Linux, and every VST3 on the development machine is a bundle -- a bare file named `.vst3` is not loadable on macOS, so it fails to probe either way. Needs a check on an affected platform.

- [ ] **Third-party `.vstpreset` interop is unverified** (review, from H2). Saving and loading are now spec-shaped and round-trip correctly, but a filesystem-wide search found no foreign `.vstpreset` on the development machine. The claim rests on the written chunk being byte-identical to what the plugin's own `IComponent::getState` produced, plus a synthesized foreign-shaped preset. One manual check against a DAW-saved preset would close it.

- [x] **Sample-accurate automation takes locks on the audio thread** (review M7). Verified fixed 2026-09-25: `mh_process_auto` and `mh_chain_process_auto` both call `mh_set_param_rt`. Original note: `minihost.h` declares the `mh_process*` family "no locks, no allocations after warmup", but `mh_process_auto` calls `setValueNotifyingHost` (which dispatches to listeners, and minihost's own then takes a mutex) and `mh_chain_process_auto` calls `mh_set_param` (which takes `stateMutex` outright). Both are priority-inversion hazards that contradict the header's own contract. Use `AudioProcessorParameter::setValue`, the RT-safe variant, as `live.cpp` already does.

- [ ] **`send_midi`'s ring is single-producer** (review, from H11). It no longer shares the libremidi input thread's ring, but its own is SPSC too, so concurrent `send_midi` from several threads would reintroduce the same corruption. The single-thread contract is documented in `minihost_audio.h` and the binding; make it MPSC if multi-thread sending ever becomes a supported pattern.

- [x] **Audio-thread diagnostics in the desktop app** Fixed 2026-10-04: the audio thread sets flags; a message-thread timer prints them. The silence scan stays on the audio thread; it is bounded and lock-free. Original note: (review L6). `live.cpp` scans every output buffer for a non-zero sample on *every* callback, and keeps doing so for as long as the project is silent; `fprintf` on the audio thread is acknowledged in comments but still an RT violation. Move both behind a lock-free flag consumed by the GUI timer.

## Low

- [ ] **MIDI/OSC input odds and ends** (`build/audit-osc-midi/`):
  - (Fixed 2026-09-25.) A failed `MidiFile.load()` left the object mutated: a header declaring 65535 tracks returns False with `num_tracks` = 65535 and the previous contents gone (confirmed). Parse into a temporary and swap on success.
  - 5-byte VLQs are accepted; the spec maximum is 4 (confirmed).
  - `load()` returns a bare bool; midifile prints the reason to stdout/stderr. Capture and raise it.
  - JUCE computes an OSC blob's `size + 3` in signed int and overflows on a huge length (UBSan-confirmed with `osc_harness blob`); no effect in minihost, since blobs become 0.0.
  - `OscFeedback.stop(timeout)` drops the thread handle even when the join timed out, so a later `start()` can run two pollers (`feedback.py:124-130`; inferred).
  - `MidiMapper` 14-bit CC keeps the old LSB when a new MSB arrives; common practice resets it, avoiding a transient value (inferred).

- [ ] **Audio buffer odds and ends** (`build/audit-audio-io/`):
  - Negative or empty sizes become 1 channel: `AudioBuffer(-3, 5)` is `(1, 5)`, `b[0:0, :]` is `(1, 8)`, `from_numpy(zeros((0, 4)))` is `(1, 4)` (`jmax(1, channels)`, `_core.cpp:120`; confirmed). Raise on negative sizes; decide what an empty channel slice should be.
  - Index errors have the wrong type: a huge int index raises `RuntimeError std::bad_cast`, and `np.int64` indices or an `np.float32` scalar assignment raise `TypeError` (`_core.cpp:175`; confirmed). Use `PyNumber_Index` / `nb::try_cast`.
  - 1-D input is handled four ways: `write_audio` accepts it, `process_audio` raises an opaque signature `TypeError`, `resample` returns `(1, N)`, `Compose` returns 1-D (confirmed). Normalise in one helper.
  - Output channels beyond the plugin's count keep their old contents: a `(4, 16)` output filled with 7.0 comes back `[1, 1, 7, 7]` (confirmed). Zero them or document it.
  - BWF description truncation at 256 bytes can split a UTF-8 character; `append_bext_chunk` ignores `fclose`'s result, and a failed WAV write leaves a partial file, unlike FLAC (inferred).
  - `process_audio(in_place=True)` with MIDI past the audio end says it is "incompatible with tail_seconds > 0" even when the tail is 0 (inferred).

- [ ] **Lifecycle odds and ends** (all confirmed, `build/audit-lifecycle/`):
  - A NUL byte in a plugin path truncates it silently (`Plugin("a\x00b")` opens `a`).
  - Invalid `in_channels` / `out_channels` (-1, 64, 100000) are ignored without warning.
  - `AudioDevice` has no `close()`; `__exit__` only stops it, and `start()` after the `with` block plays again.
  - (Fixed 2026-09-25.) `save_project` wrote a NaN layout coordinate as bare `NaN` (invalid JSON).
  - Exiting with `open_async` loads in flight prints nanobind leak warnings.
  - `Session.scan_directory` errors give the path but no reason.

- [ ] **Chain and bus error messages and validation.** NaN or inf branch gain is accepted by `add_branch` and `set_branch_gain` (`minihost_graph.cpp:141,162`), the output becomes NaN, and `get_branch_gain` then raises a misleading "Branch index out of range". Calls on a closed chain or bus give unrelated messages ("Frame count 512 exceeds max block size 0"; `set_mix` blames channel counts; `add_branch` gives an empty error). A 0-frame block is accepted by `Plugin` but fails for chains and buses. All confirmed (`build/audit-chain-bus/misc.py`). Check finiteness and a closed flag in the wrappers; make the 0-frame rule consistent.

- [ ] **Stale bus docs.** `minihost_graph.h:121` and the `process_midi` docstring say branch MIDI output comes from each branch's first plugin; since C ABI 2.5.0 it is the last plugin's. The `add_branch` docstring (`_core.cpp`) says input channels must match; the code allows fewer.

- [ ] **VST3 plugins receive or return at most 2048 MIDI events per block.** The `MinihostTestFx` fixture, a pass-through, returned 2048 of 5000 events sent in one block, as a lone plugin and in a chain. The limit sits inside JUCE's VST3 hosting, below minihost, so `midi_out_dropped` cannot see it. Where exactly JUCE applies it (input event list or output) is inferred, not traced. Worth documenting next to the MIDI capacity notes, or confirming and raising if JUCE allows it.

- [ ] **The graph staging stale-pointer fix has no direct test.** `set_node_automation`, `set_midi_input_events` and `set_node_midi` now parse into a local buffer before swapping it into the slot the graph points at, so a parse error cannot leave the graph reading freed memory. The fix is by construction and was never reproduced: the extension is not built with ASan, and freed memory still holds the old events. An ASan build of the extension (or a C-level harness driving the same sequence) would pin it.

- [ ] **MIDI tuple validation exists twice.** `process.py::_check_midi_event` and `_core.cpp::parse_midi_event` apply the same ranges. The Python copy is needed (the block slicer clamps offsets before the native parser sees them), but the two can drift. Consider exposing the native check, or a test that feeds both the same bad tuples.

- [ ] **Confirm `graph_midi_bounds` under LeakSanitizer in CI.** The harness is built with ASan, which enables LeakSanitizer on Linux; macOS arm64 has no LSan, so it was checked only with `leaks --atExit` (0 leaks). The first `native-tests` run on `ubuntu-22.04` settles it.

- [ ] `mh_audio_resample`: the single-shot `ma_resampler_process_pcm_frames` never flushes the linear filter's internal delay, dropping a few trailing output frames. (It now errors rather than truncating silently when input is left unconsumed, which was the larger half.) Small.

- [ ] **`MidiMapper` documents a value range the plugin layer clamps away** (review L1). `control.py`'s docstring shows `map_cc(..., value_range=(-1.0, 1.0))`, but `mh_set_param` clamps to `[0, 1]`, so the bottom half of the fader travel maps to a constant 0. The documented example is actively misleading.

- [ ] **Chain channel truncation is silent** (review L5). `minihost_chain.cpp` zero-pads when the next plugin needs more channels but silently drops the extras when it needs fewer -- a 6-channel plugin feeding a stereo one loses channels 2-5 with no warning. Document it, or offer a downmix.

- [ ] **Decide on `sdist.include = ["thirdparty/JUCE"]`** (review L10). The sdist ships the whole JUCE tree (24 MB compressed, 4377 files, verified self-contained). That is deliberate and structural -- a source install builds without fetching JUCE -- but if the directory is absent at build time the sdist silently ships without it, which is the worse failure. Either document the intent or make its absence an error.

- [ ] **`mh_check_buses_layout` has a tautological guard** (review L12). `(input_channels && i < num_input_buses)` -- the second conjunct is the loop condition. Harmless, but it obscures intent.
