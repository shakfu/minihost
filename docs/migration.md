# Migration guide

This guide covers the breaking changes introduced by the `AudioBuffer`
migration (see CHANGELOG `[Unreleased]`). Every breaking change has a
one-keyword fix to keep existing code working; the recommended patterns
shown below are improvements, not requirements.

## TL;DR

**Install:**
- Was: `pip install minihost` (numpy was a hard dependency, installed transparently)
- Now: `pip install minihost` (no numpy) **or** `pip install minihost[numpy]` (with numpy interop)

**Smallest possible code change:** add `as_=np.ndarray` to your `read_audio` / `render_midi*` calls and you're done.

```python
data, sr = minihost.read_audio(path, as_=np.ndarray)
audio = minihost.render_midi(plugin, "song.mid", as_=np.ndarray)
```

**Recommended:** drop the `as_=` and migrate to `AudioBuffer`. It's the canonical type, accepted directly by every other minihost API via DLPack, and a `.as_ndarray()` call gets you a zero-copy numpy view at the boundary.

```python
data, sr = minihost.read_audio(path)              # AudioBuffer
plugin.process(data, output_buffer)                # AudioBuffer in directly
np_data = data.as_ndarray()                        # zero-copy numpy view if you need one
```

---

## Breaking changes

### 1. `pip install minihost` no longer installs numpy

The default install has zero required Python dependencies. Pass `[numpy]` if you want numpy-typed APIs:

```bash
pip install minihost[numpy]
```

What requires the `[numpy]` extra:

- `read_audio(path, as_=np.ndarray)` (and the equivalent on `render_midi*`)
- `AudioBuffer.as_ndarray()`
- Passing a `np.ndarray` as input to `write_audio`, `resample`, `process_audio*`, `Plugin.process*`, `PluginChain.process*`

What does **not** require numpy:

- All `Plugin` / `PluginChain` plugin loading and processing (when fed `AudioBuffer` inputs)
- `read_audio(path)` (returns `AudioBuffer`)
- `write_audio(path, audio_buffer, sr)`
- `resample(audio_buffer, sr_in, sr_out)`
- `process_audio` / `process_audio_to_file`
- `render_midi*` / `MidiRenderer` (with default `as_=AudioBuffer`)
- `MidiMapper`, `AudioDevice`, `MidiIn`, `MidiFile`, all CLI commands

**Symptom if you forget the extra:** the package imports fine, but the first call into a numpy-typed code path raises:

```
ImportError: AudioBuffer.as_ndarray() requires numpy. Install minihost
with the numpy extra: 'pip install minihost[numpy]'.
```

### 2. `read_audio()` returns `AudioBuffer` by default

```python
# Old:
data, sr = minihost.read_audio("input.wav")
# data was a numpy.ndarray of shape (channels, samples), dtype float32

# Minimum fix:
data, sr = minihost.read_audio("input.wav", as_=np.ndarray)

# Recommended:
data, sr = minihost.read_audio("input.wav")
# data is an AudioBuffer. Use data.frames instead of data.shape[1].
# Pass data directly to plugin.process / write_audio / resample / etc.
```

### 3. `render_midi()` and `render_midi_stream()` return `AudioBuffer` by default

```python
# Old:
audio = minihost.render_midi(plugin, "song.mid")

# Minimum fix:
audio = minihost.render_midi(plugin, "song.mid", as_=np.ndarray)

# Recommended:
audio = minihost.render_midi(plugin, "song.mid")
# AudioBuffer; use audio.frames, audio.channels.
```

`render_midi_stream` accepts the same `as_=` selector (yields `AudioBuffer` blocks by default).

### 4. `MidiRenderer.render_block()` returns `AudioBuffer` (no `as_=`)

The block-level API does not have an `as_=` parameter. Call `.as_ndarray()` on the returned block if you need a numpy view.

```python
# Old:
while not renderer.is_finished:
    block = renderer.render_block()      # numpy.ndarray
    process_block_with_numpy(block)

# Minimum fix:
while not renderer.is_finished:
    block = renderer.render_block()      # AudioBuffer
    process_block_with_numpy(block.as_ndarray())

# Or use AudioBuffer-native ops:
while not renderer.is_finished:
    block = renderer.render_block()
    peak = block.magnitude()             # JUCE-backed, no numpy
```

`MidiRenderer.render_all()` accepts `as_=numpy.ndarray` for the whole-buffer return.

---

## "If your old code did X, do Y" table

