# Composition Pipelines

`minihost.Compose` is a callable, composable pipeline layer inspired by
[audiomentations](https://github.com/iver56/audiomentations). Where the
native `Plugin`, `PluginChain`, and `PluginBus` classes model *real-time*
signal routing, `Compose` models an *offline* pipeline: an ordered list of
transforms applied to a whole buffer and returned as a new one.

A pipeline collapses an effect chain to a single `with` block, mixes real
plugins with pure-python DSP, and -- with the stochastic combinators --
becomes a data-augmentation engine that runs training audio through real
plugins.

## Quick start

```python
import minihost

with minihost.Compose([
    minihost.Plugin("/path/to/delay.vst3", sample_rate=48000),
    minihost.Plugin("/path/to/reverb.vst3", sample_rate=48000),
    minihost.Normalize(-1.0),
], tail_seconds=4.0) as fx:
    fx.to_file("in.wav", "out.wav")     # file-to-file
```

`Compose` owns the plugins it is given and closes them on exit, so the
per-plugin context managers are no longer needed.

## The transform protocol

A *transform* is any of:

- a native processor -- `Plugin`, `PluginChain`, or `PluginBus` (run over
  the buffer via [`process_audio`](api_python.md#offline-processing));
- a nested `Compose`;
- a pure-python transform: `Gain`, `Normalize`, `Trim`, `Fade`;
- a stochastic combinator: `Maybe`, `OneOf`, `SomeOf`, `RandomParam`,
  `AddGaussianNoise`;
- any user callable with the signature `fn(audio, sample_rate) -> audio`.

The working type is `AudioBuffer`, so numpy stays optional. A transform
receives an `AudioBuffer` and returns one (a returned numpy array is
coerced). To reuse a numpy-based function, convert at the boundary with
`buffer.as_ndarray()` / `AudioBuffer.from_numpy(array)`.

## Calling a pipeline

`Compose` is callable in the audiomentations idiom:

```python
out = fx(samples, sample_rate=48000)
```

The input container family is preserved:

| Input | Output |
|-------|--------|
| `AudioBuffer` | `AudioBuffer` |
| numpy `(channels, frames)` | numpy `(channels, frames)` |
| numpy `(frames,)` (mono) | numpy `(frames,)` |

`sample_rate` is inferred from the first native processor in the pipeline
and may be omitted then. A pipeline of only pure-python transforms has no
processor to infer from, so `sample_rate` is required.

!!! note "Sample rate is validated, never silently resampled"
    Native processors are constructed at a fixed sample rate. Running a
    pipeline at a different rate raises `ValueError` rather than corrupt
    the signal. Reconstruct the plugin at the target rate instead.

## Tails

Tail handling happens once, at the pipeline boundary:

- `tail_seconds=0.0` (default) -- length-preserving.
- `tail_seconds=<float>` -- pads the input with that many seconds of
  silence up front so every element (delay, reverb) rings out into it.
- `tail_seconds="auto"` -- over-renders by `max_tail_seconds` (default
  30 s) and trims trailing silence below `tail_threshold` (default `1e-4`,
  ~ -80 dBFS). Requires numpy.

## Lifetime

By default (`close_children=True`) `Compose` closes the native processors
and nested `Compose` objects it holds when its `with` block exits (or when
`close()` is called). Pass `close_children=False` when a plugin is shared
with code outside the pipeline:

```python
plugin = minihost.Plugin("reverb.vst3", sample_rate=48000)
with minihost.Compose([plugin], close_children=False) as fx:
    out = fx(buf)
# plugin is still open here
plugin.close()
```

Closing a `PluginChain` given to `Compose` releases the chain but not the
individual plugins inside it (matching `PluginChain.close` semantics).

## Pure-python transforms

Deterministic, `AudioBuffer`-native transforms usable inside or outside a
pipeline:

```python
minihost.Gain(db)                               # fixed gain in dB
minihost.Normalize(peak_dbfs=-1.0)              # peak-normalize (silence passes through)
minihost.Trim(start=0.0, duration=None)         # keep a time window (seconds)
minihost.Fade(fade_in=0.0, fade_out=0.0)        # linear fades (seconds)
```

Each returns a new buffer. Standalone use:

```python
buf = minihost.AudioBuffer.from_numpy(samples)
quieter = minihost.Gain(-6.0)(buf, 48000)
```

## Stochastic combinators

For data augmentation. Every call re-rolls choices from the pipeline's
seeded RNG, so a pipeline is reproducible across runs but varied across
calls.

```python
minihost.Maybe(transform, p=0.5)                # apply with probability p, else pass through
minihost.OneOf([a, b, c], weights=None)         # apply exactly one, at random
minihost.SomeOf(2, [a, b, c])                   # apply a random subset (fixed count)
minihost.SomeOf((1, 3), [a, b, c])              # ... or a random count in a range
minihost.RandomParam(plugin, param, lo, hi)     # set a plugin param at random, then process
minihost.AddGaussianNoise(0.001, 0.015)         # add white noise, random amplitude
```

- `Compose(..., seed=0)` seeds the RNG; `None` (default) uses system
  entropy.
- `Compose(..., shuffle=True)` randomizes the transform order on every
  call.
- `RandomParam`'s `param` is a parameter name (case-insensitive) or an
  integer index; `lo`/`hi` are normalized parameter units (0..1).
- The routing combinators use Python's `random` module (no numpy);
  `AddGaussianNoise` uses numpy, seeded deterministically from the
  pipeline RNG.

### Augmentation example

```python
import numpy as np
import minihost

augment = minihost.Compose([
    minihost.AddGaussianNoise(min_amplitude=0.001, max_amplitude=0.015),
    minihost.OneOf([minihost.Gain(-3.0), minihost.Gain(-6.0)]),
    minihost.Maybe(minihost.Normalize(-1.0), p=0.5),
], seed=0)

samples = np.random.uniform(-0.2, 0.2, size=(2, 32000)).astype(np.float32)
variants = [augment(samples, sample_rate=16000) for _ in range(3)]  # 3 different renders
```

## Relationship to the other composition APIs

| Need | Use |
|------|-----|
| Serial plugin chain, real-time | [`PluginChain`](api_python.md#pluginchain) |
| Parallel branches summed | [`PluginBus`](api_python.md#pluginbus) |
| Arbitrary node-to-node routing | [`PluginGraph`](api_python.md#plugingraph) |
| Chain from a JSON/YAML file | [`load_chain`](api_python.md#offline-processing) |
| Callable offline pipeline, plugins + DSP, augmentation | `Compose` (this page) |

`Compose` is a Python-level orchestration layer that *uses* the native
classes as elements -- a `Compose` transform can itself be a `PluginBus`
or a nested `Compose`, giving serial-of-parallel without dropping to
`PluginGraph`.

See `examples/compose.py` for a runnable walkthrough.
