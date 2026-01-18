# minihost

Python bindings for audio plugin hosting. Supports VST3 and AudioUnit plugins.

## Installation

```bash
cd python/minihost
uv sync
```

## Usage

```python
import numpy as np
import minihost

# Load a plugin
plugin = minihost.Plugin("/path/to/plugin.vst3", sample_rate=48000)

# Check plugin info
print(f"Parameters: {plugin.num_params}")
print(f"Latency: {plugin.latency_samples} samples")

# Process audio
input_audio = np.zeros((2, 512), dtype=np.float32)
output_audio = np.zeros((2, 512), dtype=np.float32)
plugin.process(input_audio, output_audio)

# Process with MIDI
midi_events = [(0, 0x90, 60, 100), (256, 0x80, 60, 0)]  # Note on/off
midi_out = plugin.process_midi(input_audio, output_audio, midi_events)

# Set transport for tempo-synced plugins
plugin.set_transport(bpm=120.0, is_playing=True)

# Save/restore state
state = plugin.get_state()
plugin.set_state(state)
```

## Testing

Run tests with:

```bash
make test
# or
uv run pytest tests/ -v
```

### Unit tests (no plugin required)

The following tests run without a real plugin:

| Test | What it verifies |
|------|------------------|
| `test_plugin_class_has_expected_properties` | Properties like `num_params`, `bypass` exist |
| `test_plugin_constructor_docstring` | Constructor has docs with expected args |
| `test_plugin_nonexistent_directory_raises` | Error handling for bad paths |
| `test_plugin_wrong_extension_raises` | Error handling for non-plugin files |
| `test_module_docstring` | Module-level documentation exists |

### Integration tests (plugin required)

Most meaningful testing requires a real plugin. The `Plugin` class is a thin wrapper around native code that loads and interacts with actual VST3/AU binaries. Without a plugin:

- Can't test `process()`, `process_midi()`, `process_auto()`
- Can't test parameter get/set
- Can't test state save/restore
- Can't verify audio actually flows correctly

To run integration tests, set the `MINIHOST_TEST_PLUGIN` environment variable:

```bash
MINIHOST_TEST_PLUGIN=/path/to/some.vst3 uv run pytest tests/ -v
```

Options for integration testing:

1. Set `MINIHOST_TEST_PLUGIN=/path/to/some.vst3` to run the existing integration tests
2. Use a free/open-source plugin for CI (e.g., Surge, Vital, or Dexed)
3. Build a minimal test plugin as part of the project
