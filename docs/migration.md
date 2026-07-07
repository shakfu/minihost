# Migration guide

This guide covers the breaking changes shipping in **0.2.0** (see CHANGELOG `[Unreleased]`): the `AudioBuffer` / numpy-optional migration, and the routing type rename (`PluginGraph` -> `PluginBus`, `GraphV2` -> `PluginGraph`).

The `AudioBuffer` changes each have a one-keyword fix to keep existing code working; the recommended patterns shown below are improvements, not requirements. The routing rename (section 5) is the exception -- it is a hard rename with no compatibility shim, because minihost is still alpha (0.x).

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

**Routing rename (no shim):** `PluginGraph` (the parallel bus) is now `PluginBus`; `GraphV2` (the DAG) is now `PluginGraph`. Mind the swap -- `PluginGraph` now means the DAG. Full details and the C/C++ ABI rename are in breaking change 5 below.

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

```text
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

### 5. Routing types renamed (`PluginGraph` -> `PluginBus`, `GraphV2` -> `PluginGraph`)

0.2.0 gives the routing types a clean three-tier model. Unlike the `AudioBuffer` changes above, **this is a hard rename with no compatibility shim** -- the old names are gone in both Python and the C/C++ ABI.

| Concept | Old name | New name |
| --- | --- | --- |
| Series (A -> B -> C) | `PluginChain` | `PluginChain` (unchanged) |
| Parallel, summed (a mix bus) | `PluginGraph` | **`PluginBus`** |
| Arbitrary node-to-node DAG | `GraphV2` | **`PluginGraph`** |

Watch the swap: **`PluginGraph` now means the DAG executor**, not the parallel bus. Code that used the old `PluginGraph` (the bus) must move to `PluginBus`; code that used `GraphV2` must move to `PluginGraph`.

**Python:**

```python
# Old: parallel-branches-summed
bus = minihost.PluginGraph(2, 2, max_block_size=512, sample_rate=48000.0)
# New:
bus = minihost.PluginBus(2, 2, max_block_size=512, sample_rate=48000.0)

# Old: general DAG executor
g = minihost.GraphV2(512, 48000.0)
# New:
g = minihost.PluginGraph(512, 48000.0)
```

Project files (`load_project` / `render_project`) are **unaffected**: the JSON schema is identical; only the Python class backing it was renamed.

New in the same release: `PluginBus.process_midi(input, output, midi_in)` fans the same MIDI to every branch, so one part can layer across parallel instruments. See the README "Parallel routing (PluginBus)" section.

**C / C++ ABI** (`MH_API_VERSION` bumped to **2.0.0**):

| | Old | New |
| --- | --- | --- |
| bus functions | `mh_graph_*` | `mh_bus_*` |
| bus type | `MH_PluginGraph` | `MH_PluginBus` |
| DAG functions | `mh_graph_v2_*` | `mh_graph_*` |
| DAG type | `MH_GraphV2` | `MH_PluginGraph` |
| DAG C++ RAII wrapper | `minihost::GraphV2` | `minihost::PluginGraph` |

Source file names are retained for git history (`minihost_graph.{h,cpp}` is the bus; `minihost_graph_v2.{h,cpp,hpp}` is the DAG); a header note in each maps the file to its symbol family. Binaries linked against minihost should validate the ABI at startup: `if (mh_api_version() < MH_API_VERSION_NUMBER) { /* mismatch */ }`.

To migrate a C/C++ codebase, apply the substitutions in this order -- the ordering matters because `mh_graph_` is a prefix of `mh_graph_v2_`, so a naive pass would corrupt the DAG symbols:

```sh
# function symbols (placeholder avoids the prefix collision)
perl -pi -e 's/mh_graph_v2_/MHTMP_/g; s/mh_graph_/mh_bus_/g; s/MHTMP_/mh_graph_/g' FILES...
# types and the C++ wrapper class
perl -pi -e 's/MH_GraphV2/MHTMP2/g; s/\bGraphV2\b/PluginGraph/g; s/MH_PluginGraph/MH_PluginBus/g; s/MHTMP2/MH_PluginGraph/g' FILES...
```

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
| `minihost.PluginGraph(in, out, ...)` (parallel bus) | `minihost.PluginBus(in, out, ...)` | same — plus `bus.process_midi(...)` for layering |
| `minihost.GraphV2(block, sr)` (DAG) | `minihost.PluginGraph(block, sr)` | same |
| C: `mh_graph_*` / `MH_PluginGraph` (bus) | `mh_bus_*` / `MH_PluginBus` | same |
| C: `mh_graph_v2_*` / `MH_GraphV2` (DAG) | `mh_graph_*` / `MH_PluginGraph` | same |
| C++: `minihost::GraphV2` | `minihost::PluginGraph` | same |

---

## AudioBuffer differences worth knowing

If you migrate to `AudioBuffer`, a few semantic differences from `np.ndarray`:

- **Slicing returns copies, not views.** `buf[:, 100:200]` allocates a new buffer. To mutate the parent through a slice, write to it via `__setitem__`: `buf[:, 100:200] = source`.

- **2-axis indexing only.** `buf[0]` raises `TypeError`; use `buf[0, :]`. The reasoning: a 2D buffer's single-axis indexing is ambiguous (channel? frame? a 1D array?), and the explicit form is unambiguous.

- **No strided slicing, fancy indexing, boolean indexing, or `Ellipsis`.** Each raises a clear `TypeError` directing you to `.as_ndarray()`.

- **`.shape` is a tuple `(channels, frames)`.** Same as numpy. `len(buf)` returns `channels` (matches numpy's `len()` on 2D arrays).

- **`np.asarray(buf)` and `buf.as_ndarray()` both work** (zero-copy via DLPack / `__array__`). Use them at the boundary where you actually need numpy semantics.

JUCE-backed DSP ops are exposed for the common cases so you rarely need numpy:

- `buf.clear()`, `buf.apply_gain(g)`, `buf.apply_gain_ramp(start, count, lo, hi)`, `buf.apply_gain_per_channel([g0, g1, ...])`

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
