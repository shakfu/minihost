# minihost TODO

## Planned

### Lower priority

- [ ] **Parameter preset morphing** - Higher-level Python utility for interpolating between two parameter snapshots (A/B morph) using `get_state`/`set_state` and per-parameter access
- [ ] **Async plugin loading in Python** - `mh_open_async()` exists in C but isn't bound in Python. An async/awaitable `Plugin.open_async()` would help with large sample-library plugins that take seconds to load

## Feature Ideas

### High impact

- [ ] **AIFF and OGG/Vorbis write support** - Extend `mh_audio_write()` beyond WAV/FLAC to cover compressed output formats for web and game audio pipelines

### Medium impact

- [ ] **MIDI CC-to-parameter mapping in `play` command** - Map incoming MIDI CC to plugin parameters (e.g. `--map CC1:Cutoff`) to make real-time mode usable for live performance
- [ ] **Dry/wet mix on PluginChain** - Per-plugin mix knob in chain (`mh_chain_set_mix(chain, plugin_index, 0.5f)`) for parallel compression, reverb blending, etc. without manual buffer management
- [ ] **Plugin graph / parallel routing** - A `PluginGraph` allowing parallel branches (dry + wet summed) beyond the serial-only `PluginChain`, for more complex mixing scenarios

### Lower impact (low effort)

- [ ] **Output normalization** - `--normalize` flag on `process` / `render_midi_to_file` for peak or LUFS normalization
- [ ] **Progress reporting in CLI** - Surface `MidiRenderer.progress` in the `process` command (progress bar or `--progress` flag) for long renders
- [ ] **Declarative chain definitions** - JSON/YAML chain config files for reproducible rendering pipelines (plugin paths, params, presets per slot)
- [ ] **WAV metadata / BWF support** - Write timestamps, descriptions, and originator metadata into WAV `bext`/`smpl` chunks for film/broadcast workflows

## Known Limitations

- **`.vstpreset` writes use a truncated class_id** - `save_vstpreset()` falls back to `probe()["unique_id"]` as the `class_id` when none is supplied, which is the 8-char JUCE uniqueId (padded), not the 32-char VST3 processor FUID. Files round-trip correctly through `load_vstpreset` (minihost ignores the `class_id` on load), but they will likely fail strict cross-DAW validation. Preserving the `class_id` from an input `.vstpreset` (`--load-vstpreset`) avoids the problem for that workflow. Exposing the true VST3 FUID would require additional work in libminihost (fetching `PluginDescription::fileOrIdentifier` and passing it through `MH_PluginDesc`) - worth a follow-up if cross-DAW preset export becomes a real need.

## Non-goals

Intentionally omitted for headless/server use:

- Editor window management
- GUI hosting
- Preset browser UI
- MIDI learn
- Plugin shell/multi-instrument handling
