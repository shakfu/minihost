# Changelog

## [Unreleased]

### Added

- **numpy is now an optional dependency** (BREAKING CHANGE for installs that relied on numpy being pulled in transitively). Moved from `dependencies` to `[project.optional-dependencies]` as `numpy`. `pip install minihost` no longer installs numpy; `pip install minihost[numpy]` does. The default API surface (`AudioBuffer`, `read_audio`, `write_audio`, `resample`, `process_audio`, `process_audio_to_file`, `render_midi`, `render_midi_stream`, `render_midi_to_file`, `MidiRenderer`, all `Plugin` / `PluginChain` process methods) works without numpy installed. numpy-typed code paths (`as_=numpy.ndarray`, `AudioBuffer.as_ndarray()`, `AudioBuffer.from_numpy()`, passing numpy arrays as inputs) lazy-import numpy on first use and raise a clear `ImportError` directing the user to `pip install minihost[numpy]` when it is absent. Required refactors: `_core.audio_read` / `_core.audio_resample` now return `AudioBuffer` directly (skipping the previous numpy detour); `audio_io.py`, `render.py`, and `process.py` lazy-import numpy and use AudioBuffer-native ops where possible (`AudioBuffer.clear` / `magnitude` / `__setitem__`) instead of `np.zeros` / `np.max(np.abs(...))` / numpy slice assignment. Internal `MidiRenderer` scratch buffers are now `AudioBuffer` instead of `np.ndarray`. New `tests/test_numpy_optional.py` runs a sub-Python process with numpy hidden via a meta-finder and exercises the AudioBuffer-only path end-to-end.
- **`AudioBuffer` class** (`minihost.AudioBuffer`) -- planar float32 audio container, stdlib-only (no numpy required), backed by `juce::AudioBuffer<float>` via a thin C++ wrapper that enforces contiguous memory by construction. Exposes the DLPack and `__array__` protocols so instances can be passed directly to `Plugin.process` / `PluginChain.process` / `write_audio` / `numpy.asarray` without an explicit `.as_ndarray()` conversion. Numpy-style 2-axis indexing supported (`buf[ch, fr_slice]`), with documented limits: strided slices, fancy indexing, boolean indexing, and Ellipsis raise `TypeError` directing the user to `.as_ndarray()` for those. JUCE-backed DSP ops exposed: `clear`, `apply_gain`, `apply_gain_ramp`, `apply_gain_per_channel`, `add_from`, `add_from_with_ramp`, `get_rms_level`, `reverse`, `reverse_channel`, `magnitude`, `copy`. Zero-initialized on construction. Conversion to numpy is via `.as_ndarray()` (zero-copy view, requires numpy installed); construction from numpy is via `AudioBuffer.from_numpy(arr)`.
- **`process_audio()` and `process_audio_to_file()` (`minihost.process`)** -- high-level offline processing helpers that collapse the typical block-iteration loop. `process_audio(plugin_or_chain, audio, tail_seconds=...)` returns a new `AudioBuffer`; `process_audio_to_file(plugin_or_chain, input_path, output_path, tail_seconds=..., bit_depth=24)` reads, optionally resamples and channel-duplicates to match the chain, processes, and writes. Both functions handle latency compensation (extends render by `latency_samples` and trims the matching head from output) when `compensate_latency=True` (default).
- **`read_audio(path, as_=...)`** -- new `as_` selector chooses the returned container type. Default `as_=AudioBuffer` (BREAKING CHANGE: previously returned `numpy.ndarray`). Pass `as_=numpy.ndarray` to keep the previous behavior. `write_audio` and `resample` accept either type transparently; `resample` returns the same type as its input.
- **`render_midi`, `render_midi_stream`, `MidiRenderer.render_block`, `MidiRenderer.render_all` now return `AudioBuffer`** (BREAKING CHANGE). `render_midi`, `render_midi_stream`, and `render_all` accept the same `as_=...` selector as `read_audio` (default `AudioBuffer`; pass `as_=numpy.ndarray` for the previous behavior). `render_block` always returns `AudioBuffer` -- call `.numpy()` on the result if you need a numpy view. Internally `render_midi_to_file` now allocates an `AudioBuffer` for the staging area; the public return type (frame count `int`) is unchanged.
- **Latency compensation in `MidiRenderer`** -- previously, a plugin reporting `latency_samples > 0` produced output time-shifted by that many samples relative to the rendered MIDI tempo positions. The renderer now renders `latency_samples` extra input frames past the user-visible end and discards the first `latency_samples` of output, so the returned audio is time-aligned with MIDI events. User-visible properties (`duration_seconds`, `total_samples`, `progress`) continue to report the user-visible duration; the new read-only `MidiRenderer.latency_samples` property exposes the compensation amount. Auto-tail detection runs against post-skip output and uses the latency-corrected MIDI-end boundary. No-op for plugins reporting zero latency.
- **CMake install rules for libminihost** -- standalone CMake builds (where `SKBUILD` is undefined) now install `libminihost.a` to `${prefix}/lib/` and the public headers (`minihost.h`, `minihost_chain.h`, `minihost_vstpreset.h`) to `${prefix}/include/minihost/`. Gated by the new `MINIHOST_INSTALL` option (default ON for standalone, OFF for the Python wheel build so the wheel is unaffected). No `find_package(minihost)` config target is generated: the static library has PRIVATE link dependencies on JUCE modules that this project vendors via `add_subdirectory` of a downloaded JUCE source tree, so a clean export is not achievable. External C/C++ consumers should rebuild minihost from source as a subdirectory, or link the installed `libminihost.a` together with their own JUCE build.
- **JUCE pinned to a commit SHA in `download_juce.py`** -- previously downloaded by tag (mutable on the server side). Now resolves the default `JUCE_VERSION` to a content-addressed commit SHA (`29396c22c93392d6738e021b83196283d6e4d850` for 8.0.12) and downloads the SHA archive for reproducible builds. `JUCE_SHA` env var overrides the pinned SHA; `JUCE_ALLOW_TAG=1` falls back to tag-based download (use only for ad-hoc bumps where the SHA is not yet known).
- **ABI versioning** -- the C library now exposes a stable ABI version distinct from the project's release version, seeded at `1.0.0`. Header macros `MH_API_VERSION_MAJOR` / `MINOR` / `PATCH` / `NUMBER` / `STRING` describe the version the header was generated for; runtime `mh_api_version()` and `mh_api_version_string()` return the version the linked implementation was built against. Major bumps signal incompatible changes, minor bumps are backward-compatible additions, patch bumps are non-API fixes. Public structs evolve by appending fields; callers should `memset` to zero before passing in. Same surface re-exported in Python as `minihost.api_version()`, `minihost.api_version_string()`, and the `MH_API_VERSION_*` attributes.
- **`Plugin.close()` and context-manager support** -- `Plugin` now supports the `with` statement and exposes an idempotent explicit `close()` method. The same surface is added to `PluginChain` (its `close()` releases only the chain's resources; member plugins remain owned by the caller). Operations on a closed Plugin raise `RuntimeError` instead of crashing.
- **`Plugin.poll_callbacks()`** -- new method to drain pending callback events from a non-audio thread. Change, parameter-value, and gesture callbacks are now queued internally and dispatched only when `poll_callbacks()` is called, returning the number of events dispatched.

### Changed

- **Threading-contract documentation expanded in `minihost.h`** -- the original two-class model ("audio thread only" vs. "thread-safe") was misleading because some "thread-safe" functions call `releaseResources` / `prepareToPlay` and are NOT safe to overlap with `mh_process`. The header now distinguishes three classes explicitly: (1) audio-thread-only process calls, (2) functions that take an internal lock and are safe to overlap with audio (param get/set, queries, transport, callbacks), and (3) functions that reconfigure the plugin and must not overlap with audio (`mh_set_state`, `mh_set_sample_rate`, `mh_set_processing_precision`, `mh_reset`, etc.). Lifecycle ordering between `MH_AudioDevice` and `mh_close` is also documented.
- **`render_midi_to_file()` no longer triple-buffers its output** -- previously did `list(render_midi_stream(...))` (block 1) + `np.concatenate(blocks, axis=1)` (block 2) + `write_audio` (block 3), with peak memory ~3x the rendered audio size. Now allocates a single contiguous output array against `MidiRenderer.total_samples`, writes each block directly into the appropriate slice, and trims to the actual sample count before writing (auto-tail detection may finish early). Peak memory drops from ~3x to ~1x of the final audio size.
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