| Was | Quick fix | Recommended |
| --- | --- | --- |
| `data, sr = read_audio(path)` | `data, sr = read_audio(path, as_=np.ndarray)` | `data, sr = read_audio(path)` then use `data.frames` / `data.channels` |
| `data.shape` (on `read_audio` result) | unchanged (works on both types) | `(data.channels, data.frames)` |
| `data.shape[1]` | unchanged | `data.frames` |
| `data.shape[0]` | unchanged | `data.channels` |
| `audio = render_midi(p, mf)` | `audio = render_midi(p, mf, as_=np.ndarray)` | `audio = render_midi(p, mf)` (returns AudioBuffer) |
| `for block in render_midi_stream(p, mf)` | `for block in render_midi_stream(p, mf, as_=np.ndarray)` | `for block in render_midi_stream(p, mf)` (yields AudioBuffer) |
| `block = renderer.render_block()` (was numpy) | `block = renderer.render_block().as_ndarray()` | `block = renderer.render_block()` (AudioBuffer) |
| `pip install minihost` (relied on numpy) | `pip install minihost[numpy]` | same — there's no "no-numpy" recommendation if you actually use numpy |
| Custom DSP on the result (FFT, plot, etc.) | call `.as_ndarray()` once at the boundary | same |

---

## AudioBuffer differences worth knowing

If you migrate to `AudioBuffer`, a few semantic differences from `np.ndarray`:

- **Slicing returns copies, not views.** `buf[:, 100:200]` allocates a new buffer. To mutate the parent through a slice, write to it via `__setitem__`: `buf[:, 100:200] = source`.
- **2-axis indexing only.** `buf[0]` raises `TypeError`; use `buf[0, :]`. The reasoning: a 2D buffer's single-axis indexing is ambiguous (channel? frame? a 1D array?), and the explicit form is unambiguous.
- **No strided slicing, fancy indexing, boolean indexing, or `Ellipsis`.** Each raises a clear `TypeError` directing you to `.as_ndarray()`.
- **`.shape` is a tuple `(channels, frames)`.** Same as numpy. `len(buf)` returns `channels` (matches numpy's `len()` on 2D arrays).
- **`np.asarray(buf)` and `buf.as_ndarray()` both work** (zero-copy via DLPack / `__array__`). Use them at the boundary where you actually need numpy semantics.

JUCE-backed DSP ops are exposed for the common cases so you rarely need numpy:

- `buf.clear()`, `buf.apply_gain(g)`, `buf.apply_gain_ramp(start, count, lo, hi)`,
  `buf.apply_gain_per_channel([g0, g1, ...])`
- `buf.magnitude()`, `buf.get_rms_level(channel)`
- `buf.add_from(...)`, `buf.add_from_with_ramp(...)`
- `buf.reverse()`, `buf.reverse_channel(ch)`
- `buf.copy()`

---

## What's new that you should consider adopting

These additions ship in the same release. They don't break existing code but they collapse common patterns:

### `process_audio_to_file` — file → chain → file in one call

If you currently open `read_audio`, build a block loop, and call `write_audio`:

```python
# Old:
data, sr = minihost.read_audio("in.wav")
output = np.zeros(...)
for start in range(0, ..., block_size):
    plugin.process(data[:, start:start+block_size], output[:, start:start+block_size])
minihost.write_audio("out.wav", output, sr)

# New:
minihost.process_audio_to_file(plugin, "in.wav", "out.wav", tail_seconds=4.0)
```

`process_audio_to_file` handles block iteration, latency compensation, sample-rate matching, channel layout, and tail rendering.

### `MidiMapper` — control-surface CCs to plugin parameters

For mapping a USB MIDI control surface onto plugin parameters at runtime:

```python
plugin = minihost.Plugin("/path/to/synth.vst3", sample_rate=48000)
mapper = minihost.MidiMapper(plugin)
mapper.map_cc(channel=0, cc=7,  param="Volume")
mapper.map_cc(channel=0, cc=74, param="Cutoff", curve="exp")

with minihost.AudioDevice(plugin) as audio:
    with minihost.MidiIn.open(0, mapper):
        input("Press Enter to stop...\n")
```

Or via the CLI:

```bash
minihost play /path/to/synth.vst3 --midi 0 \
  --map 0:7:Volume \
  --map 0:74:Cutoff:0:1:exp \
  --map-file ~/.config/minihost/launch_control.json
```

### `--loop-midi` and `--loop-audio` on `minihost play`

For live parameter tweaking against a repeating source:

```bash
minihost play /path/to/synth.vst3 --midi 0 \
  --map 0:74:Cutoff:0:1:exp \
  --loop-midi tests/_wav/test_pattern.mid

minihost play /path/to/reverb.vst3 --midi 0 \
  --map 0:7:Mix \
  --loop-audio guitar_dry.wav
```

---

## Why the breaking change?

`minihost`'s primary user is a **plugin host** user: render MIDI to WAV, process files through chains, drive real-time playback with a control surface, build automated audio pipelines. The DSP is in the plugin; the user's code is glue.

For that usage, numpy was overhead — a 25 MB transitive install, a 50-150 ms cold import, ABI churn (numpy 2.0) — paying for a feature most users didn't reach for. Numpy is genuinely useful when minihost output crosses into analysis (FFT, plotting, ML preprocessing); for everyone else it's incidental.

`AudioBuffer` matches what you actually move around — a fixed `(channels, frames)` float32 audio block — and lets the boundary with numpy be explicit and minimal.
