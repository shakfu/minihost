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

## Feature Ideas

### High impact

- [ ] **AIFF and OGG/Vorbis write support** - Extend `mh_audio_write()` beyond WAV/FLAC to cover compressed output formats for web and game audio pipelines

### Medium impact

- [ ] **MIDI CC-to-parameter mapping in `play` command** - Map incoming MIDI CC to plugin parameters (e.g. `--map CC1:Cutoff`) to make real-time mode usable for live performance
- [ ] **Dry/wet mix on PluginChain** - Per-plugin mix knob in chain (`mh_chain_set_mix(chain, plugin_index, 0.5f)`) for parallel compression, reverb blending, etc. without manual buffer management
- [ ] **Plugin graph / parallel routing** - A `PluginGraph` allowing parallel branches (dry + wet summed) beyond the serial-only `PluginChain`, for more complex mixing scenarios
- [ ] **Session/engine object** -- share the JUCE `AudioPluginFormatManager` across plugin loads, reducing overhead for multi-plugin workflows and plugin scanning. Currently each `mh_open` creates its own format manager.
- [ ] **Dynamic MIDI output buffer** -- make the 256-event MIDI output cap configurable at the Python level, or use a dynamically-sized buffer.

### Lower impact (low effort)

- [ ] **Output normalization** - `--normalize` flag on `process` / `render_midi_to_file` for peak or LUFS normalization
- [ ] **Progress reporting in CLI** - Surface `MidiRenderer.progress` in the `process` command (progress bar or `--progress` flag) for long renders
- [ ] **Declarative chain definitions** - JSON/YAML chain config files for reproducible rendering pipelines (plugin paths, params, presets per slot)
- [ ] **WAV metadata / BWF support** - Write timestamps, descriptions, and originator metadata into WAV `bext`/`smpl` chunks for film/broadcast workflows
- [ ] **`_tick_to_seconds` optimization** -- use binary search or a running accumulator instead of linear scan for large MIDI files with many tempo changes (currently O(n*m))

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
