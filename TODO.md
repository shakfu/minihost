# minihost TODO

## Planned

### Test Gaps

- [ ] **`MidiIn` tests** -- virtual MIDI port creation and real-time MIDI input have zero coverage. Use virtual ports, skip on unsupported platforms.
- [ ] **`open_async()` tests** -- success path, error path, and timeout. Currently untested.
- [ ] **Boundary/edge-case tests** -- `nframes=0`, empty MIDI lists, zero-channel plugins, `nframes=max_block_size`.
- [ ] **Concurrent-access smoke test** -- at least one test for concurrent `mh_set_param` + `mh_process` from different threads to verify the claimed thread-safety model.
- [ ] **Callback integration tests** -- test `poll_callbacks()` with real plugin-initiated callbacks (when `MINIHOST_TEST_PLUGIN` is set), not just mocks.
- [ ] **Expand double-precision coverage** -- `process_double` is tested minimally (2 tests); should cover all process variants.
- [ ] **Performance benchmarks** -- audio processing hot-path benchmarks to catch regressions. Low priority but useful as the codebase grows.
- [ ] **Fuzz testing for VST3 preset parser** -- `read_vstpreset` with malformed/truncated input.

### Developer Experience

- [ ] **Incremental build support** -- `make test` currently forces a full rebuild via `uv sync --reinstall-package`. Add a `test-only` target or use file-based dependencies for faster development iteration.
- [ ] **Cache JUCE in CI** -- JUCE is re-downloaded on every CI run (~30s). Cache via GitHub Actions cache.
- [ ] **CI integration test plugin** -- a lightweight JUCE-built pass-through plugin checked into the repo so integration tests (~30% of suite, currently skipped in CI) can run everywhere.

### Lower priority

- [ ] **Parameter preset morphing** - Higher-level Python utility for interpolating between two parameter snapshots (A/B morph) using `get_state`/`set_state` and per-parameter access
<!-- Resolved: numpy is now an optional dependency. See CHANGELOG [Unreleased] / Added. -->
- [x] ~~**Should numpy be a hard dependency?**~~ -- DONE. numpy moved to `[project.optional-dependencies].numpy`. Default install no longer pulls it in; the AudioBuffer-only path works without numpy; numpy-typed paths lazy-import and raise a clear ImportError when absent. See `tests/test_numpy_optional.py` for the regression test.

## Feature Ideas

### High impact

- [ ] **AIFF and OGG/Vorbis write support** - Extend `mh_audio_write()` beyond WAV/FLAC to cover compressed output formats for web and game audio pipelines

### Medium impact

<!-- Resolved: --map flag wired in to cmd_play. See CHANGELOG. -->
- [x] ~~**MIDI CC-to-parameter mapping in `play` command**~~ -- DONE. `minihost play --map "channel:cc:param[:lo:hi[:curve]]"` (repeatable) builds a `MidiMapper`, routes the MIDI input through it, and forwards unmapped events to the plugin via `AudioDevice.send_midi`. Library piece is `minihost.MidiMapper`; CLI parser is `_parse_map_spec` in `cli.py`. 10 tests in `tests/test_cli.py::TestParseMapSpec` and `::TestCmdPlayMapping`.
- [ ] **Dry/wet mix on PluginChain** - Per-plugin mix knob in chain (`mh_chain_set_mix(chain, plugin_index, 0.5f)`) for parallel compression, reverb blending, etc. without manual buffer management
- [ ] **Plugin graph / parallel routing** - A `PluginGraph` allowing parallel branches (dry + wet summed) beyond the serial-only `PluginChain`, for more complex mixing scenarios
- [ ] **Session/engine object** -- share the JUCE `AudioPluginFormatManager` across plugin loads, reducing overhead for multi-plugin workflows and plugin scanning. Currently each `mh_open` creates its own format manager.
- [ ] **Dynamic MIDI output buffer** -- make the 256-event MIDI output cap configurable at the Python level, or use a dynamically-sized buffer.

### AudioBuffer migration follow-ups

The initial AudioBuffer migration (see CHANGELOG `[Unreleased]`) covers
construction, indexing, basic DSP ops, DLPack/numpy interop, and integrates
with `read_audio` / `render_midi*` / `process_audio*` via the `as_=` selector.
Items below are deliberate follow-ups that build on that base.

