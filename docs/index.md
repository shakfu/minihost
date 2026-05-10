# minihost

Minimal headless audio plugin host for VST3, AudioUnit, and LV2 plugins.

minihost provides a C API built on JUCE with Python bindings via nanobind. It builds in headless mode by default (no GUI dependencies), making it suitable for servers, batch processing, and embedded applications.

## Key Features

- **Plugin formats**: VST3 (all platforms), AudioUnit (macOS), LV2 (all platforms)
- **Headless mode**: no GUI dependencies, uses JUCE's `juce_audio_processors_headless`
- **`AudioBuffer`**: planar float32 audio container, JUCE-backed, stdlib-only. DLPack-exported so it's accepted by any 2D float32 buffer-protocol consumer (`Plugin.process`, `numpy.asarray`, PyTorch, etc.) without explicit conversion.
- **numpy is optional**: `pip install minihost` installs no Python runtime dependencies. `pip install minihost[numpy]` enables numpy-typed return values and numpy-array inputs.
- **High-level offline processing**: `process_audio_to_file(chain, "in.wav", "out.wav")` collapses block iteration, latency compensation, sample-rate matching, and tail rendering into one call.
- **Plugin chaining**: connect multiple plugins in series
- **Real-time audio**: playback and capture via miniaudio, with duplex mode for effect processing
- **Audio device selection**: enumerate and target specific playback/capture devices
- **Real-time MIDI**: input/output via libremidi, virtual ports on macOS/Linux
- **Audio file I/O**: read WAV/FLAC/MP3/Vorbis, write WAV and FLAC
- **Sample rate conversion**: built-in resampling via miniaudio
- **Batch processing**: glob patterns and directory output for processing multiple files
- **Auto-tail detection**: automatic reverb/delay tail detection in offline rendering
- **Sample-accurate automation**: parameter changes at sample resolution
- **Parameter access by name**: case-insensitive `find_param()`, `get_param_by_name()`, `set_param_by_name()` on `Plugin`
- **Async plugin loading**: `open_async()` returns a `Future` for background loading
- **VST3 preset I/O**: read and write `.vstpreset` files from C, C++, and Python
- **CLI tool**: 9 subcommands for plugin inspection, device listing, preset export, playback, processing, and resampling

## Quick Start

```bash
# Install
git clone https://github.com/shakfu/minihost.git
cd minihost
uv sync
make build
```

Process a WAV file through a chain of effect plugins:

```python
import minihost

with (
    minihost.Plugin("/path/to/delay.vst3", sample_rate=48000) as delay,
    minihost.Plugin("/path/to/reverb.vst3", sample_rate=48000) as reverb,
    minihost.PluginChain([delay, reverb]) as chain,
):
    minihost.process_audio_to_file(
        chain, "in.wav", "out.wav",
        tail_seconds=4.0,           # capture reverb tail
    )
```

Lower-level processing with `AudioBuffer`:

```python
import minihost

plugin = minihost.Plugin("/path/to/plugin.vst3", sample_rate=48000)

input_audio = minihost.AudioBuffer(2, 512)
output_audio = minihost.AudioBuffer(2, 512)
plugin.process(input_audio, output_audio)

# Real-time playback with MIDI
with minihost.AudioDevice(plugin) as audio:
    audio.send_midi(0x90, 60, 100)  # Note on
```

```bash
# CLI usage
minihost info /path/to/plugin.vst3
minihost play /path/to/synth.vst3 --midi 0
minihost process /path/to/effect.vst3 -i input.wav -o output.wav
minihost resample input.wav -o output.wav -r 48000
```

## Requirements

- CMake 3.20+, C++17 compiler
- JUCE 8.0.11+ (auto-downloaded)
- Vendored C libraries: miniaudio, tflac, libremidi, midifile ([details](vendored.md))
- Python bindings: nanobind, scikit-build-core, uv
- Runtime: no required Python dependencies. Install `minihost[numpy]` for numpy interop.

See [Getting Started](getting_started.md) for full build instructions.

If you're upgrading from a previous version, see the
[Migration Guide](migration.md) for the breaking changes (default
return type of `read_audio` / `render_midi*`, numpy moved to an optional
extra) and the one-keyword fixes to keep existing code working.
