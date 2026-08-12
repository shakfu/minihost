# Changelog

## [Unreleased]

## [0.5.2]

Follow-ups from a review pass over the Python bindings and CI. No C ABI change (stays at
**2.4.0**): no symbols added or removed and no behaviour change on the C boundary -- the GIL
fix is entirely on the Python binding side.

### Fixed

- **`Session.open` and `Plugin.from_descriptor` held the GIL across the blocking plugin load.** The direct `Plugin(...)` constructor releases the GIL around the multi-second native instantiation (added in 0.5.0), but its two siblings did not. So loading a plugin through a `Session` -- the shared-format-manager path meant for multi-plugin and directory-scanning workflows -- or from a serialized `PluginDescription` (`from_descriptor`, the AudioUnit path) stalled every other Python thread for the whole load. The advertised "load one plugin on a background thread while another runs" pattern silently serialized on exactly these entry points. Both now carry the same `gil_scoped_release` call guard as the constructor. `open_async`, which already went through the constructor, was never affected.

### Testing

- **The desktop test suite now runs in CI.** The `build-desktop` job compiled the `minihost_desktop` binary and smoke-checked it with `--save-roundtrip`, but never ran pytest against it; the `build-wheels` job ran the full suite but built no desktop binary. The two never intersected, so the entire `@skip_if_no_desktop` set -- undo, autosave / crash-recovery, headless-render smoke, and C++/Python render parity -- executed in no job at all. `build-desktop` now runs it, split by cost: the binary-only tests (undo, autosave -- the platform-sensitive recovery paths) run on all three platforms, while the tests that import the `minihost` package to compare against `render_project` (smoke, render parity) run on Linux, where a single extra headless build backs them. Plugin-gated cases (MIDI-chain parity, plugin scan) skip cleanly on runners with no plugin installed.

## [0.5.1]

Continues the code-review pass: correct sidechain channel accounting, a host playhead that
actually moves during offline renders, and two more silent-discard paths turned into errors.
The C ABI moves to **2.4.0**: no symbols added or removed, but `MH_Info.num_input_ch`
changes meaning -- see below.

### Fixed

- **Sidechain audio was written to channels the plugin never reads.** `MH_Plugin::inCh` was set from JUCE's `getTotalNumInputChannels()`, which already sums every enabled input bus *including* the sidechain; `sidechainCh` then counted the sidechain a second time. For a compressor opened as `mh_open_ex(main_in=2, sidechain=2)` this meant `MH_Info.num_input_ch` reported 4 rather than 2, so callers had to hand `process_sidechain` an over-provisioned 4-channel "main" buffer *plus* a redundant 2-channel sidechain buffer -- and `mh_process_sidechain` then wrote the sidechain at channels [4, 6) while the plugin's sidechain bus lives at [2, 4). The sidechain signal landed in scratch channels the plugin never reads, and the real sidechain bus received whatever happened to be sitting in `main_in[2..3]`. The single overloaded count is now split in two: `mainInCh` (input bus 0 -- what the caller supplies, and what `num_input_ch` reports) and `totalInCh` (every enabled input channel, used only to size the internal process buffer). The plain `mh_process*` paths copy `mainInCh` channels and zero everything above, so a sidechain-configured plugin processed through the normal path is fed sidechain silence rather than reading past the caller's buffer.

  Verified end to end: a loud external sidechain now ducks a compressor by -7.3 dB where the pre-fix build measured 0.00 dB, with an internal-sidechain control confirming the change is attributable to sidechain routing.

  On the Python side, `process_audio` now sizes the sidechain buffer from `plugin.sidechain_channels` instead of the main input width, and `process_audio_to_file` matches a sidechain *file* to the sidechain bus rather than the main bus.

- **Offline renders never advanced the host playhead.** `MidiRenderer` did not call `set_transport` at all -- despite already parsing the MIDI file's tempo map to place events -- so anything tempo-synced (a synced delay, an arpeggiator, an LFO, a step sequencer) ran at its own default tempo with the playhead pinned at sample 0, and a rendered file did not follow its own tempo. `process_audio(..., bpm=...)` was only marginally better: it called `set_transport` exactly once, before the block loop, with `position_samples=0`, so the tempo was right but time stood still. Both now push an advancing transport before every block. `MidiRenderer` derives the tempo *and* the beat position from the file's tempo map through a new inverse of the tick-to-seconds conversion it already used, so musical position follows the map rather than wall-clock time scaled by a single tempo -- a file that changes tempo stays in sync. `PluginChain` still cannot be driven this way (there is no chain-level `set_transport`) and is skipped.

- **`process_audio` silently discarded MIDI when a sidechain was supplied.** `mh_process_sidechain` is the only process entry point with no MIDI parameter, so the sidechain block loop collected each block's events and then dropped them. The combination is now rejected with an explanation instead. The underlying gap remains: a MIDI-driven plugin with a sidechain cannot be rendered offline until `mh_process_sidechain` grows MIDI input.

- **Plugin scanning missed single-file VST3 plugins.** `mh_scan_directory` searched only for *directories* matching `*.vst3`. That is right on macOS, where a VST3 is a bundle, but on Windows and Linux a VST3 is very often a single shared library file -- and every such plugin was invisible to `scan_directory`, `Session.scan_directory` and `minihost scan`. Both scan paths now search files as well. Untested on the platforms it affects: every VST3 on the development machine is a bundle.

- **Two- and one-byte MIDI messages were built as three-byte messages.** Program Change (0xC0) and Channel Pressure (0xD0) take one data byte, and System Real-Time (0xF8-0xFF) take none, but every `MH_MidiEvent` was handed to `juce::MidiMessage`'s three-byte constructor, which hard-codes `size = 3` and trips JUCE's own length assertion in debug builds. Message construction now picks the right constructor from `getMessageLengthFromFirstByte`. In a release build this is not observable with the plugins tested -- Program Change, Channel Pressure and Pitch Bend round-trip identically through a MIDI-effect plugin either way -- so the practical impact is the debug-build assertion and malformed `MidiBuffer` contents that a strict plugin could reject.

- **`AudioDevice.send_midi` raced the MIDI input thread.** It pushed onto the same lock-free ring buffer the libremidi input thread owns. That structure is single-producer/single-consumer, so a connected MIDI input plus programmatic sends made two producers on it -- corrupting its indices and losing or duplicating events. This is precisely the combination `MidiMapper`'s own documentation recommends (mapping a pad to `audio.send_midi(...)` while a `MidiIn` is open). Sends now use a separate ring, drained alongside the input ring by the audio thread. `send_midi` must still be called from a single thread; that contract is now documented in both the C header and the Python binding.

- **Desktop: stale audio leaked into live input buffers.** `LiveEngine` cleared its planar input buffers with one contiguous `memset` of `n * channels` floats, which only covers the first channel's worth when the device callback is smaller than the project's block size -- the normal case. Any input node with two or more channels therefore kept the previous block's samples in every channel after the first. Now cleared per channel.

- **Desktop: a device/project sample-rate mismatch was ignored.** The engine logged the device rate and carried on, while transport, metronome and MIDI-clock maths all used the project rate and the plugins had been instantiated at it -- so a 48 kHz project on a 44.1 kHz device played at the wrong speed and pitch with the tempo drifting, and the only symptom was "it sounds wrong". The mismatch is now detected, logged as an error naming both rates and what to change, and exposed via `hasSampleRateMismatch()` so the app can surface it.

### Changed

- **`MH_Info.num_input_ch` / `Plugin.num_input_channels` now report the main input bus only** (C ABI 2.4.0; struct layout unchanged, meaning changed). Sidechain channels are reported separately by `mh_get_sidechain_channels` / `Plugin.sidechain_channels`. Code that sized its main input buffer from this value against a sidechain-configured plugin was over-provisioning; it will now allocate the correct width. Plugins without a sidechain or aux input bus are unaffected -- for them the main bus *is* the total.

- **Supplying sidechain audio to a plugin with no sidechain bus is now an error.** `process_audio(..., sidechain=...)` raises `ValueError` instead of silently discarding the audio. Note that `Plugin(..., sidechain_channels=N)` still succeeds for such a plugin (it simply reports `sidechain_channels == 0` afterwards), so "it opened" was never evidence that a sidechain existed.

### Testing

- **`tests/test_sidechain_channels.py`** -- six tests pinning the per-bus invariants: `num_input_channels` equals the main bus, `sidechain_channels` equals the sidechain bus, the two are disjoint, and buffers sized by the reported counts are accepted. Three fail against the pre-fix build. They need a plugin that genuinely has a sidechain bus and skip otherwise -- no VST3 on the development machine has one, but several MeldaProduction AudioUnits do, and the suite passes against those.

- **`tests/test_scan_and_midi_bytes.py`** -- sixteen tests over scanning and MIDI message lengths. Deliberately modest: each docstring states what it actually pins, since the scan fix is not observable on macOS and the MIDI length fix is not observable in a release build.

- **`tests/test_sidechain_signal.py`** -- the behavioural counterpart to the accounting tests: with a 440 Hz main tone and an 80 Hz sidechain tone, an external sidechain must duck the output (measured -7.3 dB on FabFilter Pro-C 3), while the same audio with the sidechain source set to internal must change nothing (control). Against the pre-fix build the external case measures +0.00 dB -- the sidechain never reached the detector. Needs a compressor that acts on an external sidechain; override with `MINIHOST_TEST_SIDECHAIN_PLUGIN` or it skips.

- **`tests/test_transport_advance.py`** -- nine tests covering the tempo-map inverse (including across a tempo change), a monotonically advancing playhead, beats tracking the configured tempo, no transport being fabricated when no BPM is given, and the MIDI+sidechain rejection. Five fail against the pre-fix code. They assert on what minihost *sends*: `MH_PlayHead` is write-only from the host side and nothing reads it back, so a plugin visibly reacting to host position is not covered.

- Two pre-existing sidechain tests in `tests/test_process_audio_extended.py` were passing against Dexed, which has no sidechain bus at all, so they were asserting that a path which quietly discarded the sidechain "runs". They now skip unless the configured plugin really has a sidechain, and a new test covers the rejection case.

## [0.5.0]

From a code-review pass over the native layer, the Python bindings, the CLI and the desktop
app. All three crash-class findings are fixed, along with two subsystems that reported
success while doing nothing: MIDI ports and `.vstpreset` interchange. Block-size and
sample-rate contracts are now validated where they are configured rather than failing
per-block, and the Python bindings finally release the GIL around native work. The C ABI
bumps to **2.3.0** (additive: `mh_get_max_block_size`). Behaviour changes are called out
under *Changed*.

### Fixed

- **`AudioDevice` corrupted memory whenever the device and plugin channel counts disagreed.** `MH_AudioDevice` allocated its non-interleaved `input_buffers` / `output_buffers` with exactly the *device's* channel count, then handed those pointer tables straight to `mh_process` / `mh_chain_process`, which read the plugin's input count and write its output count -- neither bounded by the device's. Any plugin with more channels than the device walked off the end of a `float**` on the audio thread. Two documented routes in: `AudioDevice(plugin, output_channels=N)` with `N` below the plugin's count (a supported override), and the device simply negotiating fewer channels than requested, since the count is read back from the *actual* device. `AudioDevice(stereo_plugin, output_channels=1).start()` segfaulted the interpreter within one audio callback. Fixed by sizing the buffers to `max(device channels, plugin inputs, plugin outputs)` and zero-filling any channel the device does not supply; the public `channels` property still reports the device's own count, because `write_input` interleaves against it. Surplus plugin outputs beyond the device's channel count are truncated at the interleave step (a downmix is left as future work).

- **`MidiIn.open` / `MidiIn.open_virtual` registered a dangling pointer -- the whole standalone MIDI-input feature was a use-after-free.** Both factories built a `MidiIn` on the stack, passed `&m` to `mh_midi_in_open` as the callback's `user_data`, then returned *by value*; nanobind move-constructed the result into its heap instance, leaving the C layer pointing at destroyed stack memory. The first MIDI byte received dereferenced it, acquired the GIL, and called through a garbage `nb::callable`. Affected `MidiIn`, `MidiMapper`, `minihost midi -m`, and the control-surface workflow. Fixed by heap-allocating in the factory, registering the final address, and transferring ownership to Python (`nb::rv_policy::take_ownership`); `MidiIn`'s move operations are now `= delete`d alongside its copy operations, so the address can never change after registration -- a compile-time guard against the same mistake returning.

- **MIDI was non-functional on every platform: the back-end was never compiled in.** `libminihost_audio` builds libremidi in header-only mode, which bypasses libremidi's own CMakeLists and therefore never defined the platform back-end macro its `cmake/libremidi.{macos,winmm,alsa}.cmake` would have set. `backends.hpp` fell through to `#define LIBREMIDI_DUMMY`, so every shipped build -- macOS, Windows, Linux, wheel, CLI and desktop app -- compiled a stub: `midi_get_input_ports()` / `midi_get_output_ports()` always returned `[]`, every open failed, and `AudioDevice(midi_input_port=...)` silently never connected. Linking `-framework CoreMIDI` / `winmm` / `libasound` is necessary but not sufficient; the macro is what pulls the back-end in. Enabling it exposed three further defects, all fixed here:

  - **Virtual and software ports were filtered out.** libremidi defaults to `track_hardware=true, track_virtual=false`, and its CoreMIDI classifier treats any endpoint without a hardware entity as virtual -- which covers macOS IAC buses, ALSA/JACK software ports, and endpoints published by other applications. minihost constructed the observer with all defaults, so enumeration returned nothing while the platform reported ports. It now passes an explicit `observer_configuration` tracking hardware, virtual and "any", and enumeration matches the platform's own view.

  - **Enumeration and port opening crashed or spuriously failed in a long-lived process.** libremidi's CoreMIDI back-end pumps the *calling thread's* CoreFoundation run loop (`CFRunLoopRunInMode`) at six sites, including `observer::get_{input,output}_ports` and `midi_{in,out}::open_port`. On an application main thread that is also driving JUCE, CoreAudio and plugin code, that single pass dispatches whatever those subsystems have queued -- including work whose owner has already been torn down. Enumeration killed the process (`SIGABRT` / `SIGBUS`), uncatchably, because `get_input_ports()` is `noexcept` and a throw inside it reaches `std::terminate` before any handler can unwind; `open_port` threw `std::bad_function_call` from an unrelated queued callback, surfacing as a spurious "failed to open MIDI port". Both are fixed by running these calls on a fresh thread whose run loop has nothing queued, so the pump is a no-op. Message delivery is unaffected -- CoreMIDI invokes `MIDIReadProc` on its own thread, not the run loop of whichever thread created the port.

  - **Failures reported "Unknown error".** libremidi signals with `stdx::error`, which deliberately does not derive from `std::exception`, so the `catch (const std::exception&)` arm missed it entirely and the `catch (...)` fallback discarded the reason -- which is what made the above undiagnosable. All four open paths now share a helper that handles `stdx::error` explicitly, falls back to `std::exception`, and otherwise names the dynamic exception type.

  Verified on macOS/CoreMIDI only. The WinMM and ALSA macros mirror libremidi's own cmake but have not been built or exercised; neither platform has the run-loop hazard that motivated the isolation work. Both want a smoke test before release.

- **`.vstpreset` files were not interoperable, and loading a real one silently did nothing.** JUCE's VST3 host does not expose the raw component state through `getStateInformation`: it emits its own `<VST3PluginState>` XML (with the raw `IComponent` / `IEditController` chunks base64'd inside) wrapped in `copyXmlToBinary`'s `'VC2!'` container. minihost wrote that whole container into the file's `Comp` chunk, where the format specifies the *raw* component state -- so the resulting file was readable only by minihost, and any other host would hand those bytes to `IComponent::setState` and get garbage. The reverse was worse: `load_vstpreset` fed a genuine preset's raw chunk to `set_state()`, JUCE's `setStateInformation` failed its `getXmlFromBinary` check and simply returned, and the user saw success with the default patch still loaded. Both directions now convert between the two representations, so saved files are spec-shaped and third-party presets actually apply. Presets written by older minihost versions are detected and passed through unchanged -- the discriminator is the `<VST3PluginState>` root element rather than the `VC2!` marker, because a JUCE-*built* plugin returns its own `copyXmlToBinary` blob from `IComponent::getState` and the marker alone does not separate the layers.

