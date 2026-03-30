# minihost TODO

## Planned

### High priority

- [ ] **OGG write support** - `mh_audio_write()` does not support OGG/Vorbis output; requires a vendored encoder
- [ ] **Expose audio input callback in Python** - Bind `mh_audio_set_input_callback` in `_core.cpp` to enable real-time effect processing from Python (e.g., guitar amp sims, live vocal processing)

### Medium priority

- [ ] **CLI unit tests** - `cli.py` has six subcommands (`scan`, `info`, `params`, `midi`, `play`, `process`) with zero test coverage. At minimum test argument parsing and error paths
- [ ] **Render internals unit tests** - `_build_tempo_map`, `_tick_to_seconds`, `_collect_midi_events` in `render.py` are non-trivial pure-Python functions with no unit tests; edge cases (tempo changes mid-song, empty tracks, zero-duration notes) can silently produce wrong output
- [ ] **Offline bounce with tail detection** - `mh_get_tail_time()` exists but the render pipeline doesn't use it. Add auto-tail mode that keeps rendering until the plugin's tail decays below a threshold (useful for reverb/delay tails)

### Lower priority

- [ ] **Parameter preset morphing** - Higher-level Python utility for interpolating between two parameter snapshots (A/B morph) using `get_state`/`set_state` and per-parameter access
- [ ] **Async plugin loading in Python** - `mh_open_async()` exists in C but isn't bound in Python. An async/awaitable `Plugin.open_async()` would help with large sample-library plugins that take seconds to load

## Feature Ideas

### High impact

- [ ] **Sample rate conversion / resampling** - Built-in resampler (e.g. libsamplerate or sinc interpolator) so users can seamlessly load files at a different sample rate than the plugin session (e.g. 44.1k file into 48k plugin)
- [ ] **Batch / multi-file processing in CLI** - Glob support for `minihost process` (e.g. `minihost process effect.vst3 -i "*.wav" -o output/`) for batch sound design, mastering, or ML dataset preparation
- [ ] **Preset management CLI commands** - Commands for listing, loading, and saving presets (`minihost presets plugin.vst3`, `--save out.vstpreset`, `--load my.vstpreset`); infrastructure already exists via `VstPreset` and `mh_get/set_program`
- [ ] **AIFF and OGG/Vorbis write support** - Extend `mh_audio_write()` beyond WAV/FLAC to cover compressed output formats for web and game audio pipelines

### Medium impact

- [ ] **MIDI CC-to-parameter mapping in `play` command** - Map incoming MIDI CC to plugin parameters (e.g. `--map CC1:Cutoff`) to make real-time mode usable for live performance
- [ ] **Dry/wet mix on PluginChain** - Per-plugin mix knob in chain (`mh_chain_set_mix(chain, plugin_index, 0.5f)`) for parallel compression, reverb blending, etc. without manual buffer management
- [ ] **Audio input for effect processing in `play` mode** - Support system audio input (`--input` / `--input-device`) so users can run effects on live audio (guitar through amp sim, vocal processing)
- [ ] **Plugin graph / parallel routing** - A `PluginGraph` allowing parallel branches (dry + wet summed) beyond the serial-only `PluginChain`, for more complex mixing scenarios

### Lower impact (low effort)

- [ ] **Output normalization** - `--normalize` flag on `process` / `render_midi_to_file` for peak or LUFS normalization
- [ ] **Progress reporting in CLI** - Surface `MidiRenderer.progress` in the `process` command (progress bar or `--progress` flag) for long renders
- [ ] **Declarative chain definitions** - JSON/YAML chain config files for reproducible rendering pipelines (plugin paths, params, presets per slot)
- [ ] **WAV metadata / BWF support** - Write timestamps, descriptions, and originator metadata into WAV `bext`/`smpl` chunks for film/broadcast workflows

## Non-goals

Intentionally omitted for headless/server use:

- Editor window management
- GUI hosting
- Preset browser UI
- MIDI learn
- Plugin shell/multi-instrument handling