<!-- Resolved: extended JUCE DSP ops shipped on AudioBuffer. See CHANGELOG. -->
- [x] ~~**Expose more JUCE AudioBuffer DSP ops on `minihost.AudioBuffer`**~~ -- DONE. Added `apply_gain_ramp`, `apply_gain_per_channel`, `add_from`, `add_from_with_ramp`, `get_rms_level`, `reverse`, `reverse_channel`. (`reverse_in_place` from the original list became `reverse()` with optional `start` / `count`; the no-arg form is the whole-buffer convenience.) 19 tests in `tests/test_audiobuffer_dsp.py` verify each op against a numpy reference and exercise the bounds-checking error paths.
- [ ] **Double-precision `AudioBufferD`** -- the v1 `AudioBuffer` is float32 only. `Plugin.process_double` still requires numpy float64 arrays for symmetry with the C++ double path. Add a parallel `MhAudioBufferD` wrapper around `juce::AudioBuffer<double>` and wire it into `process_double`. Same template/binding shape, just `<double>`. Decide whether to expose as `AudioBufferD` (separate class) or `AudioBuffer(channels, frames, dtype="float64")` (one class with a precision flag).
- [ ] **Zero-copy channel-range slicing** -- `buf[k:m, :]` currently always copies (the v1 `__getitem__` rule was "slices return copies, not views" to keep semantics simple). Channel-range slices ARE contiguous in the planar layout, so a true zero-copy view is feasible without strided-view complexity. Add `AudioBuffer.channel_view(start, count)` returning a new `AudioBuffer` that aliases (rather than copies) a contiguous channel range of the parent. Frame slicing stays copy-only because that path requires strided views. Document the lifetime relationship and aliasing rules.
- [ ] **`process_audio_stream(plugin_or_chain, audio, ...)` generator** -- mirror `render_midi_stream` for the audio-in case. Lets users write block-by-block to disk for very long renders without holding the full output in memory. Same `tail_seconds` / latency-compensation contract as `process_audio`.
- [ ] **`process_audio` in-place mode** -- when `input.channels == output.channels`, allow `process_audio(plugin, audio, in_place=True)` to skip allocating a separate output buffer and write into the input. Saves one buffer's worth of memory for the common stereo-in / stereo-out case.
- [ ] **MidiRenderer's internal buffers as AudioBuffer** -- `MidiRenderer._input_buffer` and `_output_buffer` are still numpy ndarrays (slicing is easier in numpy). Migrate to `AudioBuffer` for internal consistency. Lowest priority; current code works and the per-block conversion is already cheap.
<!-- Resolved (partial): batch worker now delegates. See CHANGELOG. -->
- [x] ~~**Convert `minihost process` CLI to use `process_audio_to_file`**~~ -- DONE for the batch path (`_process_single_file` in `cli.py`); ~70 lines of bespoke loop replaced with one delegation. The non-batch `cmd_process` path is **intentionally not converted** because it carries MIDI / sidechain / automation / transport features that `process_audio_to_file` doesn't expose. To finish the conversion later: extend `process_audio_to_file` (or add a richer helper) to accept MIDI events, automation, sidechain input, and BPM transport -- those features then collapse the remaining ~80 lines of `cmd_process`.
- [ ] **Update README to lead with `AudioBuffer` + `process_audio_to_file`** -- the README's current Quick Start examples predate both APIs. New canonical example: load a chain, call `process_audio_to_file` (5 lines total). Move the manual block-loop example to a "Lower-level API" subsection for users who need it.
- [ ] **Migration / breaking-changes guide** -- the AudioBuffer migration introduced two breaking changes (`read_audio` and `render_midi*` default return type). Write a short `docs/migration.md` (or CHANGELOG section under the next versioned release) showing the one-keyword fix (`as_=np.ndarray`) and the recommended new patterns (use `AudioBuffer` + `np.asarray()` at the boundary if you still need numpy ops). Include a minimal "if your old code did X, change to Y" table.
- [ ] **DLPack interop verification with PyTorch / JAX** -- `MhAudioBuffer.__dlpack__` should make `AudioBuffer` consumable by any framework that accepts DLPack (PyTorch via `torch.from_dlpack(buf)`, JAX via `jax.dlpack.from_dlpack`). Smoke-test this and add a one-paragraph "interop with other array libraries" section to the docs. Frees users to flow audio through ML pipelines without going through numpy.
- [ ] **Output normalization** - `--normalize` flag on `process` / `render_midi_to_file` for peak or LUFS normalization
- [ ] **Progress reporting in CLI** - Surface `MidiRenderer.progress` in the `process` command (progress bar or `--progress` flag) for long renders
- [ ] **Declarative chain definitions** - JSON/YAML chain config files for reproducible rendering pipelines (plugin paths, params, presets per slot)
- [ ] **WAV metadata / BWF support** - Write timestamps, descriptions, and originator metadata into WAV `bext`/`smpl` chunks for film/broadcast workflows
- [ ] **`_tick_to_seconds` optimization** -- use binary search or a running accumulator instead of linear scan for large MIDI files with many tempo changes (currently `O(n*m)`)

### Style / minor code quality

- [ ] `minihost.cpp`: `jmax(1, inst->getTotalNumInputChannels())` forces minimum 1 input channel. Synthesizers with 0 inputs get unnecessary buffer allocation.
- [ ] `vstpreset.py:230`: `assert len(header) == _HEADER_SIZE` is stripped by `python -O`. Should be a proper check.
- [ ] `render.py:255`: `max(plugin.num_output_channels, 2)` hardcodes stereo minimum output. Should use actual channel count for mono plugins.

## Non-goals

Intentionally omitted for headless/server use:

- Editor window management
- GUI hosting
- Preset browser UI
- MIDI learn
- Plugin shell/multi-instrument handling