- **`set_state` / `set_program_state` could never report failure.** `setStateInformation` is `void`, so a plugin (or format wrapper) that rejects a blob does so silently and minihost returned 1 regardless -- `Plugin.set_state(b"garbage")` "succeeded". This is what let the `.vstpreset` defect above stay invisible, and it applies to every state path (`--state`, `--vstpreset`, `load_chain`, project `state_b64`, and the batch loop's per-file reset). There is no format-agnostic way to ask a plugin whether it accepted a state, so the call is now *observed*: parameter values are snapshotted before and after, falling back to comparing the serialized state only when parameters cannot tell the cases apart; if nothing moved and the caller asked for something different from what was already loaded, the blob was ignored and the function returns 0. False negatives remain possible for a plugin whose serialized state is non-deterministic -- the safe direction, since it never reports failure for a state that *was* applied. Costs one extra `getStateInformation` per call.

- **`mh_chain_process_auto` dereferenced null channel pointers on the documented "silence" contract.** `minihost.h` documents a `NULL` input/output pointer table as "supply silence / discard output", and `mh_process_midi_io` honors it, but the chain's automation path built a non-null table *of null pointers* and passed it down, so `mh_process_midi_io` saw a truthy table and `memcpy`'d from `NULL`. Not reachable from Python (which always materializes arrays), but it is a documented C API contract with shipped C/C++ consumers. Null-ness is now propagated.

- **Block-size and sample-rate mismatches failed per-block, or not at all.** Nothing could ask a plugin what block size it had been prepared for, so no layer validated and every mismatch surfaced late and unhelpfully. `PluginChain` hard-coded `max_block_size = 8192`, advertising a ceiling no member could honour: a caller sizing blocks against it passed the Python shape check and then failed inside `mh_process` with "Chain process failed", naming neither the real limit nor the plugin imposing it (and every intermediate and dry-mix buffer was allocated for 8192 frames regardless of need). `AudioDevice` never compared the device period to the plugin's limit, so every process call was refused, the return value was ignored, and -- because the output buffers are allocated once and never cleared -- the device replayed the previous block indefinitely as a buzz, with no error anywhere. `PluginGraph` documented that plugin nodes must match its sample rate and block size but checked neither, so a rate mismatch rendered silently at the wrong rate. All three now validate at configuration time with messages naming both numbers and what to change; the chain derives its limit as the minimum across its members, and the audio callback additionally zero-fills on a refused block so an unexpected failure degrades to silence rather than a buzz. Validation is against the device *period*, not the 2x internal headroom on the conversion buffers -- the common case of a plugin and device sized alike keeps working.

- **The Python bindings never released the GIL around native work.** Every process call held it for the full duration of `processBlock`, so a multi-threaded host got no parallelism whatsoever (measured 1.00x on 4 threads), and plugin construction, state save/restore and audio-file I/O all blocked the interpreter. It also meant a `MidiIn` callback -- which must acquire the GIL -- could not run until the in-flight process call returned, delaying or dropping MIDI in exactly the live-control scenario the API is for. The pure-native bindings now carry `nb::call_guard<nb::gil_scoped_release>`; the ones that also handle Python lists or bytes (`process_midi`, `process_auto`, `render_block`, `get_state` / `set_state`, the audio-file functions) release it around just the native call, with errors raised only after it is reacquired. Same workload now measures 3.47x on 4 threads.

### Added

- **`mh_get_max_block_size` (C API, additive -- ABI 2.3.0).** Reports the largest block a plugin was prepared for. Exposed in Python as `Plugin.max_block_size` and `PluginChain.max_block_size` (the latter being the minimum across its plugins), so callers can size blocks against the real limit instead of guessing. `process_audio`'s internal block size now derives from it rather than a hard-coded 512.

- **`vst3_state_split` / `vst3_state_join` (Python bindings).** Convert between a JUCE VST3 plugin-state blob (what `Plugin.get_state()` returns) and the raw component / controller chunks the `.vstpreset` format specifies. Used by `save_vstpreset` / `load_vstpreset`, and useful directly for anyone moving state between minihost and another host. Implemented in C++ rather than Python because JUCE's base64 is a custom variant -- a `"<size>."` prefix followed by LSB-first 6-bit groups over its own alphabet -- so `juce::MemoryBlock`'s own codec is used and stays in step with JUCE by construction. `vst3_state_split` raises for anything that is not a JUCE VST3 *host* blob, which is also the discrimination the preset loader relies on.

### Changed

- **`Plugin.set_state` / `set_program_state` now raise on a blob the plugin demonstrably ignored** (see *Fixed*). Previously every call reported success. Code that was unknowingly feeding a plugin state it could not use will now see a `RuntimeError` where it used to see a silent no-op. Valid states -- including restoring the state a plugin is already in -- are unaffected.

- **`save_vstpreset` now writes the raw component chunk** rather than JUCE's container, making the files readable by other VST3 hosts. Files written by earlier versions still load (they are detected and passed through), so this is forward-compatible in the direction that matters; presets written by this version are *not* readable by older minihost.

- **`PluginChain.max_block_size` is now the minimum across its plugins**, not a fixed 8192. Chains whose members were opened with a smaller block size will reject an oversized block up front (with the real limit named) where they previously accepted it and failed mid-process. `AudioDevice` likewise now refuses to open when the plugin cannot span the device period, instead of opening and playing a buzz.

- **MIDI port enumeration now includes virtual and software ports** (IAC buses, ALSA/JACK software ports, endpoints published by other applications), where previously it reported hardware ports only -- moot in practice, since the back-end was a stub and nothing was reported at all.

### Testing

- **`tests/test_audiodevice_channels.py`** -- seven tests over the `AudioDevice` channel-count regression: narrower and wider device counts, the default path, duplex capture, and the `enable_input()` ring-buffer path. Confirmed adversarially (reverting the fix makes the suite crash pytest with SIGSEGV).

- **`tests/test_midiin_lifetime.py` + `tests/coremidi_loopback.py`** -- structural tests for the `MidiIn` lifetime fix plus an end-to-end reproduction that drives real MIDI through a minihost *virtual* input port and asserts the callback fires with an intact payload. The loopback is self-contained (a small ctypes CoreMIDI sender; no IAC bus, no second application, no new dependency), so it runs on any macOS machine. Confirmed adversarially: reintroducing the lifetime bug segfaults pytest at exactly that test. Four further tests need a real MIDI input port and skip without one.

- **`tests/test_vstpreset_interop.py`** -- nine tests against a real plugin covering chunk extraction, a split/join round-trip through the plugin, save/load parameter restoration, a foreign-style preset built from a raw chunk, legacy pass-through, and a corrupt preset that must now raise instead of silently doing nothing. Both interop tests fail against the pre-fix code. Interop with an actual *third-party* preset file remains unverified -- a filesystem-wide search found none on the development machine -- so that claim rests on the written chunk being byte-identical to what the plugin's own `IComponent::getState` produced.

- **`tests/test_block_size_contracts.py`** -- nine tests pinning the new behaviour across all three layers: the chain's derived limit and its up-front rejection, both graph preconditions, the device refusing an undersized plugin with an actionable message, and -- importantly -- that a plugin and device sized alike still opens, which an earlier draft of the validation wrongly rejected.

- **`tests/test_concurrency.py::test_process_releases_the_gil`** -- deliberately not a threaded-vs-serial speedup assertion, which would conflate minihost's behaviour with the plugin's: Dexed (the default test plugin) serializes internally and measures 0.89x on 4 threads where three other VST3s measure 3.3-3.7x. It instead spins a counter in a background thread and samples it either side of one long `process()` call -- 0 increments before the fix, ~250k after, for both a self-serializing and a well-behaved plugin. It has no "too fast to measure" guard on purpose: holding the GIL makes the call *faster*, so such a guard skipped exactly the regression it exists to catch.

- Mock-based tests in `tests/test_vstpreset.py` and `tests/test_cli.py` were asserting the old pass-through behaviour with arbitrary fake state bytes; they now use realistic JUCE-format blobs and assert the corrected contract.

## [0.4.2]

### Added

- **Autosave / crash recovery in the desktop app.** Plugins run in-process, so a misbehaving plugin can crash the app and lose unsaved canvas edits. The working `ProjectDocument` is now snapshotted to a sidecar (`autosave.json` + an `autosave.meta` recording its origin path, next to the desktop settings) on a heartbeat timer whenever it has unsaved edits, so a crash costs at most a few seconds of editing. A clean exit and every explicit Save delete the sidecar; a *surviving* sidecar on the next launch means the previous session ended without a clean shutdown, and the app offers to recover it (re-associating the original path so Save writes back to the right file). The About dialog and README now document the in-process limitation. The sidecar write/parse/clear mechanics are covered headlessly by `minihost_desktop --autosave-selftest=<project.json>` (`tests/test_desktop_autosave.py`); the timer and recovery dialog are GUI-thread orchestration and stay manual, as with undo/redo. All file paths in the schema are absolute, so the sidecar's own location does not affect reload.

- **Save-before-quit prompt in the desktop app.** Quitting with unsaved changes (window close, File > Quit, Cmd+Q, OS logout) now shows a Save / Don't Save / Cancel dialog instead of silently discarding the work; Save quits only after the write lands (routing through the Save As chooser for an untitled project), Cancel keeps the app open. This tracks a distinct "changed since last explicit Save" flag, separate from the autosave heartbeat's dirty flag, and is reset by Save / Save As / New / Open. Headless and single-plugin modes are unaffected (they never arm it).

- **Opt-in input resampling in the project renderer.** Project inputs are still strict about sample rate by default (a file whose rate differs from the project rate is an error), but an input node now accepts an optional `resample: true` field that converts a mismatched file to the project rate at load time. Both loaders implement it over the same C resampler (`mh_audio_resample`, miniaudio linear + 4th-order anti-aliasing low-pass): Python's `render_project` resamples via `audio_io.resample`; the C++ desktop resamples in `loadProject`. Because it is the same underlying function, a resampled render is bit-identical between the two pipelines (asserted in `tests/test_desktop_smoke.py::test_resample_render_parity`). The desktop's "Add Input..." flow offers a Resample / Add-as-is choice on a rate mismatch instead of the old warning, and the per-input Properties dialog exposes the flag. The schema stays version 1 (additive; the field is only emitted when true). No new dependency -- `libsamplerate` is not required.

- **`morph` command in the Python CLI.** Brings the `minihost` CLI to parity with the C/C++ front-ends and the library API: `minihost morph PLUGIN [-t T]` captures snapshots A and B from factory programs (`--a-program` / `--b-program`) or saved state files (`--a-state` / `--b-state`), interpolates at blend `-t` (default 0.5), prints an A/B/blend table (or `--json`), and optionally `--apply` / `--save`s the result. Built on the native `Plugin.morph_capture` / `morph_apply` bindings plus `minihost.lerp_params`. Documented in the CLI reference.

### Fixed

- **C and C++ front-ends aborted (SIGABRT) at process exit after loading a plugin.** Neither `minihost_c` nor `minihost_cpp` shut the dedicated JUCE plugin thread down, so on exit its `std::thread` was still joinable and `std::terminate` fired -- any command that loaded a plugin (`params`, `load-preset`, `process`, `morph`, ...) exited with code 134 after printing correct output. Latent since the plugin-thread work in 0.3.0 (the front-ends were last synced at 0.2.x). Fixed by bringing the thread up with `mh_message_thread_init()` and registering `mh_message_thread_shutdown()` via `atexit` at the top of `main`; constructing the thread before the registration makes C++ teardown ordering run the shutdown before the thread's own destructor. Surfaced by the new CLI conformance test's exit-code check.

### Testing

- **CLI conformance test (`tests/test_cli_conformance.py`).** Runs the same deterministic data commands (`probe`, `info`, `params`, `presets`, `morph`) through both `minihost_c` and `minihost_cpp` and asserts their stdout is byte-identical and both exit cleanly, so the two independently-written front-ends can no longer drift apart silently. Discovers the binaries via `MINIHOST_C_BIN` / `MINIHOST_CPP_BIN` or the build tree (preferring the most recently built), and skips when the binaries or a test plugin are absent (same plugin-gated pattern as the other integration tests).

## [0.4.0]

Additive release: a callable composition layer over the existing routing classes (Python), plus parameter morphing pushed down into the C library and its C/C++ front-ends. The C ABI bumps to **2.2.0** (additive).

### Added

- **Parameter morphing in the C library (`libminihost`).** The A/B parameter-interpolation capability that previously existed only as the pure-Python `minihost.morph` module is now a first-class part of the C API: `mh_morph_capture` (snapshot every parameter's normalized value), `mh_morph_apply` (restore a snapshot, clamped to `[0, 1]`), `mh_morph_lerp` / `mh_morph_lerp_per_param` (interpolate two snapshots with a scalar or per-parameter blend), and `mh_morph` (interpolate and apply in one call). Composed from the existing `mh_get_param` / `mh_set_param` / `mh_get_num_params` entry points, so each per-parameter access keeps class-2 thread safety (safe to overlap `mh_process`); the lerp helpers are pure array math. Additive symbols only; the C ABI moves to 2.2.0.

- **`morph` command in the C and C++ front-ends.** Both `minihost_c` and `minihost_cpp` gain a `morph PLUGIN` subcommand exposing the new API: capture snapshots A and B from factory programs (`--a-program` / `--b-program`) or saved state files (`--a-state` / `--b-state`), interpolate at a blend `-t` (default 0.5), print an A/B/blend table (or `--json`), and optionally `--apply` and `--save` the morphed state. Defaults to morphing factory programs 0 and 1 when no source is given. The two front-ends produce byte-identical output.

- **Native morph bindings on `Plugin` (Python).** For parity with the C/C++ front-ends, the morph C API is now bound at the nanobind layer: `Plugin.morph_capture()` (snapshot as a list, one value per parameter), `Plugin.morph_apply(values)` (restore, clamped; `ValueError` on length mismatch), and `Plugin.morph(a, b, t)` (interpolate, apply, and return the applied snapshot). These run natively in a single call and are distinct from the existing duck-typed pure-Python `minihost.morph` module, which is unchanged.

- **`minihost.Compose` -- callable, composable audio pipelines.** An [audiomentations](https://github.com/iver56/audiomentations)-style layer over the native `Plugin` / `PluginChain` / `PluginBus` classes. Where those model *real-time* signal routing, `Compose` models an *offline* pipeline: an ordered list of transforms applied to a whole buffer and returned as a new one. A pipeline is callable in the audiomentations idiom (`fx(samples, sample_rate=...)`), preserves the input container family (`AudioBuffer` in -> `AudioBuffer` out; numpy in -> numpy out; 1-D in -> 1-D out), and owns/closes the plugins it holds (`close_children=True` by default) so an effect chain collapses to a single `with`. A `.to_file(input, output)` convenience reads, processes, and writes in one call. Tails are handled once at the pipeline boundary: a numeric `tail_seconds` pads the input up front so every element rings out, and `tail_seconds="auto"` over-renders then trims trailing silence. Sample rate is validated against each native processor's construction rate and never silently resampled (a mismatch raises).

  A *transform* is a native processor, a nested `Compose`, one of the pure-python transforms below, one of the stochastic combinators below, or any callable `fn(audio, sample_rate) -> audio`. The working type is `AudioBuffer`, so numpy stays an optional dependency (imported only for numpy input, `tail_seconds="auto"`, or the transforms that need it).

- **Pure-python transforms.** Deterministic, `AudioBuffer`-native, usable inside or outside a pipeline: `Gain(db)`, `Normalize(peak_dbfs=-1.0)` (silence passes through), `Trim(start, duration)` (time window in seconds), and `Fade(fade_in, fade_out)` (linear fades in seconds). Each returns a new buffer.

- **Stochastic combinators for data augmentation.** `Maybe(transform, p=)`, `OneOf([...], weights=)`, `SomeOf(n, [...])` (fixed count or a `(min, max)` range), `RandomParam(plugin, param, lo, hi)` (set a plugin parameter at random in normalized units, then process), and `AddGaussianNoise(min_amplitude, max_amplitude)`. `Compose(seed=...)` seeds the pipeline RNG and `Compose(shuffle=True)` randomizes transform order per call, so a pipeline is reproducible across runs but varied across calls. The routing combinators use Python's `random` module (no numpy); only `AddGaussianNoise` uses numpy, seeded deterministically from the pipeline RNG.

  All exported at the package top level. Coverage in `tests/test_compose.py` (34 numpy-only tests plus 6 gated on a real plugin). A runnable walkthrough in `examples/compose.py` and a dedicated [Composition Pipelines](composition.md) documentation page.

## [0.3.2]

### Fixed

- **Process-exit hang on Linux from the dedicated plugin thread (completes the 0.3.1 fix).** 0.3.1 fixed the *import* hang, but the plugin thread could still be started by a plugin-load *attempt* -- including a load of a nonexistent path -- which created a JUCE `MessageManager` on a background thread. Left alive, that MessageManager deadlocked process exit on Linux: the test suite passed, then the process hung until killed. (macOS/Windows tolerated it.) Two changes: (1) the plugin thread is now only started for a plugin that actually exists on disk, so a failing/probing load never touches JUCE; and (2) the thread is cleanly stopped and its MessageManager torn down on its own thread at interpreter exit, via an `atexit` handler (`mh_message_thread_shutdown`). Well-behaved processes now exit promptly after using a plugin.

## [0.3.1]

### Fixed

- **Headless / CI hang from the dedicated plugin thread (regression in 0.3.0).** 0.3.0 started the plugin thread eagerly at `import minihost` and initialized it with `juce::initialiseJuce_GUI()`. That pulls in GUI/display setup which blocks in a headless environment with no X server (e.g. a Linux manylinux/CI container), so `import minihost` -- and therefore any process using the package -- could hang. macOS and Windows were unaffected. Two fixes: (1) the plugin thread now creates only the JUCE `MessageManager` (`MessageManager::getInstance()`), not the GUI subsystem -- the same MessageManager that plugin construction already created on the headless path before 0.3.0; and (2) the thread starts lazily on the first plugin load instead of at import, so a process that never loads a plugin does no JUCE initialization at all. `open_async` and cross-thread plugin use are unchanged. (The CI wheel job also gained a `timeout-minutes` backstop so a future hang fails fast instead of running to the 6-hour ceiling.)

## [0.3.0]

Bug fixes on flagship paths (sample-accurate automation, honest channel counts), additive features (preset morphing, zero-copy channel views, BWF metadata, a `MIDI_OUT_CAPACITY` constant), and a threading overhaul: minihost now runs a dedicated native plugin thread so plugins are safe to use across threads and `open_async` works for real. Additive C symbols only (`mh_message_thread_init` in `minihost.h`, `mh_audio_write_bwf` in the audio-file library); the process/parameter ABI is unchanged. From a code-review pass over the native layer, the Python bindings, and the test suite.

### Fixed

- **Sample-accurate automation dropped in-block parameter changes.** `mh_process_auto` and `mh_chain_process_auto` computed the chunk boundary from the pending parameter change *before* the apply-loop advanced past changes already due at the chunk start. When two or more changes fell in a single process block, every change after the first was silently swallowed (never applied) rather than mis-timed. Fixed by applying all due changes first, then setting the chunk boundary from the next still-pending change. Regression coverage in `tests/test_process_auto_automation.py` (reproduced against the pre-fix build, passes after).

- **Honest channel counts.** A plugin now reports its true JUCE channel counts instead of an inflated minimum of one: a synthesizer with no audio input reports `num_input_channels == 0` (was `1`), and a rare 0-output plugin reports `0`. The internal processing buffer keeps a one-channel floor so a pure-MIDI plugin still receives a valid buffer, and the Python process pipeline was made synth-input-aware. **Potentially breaking** for callers that assumed at least one input channel; buffer validation is "at least N", so over-provisioned callers are unaffected. Relatedly, the offline MIDI renderer now honors a plugin's real output channel count (`max(num_output_channels, 1)`), so a genuine mono plugin renders one channel instead of a stereo file with a silent second channel.

### Added

- **`AudioBuffer.channel_view(start, count)`.** Returns a new `AudioBuffer` that aliases (zero-copy) the contiguous channel range `[start, start+count)` of the parent. Writes are visible in both directions; the parent is pinned for the view's lifetime. Channel ranges only (channels are stored contiguously); frame slicing would require strided views and is not offered. Also available on `AudioBufferD`.

- **Preset morphing (`minihost.morph`).** A small utility for A/B parameter interpolation: `capture(plugin)` snapshots normalized parameter values, `apply(plugin, snapshot)` restores them (clamped to `[0, 1]`), and `lerp(a, b, t)` interpolates two snapshots (scalar or per-parameter `t`). `morph(plugin, a, b, t)` interpolates and applies in one call. Re-exported as `capture_params` / `apply_params` / `lerp_params` / `morph_params`. Operates on per-parameter values, not opaque VST/AU state blobs.

- **Broadcast Wave (BWF) metadata.** `write_audio(path, data, sr, bwf=dict(...))` embeds an EBU Tech 3285 `bext` chunk in WAV output (`description`, `originator`, `originator_reference`, `origination_date`, `origination_time`, `time_reference`). New C entry point `mh_audio_write_bwf` in the audio-file library; `mh_audio_write` is now a NULL-metadata wrapper over it. WAV only (FLAC raises). `smpl` sampler-loop chunks are out of scope.

- **`minihost.MIDI_OUT_CAPACITY` constant.** Publishes the default 256-event MIDI-output buffer capacity used by `process_midi` / `process_auto`; a returned MIDI-out list whose length equals the capacity signals possible truncation (raise the per-call `midi_out_capacity` for dense streams).

### Verified

- **DLPack export confirmed zero-copy.** A review flagged `AudioBuffer. __dlpack__` as possibly not returning a DLPack capsule; empirical testing refuted it -- it returns a proper `"dltensor"` capsule, shares memory with `as_ndarray()`, and buffer mutations are observed through the view (no hidden copy). No code change. Added `tests/test_dlpack_interop.py` (numpy always; torch/jax via `importorskip`). Documented nuance: `numpy. from_dlpack` imports read-only (numpy 2.x default); use `as_ndarray()` for a writable zero-copy view.

### Documentation

- **Process-vs-control threading contract on `Plugin`.** Added a class docstring making the Python-facing contract explicit: the lock-free process methods must be called from a single thread and must not overlap the reconfiguring setters (`sample_rate`, `set_state`, `set_processing_precision`, `set_non_realtime`, `reset`), which reconfigure the audio pipeline and are not protected by the internal control-thread mutex.

### Changed

- **Dedicated native plugin thread: `open_async` now works, and plugins are thread-safe for control operations.** JUCE VST3/AU instances are thread-affine -- construction, destruction, and control-plane queries (state, parameter text, program names, reset, sample-rate, precision) must all run on one thread -- which made the old `open_async` deadlock (it built the plugin on a short-lived thread and used/closed it from another). minihost now runs one persistent JUCE plugin thread and marshals every thread-affine control op onto it via an internal request queue, so a plugin may be constructed on one thread and used or closed from any other. The real-time `process*()` path stays lock-free on the caller's thread. Enabled by default; opt out with the `MINIHOST_MESSAGE_THREAD=0` environment variable (after which cross-thread plugin use is unsafe again).

  Consequently `open_async` is now a plain, safe async loader: it returns a `Future` resolving to a real `Plugin` (no proxy wrapper, no warning), usable and closable from any thread. Loads are serialized on the plugin thread, so this is non-blocking rather than parallel loading. The prior investigation (a persistent-worker proxy, then a background JUCE dispatch loop) is superseded; the working design marshals via a plain condition-variable queue rather than JUCE's own `callFunctionOnMessageThread` / `CallbackMessage`, which proved unreliable on macOS.

## [0.2.1]

All changes below are additive or bug fixes relative to the published 0.2.0; the C ABI is bumped to **2.1.0** (additive).

### Added

- **Persistent plugin-scan cache.** A new `minihost.plugincache` module keeps a JSON index of probe metadata keyed by plugin path, with a filesystem fingerprint (mtime + size) so stale entries are re-probed automatically. A repeat scan of an unchanged directory probes nothing and returns instantly; failed probes are remembered (not retried every scan). API: `scan()`, `info()`, `query(...)` (filter by format / name / vendor / MIDI / I/O), `all_entries()`, `prune()`, `clear()`, `stats()`, `cache_file()`. Convenience re-exports `minihost.scan_plugins` and `minihost.query_plugins`. The CLI `scan` now uses the cache by default (`--refresh` re-probes, `--no-cache` bypasses); `info --probe` is served from it; and a new `cache` subcommand (`path` / `stats` / `list` / `prune` / `clear`) manages and queries the index. Cache location honors `MINIHOST_CACHE_DIR` (defaults to the platform cache dir). Probe-level metadata only -- parameter lists need a full load and are not cached.

- **`AudioBufferD` (float64 audio buffer).** A double-precision sibling of `AudioBuffer` with the identical surface (indexing, slicing, DSP ops, numpy interop, DLPack). It completes the numpy-optional story for float64: `AudioBufferD.dtype == "float64"`, `as_ndarray()` returns a float64 view, and its DLPack export is float64 -- so it feeds `Plugin.process_double(in, out)` directly, with no numpy arrays involved. Implemented as a single C++ template (`MhAudioBufferT<T>`) instantiated for float and double, so the two classes never drift. `AudioBuffer` (float32) is unchanged. Exported as `minihost.AudioBufferD`.

- **MIDI routing in the project schema.** Offline project files (`minihost.render_project`) can now express MIDI graphs, not just audio. New node kinds: `midi_input` (offline `source` .mid path), `midi_output` (offline `sink` .mid path), `midi_filter` / `midi_transpose` / `midi_velocity_curve` (per-event processors), and `midi_merge` (fan-in). MIDI edges share the `edges` list, tagged `"kind": "midi"` (audio edges remain the default). This mirrors the node/edge schema the desktop app already parses, so projects round-trip between the two; the offline `source`/`sink` fields are additive (the desktop uses live `port_name`). The renderer reads each `source` at the project sample rate, stages events per block, and drains `midi_output` nodes to their `.mid` sink (written on a canonical 120 BPM / 480-tpq grid; sample timing preserved). New helper `minihost.midi_file_to_events(midi_file, sample_rate)` exposes the tempo-mapped flatten used internally. Schema version stays `1` (additive); audio-only projects are unaffected. Still deferred: parameter automation in the schema, and offline `.mid` reading in the desktop's headless `--render-project` (its renderer is live-port based).

- **`PluginBus` MIDI-out merge.** `PluginBus.process_midi` now collects the MIDI produced by each branch (from each branch's first plugin) and returns it as an `(events, overflow)` tuple -- `events` is the merged stream, a list of `(sample_offset, status, data1, data2)` tuples stably sorted by sample offset (events at the same offset keep branch order), and `overflow` flags truncation (see the Changed entry below). Previously it returned `None` and discarded branch MIDI. This completes the bus for parallel MIDI effects (e.g. a layer of arpeggiators driven by one part); 0.2.0 had shipped MIDI fan-*in* but not the *out* merge. The audio fan-out-and-sum is unchanged.

  New C ABI entry point `mh_bus_process_midi_io` (additive). It appends each branch's MIDI into the caller's buffer with no internal allocation, then stably insertion-sorts by offset on the audio thread, and reports truncation via an optional `midi_out_overflow` flag (conservative: it may flag an exact fill, but never misses a real drop). The existing `mh_bus_process_midi` (no MIDI out) is unchanged.

- **ThreadSanitizer stress harness for the lock-free ring buffers** (`tests/tsan/`, run with `make tsan`). Drives the SPSC MIDI and audio ring buffers from two threads under ThreadSanitizer and asserts SPSC correctness (exactly-once, in-order delivery with no field tearing). Contributor tooling; the instrumented binary is not shipped in the wheel.

### Changed

- **Configurable MIDI-out capacity.** `Plugin` / `PluginChain` / `PluginBus` `process_midi` and `process_auto` take an optional `midi_out_capacity` argument (default 256), so callers with dense MIDI output (e.g. MPE) are no longer silently capped. A returned event count equal to the capacity means output may have been truncated -- raise the cap if so. **`PluginBus.process_midi` now returns an `(events, overflow)` tuple** (the bus merge was added in this same release, so this is not a break vs 0.2.0): `overflow` is `True` when the merge filled `midi_out_capacity` and events may have been dropped. `Plugin` / `PluginChain` `process_midi` still return a plain list (single-plugin output; use the count-equals-capacity check).

### Fixed

- **Data race on the audio input callback.** `mh_audio_set_input_callback` wrote the callback pointer (and user-data) from the app thread while the audio thread read it unsynchronized (`minihost_audio.c`), risking a torn pointer on weakly-ordered CPUs. The pointer is now published / read through portable acquire/release atomics (`__atomic` builtins on Clang/GCC, `Interlocked` intrinsics on MSVC -- no C11 `<stdatomic.h>`, which MSVC gates behind an opt-in flag), with user-data published before the pointer, so the audio thread never observes a torn or mismatched callback. Callers must still clear (set NULL) before installing a different callback -- the existing live-source start/stop contract.

- **`MidiFile.save()` crash on empty tracks.** The vendored midifile library's `write()` called `vector::back()` on a track with no events (undefined behaviour -> segfault). A fresh `MidiFile` has one empty track, so even `MidiFile().save()` crashed; likewise `add_track()` followed by writing events only to the new track left track 0 empty and crashed on save. Patched `projects/midifile/src/MidiFile.cpp` to emit the end-of-track marker without dereferencing a non-existent last event. (Local patch to the vendored BSD-2 library; re-apply on any re-vendor.)

## [0.2.0]

### Changed

- **BREAKING (0.2.0): routing types renamed, top to bottom.** The parallel-branches-summed type formerly called `PluginGraph` is now **`PluginBus`**, and the general-DAG executor formerly called `GraphV2` is now **`PluginGraph`**. This gives the three routing primitives a clean tier: `PluginChain` (series), `PluginBus` (parallel, summed), `PluginGraph` (arbitrary DAG). As an alpha (0.x) project this is a clean break with no deprecation aliases; package version bumped to 0.2.0.

  The rename goes all the way down to the C ABI (**`MH_API_VERSION` bumped to 2.0.0**, a deliberate incompatible-ABI major bump per the policy in `minihost.h`):
  - bus: `mh_graph_*` -> `mh_bus_*`, `MH_PluginGraph` -> `MH_PluginBus`;

  - DAG: `mh_graph_v2_*` -> `mh_graph_*`, `MH_GraphV2` -> `MH_PluginGraph`, C++ RAII wrapper `minihost::GraphV2` -> `minihost::PluginGraph`.

  The source file names (`minihost_graph.{h,cpp}` for the bus, `minihost_graph_v2.{h,cpp,hpp}` for the DAG) are retained for git history; a header note in each maps the file to its symbol family. Both consumers (the Python wheel and the `minihost_desktop` binary) were updated and build clean. Earlier changelog entries that mention `GraphV2` / `mh_graph_v2_*` describe what is now `PluginGraph` / `mh_graph_*`.

### Added

- **MIDI fan-out on `PluginBus`** -- `PluginBus.process_midi(input, output, midi_in)` delivers the same MIDI events to every branch (each branch's first plugin), making the bus a layering primitive: one MIDI part drives N parallel instruments whose audio is summed with per-branch gain. Muted branches (gain 0.0) are skipped. New C API `mh_graph_process_midi` in `projects/libminihost/minihost_graph.{h,cpp}` (shares the fan-out-and-sum core with `mh_graph_process`; delegates per branch to `mh_chain_process_midi_io` with MIDI output discarded). Branch MIDI *output* is not collected -- that remains a `PluginGraph` (DAG) capability and a possible follow-up. New tests in `tests/test_chain_mix_and_graph.py` (audio-only path parity with empty MIDI; two-branch layering equals the sum of two independent single renders).

- **MIDI routing in `GraphV2`** -- `graph_v2` gains first-class MIDI as a sibling of audio routing. Two new node kinds (`MH_NODE_MIDI_INPUT`, `MH_NODE_MIDI_OUTPUT`) and a separate MIDI edge list (`mh_graph_v2_connect_midi`) let callers express MIDI fan-out, MIDI effect chains (e.g. arpeggiator -> synth), and per-plugin MIDI sources without the old global "fan MIDI to every plugin" hack. One MIDI edge per dst (fan-out from a source is allowed); the topo-sort indegree includes both audio and MIDI edges so dependencies are respected. Plugin nodes whose `produces_midi=1` *and* have an outgoing MIDI edge are dispatched via `mh_process_midi_io` (or `mh_process_auto` with midi_out when automation is also active); their MIDI output is captured into a per-node buffer (default capacity 1024 events) and forwarded along the edge. New C API: `mh_graph_v2_add_midi_input`, `mh_graph_v2_add_midi_output`, `mh_graph_v2_connect_midi`, `mh_graph_v2_set_midi_input_events` (stages caller events for a MIDI_INPUT node per block; borrowed pointer, cleared after `render_block`), `mh_graph_v2_get_midi_output_events` (drains a MIDI_OUTPUT node after render; truncation-aware count). The legacy `mh_graph_v2_set_node_midi` remains for direct staging on plugin nodes that aren't wired through the new routing; if a MIDI edge is connected, the edge takes precedence. C++ wrapper (`minihost::GraphV2::addMidiInput` / `addMidiOutput` / `connectMidi` / `setMidiInputEvents` / `getMidiOutputEvents`) and Python bindings (`GraphV2.add_midi_input`, `add_midi_output`, `connect_midi`, `set_midi_input_events`, `get_midi_output_events`) mirror the C API. 12 new tests in `tests/test_graph_v2_midi.py` (topology validation, passthrough, staging clear-on-render, fan-out, post-compile rejection, MIDI-edge-overwrites-on-same-dst, plugin MIDI input via graph edge matches direct `process_midi` staging).

- **MIDI input / output nodes in `minihost_desktop` project format** -- `ProjectDocument` gains `midi_inputs: [MidiInputNodeSpec]` and `midi_outputs: [MidiOutputNodeSpec]`. Each carries an optional `port_name` (MIDI port name as enumerated by `mh_midi_get_input_name` / `mh_midi_get_output_name`; empty = engine default). `EdgeSpec` gains a `kind` field (`audio` | `midi`); audio remains the default for back-compat. The loader maps MIDI nodes to `mh_graph_v2_add_midi_input/_output` and MIDI edges to `mh_graph_v2_connect_midi`. Migration: pre-MIDI-routing projects that used the per-plugin `receives_midi=true` flag get a synthesized MIDI_INPUT node at load time, wired by MIDI edge to every opted-in plugin -- the in-memory graph behaves identically to the old "fan-out via `mh_graph_v2_set_node_midi`" path. The on-disk schema is left untouched so the migration is non-destructive; new saves use explicit MIDI nodes + edges. `LoadedProject::midi_input_node_ids` exposes the graph node ids (declared + migrated) so `LiveEngine` knows where to stage live device MIDI.

- **Live MIDI input via `libminihost_audio` instead of JUCE** -- `LiveEngine` (`projects/minihost_desktop/src/live.{h,cpp}`) drops `juce::MidiInput` + `MidiInputCallback` in favor of `mh_midi_in_open` (libremidi-backed via `libminihost_audio`). Port enumeration also goes through the C API (`mh_midi_get_num_inputs` / `mh_midi_get_input_name`), used by both the engine (port-name lookup at `setMidiInputDevice`) and `main.cpp`'s MIDI input menu. The lock-free SPSC ring + audio-thread drain is unchanged; only the producer side moved. Settings persistence key renamed `identifier` -> `port_name`; legacy `identifier` values are still read and treated as port names so existing settings files don't break (they'll just miss the device if the JUCE identifier never matched a libremidi port name -- a one-time silent regression rather than a load error). At render time, drained events go to every project `MIDI_INPUT` node via `mh_graph_v2_set_midi_input_events` instead of per-plugin `mh_graph_v2_set_node_midi` calls -- the routing inside the graph then fans out as the user wired it.

- **Canvas support for MIDI nodes and edges** -- `CanvasComponent` renders the new node kinds (`midi_input` in purple, `midi_output` in magenta) and draws MIDI edges as dashed lilac strokes (distinguishable from audio edges even on grayscale displays). Right-click menu gains "Add MIDI Input" / "Add MIDI Output" entries. The edge-create drag is kind-aware: if either endpoint is a MIDI node it creates a MIDI edge with no channel-count validation; the graph compiler enforces MIDI capability at load time. The node property dialog gains a `port_name` field for MIDI nodes, and the canvas-index -> spec-index mapping was extended to handle the two new sections (added after outputs in `rebuildLayout`, preserved by `showNodePropertiesDialog`'s segmentation). `removeNodeFromDoc` sweeps `midi_inputs` / `midi_outputs` alongside the audio specs so deletes leave no orphan references.

- **`DeviceOutputNodeSpec` for live audio device routing (speakers)** -- new project node kind that represents the system audio output device as an explicit graph destination, symmetric with the MIDI nodes. Carries `channels` (default 2) and an optional informational `device_name`. At load time each `device_output` becomes an additional `MH_NODE_OUTPUT` in the graph appended after the file-sink outputs; `LoadedProject::device_output_buffer_indices` records the position in `output_buffers[]` for each so `LiveEngine` can find them. The audio callback sums per-channel across all `device_output` buffers into the device's output channels (extras silenced, overflow dropped). When a project has *no* `device_output` nodes, `LiveEngine` falls back to the legacy "first file-sink output node also plays through speakers" rule, so existing projects continue to work without edits. File rendering allocates a discardable scratch buffer for each `device_output` (the renderer still has to provide every audio output buffer the compiled graph expects) but never writes their samples to disk. Canvas: distinct burnt-orange colour, "Add Device Output (speakers)" context-menu entry, property dialog edits channels + device name, `removeNodeFromDoc` sweeps `device_outputs`, and the channel-validation helper / property-dialog segmentation were extended to cover the new section.

- **`DeviceInputNodeSpec` for live audio device capture (mic / line-in)** -- the input-side counterpart to `device_output`. Carries `channels` (default 2) and an optional informational `device_name`. At load time each `device_input` becomes an additional `MH_NODE_INPUT` in the graph appended after the file-source inputs; `LoadedProject::device_input_buffer_indices` records the slot in `input_buffers[]` for each. The audio callback copies the live device's `inputChannelData` into those buffers each block (extras silenced, surplus device channels ignored); file-source inputs continue to receive silence during live (their WAV data is only consumed by the file renderer). Multiple `device_input` nodes share the same physical device channels, so the same mic feed can drive several independent paths. `LiveEngine::start()` widens the `AudioDeviceManager` setup to enable the required number of input channels (JUCE defaults to 0 inputs); if the device-setup change fails the engine logs and continues with silence rather than refusing to start. File rendering keeps `device_input` buffers zero-filled. Canvas: distinct sky-blue colour, "Add Device Input (mic/line-in)" context-menu entry, property dialog edits channels + device name, `removeNodeFromDoc` sweeps `device_inputs`, channel-validation helper and property-dialog segmentation extended.

- **Node kind registry refactor in `minihost_desktop`** -- the 19 node kinds previously had per-kind logic scattered across ~13-17 sites in 4 files. The new `node_registry.{h,cpp}` consolidates the dispatch into a single `NodeKindEntry` table; adding a new kind is now one entry in `nodeRegistry()` (in canonical order), not a 17-file safari.

  - **`NodeKindEntry`** bundles every per-kind hook: `kind_string`, `colour`, `count` / `id_at` / `erase_by_id` over the doc, `parse` (with `project_dir` for relative-path resolution) / `serialize_all`, `canvas_info` (label + port counts) / `channels_for` (asymmetric for pick_channel / merge_channels), `dialog_title` / `dialog_emit` / `dialog_apply`, `menu_label` / `menu_add`, `is_midi_source` / `is_midi_sink`, and `load_one` (translates a spec into one or more graph nodes + records side effects on `LoadedProject`).

  - **Registry order is the canonical iteration order** for parse, serialize, rebuildLayout, `mapCanvasIndex`, and `loadProject`. Audio inputs (`input`, then `device_input`, then `metronome`) come first, then `plugin` and `mix`, then audio outputs (`output`, `device_output`, `meter`), then MIDI nodes and routing kinds. This ordering is what determines `input_buffers[]` / `output_buffers[]` slot positions in the compiled graph, so the buffer-index bookkeeping (`device_input_buffer_indices`, `metronome_buffer_indices`, etc.) lives inside each kind's `load_one` rather than in `loadProject`.

  - **Collapsed sites in `project.cpp`**: the parser's 19-arm if/else chain (~220 lines) is now `findKind(kind)->parse(n, id, out, project_dir)`. The serializer's 19 `for (... doc.<vec>)` blocks (~140 lines) is `for (entry : nodeRegistry()) entry.serialize_all(doc, push)`. The layout-known-id sweep (19 `for` lines) is one `for` over the registry calling `entry.count` + `entry.id_at`. The loader's 18 hand-coded `addInput` / `addPlugin` / `addMidiInput` / `mh_graph_v2_add_*` chains (~170 lines) is `for (entry : nodeRegistry()) for (i : entry.count) entry.load_one(doc, i, g, id_to_node, *loaded)`. Plugin pre-pass (opening MH_Plugins and reading state) stays outside the loop; `load_one` for `plugin` attaches the already-opened instance to the graph.

  - **Collapsed sites in `canvas.cpp`**: `kindColour` is `findKind(kind)->colour`. `rebuildLayout`'s 19 `addNode` calls (~95 lines) is one `for` over the registry. `removeNodeFromDoc`'s 19 `eraseById` calls (~21 lines) is one `for` calling `entry.erase_by_id`. `addEdgeToDoc`'s 70-line `channels_for` lambda + 12-line MIDI-source/sink detection (~85 lines) become `mapCanvasIndex` + `entry.channels_for` and direct reads of `entry.is_midi_source` / `is_midi_sink`. `showNodePropertiesDialog`'s 466-line per-kind segmentation + dialog UI + OK handler shrinks to ~60 lines using `mapCanvasIndex` + `entry.dialog_title` / `dialog_emit` / `dialog_apply`. The context menu's 22-item hand-coded list collapses to a single loop over the registry (plus 4 hard-coded items for file-chooser flows and the channel-split convenience helper).

  - **Net effect**: `project.cpp` and `canvas.cpp` shrank by ~700 lines combined. `node_registry.cpp` is ~1100 lines (one ~50-line block per kind) but everything per-kind is now collocated in one entry rather than scattered. Adding the next node kind: declare the spec in `project.h`, add it to `ProjectDocument`'s vectors, write one `makeXxx()` builder in `node_registry.cpp`, append it to the registry list. No edits to `parseProjectFile`, `saveProjectFile`, `loadProject`, `rebuildLayout`, `showNodePropertiesDialog`, `addEdgeToDoc`, `showContextMenu`, or `removeNodeFromDoc`. Test suite still **651 passed, 71 skipped** -- no behavioral changes.

- **MIDI processor nodes in `GraphV2` and `minihost_desktop`** -- two new libminihost node kinds underpin four new project-level MIDI processing primitives:

  - **`MH_NODE_MIDI_PROCESSOR`** (libminihost): single MIDI input / single MIDI output. An `MH_MidiProcessorParams` struct selects one of three ops via `MH_MidiOp`:

    - `MH_MIDI_OP_FILTER` -- pass events whose channel bit is set in `channel_mask`. Note On/Off (status `0x80`/`0x90`) additionally require `data1` in `[min_note, max_note]`. System messages (status `>= 0xF0`) always pass.

    - `MH_MIDI_OP_TRANSPOSE` -- add `transpose_semitones` to `data1` of Note On/Off events; results outside `[0, 127]` drop the event. Other event kinds pass unchanged.

    - `MH_MIDI_OP_VELOCITY_CURVE` -- for Note On with non-zero velocity, remap `vel := round(pow(vel/127, gamma) * 127)` clamped to `[1, 127]`. Note On with `vel=0` (MIDI's wire-format Note Off) passes unchanged so downstream consumers still see the note-off.

    - C API: `mh_graph_v2_add_midi_processor(params)`, `mh_graph_v2_set_midi_processor_params(node, params)`. C++ wrapper `addMidiProcessor` / `setMidiProcessorParams`. Python `GraphV2.add_midi_processor(params_dict)` / `set_midi_processor_params(node, params_dict)`.

  - **`MH_NODE_MIDI_MERGE`** (libminihost): N MIDI input ports (one per port), single MIDI output. Concatenates events from all ports into the output buffer, then stable-sorts by `sample_offset` (insertion sort -- typical event counts per block are small and stability matters). C API: `mh_graph_v2_add_midi_merge(num_inputs)`. C++ `addMidiMerge`, Python `GraphV2.add_midi_merge`.

  - **MIDI edges gain `dst_port`**: the existing "one MIDI edge per dst" rule generalizes to "one per `(dst, dst_port)`". Single-port MIDI consumers (plugin, MIDI_OUTPUT, MIDI_PROCESSOR) accept only `dst_port == 0`; MIDI_MERGE accepts `0..num_inputs-1`. New C API `mh_graph_v2_connect_midi_port(src, dst, dst_port)`; existing `mh_graph_v2_connect_midi(src, dst)` becomes a wrapper calling the new function with `dst_port=0`. C++ `connectMidiPort`, Python `connect_midi_port`. The compile pass validates per-port: every required MIDI input port on MIDI_OUTPUT / MIDI_PROCESSOR / MIDI_MERGE must be connected.

  - **Internal**: the `Node::midi_src` single-source field becomes `std::vector<MH_NodeId> midi_srcs` indexed by port; `num_midi_input_ports` is now per-node (0 for sources, 1 for single-port consumers, N for merge). `MidiEdge` carries `dst_port`. The render loop adds two new cases (`MH_NODE_MIDI_PROCESSOR` runs the per-op switch over upstream events; `MH_NODE_MIDI_MERGE` concatenates and sorts). Both use the existing `midi_out_buf` capture-buffer infrastructure; `needs_buf` extends to allocate one for any MIDI source with `has_outgoing_midi_edge`. The audio-edge validator now rejects all four MIDI node kinds (not just MIDI_INPUT/MIDI_OUTPUT) -- only `connect_midi[_port]` can wire them.

  - **Project format**: four new specs (`MidiFilterNodeSpec`, `MidiTransposeNodeSpec`, `MidiVelocityCurveNodeSpec`, `MidiMergeNodeSpec`) map to the two libminihost kinds. Loader translates filter/transpose/velocity_curve to a `MH_NODE_MIDI_PROCESSOR` with the appropriate op; merge maps directly. Edge spec's existing `dst_port` field is now used for MIDI edges too (serialized only when non-zero on MIDI edges to keep audio-only project files backward-compatible).

  - **Canvas**: four new colours (purple-violet family), four context-menu entries ("Add MIDI Filter" / "Add MIDI Transpose" / "Add MIDI Velocity Curve" / "Add MIDI Merge (2 in)"), property dialogs for each (min_note/max_note/channel_mask for filter, semitones for transpose, gamma for velocity_curve, num_inputs for merge). The MIDI-edge-detection in `addEdgeToDoc` treats all four new kinds as MIDI sources / sinks (processors and merges are both). Edge-drag into a `midi_merge` auto-assigns to the lowest unconnected input port (or shows a "merge full" alert when all ports are used). `removeNodeFromDoc` sweeps all four new spec vectors.

  - **Tests**: 13 new tests in `tests/test_graph_v2_midi_processors.py` covering filter note-range and channel-mask behavior, transpose with out-of-range drop, velocity curve identity / compress / zero-velocity preservation, merge concatenation + sort + port-range rejection + per-port compile validation, processor topology validation, `set_midi_processor_params` live updates, and a filter→transpose chain. Test suite: **651 passed, 71 skipped**.

- **Transport-driven `metronome` and `midi_clock` nodes** -- two new project-level node kinds that emit audio / MIDI synchronized to `LiveEngine`'s transport (`transport_bpm_`, `transport_playing_`, `transport_pos_samples_`). No libminihost changes: each rides on an existing graph node kind, and `LiveEngine` fills its buffer / event list per block.

  - **`MetronomeNodeSpec`**: audio source. Maps to an `MH_NODE_INPUT` at the graph layer; `LoadedProject::metronome_buffer_indices` records the input-buffer slot per metronome. `LoadedProject::renderMetronomes(planar_inputs, block_size, nframes, pos, sr, bpm, playing)` runs each block on the audio thread: zero-fills the buffer, identifies beat onsets that fall within the block (using `samples_per_beat = sr * 60 / bpm`), and paints a `freq_hz` sine windowed by an exponential envelope with `decay_ms` half-life. Carries click phase across blocks via `MetronomeState::phase_samples`. Defaults: 1 ch, gain 0.5, 1000 Hz, 20 ms decay. Silent during file rendering and when transport is paused.

  - **`MidiClockNodeSpec`**: MIDI source emitting 24-PPQN Clock (`0xF8`) ticks. Maps to a dedicated `MH_NODE_MIDI_INPUT` per clock (intentionally *not* added to `LoadedProject::midi_input_node_ids`, so device MIDI does not mix with clock pulses). `LoadedProject::stageMidiClocks(...)` builds the per-block event list -- 24-PPQN ticks plus `0xFA` (Start) / `0xFC` (Stop) on transport rising/falling edges via `MidiClockState::was_playing` -- and stages it via `mh_graph_v2_set_midi_input_events`. Connect to a `midi_output` to drive external gear.

  - **Canvas**: distinct teal (metronome) and rust (midi_clock) colours, "Add Metronome" / "Add MIDI Clock" context-menu entries. Metronome property dialog edits channels / gain / freq_hz / decay_ms; midi_clock dialog edits only the id (the node is parameter-free at the project layer). `channels_for` returns the metronome's `channels` on its output port; midi_clock has no audio channels and is rejected by the audio-edge validator (the canvas's MIDI-edge branch routes it through `connectMidi` instead). `addEdgeToDoc` recognizes `midi_clock` as a MIDI source. `removeNodeFromDoc` sweeps both spec vectors.

- **Routing node kinds in `GraphV2` and `minihost_desktop`** -- four new node kinds bring sample-level routing control into the graph without requiring a plugin:

  - **`MH_NODE_PICK_CHANNEL`** (libminihost): takes an N-channel input and outputs a 1-channel signal at `channel_index`. Single input port (N ch), single output port (1 ch). C API: `mh_graph_v2_add_pick_channel(in_channels, channel_index)`. C++ wrapper `addPickChannel`, Python `GraphV2.add_pick_channel`. Validates `0 <= channel_index < in_channels` at add time.

  - **`MH_NODE_MERGE_CHANNELS`** (libminihost): interleaves `out_channels` separate 1-channel inputs into one `out_channels`-channel output. Each input port consumes a single channel and writes it as output channel `dst_port`. Distinct from `mix` (which sums); merge_channels just `memcpy`s each port into its slot. C API: `mh_graph_v2_add_merge_channels(out_channels)`. C++ wrapper `addMergeChannels`, Python `GraphV2.add_merge_channels`.

  - **`GainNodeSpec` and `BusNodeSpec`** (`minihost_desktop`): single-input, single-output gain and labeled passthrough. Both map to `mh_graph_v2_add_mix(1, channels)` at the graph layer (a 1-input mix with a settable gain is a gain stage; with gain=1.0 it's a bus). The project format keeps them distinct so the canvas can present them differently and the bus has no editable gain.

  - **`PickChannelNodeSpec` and `MergeChannelsNodeSpec`** (`minihost_desktop`): direct project-format projections of the new libminihost node kinds.

  - **Canvas**: distinct colours for each, "Add Gain (stereo)" / "Add Bus (stereo)" / "Add Channel Split (stereo -> 2 mono)" / "Add Channel Merge (2 mono -> stereo)" context-menu entries. "Channel Split" is a convenience that creates two `pick_channel` nodes (channel_index 0 and 1) with auto-generated ids `L1`/`R1` -- there's no grouped doc-level "split" entity. Property dialog edits channels / gain / in_channels / channel_index / out_channels per kind. The `channels_for` channel-validation helper handles pick_channel and merge_channels' asymmetric port shapes (pick_channel: input port carries `in_channels`, output is 1; merge_channels: each input port is 1, output is `out_channels`). `removeNodeFromDoc` sweeps all four new spec vectors.

  - **Tests**: 11 new tests in `tests/test_graph_v2_channels.py` covering pick_channel index validation, channel extraction (L and R), connect-time channel-mismatch rejection, merge_channels interleaving, mono-only input port enforcement, the pick->merge identity round-trip, and the gain / bus behaviors via `mix(1, channels)`.

- **`MeterNodeSpec` for real-time per-channel peak visualization** -- new audio sink that captures `max |sample|` per channel each block and surfaces it for canvas display. Carries `channels` (default 2). Graph-wise it's a regular `MH_NODE_OUTPUT` appended after `device_outputs`; its samples are computed but never written to disk and never routed to a device. `LoadedProject::MeterState` holds one `std::atomic<float>` per channel; `LoadedProject::updateMeters(out_buffers, nframes)` is called from `LiveEngine`'s audio callback right after `renderBlock` to refresh the atomics. File renderer allocates scratch for meters but skips the peak update (no GUI is watching). `LiveEngine::loadedProject()` exposes the running project so the canvas can read meter state; both `start()` and `stop()` (including the load-project and close-project paths in `main.cpp`) wire / unwire the canvas via `setLiveProject()`. Canvas: distinct slate-grey colour, "Add Meter" context-menu entry, property dialog edits channels, `removeNodeFromDoc` sweeps `meters`. While a live project is set, the canvas runs a 30 Hz `juce::Timer` that triggers `repaint()`; the paint routine overlays per-channel vertical level bars (green / yellow / red, sqrt-scaled) on the bottom 55% of every meter node by reading the atomics with `memory_order_relaxed`. No measurable audio-thread overhead beyond the per-channel `max(|x|)` reduction.

### Changed

- **`PluginNodeSpec`** -- added cached `accepts_midi` / `produces_midi` flags (best-effort, set at canvas-add time; authoritative values come from `mh_get_info` at load time). The legacy `receives_midi` field is preserved for back-compat with on-disk projects but is marked deprecated in the header comment; new projects should express live MIDI routing via `MidiInputNodeSpec` + MIDI edges.

- **`EdgeSpec`** -- now carries an `EdgeKind kind` field (defaults to `Audio`); MIDI edges set `kind=Midi` and ignore `dst_port`. Serializer emits `"kind": "midi"` only for MIDI edges (audio edges remain wire-compatible with the v1 schema).

### Internal

- New test file `tests/test_graph_v2_midi.py` -- 12 tests (topology + passthrough + plugin parity, see above). Full suite remains green: 627 passed, 71 skipped.

- `projects/libminihost/minihost_graph_v2.{h,cpp,hpp}`: new node kinds, edge list, plugin MIDI-out capture, staging/drain functions.

- `src/minihost/_core.cpp`: GraphV2 Python wrapper gains MIDI methods + per-node MIDI input scratch buffer that outlives Python call boundaries (mirrors the existing automation scratch pattern).

- `projects/minihost_desktop`: `project.{h,cpp}`, `live.{h,cpp}`, `canvas.{h,cpp}`, `main.cpp` updated as described above; CMake already linked `minihost_audio_gui` so no build-system changes were needed.

## [0.1.7]

### Added

- **`process_audio(plugin, audio, ..., in_place=True)`** -- new kwarg that writes output into the input buffer instead of allocating a new one, saving one buffer's worth of memory for the stereo-in / stereo-out case. Returns the same buffer object as `audio` (the input is mutated). Requires `audio` to be an `AudioBuffer` (numpy / buffer-protocol producers go through a coercion path and cannot alias), matching input / output channel counts, and `tail_seconds == 0` (a tail would need extra frames the input doesn't have). The existing block loop is already structured to snapshot each input block into a scratch buffer before processing, so writing output into the input's storage at a latency-lagged position is safe. 6 tests in `tests/test_in_place_and_session.py`.

- **`minihost.Session`** -- new type that holds one shared JUCE `AudioPluginFormatManager` and reuses it across loads, probes, and directory scans. Most useful for multi-plugin and directory-scanning workflows where the per-call format registration cost otherwise stacks up. API: `Session()` + `open(path, sample_rate, max_block_size, in_channels, out_channels, sidechain_channels)` returns a `Plugin`; `probe(path)` returns the same dict as the module-level `probe()`; `scan_directory(directory_path)` returns the same list-of-dicts as the module-level `scan_directory()`. Context-manager friendly. New C API in `projects/libminihost/minihost.{h,cpp}`: `mh_session_create` / `mh_session_close` / `mh_session_open` / `mh_session_probe` / `mh_session_scan_directory`. Sessions are thread-safe via an internal mutex (JUCE's `AudioPluginFormatManager` is not). **Refactor:** removed the per-plugin `AudioPluginFormatManager fm` field from `MH_Plugin` (the manager is only used at plugin construction; the `AudioPluginInstance` is self-contained afterwards). `mh_open_ex` now constructs a stack-local manager via the existing `initFormatManager` helper; the construction core was factored into `createPluginWithFm(fm, ...)` which both `mh_open_ex` and `mh_session_open` call. `mh_scan_directory`'s body was extracted similarly into `scanDirectoryWithFm(fm, ...)`. Plugins created via `Session.open` survive the session that loaded them (closing the session does not invalidate them). 8 tests in `tests/test_in_place_and_session.py`.

- **`process_audio_stream(plugin_or_chain, audio, ...)`** -- new generator that mirrors `render_midi_stream` for the audio-in case. Yields user-visible output blocks (post-latency-compensation, post-trim) so a consumer that concatenates every yielded block reproduces `process_audio`'s return value. Same kwargs as `process_audio` (`midi=`, `sidechain=`, `param_changes=`, `bpm=`, synth-mode `audio=None`); `as_=numpy.ndarray` selector matches `render_midi_stream`. `normalize=` is intentionally absent here -- peak normalization requires the full output's peak, which a streaming consumer doesn't have. Refactor: extracted `_prepare_render` (setup + validation + transport) and `_iter_blocks` (block loop) from `process_audio` so both the in-memory and streaming entry points share one implementation. `_iter_blocks` yields independent buffer copies by default (`copy=True`) so streaming consumers don't see the reused internal buffer get overwritten on the next iteration; `process_audio` opts out (`copy=False`) since it memcpys each block straight into a pre-allocated output. 9 tests in `tests/test_process_audio_stream.py` cover concat-equals-`process_audio` across effect and synth-mode paths, block-size cap, `as_=` selector, progress callback, validation paths, and lazy-generator behavior.

- **Dry/wet mix on `PluginChain`** -- new `chain.set_mix(plugin_index, mix)` / `chain.get_mix(plugin_index)` (C API: `mh_chain_set_mix` / `mh_chain_get_mix`). `mix` is in `[0.0, 1.0]`: `1.0` (default) is full wet (current behavior), `0.0` is full dry (plugin output bypassed; its input forwards to the next stage), `0.5` is an equal blend. Applied uniformly across `process` / `process_midi` / `process_auto` (the auto chunker delegates to `process_midi_io`, so mix application happens transparently). **Restriction:** the plugin's input and output channel counts must match for mix to be enabled -- ineligible plugins stay locked at `1.0` forever and `set_mix` raises. Dry-signal snapshot storage is pre-allocated at chain construction (per eligible plugin, sized `channels * max_block_size`); the audio thread never allocates. 7 tests in `tests/test_chain_mix_and_graph.py`.

- **`PluginGraph` parallel-branches-summed** (`minihost.PluginGraph`) -- new type for parallel plugin routing beyond the serial `PluginChain`. `PluginGraph(in_ch, out_ch, max_block_size, sample_rate)` + `add_branch(chain, gain=1.0)` + `process(input, output)`. Each branch is a `PluginChain` that receives the same input; their outputs are summed with per-branch linear gain into the graph's output. Per-branch `set_branch_gain` / `get_branch_gain` lets a branch be muted (gain=0 skips processing entirely) or attenuated dynamically. Pre-allocates one scratch output buffer per branch at `add_branch` time; the audio thread is allocation-free. Branches must agree with the graph on channel counts and sample rate -- `add_branch` rejects mismatches with descriptive errors. Covers parallel compression, dry-bus + reverb-send, multi-band-style routing (when each band is wrapped in its own chain). Scope v1: fan-out + summed mix only; arbitrary DAG topology (multi-output, per-edge gain, sidechain into branches) is a possible v2. New C API in `projects/libminihost/minihost_graph.{h,cpp}`; Python wrapper in `src/minihost/_core.cpp`. 13 tests in `tests/test_chain_mix_and_graph.py`.

- **`process_audio_to_file` / `process_audio` absorb the rest of `cmd_process`** -- both gained `midi=` (file path, `MidiFile`, or pre-resolved event list), `sidechain=` (file path or in-memory buffer; `Plugin` only -- `PluginChain` has no sidechain method and is rejected up front), `param_changes=` (sample-accurate automation: `(sample, param_idx, value)` for `Plugin`, `(sample, plugin_idx, param_idx, value)` for `PluginChain`), and `bpm=` (transport tempo; `Plugin` only) kwargs. `audio=None` enables synth-mode renders driven entirely by MIDI -- length is derived from `max(midi_max_sample, src_frames) + tail_seconds`. Per-block routing chooses `process_sidechain` / `process_auto` / `process_midi` / `process` based on which inputs are supplied; latency compensation, normalize, and progress-callback contracts are unchanged. New private helpers: `process._load_midi_events` (lazy-imports the MIDI helpers from `render.py`), `process._slice_block_events`, `process._read_optional_audio`, `process._maybe_duplicate_to_match`. 18 new tests in `tests/test_process_audio_extended.py` cover the pure-Python helpers, validation paths (sidechain-on-chain, bpm-on-chain, missing input/midi/tail), synth mode, MIDI+audio, automation, sidechain, BPM, and `process_audio_to_file` synth + sidechain end-to-end.

- **`minihost process --progress`** -- per-block progress bar on stderr for single-file renders and each file in batch mode. CLI helper `_ProgressBar` in `cli.py` matches the `progress_callback=(current, total)` signature used by the library so it can be passed directly. Library: `progress_callback` kwarg on `process_audio`, `process_audio_to_file`, and `render_midi_to_file`. Disabled by default; opt-in per invocation.

- **`minihost process --normalize [dBFS]`** -- peak-normalize the output to a target dBFS (0 dBFS = full scale; default 0 when the flag is given without a value, e.g. `--normalize -1.0` for 1 dB headroom). Library: `normalize=<dbfs>` kwarg on `process_audio`, `process_audio_to_file`, and `render_midi_to_file`. Silent buffers are left untouched. Helper: `minihost.process._normalize_peak(buf, target_dbfs)` uses `AudioBuffer.magnitude()` + `apply_gain` (JUCE-backed, no numpy). LUFS normalization is a follow-up.

- **`minihost process --chain SPEC` and `minihost.load_chain(spec_path, sample_rate, block_size)`** -- declarative plugin chains from JSON (stdlib only) or YAML (PyYAML imported lazily; clear ImportError when absent). Schema: top-level `plugins: [...]` with `path`, optional `params: {name: value}` (case-insensitive name lookup via `Plugin.find_param`), `preset: N`, `vstpreset: PATH`, `state: PATH` (mutually exclusive; validated up front), and optional `in_channels` / `out_channels` per-plugin or top-level `sample_rate` / `block_size`. Returns a `_OwningPluginChain` subclass (in `src/minihost/chain.py`) that pins the constructed plugins so callers only need to close the chain. CLI: `plugin` positional becomes optional; `--chain` combined with `--state` / `--vstpreset` / `--preset` / `--param` / `--param-file` / `--out-channels` / `--bpm` / `--non-realtime` is rejected (the spec is the source of truth); sidechain input with `--chain` is rejected (chain has no sidechain method). Works in both single-file and batch modes. 10 spec-parsing tests + 1 plugin-gated construction test in `tests/test_tier1_features.py`.

### Changed

- **`cmd_process` collapsed from ~410 to ~200 lines** -- the non-batch CLI path is now a thin shim that validates args, expands globs, peeks audio metadata for plugin construction, parses `--param` / `--param-file` into a `param_changes` list, and delegates everything else to `process_audio_to_file`. Block iteration, MIDI / sidechain / automation routing, latency compensation, normalize, and write all live in the library; the CLI no longer carries a bespoke block loop. Single-test-mock breakage was minimal: `tests/test_cli.py::TestCmdProcessErrors::test_process_plugin_load_error` was updated to include `channels` / `frames` keys in its `get_audio_info` mock (the old code only read `sample_rate`).

- **`process_audio_to_file` signature** -- `input_path` is now optional (defaults to `None` for synth mode). `output_path` remains required and is validated up front. Existing positional callers continue to work unchanged.

- **`process_audio` internals refactored** -- block loop extracted into `_prepare_render` + `_iter_blocks` to share with `process_audio_stream`. `process_audio`'s observable behavior is unchanged (verified by the existing test suite); the change is purely internal.

### Internal

- New test file `tests/test_tier1_features.py` -- 12 tests covering `_normalize_peak` math (3), `_ProgressBar` enabled/disabled behavior (2), `load_chain` validation paths (7: missing file, unknown extension, non-mapping top, empty plugins list, missing plugin path, multiple state sources, YAML without PyYAML).

- New test file `tests/test_process_audio_extended.py` -- 18 tests (see above).

- New test file `tests/test_chain_mix_and_graph.py` -- 20 tests (7 for `PluginChain.set_mix` / `get_mix`, 13 for `PluginGraph`); see the dry/wet and `PluginGraph` entries above.

- New test file `tests/test_process_audio_stream.py` -- 9 tests; see the `process_audio_stream` entry above.

- New test file `tests/test_in_place_and_session.py` -- 14 tests (6 in-place: object identity, equivalence with out-of-place, mutation, type / tail / channel-mismatch rejection; 8 session: construction, open, multi-open, probe equivalence with module-level, scan equivalence, bad-path error, plugin-outlives-session, context manager).

- Build: `projects/libminihost/CMakeLists.txt` adds `minihost_graph.cpp` to the static library; top-level `CMakeLists.txt`'s install rule adds `minihost_graph.h` alongside the other public headers.

### Test summary

`make test`: 576 passed, 71 skipped, 0 failed -- up from 515 before the Tier 1 / extension / mix+graph / streaming / in-place+session work (+61 net new tests across five new files).

## [0.1.6]

### Added

- **numpy is now an optional dependency** (BREAKING CHANGE for installs that relied on numpy being pulled in transitively). See [docs/migration.md](docs/migration.md). Moved from `dependencies` to `[project.optional-dependencies]` as `numpy`. `pip install minihost` no longer installs numpy; `pip install minihost[numpy]` does. The default API surface (`AudioBuffer`, `read_audio`, `write_audio`, `resample`, `process_audio`, `process_audio_to_file`, `render_midi`, `render_midi_stream`, `render_midi_to_file`, `MidiRenderer`, all `Plugin` / `PluginChain` process methods) works without numpy installed. numpy-typed code paths (`as_=numpy.ndarray`, `AudioBuffer.as_ndarray()`, `AudioBuffer.from_numpy()`, passing numpy arrays as inputs) lazy-import numpy on first use and raise a clear `ImportError` directing the user to `pip install minihost[numpy]` when it is absent. Required refactors: `_core.audio_read` / `_core.audio_resample` now return `AudioBuffer` directly (skipping the previous numpy detour); `audio_io.py`, `render.py`, and `process.py` lazy-import numpy and use AudioBuffer-native ops where possible (`AudioBuffer.clear` / `magnitude` / `__setitem__`) instead of `np.zeros` / `np.max(np.abs(...))` / numpy slice assignment. Internal `MidiRenderer` scratch buffers are now `AudioBuffer` instead of `np.ndarray`. New `tests/test_numpy_optional.py` runs a sub-Python process with numpy hidden via a meta-finder and exercises the AudioBuffer-only path end-to-end.

- **`AudioBuffer` class** (`minihost.AudioBuffer`) -- planar float32 audio container, stdlib-only (no numpy required), backed by `juce::AudioBuffer<float>` via a thin C++ wrapper that enforces contiguous memory by construction. Exposes the DLPack and `__array__` protocols so instances can be passed directly to `Plugin.process` / `PluginChain.process` / `write_audio` / `numpy.asarray` without an explicit `.as_ndarray()` conversion. Numpy-style 2-axis indexing supported (`buf[ch, fr_slice]`), with documented limits: strided slices, fancy indexing, boolean indexing, and Ellipsis raise `TypeError` directing the user to `.as_ndarray()` for those. JUCE-backed DSP ops exposed: `clear`, `apply_gain`, `apply_gain_ramp`, `apply_gain_per_channel`, `add_from`, `add_from_with_ramp`, `get_rms_level`, `reverse`, `reverse_channel`, `magnitude`, `copy`. Zero-initialized on construction. Conversion to numpy is via `.as_ndarray()` (zero-copy view, requires numpy installed); construction from numpy is via `AudioBuffer.from_numpy(arr)`.

- **`process_audio()` and `process_audio_to_file()` (`minihost.process`)** -- high-level offline processing helpers that collapse the typical block-iteration loop. `process_audio(plugin_or_chain, audio, tail_seconds=...)` returns a new `AudioBuffer`; `process_audio_to_file(plugin_or_chain, input_path, output_path, tail_seconds=..., bit_depth=24)` reads, optionally resamples and channel-duplicates to match the chain, processes, and writes. Both functions handle latency compensation (extends render by `latency_samples` and trims the matching head from output) when `compensate_latency=True` (default).

- **`read_audio(path, as_=...)`** -- new `as_` selector chooses the returned container type. Default `as_=AudioBuffer` (BREAKING CHANGE: previously returned `numpy.ndarray`). Pass `as_=numpy.ndarray` to keep the previous behavior. `write_audio` and `resample` accept either type transparently; `resample` returns the same type as its input.

- **`render_midi`, `render_midi_stream`, `MidiRenderer.render_block`, `MidiRenderer.render_all` now return `AudioBuffer`** (BREAKING CHANGE). `render_midi`, `render_midi_stream`, and `render_all` accept the same `as_=...` selector as `read_audio` (default `AudioBuffer`; pass `as_=numpy.ndarray` for the previous behavior). `render_block` always returns `AudioBuffer` -- call `.numpy()` on the result if you need a numpy view. Internally `render_midi_to_file` now allocates an `AudioBuffer` for the staging area; the public return type (frame count `int`) is unchanged.

- **Latency compensation in `MidiRenderer`** -- previously, a plugin reporting `latency_samples > 0` produced output time-shifted by that many samples relative to the rendered MIDI tempo positions. The renderer now renders `latency_samples` extra input frames past the user-visible end and discards the first `latency_samples` of output, so the returned audio is time-aligned with MIDI events. User-visible properties (`duration_seconds`, `total_samples`, `progress`) continue to report the user-visible duration; the new read-only `MidiRenderer.latency_samples` property exposes the compensation amount. Auto-tail detection runs against post-skip output and uses the latency-corrected MIDI-end boundary. No-op for plugins reporting zero latency.

- **CMake install rules for libminihost** -- standalone CMake builds (where `SKBUILD` is undefined) now install `libminihost.a` to `${prefix}/lib/` and the public headers (`minihost.h`, `minihost_chain.h`, `minihost_vstpreset.h`) to `${prefix}/include/minihost/`. Gated by the new `MINIHOST_INSTALL` option (default ON for standalone, OFF for the Python wheel build so the wheel is unaffected). No `find_package(minihost)` config target is generated: the static library has PRIVATE link dependencies on JUCE modules that this project vendors via `add_subdirectory` of a downloaded JUCE source tree, so a clean export is not achievable. External C/C++ consumers should rebuild minihost from source as a subdirectory, or link the installed `libminihost.a` together with their own JUCE build.

- **JUCE pinned to a commit SHA in `download_juce.py`** -- previously downloaded by tag (mutable on the server side). Now resolves the default `JUCE_VERSION` to a content-addressed commit SHA (`29396c22c93392d6738e021b83196283d6e4d850` for 8.0.12) and downloads the SHA archive for reproducible builds. `JUCE_SHA` env var overrides the pinned SHA; `JUCE_ALLOW_TAG=1` falls back to tag-based download (use only for ad-hoc bumps where the SHA is not yet known).

- **ABI versioning** -- the C library now exposes a stable ABI version distinct from the project's release version, seeded at `1.0.0`. Header macros `MH_API_VERSION_MAJOR` / `MINOR` / `PATCH` / `NUMBER` / `STRING` describe the version the header was generated for; runtime `mh_api_version()` and `mh_api_version_string()` return the version the linked implementation was built against. Major bumps signal incompatible changes, minor bumps are backward-compatible additions, patch bumps are non-API fixes. Public structs evolve by appending fields; callers should `memset` to zero before passing in. Same surface re-exported in Python as `minihost.api_version()`, `minihost.api_version_string()`, and the `MH_API_VERSION_*` attributes.

- **`MidiMapper`** (`minihost.MidiMapper`) -- maps incoming MIDI events from a USB MIDI control surface (knobs, faders, pads) to plugin parameter writes or user callbacks. Designed to be passed as the callback to `MidiIn.open` / `MidiIn.open_virtual`. `map_cc(channel, cc, param, value_range=(0,1), curve="linear"|"exp"|"log")` translates CCs to `plugin.set_param`; `map_note(channel, note, callback)` invokes a user callback on note-on (zero-velocity note-ons treated as note-offs and not dispatched, per convention); unmapped events fall through to an optional `on_unmapped` callback (useful for forwarding hybrid controllers' notes to the plugin via `audio_device.send_midi`). `set_on_unmapped(callback)` lets the fallback be replaced after construction (the CLI uses this to late-bind the AudioDevice forwarder). Internal `threading.RLock` makes `map_*` / `unmap_*` / `clear()` safe to call from another thread while the MIDI callback fires on the libremidi thread. Parameter names are resolved at mapping time via `Plugin.find_param`, so typos fail fast before MIDI starts flowing. 17 tests in `tests/test_midi_mapper.py` cover dispatch, value-range translation, curves, channel filtering, fall-through, and concurrent remap-vs-dispatch.

- **`minihost play --map "channel:cc:param[:lo:hi[:curve]]"`** -- new CLI flag (repeatable) that wires a `MidiMapper` between the MIDI input and the plugin. When `--map` is set, the MIDI port is owned by a standalone `MidiIn` driving the mapper; mapped CCs become `plugin.set_param` writes; unmapped events (notes, unmapped CCs) are forwarded to the plugin via `AudioDevice.send_midi` so the user can still play notes through the same controller. `--map` requires `--midi N` or `--virtual-midi NAME` to provide an input source. Examples: `--map 0:7:Volume`, `--map 3:10:Pan:-1:1`, `--map 0:74:Cutoff:0:1:exp`. 10 tests in `tests/test_cli.py` (`TestParseMapSpec`, `TestCmdPlayMapping`) cover spec parsing and the play-command error paths (unknown param, malformed spec, --map without --midi).

- **`minihost play --map-file PATH`** -- load CC->parameter mappings from a JSON file for reuse across sessions. Format: `{"mappings": [{"channel": 0, "cc": 7, "param": "Volume"}, {"channel": 0, "cc": 10, "param": "Pan", "value_range": [-1.0, 1.0]}, {"channel": 0, "cc": 74, "param": "Cutoff", "curve": "exp"}]}`. Required fields: `channel`, `cc`, `param`. Optional: `value_range` (default `[0.0, 1.0]`), `curve` (default `"linear"`; one of `linear`/`exp`/`log`). Combinable with `--map` (file is loaded first, CLI args appended). 8 tests in `TestLoadMapFile`.

- **`minihost play --loop-midi PATH`** -- play a MIDI file in a loop while playback runs. A Python thread schedules events at wall-clock-correct times via `audio.send_midi`, using `time.monotonic()` for pacing. All Notes Off (CC 123) is sent on every channel between loop iterations to silence sustained notes from the previous pass. Combinable with `--midi` (live and file MIDI both reach the plugin); responsive to Ctrl+C / SIGTERM via a `threading.Event` checked between every event and during waits.

- **`minihost play --loop-audio PATH`** -- loop an audio file into the plugin's input ring buffer at real time. Auto-enables `audio.enable_input()`; the file is resampled to the device sample rate if needed. A Python thread paces `audio.write_input(chunk)` at 0.9x real-time block period to keep the ring buffer fed without overflow. Mutually exclusive with `--input` (both write to the same ring buffer). 1 test for the mutual-exclusion check; the looping threads themselves are not unit-tested (they need real audio hardware).

- **`Plugin.close()` and context-manager support** -- `Plugin` now supports the `with` statement and exposes an idempotent explicit `close()` method. The same surface is added to `PluginChain` (its `close()` releases only the chain's resources; member plugins remain owned by the caller). Operations on a closed Plugin raise `RuntimeError` instead of crashing.

- **`Plugin.poll_callbacks()`** -- new method to drain pending callback events from a non-audio thread. Change, parameter-value, and gesture callbacks are now queued internally and dispatched only when `poll_callbacks()` is called, returning the number of events dispatched.

### Changed

- **Threading-contract documentation expanded in `minihost.h`** -- the original two-class model ("audio thread only" vs. "thread-safe") was misleading because some "thread-safe" functions call `releaseResources` / `prepareToPlay` and are NOT safe to overlap with `mh_process`. The header now distinguishes three classes explicitly: (1) audio-thread-only process calls, (2) functions that take an internal lock and are safe to overlap with audio (param get/set, queries, transport, callbacks), and (3) functions that reconfigure the plugin and must not overlap with audio (`mh_set_state`, `mh_set_sample_rate`, `mh_set_processing_precision`, `mh_reset`, etc.). Lifecycle ordering between `MH_AudioDevice` and `mh_close` is also documented.

- **`render_midi_to_file()` no longer triple-buffers its output** -- previously did `list(render_midi_stream(...))` (block 1) + `np.concatenate(blocks, axis=1)` (block 2) + `write_audio` (block 3), with peak memory ~3x the rendered audio size. Now allocates a single contiguous output array against `MidiRenderer.total_samples`, writes each block directly into the appropriate slice, and trims to the actual sample count before writing (auto-tail detection may finish early). Peak memory drops from ~3x to ~1x of the final audio size.

- **JUCE moved from `JUCE/` to `thirdparty/JUCE/`** -- the vendored JUCE source tree now lives alongside other third-party code instead of cluttering the repo root. `JUCE_PATH` default in `CMakeLists.txt` and `JUCE_DIR` default in `scripts/download_juce.py` updated accordingly. `pyproject.toml`'s `sdist.include` updated. The `JUCE_PATH` cmake var and `JUCE_DIR` env var still let users override; CI uses the script's default and is unaffected. Existing checkouts: run `mv JUCE thirdparty/JUCE` (or just `rm -rf JUCE && python scripts/download_juce.py` to re-download into the new location).

- **`minihost process` batch worker now uses `process_audio_to_file`** -- the per-file batch path (`_process_single_file` in `cli.py`) previously open-coded the read / pad / block-loop / latency-compensation / write pipeline. It now delegates to `process_audio_to_file`. Batch-mode invariants (sample-rate / channel-count consistency across files) are preserved via a cheap `get_audio_info` pre-check before the worker fires; exceptions are translated into the existing int return contract. Net: ~70 lines of bespoke loop replaced with a one-call delegation. The non-batch `cmd_process` path is unchanged because it has MIDI / sidechain / automation / transport features that are out of scope for `process_audio_to_file`.

- **Deduplicated `find_param_by_name`** -- `minihost.automation.find_param_by_name()` was a pure-Python loop over `plugin.get_param_info(i)`, duplicating the case-insensitive search in `Plugin.find_param`. The Python helper now delegates to `Plugin.find_param` and only translates the C++ binding's `RuntimeError` into the documented `ValueError` plus a CLI-discovery hint. Behavior is unchanged.

- **Deduplicated `.vstpreset` parser** -- `minihost.vstpreset` previously reimplemented the binary parser in pure Python alongside the C implementation in `projects/libminihost/minihost_vstpreset.c`. The Python module now delegates `read_vstpreset()` and `write_vstpreset()` to new `_core.vstpreset_read` / `_core.vstpreset_write` nanobind bindings (which call `mh_vstpreset_read` / `mh_vstpreset_write` directly). The user-facing `VstPreset` dataclass and the high-level helpers (`load_vstpreset`, `save_vstpreset`, `read_class_id_from_bundle`) are unchanged. The C parser is now the single source of truth; bug fixes land once.

- **Callback dispatch moved off the audio thread** -- `set_change_callback()`, `set_param_value_callback()`, and `set_param_gesture_callback()` no longer acquire the Python GIL from the audio thread. Callback events from the plugin (via JUCE's `AudioProcessorListener`) are pushed to a lightweight mutex-protected queue and dispatched to Python only when `poll_callbacks()` is called. This eliminates a class of audio dropouts caused by GIL contention.

- **`mh_open()` now delegates to `mh_open_ex()`** with `sidechain_in_ch=0`, removing ~50 lines of duplicated plugin-loading logic. `tryConfigureBuses()` removed (subsumed by `tryConfigureBusesEx()` which already handles zero sidechain correctly).

- **`mh_process_sidechain()` no longer heap-allocates on the audio thread** -- the combined main+sidechain buffer is pre-allocated once in `mh_open_ex()` and reused across calls, matching the zero-allocation pattern used by all other process functions.

- **`mh_process_double()` no longer heap-allocates on the audio thread** -- previously allocated `AudioBuffer<double>`/`AudioBuffer<float>` and `MidiBuffer` per call, violating the header's documented RT-safety contract. Added a persistent `AudioBuffer<double> processBufD` to `MH_Plugin` (sized once in `mh_open_ex` to match the float `processBuf`); the float-fallback path reuses `processBuf`. Both branches now reuse `p->midi`. Combined-buffer pattern also resolves the same `inCh > outCh` data-loss bug as the float path.

- **`mh_chain_process_auto()` no longer heap-allocates on the audio thread** -- previously allocated four `std::vector`s per chunk (`chunk_inputs`, `chunk_outputs`, `chunk_midi`, a 256-element `chunk_midi_out`); a block with 16 param changes did 64 heap allocations. Added persistent `autoChunkIn` / `autoChunkOut` / `autoChunkMidiIn` / `autoChunkMidiOut` members to `MH_PluginChain`, pre-sized at construction; the chunk loop now uses `clear()` + `push_back` against preserved capacity.

- **Listener trampoline no longer allocates on the audio thread** -- `Plugin::set_*_callback()` trampolines previously did `lock_guard<mutex>` + `vector::push_back`, which can reallocate. The queue is now reserved to a fixed capacity (1024) at `Plugin` construction, and `poll_callbacks()` uses `clear()` instead of `swap()` so capacity is preserved across drains. The mutex is retained (briefly held for one bounded `push_back`, microseconds), giving multi-producer safety without the maintenance cost of a hand-rolled MPSC ring buffer. When the queue is full, events are dropped and counted; the new `Plugin.callback_events_dropped()` method returns and resets the count for diagnostics.

- **Unified processing buffer in `MH_Plugin`** -- removed `inBuf`, `outBuf`, `sidechainBuf`, `combinedBuf`, `autoChunkIn`, `autoChunkOut` (six members). Replaced with a single `processBuf` (and its `processBufD` double-precision mirror) used uniformly by `mh_process_midi_io`, `mh_process_auto`, `mh_process_sidechain`, and `mh_process_double`. Reduces struct size and gives every audio path the same correctness guarantees.

- **Extracted shared helpers in Python bindings** (`_core.cpp`):

  - `planar_to_interleaved()` / `interleaved_to_planar()` replace 4 inline loop nests across `audio_read`, `audio_write`, and `audio_resample`

  - `plugin_desc_to_dict()` replaces 2 identical 10-field dict constructions in `probe()` and `scan_directory()`

### Fixed

- **`mh_set_sample_rate()` now fails loudly on rate rejection** -- previously returned 1 unconditionally even if the plugin internally rejected or clamped the requested rate. Negative, zero, and NaN inputs are now rejected up front, and after `prepareToPlay` the function verifies `getSampleRate()` matches the requested rate (within 0.5 Hz). On mismatch it rolls back its own bookkeeping (so subsequent `mh_get_sample_rate()` reflects reality) and returns 0. The Python wrapper raises `RuntimeError` accordingly.

- **`mh_set_processing_precision()` now fails loudly on precision rejection** -- previously returned 1 even when the plugin silently kept its current precision (some plugins decline `doublePrecision` even when `supportsDoublePrecisionProcessing()` returns true). The function now verifies `getProcessingPrecision()` matches the requested value after `setProcessingPrecision` + `prepareToPlay`; on mismatch, restores state to the plugin's chosen precision and returns 0.

- **`PluginChain` and `AudioDevice` no longer dangle on anonymous Python inputs** -- `PluginChain([Plugin(...)])` and `AudioDevice(Plugin(...))` previously stored raw `Plugin*` pointers without holding a Python reference, so once the temporary `Plugin` / list went out of scope the wrappers could be garbage-collected, leaving the chain or device with dangling pointers. Added `nb::keep_alive<1, 2>()` to both constructors (and the `AudioDevice(PluginChain&, ...)` overload) so the inputs' Python lifetime is pinned to the new instance. Crash was nondeterministic and depended on GC timing.

- **`mh_process_midi_io()` silently dropped input channels when `inCh > outCh`** -- the buffer passed to JUCE's `processBlock` was sized only to `outCh` channels, so plugins configured with more inputs than outputs (e.g. 4-in / 2-out downmix) only ever saw the first `outCh` input channels. Replaced the dual `inBuf`/`outBuf` setup with a single persistent `processBuf` sized to `max(inCh + sidechainCh, outCh)`; main inputs are copied into channels `[0, inCh)`, remaining channels are zeroed, and outputs are copied back from channels `[0, outCh)`. The same fix is applied per chunk in `mh_process_auto()`. Symmetric (`inCh == outCh`) plugins pay one extra `memcpy` per block; the previous code already did the equivalent copy for the in-place pre-fill.

- **Channel-count validation on `Plugin.process*` and `PluginChain.process*`** -- passing a numpy array with fewer channels than the plugin requires previously dereferenced past the internal `std::vector<const float*>` (undefined behavior). All eight process methods now validate via a new `validate_process_shape()` helper and raise `RuntimeError` with a message naming the actual vs. required channel counts. Extra channels remain accepted (harmless; the C layer only references the first N).

- **`mh_probe()` MIDI flag heuristic documented** -- `MH_PluginDesc.accepts_midi` was previously set from `desc.isInstrument` with no caveat, mislabeling MIDI effects, MIDI generators, and analyzer plugins. The implementation is unchanged in this version of JUCE (no authoritative MIDI flags available without instantiation), but the field comments and the implementation comment now state explicitly that probe-time MIDI flags are a best-effort heuristic and that callers needing authoritative values must call `mh_open` + `mh_get_info`.

- **MIDI event tuple validation** -- `process_midi()` and `process_auto()` (on both `Plugin` and `PluginChain`) now validate that each MIDI event tuple has at least 4 elements before indexing, producing a clear `RuntimeError` instead of an opaque `IndexError` from the nanobind layer.

- **Null plugin guard in `PluginChain`** -- the `PluginChain` constructor now checks each `Plugin` for a valid internal pointer. Passing a moved-from or otherwise invalid `Plugin` now raises a descriptive `RuntimeError` instead of causing undefined behavior.

- **MIDI output buffer limit documented** -- `process_midi()` and `process_auto()` docstrings now state that MIDI output is capped at 256 events per call, with excess events silently dropped. The hard-coded buffer size is consolidated into a named constant (`MIDI_OUT_CAPACITY`).

### Internal

- **New regression tests** under `tests/`:

  - `test_chain_gc.py` -- `PluginChain`/`AudioDevice` lifetime pinning

  - `test_channel_validation.py` -- shape validation on all process entry points

  - `test_asymmetric_channels.py` -- combined-buffer correctness for `process` and `process_auto`

  - `test_rt_allocations.py` -- repeated-call stability for `process_double` and chain `process_auto`

  - `test_context_manager.py` -- `Plugin` / `PluginChain` close + `with` semantics

  - `test_api_version.py` -- header / runtime ABI-version agreement

  - `test_concurrency.py` -- `set_param` racing `process` from multiple threads, callback-queue ordering and overflow reporting

  - `test_setters_fail_loud.py` -- `set_sample_rate` rejects negative/zero/NaN; `set_processing_precision` rejects unsupported double precision

  - `test_render_latency_compensation.py` -- mock-plugin-based verification that `MidiRenderer` skips the first `latency_samples` of output and emits time-aligned audio

  - `test_audiobuffer.py` (20 tests) -- AudioBuffer construction, indexing semantics (positive / negative / slice / scalar), rejection of strided / fancy / Ellipsis / single-axis access, DSP ops (clear, apply_gain, magnitude, copy), numpy zero-copy interop, direct `Plugin.process` consumption via DLPack

  - `test_audiobuffer_dsp.py` (19 tests) -- extended JUCE DSP ops on AudioBuffer (`apply_gain_ramp`, `apply_gain_per_channel`, `add_from`, `add_from_with_ramp`, `get_rms_level`, `reverse`, `reverse_channel`), each verified against a numpy reference plus bounds-checking error paths

  - `test_process_audio.py` (7 tests) -- `process_audio` shape/tail/channel-validation, `process_audio_to_file` round-trip, automatic resampling, mono->stereo channel duplication

## [0.1.5]

### Added

- **Audio device selection** -- enumerate and select specific playback/capture audio devices

  - C API: `MH_AudioDeviceInfo`, `mh_audio_enumerate_playback_devices()`, `mh_audio_enumerate_capture_devices()`; new `playback_device_index` and `capture_device_index` fields on `MH_AudioConfig`

  - Python: `minihost.audio_get_playback_devices()`, `minihost.audio_get_capture_devices()`; `AudioDevice(..., playback_device_index=N, capture_device_index=N)`

  - Python CLI: new `minihost devices` subcommand lists available devices; `minihost play --playback-device` and `--capture-device` accept either an index or a case-insensitive substring of the device name

  - C/C++ CLIs: new `devices` subcommand (text + `-j`/`--json` output). The C/C++ CLIs have no real-time `play` command, so device-selection flags are not applicable there.

- **Preset management CLI** -- `presets <plugin>` subcommand extended across all three frontends

  - Default: lists all factory presets (no longer truncated at 10 like `info`); `-j/--json` for structured output

  - `--save FILE.vstpreset` saves the current plugin state as a `.vstpreset`

  - Combinable with `--program N`, `-s/--state FILE`, or `--load-vstpreset FILE` to apply an input state before saving; when `--load-vstpreset` is used, the source file's class_id is preserved in the output

  - `-y`/`--overwrite` allows overwriting an existing `--save` target

  - Byte-exact round-trip verified between Python, C, and C++ CLIs writing/reading the same `.vstpreset`

- **`minihost_vstpreset.h/.c` in libminihost** -- new C module exposing `mh_vstpreset_read()`, `mh_vstpreset_write()`, and `mh_vstpreset_free()` for programmatic .vstpreset I/O from C/C++ callers (portable little-endian packing, no external dependencies)

- **`write_vstpreset()` / `save_vstpreset()` in `minihost.vstpreset`** -- programmatic .vstpreset writer to complement the existing reader

- **C/C++ CLI feature parity with Python frontend** -- brought `minihost_c` and `minihost_cpp` up to date with features already available in libminihost and the Python CLI

  - `process` (both CLIs): audio file I/O via `minihost_audiofile.h` -- process WAV, FLAC, MP3 input and write WAV/FLAC output (raw float32 retained as fallback for non-audio extensions)

  - `process` (both CLIs): `-i`/`--input` and `-o`/`--output` named arguments for input/output files (C CLI retains legacy positional syntax)

  - `process` (both CLIs): latency compensation -- output automatically trimmed using `mh_get_latency_samples()`

  - `process` (both CLIs): `-p`/`--preset N` -- load factory preset before processing via `mh_set_program()`

  - `process` (both CLIs): `--param "Name:value"` -- set parameters by name or index (repeatable), dispatched via `mh_process_auto()` for sample-accurate application

  - `process` (both CLIs): `--sidechain FILE` -- sidechain input via `mh_open_ex()` + `mh_process_sidechain()`

  - `process` (both CLIs): `--non-realtime` -- enable higher-quality offline processing via `mh_set_non_realtime()`

  - `process` (both CLIs): `--bpm BPM` -- set transport tempo for tempo-synced plugins via `mh_set_transport()`

  - `process` (both CLIs): `--bit-depth 16|24|32` -- control output bit depth (default: 24)

  - `process` (C++ CLI only): `-m`/`--midi-input FILE` -- render MIDI files through synth/effect plugins via midifile library + `mh_process_midi()`

  - `process` (C++ CLI only): `-t`/`--tail SECONDS` -- configurable tail length for MIDI-only rendering (default: 2.0s)

  - `params` (both CLIs): `-V`/`--verbose` -- extended parameter display with ranges, defaults, and flags using `mh_param_to_text()`

  - `info` (both CLIs): `--probe` -- lightweight metadata-only mode (no full plugin load)

  - `info` (both CLIs): `-j`/`--json` -- JSON output with merged probe and runtime info

- **Parameter access by name** on the Python `Plugin` class -- `plugin.find_param("Cutoff")`, `plugin.get_param_by_name("Cutoff")`, `plugin.set_param_by_name("Cutoff", 0.5)`. Case-insensitive lookup, raises `RuntimeError` if not found. The index-based API remains for hot-path use.

- **`minihost.open_async()`** -- load a plugin in a background thread, returns a `concurrent.futures.Future` that resolves to a `Plugin`. Useful for large sample-library plugins that take seconds to load.

- **`VENDORED.md`** -- documents vendored dependency versions (miniaudio 0.11.24, tflac, libremidi 5.3.1, midifile) with upstream URLs and update instructions.

### Changed

- `minihost_cpp` now links against `minihost_audio` and `midifile` libraries

- `minihost_c` now links against `minihost_audio` library

- **`save_vstpreset` now produces valid VST3 FUIDs.** When called with `class_id=None` (the default), the FUID is auto-detected from the plugin bundle's `Contents/Resources/moduleinfo.json` instead of writing a placeholder string. This requires the plugin to be built against VST3 SDK 3.7.5+ (which all modern plugins ship). For legacy plugins, callers must pass `class_id` explicitly or use `load_vstpreset()` to inherit one from an existing preset; there is no silent fallback. The same change applies to the `presets <plugin> --save` subcommand across all three CLI frontends.

- `Plugin` Python class and `MH_Plugin` C struct now expose the constructor's plugin path via `Plugin.path` (Python) / `mh_get_path()` (C).

- **`mh_scan_directory()` reuses a single `AudioPluginFormatManager`** instead of creating one per plugin via `mh_probe()`. Reduces overhead for large plugin collections.

- **`render_midi_stream()` now delegates to `MidiRenderer`** instead of reimplementing the render loop. Eliminates ~60 lines of duplicated setup and block-processing logic between the two code paths.

### Fixed

- **Python wheel included entire JUCE SDK and midifile install artifacts** -- CMake's `install()` rules from the JUCE and midifile subdirectories were propagating into the scikit-build-core wheel, bundling ~2200 extraneous files (headers, `juceaide` binary, CMake configs, static libraries) and inflating the wheel from ~3 MB to ~60 MB. Fixed by adding `EXCLUDE_FROM_ALL` to the `add_subdirectory()` calls for JUCE and midifile so their install targets are excluded from the default install.

- **`.vstpreset` files written by `save_vstpreset()` (and the `presets --save` CLI) previously contained a bogus class ID** -- either the literal `"minihost_unknown"` or an 8-character hash from JUCE's `PluginDescription.uniqueId`, neither of which is a valid 32-character VST3 FUID. Files written this way round-tripped through minihost's own loader but were unrecognised by other VST3 hosts. Fixed by reading the real processor FUID from the plugin bundle's `moduleinfo.json` (see Changed). New `mh_vstpreset_read_class_id_from_bundle()` C function and `minihost.vstpreset.read_class_id_from_bundle()` Python helper expose the underlying lookup.

- **`mh_audio_read()` opened the audio file twice** -- once via `ma_decode_file()` to decode audio, then again via `ma_decoder_init_file()` just to read channel count and sample rate. The second open was unnecessary: `ma_decode_file()` already populates `config.channels` and `config.sampleRate` upon return. Removed the redundant decoder open.

- **CI workflow did not run the test suite** -- `build-wheels` job built Python wheels on all platforms but never ran `pytest`. Added `CIBW_TEST_REQUIRES` and `CIBW_TEST_COMMAND` so cibuildwheel runs `pytest tests/ -v` against each built wheel. Plugin-dependent integration tests skip gracefully via `MINIHOST_TEST_PLUGIN` guard.

- **`mh_set_transport()` data race** -- transport fields (`bpm`, `positionSamples`, etc.) were written from the control thread without synchronisation while `getPosition()` read them from the audio thread, risking torn reads. Replaced with a seqlock: the writer snapshots all fields into an `MH_PlayHead::State` struct and bumps an atomic sequence counter before/after the copy; the reader retries if the counter changed mid-read. Zero overhead on the audio thread (no mutex, no CAS loop -- just two relaxed loads and a compare).

- **`mh_process_auto()` buffer overread with >64 channels** -- chunk pointer arrays were hard-coded to 64 entries, but `setDataToReferTo` was passed the plugin's actual channel count, causing an overread into uninitialised stack memory for plugins with more than 64 channels. Replaced with persistent `std::vector` members on `MH_Plugin`, sized once on first call and reused to avoid per-call heap allocation.

- **FLAC encoding crashed on Windows (`STATUS_STACK_BUFFER_OVERRUN`)** -- tflac's bitwriter always writes a full 8-byte `tflac_uint` at the current buffer position, even when only a few logical bytes remain. In `write_flac()`, the 38-byte stack buffer for STREAMINFO was too small: near the end (e.g. `pos=34`), the 8-byte write overflowed by 3+ bytes, corrupting the MSVC `/GS` stack cookie. On macOS/Linux the overflow went undetected due to stack layout differences. Fixed by padding the buffer to 46 bytes (`38 + 8`).

### Tests

- **Audio processing data-pipeline tests** (`tests/test_audio_processing.py`) -- 33 new tests covering MIDI event conversion, timing accuracy, render pipeline data flow, and automation interpolation edge cases, all runnable without a real plugin. Additional 8 plugin-dependent integration tests (gated behind `MINIHOST_TEST_PLUGIN`) verify process/process_midi/process_auto output correctness, multi-block state continuity, transport stability, and render_midi output shape.

## [0.1.4]

### Added

- **Audio input for effect processing** -- lock-free ring buffer API for feeding audio through effect plugins in real time, without GIL contention on the audio thread

  - C API: `mh_audio_enable_input()`, `mh_audio_disable_input()`, `mh_audio_write_input()`, `mh_audio_input_available()`

  - C internals: `MH_AudioRingBuffer` -- SPSC lock-free ring buffer (`audio_ringbuffer.h/.cpp`)

  - Python: `AudioDevice.enable_input(capacity_frames=0)`, `disable_input()`, `write_input(data)`, `input_available` property

  - Example: `examples/audio_input.py` -- sine wave and file-through-effect demos

- **Offline bounce with automatic tail detection** -- `tail_seconds="auto"` in render functions monitors output amplitude after MIDI ends and stops when it decays below a threshold

  - `render_midi_stream()`, `render_midi()`, `render_midi_to_file()`, `MidiRenderer` all accept `tail_seconds="auto"`

  - Configurable `tail_threshold` (default: -80 dB / `1e-4` linear) and `max_tail_seconds` (default: 30s safety cap)

  - Stops after 4 consecutive blocks below threshold to avoid cutting during brief silences

  - Example: `examples/auto_tail.py` -- compares fixed vs auto tail at different thresholds

- **Duplex audio (capture) for real-time effect processing** -- system audio input routed through plugin via miniaudio duplex mode

  - C layer: `MH_AudioConfig.capture` field; when set, audio device opens in duplex mode and the audio callback de-interleaves capture input directly into the plugin's input buffers (zero additional latency)

  - Python: `AudioDevice(plugin, capture=True)` -- new `capture` parameter on both `Plugin` and `PluginChain` constructors

  - CLI: `minihost play /path/to/effect.vst3 --input` (`-i`) enables duplex mode for live effect processing (guitar through amp sim, vocal processing, etc.)

- **Batch / multi-file processing in CLI** -- glob pattern expansion and directory output for `minihost process`

  - `-i "*.wav"` expands glob patterns; `-o output/` writes each result to the output directory

  - Batch mode detected when output path is a directory (exists or ends with `/`) and input contains audio files (no MIDI)

  - Plugin loaded once, state saved and restored between files for consistent processing

  - Skips existing output files unless `-y`/`--overwrite` is set

  - Example: `minihost process reverb.vst3 -i "drums/*.wav" -o processed/`

- **Sample rate conversion / resampling** -- built-in resampling using miniaudio's `ma_resampler` (linear interpolation with 4th-order low-pass anti-aliasing filter)

  - C API: `mh_audio_resample()` in `minihost_audiofile.h` -- resamples interleaved float32 audio between any two sample rates

  - Python: `minihost.resample(data, sr_in, sr_out)` -- takes `(channels, frames)` numpy array, returns resampled array

  - CLI: `minihost process` and batch mode automatically resample mismatched input files to match the plugin's sample rate; use `--no-resample` to error instead

  - CLI: `minihost resample input.wav -o output.wav -r 48000` -- standalone resampling subcommand with `--bit-depth` and `-y`/`--overwrite` options

  - No-op fast path when source and target rates are equal (memcpy, no resampler init)

### Tests

- **Render internals unit tests** (`tests/test_render_internals.py`) -- 55 tests covering `_build_tempo_map`, `_tick_to_seconds`, `_collect_midi_events`, `_event_to_midi_tuple`, `_seconds_to_samples`, and end-to-end tempo map integration

- **CLI unit tests** (`tests/test_cli.py`) -- 84 tests covering argument parsing for all 7 subcommands, global options, error paths, `--input` capture flag for `play`, glob expansion, batch output detection, batch error paths, `--no-resample` flag, and `resample` subcommand (arg parsing + functional tests)

- **Resampling tests** (`tests/test_audio_io.py`) -- 7 tests covering upsample, downsample, identity (same rate), stereo, silence preservation, large ratio (8k to 48k), and round-trip frame count

## [0.1.3]

### Added

- `mh_chain_process_auto()` for sample-accurate parameter automation across plugin chains

  - New `MH_ChainParamChange` struct with `plugin_index` field to target specific plugins in the chain

  - Python: `PluginChain.process_auto(input, output, midi_in, param_changes)` with 4-tuple param changes `(sample_offset, plugin_index, param_index, value)`

- FLAC write support in `mh_audio_write()` via vendored [tflac](https://github.com/jprjr/tflac) encoder (BSD-0, single-header C89)

  - Supports 16-bit and 24-bit FLAC output; 32-bit raises an error (FLAC max is 24-bit)

  - Format selected by file extension: `.wav` for WAV, `.flac` for FLAC

  - Python: `write_audio("out.flac", data, sr, bit_depth=24)` works without any API changes

### Fixed

- Fixed stale version assertion in `test_minihost.py` (`"0.1.1"` -> `"0.1.2"`)

- Removed dead bit-depth auto-detection code in CLI `process` subcommand that relied on `get_audio_info()` returning a `subtype` key (removed in 0.1.2); now defaults to 24-bit when `--bit-depth` is not specified

- Fixed `render_midi_to_file()` docstring listing unsupported output formats (FLAC, AIFF, OGG); only WAV is supported

## [0.1.2]

### Added

- Audio file I/O via miniaudio (C layer): `mh_audio_read()`, `mh_audio_write()`, `mh_audio_get_file_info()`

  - Python: `minihost.read_audio()`, `minihost.write_audio()`, `minihost.get_audio_info()` now backed by miniaudio

  - Read support: WAV, FLAC, MP3, Vorbis

  - Write support: WAV only (16-bit PCM, 24-bit PCM, 32-bit float)

- `MidiIn` class for standalone MIDI input monitoring (no plugin required)

  - `MidiIn.open(port_index, callback)` -- open a hardware MIDI input port with raw bytes callback

  - `MidiIn.open_virtual(name, callback)` -- create a virtual MIDI input port with raw bytes callback

  - `close()` method and context manager (`with`) support

  - Python: `minihost.MidiIn`

- MIDI monitor mode in `minihost midi` CLI subcommand

  - `minihost midi -m N` -- monitor incoming MIDI on hardware port N

  - `minihost midi --virtual-midi NAME` -- create a virtual MIDI port and monitor it

  - Human-readable output: Note On/Off (with note names), CC, Pitch Bend, Program Change, Channel Pressure, Poly Aftertouch, SysEx

### Changed

- Merged `probe` CLI subcommand into `info` -- use `minihost info <plugin> --probe` for lightweight metadata-only mode

  - `minihost info` now shows full runtime details by default (was already doing this)

  - `minihost info --probe` replaces the old `minihost probe` (no full plugin load)

  - `minihost info --json` outputs combined probe + runtime data as JSON

- Merged `render` CLI subcommand into `process` -- use `minihost process <plugin> -m song.mid -o output.wav` instead of `minihost render`

- Added `-t, --tail` option to `process` subcommand (default: 2.0s) for configurable tail length in MIDI-only synth mode

- Renamed `midi-ports` CLI subcommand to `midi`

### Removed

- `soundfile` runtime dependency -- audio file I/O now uses miniaudio (already vendored)

  - AIFF and OGG write support removed (miniaudio encoder is WAV-only)

  - `get_audio_info()` no longer returns `format` or `subtype` keys

## [0.1.1]

### Added

- MIDI capability queries on live plugin instances via `MH_Info` fields: `accepts_midi`, `produces_midi`, `is_midi_effect`, `supports_mpe`

  - Python: `Plugin.accepts_midi`, `Plugin.produces_midi`, `Plugin.is_midi_effect`, `Plugin.supports_mpe` read-only properties

- Stable parameter IDs via `MH_ParamInfo.id` field (uses `getParameterID()` for version-safe state management)

  - Python: `get_param_info()` dict now includes `"id"` key

- Parameter categories via `MH_ParamInfo.category` field with `MH_PARAM_CATEGORY_*` constants

  - Python: `get_param_info()` dict now includes `"category"` key

- Bus layout validation via `mh_check_buses_layout()` -- query whether a bus layout is supported before attempting to apply it

  - Python: `Plugin.check_buses_layout(input_channels, output_channels) -> bool`

- Change notifications via `AudioProcessorListener` integration

  - `mh_set_change_callback()` -- processor-level changes (latency, param info, program, non-param state) with `MH_CHANGE_*` bitmask flags

  - `mh_set_param_value_callback()` -- plugin-initiated parameter value changes

  - `mh_set_param_gesture_callback()` -- parameter gesture begin/end from plugin UI

  - Python: `Plugin.set_change_callback()`, `Plugin.set_param_value_callback()`, `Plugin.set_param_gesture_callback()`

  - Python: module-level constants `MH_CHANGE_LATENCY`, `MH_CHANGE_PARAM_INFO`, `MH_CHANGE_PROGRAM`, `MH_CHANGE_NON_PARAM_STATE`

- Parameter gesture bracketing via `mh_begin_param_gesture()` / `mh_end_param_gesture()` -- signal start/end of automation drags to plugins

  - Python: `Plugin.begin_param_gesture(index)`, `Plugin.end_param_gesture(index)`

- Current program state save/restore via `mh_get_program_state_size()` / `mh_get_program_state()` / `mh_set_program_state()` -- lighter-weight per-program state

  - Python: `Plugin.get_program_state() -> bytes`, `Plugin.set_program_state(data)`

- Processing precision selection via `mh_get_processing_precision()` / `mh_set_processing_precision()` -- explicitly select float vs double processing mode

  - Python: `Plugin.processing_precision` read/write property

  - Module-level constants `MH_PRECISION_SINGLE`, `MH_PRECISION_DOUBLE`

- Track properties via `mh_set_track_properties()` -- pass track name/color metadata to plugins

  - Python: `Plugin.set_track_properties(name=None, colour=None)`

- LV2 plugin format support (`JUCE_PLUGINHOST_LV2=1`)

  - Load and process LV2 plugins on all platforms

  - `mh_scan_directory()` now finds `.lv2` bundles

  - CLI updated with LV2 examples for `probe`, `info`, and `scan` commands

- Headless build mode (`MINIHOST_HEADLESS`, default ON)

  - Builds without GUI dependencies using JUCE's `juce_audio_processors_headless` module (requires JUCE 8.0.11+)

  - Uses headless format classes (`VST3PluginFormatHeadless`, `AudioUnitPluginFormatHeadless`, `LV2PluginFormatHeadless`)

  - Disable with `cmake -DMINIHOST_HEADLESS=OFF`

### Changed

- `addFormat()` calls now use `std::make_unique` instead of raw `new`

- Removed unused `KnownPluginList` variable from `findFirstTypeForFile`

- Added cross-platform Python script (`scripts/download_juce.py`) for downloading JUCE

  - Works on Windows, macOS, and Linux without requiring bash

  - Uses only Python standard library (no external dependencies)

  - Handles Python 3.14+ tarfile deprecation warning

- Bumped default JUCE version from 8.0.6 to 8.0.12 (required for `juce_audio_processors_headless`)

- Updated CI workflow to use Python script instead of bash for JUCE download

- Updated Makefile to prefer Python script with bash fallback

## [0.1.0]

### Added

#### Plugin Chaining

- `MH_PluginChain` opaque struct for managing chains of plugins

- `mh_chain_create()` - Create a chain from an array of plugins (all must have same sample rate)

- `mh_chain_close()` - Close chain and free resources (does not close individual plugins)

- `mh_chain_process()` - Process audio through the chain

- `mh_chain_process_midi_io()` - Process audio with MIDI I/O (MIDI goes to first plugin only)

- `mh_chain_get_latency_samples()` - Get total chain latency (sum of all plugin latencies)

- `mh_chain_get_num_plugins()` - Get number of plugins in the chain

- `mh_chain_get_plugin()` - Get plugin from chain by index

- `mh_chain_get_num_input_channels()` - Get input channel count (from first plugin)

- `mh_chain_get_num_output_channels()` - Get output channel count (from last plugin)

- `mh_chain_get_sample_rate()` - Get sample rate (all plugins share same rate)

- `mh_chain_get_max_block_size()` - Get maximum block size

- `mh_chain_reset()` - Reset all plugins in the chain

- `mh_chain_set_non_realtime()` - Set non-realtime mode for all plugins

- `mh_chain_get_tail_seconds()` - Get maximum tail length (max of all plugin tails)

- `mh_audio_open_chain()` - Open audio device for real-time playback through a plugin chain

- Python `PluginChain` class with `process()`, `process_midi()`, `reset()`, `set_non_realtime()`, `get_plugin()` methods

- Python `PluginChain` properties: `num_plugins`, `latency_samples`, `num_input_channels`, `num_output_channels`, `sample_rate`, `tail_seconds`

- `AudioDevice` now accepts either `Plugin` or `PluginChain`

- `render_midi()`, `render_midi_stream()`, `render_midi_to_file()`, and `MidiRenderer` now accept either `Plugin` or `PluginChain`

#### Real-time Audio Playback (miniaudio integration)

- `MH_AudioDevice` opaque struct for audio device management

- `MH_AudioConfig` struct for device configuration (sample_rate, buffer_frames, output_channels, midi_input_port, midi_output_port)

- `MH_AudioInputCallback` typedef for effect plugin input audio

- `mh_audio_open()` - Open audio device for real-time playback through a plugin

- `mh_audio_close()` - Close audio device

- `mh_audio_start()` / `mh_audio_stop()` - Start/stop audio playback

- `mh_audio_is_playing()` - Check if audio is currently playing

- `mh_audio_set_input_callback()` - Set input callback for effect plugins

- `mh_audio_get_sample_rate()` - Get actual device sample rate

- `mh_audio_get_buffer_frames()` - Get actual buffer size

- `mh_audio_get_channels()` - Get number of output channels

- New `libminihost_audio` library using miniaudio for cross-platform audio I/O

#### Real-time MIDI I/O (libremidi integration)

- `MH_MidiPortInfo` struct for MIDI port information

- `MH_MidiPortCallback` typedef for port enumeration callbacks

- `mh_midi_enumerate_inputs()` / `mh_midi_enumerate_outputs()` - Enumerate available MIDI ports

- `mh_midi_get_num_inputs()` / `mh_midi_get_num_outputs()` - Get MIDI port count

- `mh_midi_get_input_name()` / `mh_midi_get_output_name()` - Get MIDI port name by index

- `mh_audio_connect_midi_input()` / `mh_audio_connect_midi_output()` - Connect MIDI ports to AudioDevice

- `mh_audio_disconnect_midi_input()` / `mh_audio_disconnect_midi_output()` - Disconnect MIDI

- `mh_audio_get_midi_input_port()` / `mh_audio_get_midi_output_port()` - Query connected MIDI ports

- Lock-free ring buffer for thread-safe MIDI transfer between MIDI and audio threads

#### Virtual MIDI Ports

- `mh_midi_in_open_virtual()` - Create a virtual MIDI input port (other apps can send MIDI to it)

- `mh_midi_out_open_virtual()` - Create a virtual MIDI output port (other apps can receive MIDI from it)

- `mh_audio_create_virtual_midi_input()` - Create virtual MIDI input for AudioDevice

- `mh_audio_create_virtual_midi_output()` - Create virtual MIDI output for AudioDevice

- `mh_audio_is_midi_input_virtual()` / `mh_audio_is_midi_output_virtual()` - Check if MIDI port is virtual

- `mh_audio_send_midi()` - Send MIDI events programmatically during real-time playback

- Virtual ports appear in system MIDI port lists, allowing DAWs and other apps to connect

- Platform support: macOS (CoreMIDI), Linux (ALSA); not supported on Windows

#### MIDI File Read/Write (midifile integration)

- Integrated `midifile` library for Standard MIDI File (SMF) read/write capability

- Python `MidiFile` class for creating, loading, and saving MIDI files

  - Create MIDI files programmatically with note on/off, tempo, control change, program change, pitch bend events

  - Load existing MIDI files and iterate through events

  - Save MIDI files to disk

  - Access event timing in both ticks and seconds

#### MIDI File Rendering

- `render_midi()` - Render MIDI file through plugin to numpy array

- `render_midi_stream()` - Generator yielding audio blocks for streaming/large files

- `render_midi_to_file()` - Render MIDI file directly to WAV file (16/24/32-bit)

- `MidiRenderer` class - Stateful renderer with progress tracking and fine-grained control

  - Properties: `duration_seconds`, `progress`, `is_finished`, `current_time`

  - Methods: `render_block()`, `render_all()`, `reset()`

- Automatic tempo map handling for correct timing

- Configurable tail length for reverb/delay tails

#### Core Utilities

- `mh_reset()` - Reset plugin internal state (clears delay lines, reverb tails, filter states)

- `mh_set_non_realtime()` - Enable higher-quality algorithms for offline/batch processing

- `mh_probe()` - Get plugin metadata without full instantiation

- `MH_PluginDesc` struct for plugin metadata (name, vendor, version, format, unique_id, MIDI flags, channel counts)

- `mh_set_sample_rate()` - Change sample rate without reloading plugin (preserves parameter state)

- `mh_get_sample_rate()` - Query current sample rate

- `MH_ScanCallback` typedef for plugin scanning callback

- `mh_scan_directory()` - Recursively scan directory for VST3/AudioUnit/LV2 plugins

- `MH_PluginDesc.path` field added for scan results

- `mh_process_double()` - Process audio with 64-bit double precision

- `mh_supports_double()` - Check if plugin supports native double precision

- `MH_LoadCallback` typedef for async loading callback

- `mh_open_async()` - Load plugin in background thread

#### Parameter & Preset Access

- `mh_param_to_text()` - Convert normalized parameter value to display string (e.g., "2500 Hz")

- `mh_param_from_text()` - Convert display string to normalized value

- `mh_get_num_programs()` - Get number of factory presets

- `mh_get_program_name()` - Get factory preset name by index

- `mh_get_program()` / `mh_set_program()` - Get/set current factory preset

#### Bus Layout & Sidechain

- `MH_BusInfo` struct for bus information (name, channels, is_main, is_enabled)

- `mh_get_num_buses()` - Query number of input/output buses

- `mh_get_bus_info()` - Get detailed bus information

- `mh_open_ex()` - Open plugin with sidechain channel configuration

- `mh_process_sidechain()` - Process audio with sidechain input

- `mh_get_sidechain_channels()` - Query configured sidechain channel count

### Fixed

#### Linux Compilation

- Added Linux build dependencies to README.md (JUCE requires freetype, fontconfig, webkit2gtk, gtk3, etc.)

- Fixed `addFormat()` calls to use raw pointers instead of `std::make_unique<>()` (JUCE's API expects raw pointers)

- Added `POSITION_INDEPENDENT_CODE ON` to libminihost CMakeLists.txt for linking into shared libraries (e.g., Python module)

### Command Line Interface

- `minihost` CLI tool with subcommands for common operations:

  - `probe` - Get plugin metadata without full instantiation

  - `scan` - Recursively scan directory for VST3/AudioUnit/LV2 plugins

  - `info` - Show detailed plugin info (buses, presets, latency)

  - `params` - List plugin parameters with current values

  - `midi` - List available MIDI input/output ports

  - `play` - Real-time audio playback with MIDI input

  - `process` - Offline audio processing through effects, or MIDI-to-audio rendering for synths

- Global options: `--sample-rate`, `--block-size`

- JSON output support (`--json`) for probe, scan, params, midi

- Plugin state and preset loading for process command

- Virtual MIDI port creation for play command

### Python Bindings

All C API additions are exposed in the Python `minihost` module:

- `minihost.AudioDevice` class for real-time audio playback with MIDI

  - Constructor: `AudioDevice(plugin, sample_rate=0, buffer_frames=0, output_channels=0, midi_input_port=-1, midi_output_port=-1)`

  - Methods: `start()`, `stop()`, `connect_midi_input()`, `connect_midi_output()`, `disconnect_midi_input()`, `disconnect_midi_output()`, `create_virtual_midi_input()`, `create_virtual_midi_output()`, `send_midi()`

  - Properties: `is_playing`, `sample_rate`, `buffer_frames`, `channels`, `midi_input_port`, `midi_output_port`, `is_midi_input_virtual`, `is_midi_output_virtual`

  - Context manager support (`with AudioDevice(plugin) as audio:`)

- `minihost.midi_get_input_ports()` - Get list of available MIDI input ports

- `minihost.midi_get_output_ports()` - Get list of available MIDI output ports

- `minihost.MidiFile` class for MIDI file read/write

  - Methods: `load()`, `save()`, `add_track()`, `add_tempo()`, `add_note_on()`, `add_note_off()`, `add_control_change()`, `add_program_change()`, `add_pitch_bend()`, `get_events()`, `join_tracks()`, `split_tracks()`

  - Properties: `num_tracks`, `ticks_per_quarter`, `duration_seconds`

- `minihost.probe(path)` - Module-level function for plugin metadata

- `minihost.scan_directory(path)` - Scan directory for VST3/AudioUnit/LV2 plugins, returns list of metadata dicts

- `Plugin` constructor now accepts `sidechain_channels` parameter

- New properties: `non_realtime`, `num_programs`, `program`, `sidechain_channels`, `num_input_buses`, `num_output_buses`, `sample_rate` (read/write), `supports_double`

- New methods: `reset()`, `param_to_text()`, `param_from_text()`, `get_program_name()`, `get_bus_info()`, `process_sidechain()`, `process_double()`

- Note: For async loading in Python, use Python's `threading` module with the regular `Plugin()` constructor
