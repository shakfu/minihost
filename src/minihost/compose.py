"""Callable, composable audio pipelines.

This module adds an audiomentations-style composition layer on top of
minihost's native routing objects (:class:`Plugin`, :class:`PluginChain`,
:class:`PluginBus`). Where those classes model *real-time* signal routing,
:class:`Compose` models an *offline* pipeline: a sequence of transforms
applied to a whole buffer, returned as a new buffer.

The core idea is a small transform protocol. A *transform* is any of:

* a native processor -- :class:`Plugin`, :class:`PluginChain`, or
  :class:`PluginBus` -- which is run over the buffer via
  :func:`minihost.process_audio`;
* a nested :class:`Compose`;
* one of the pure-python transforms in this module
  (:class:`Gain`, :class:`Normalize`, :class:`Trim`, :class:`Fade`,
  :class:`AddGaussianNoise`);
* one of the stochastic combinators (:class:`Maybe`, :class:`OneOf`,
  :class:`SomeOf`, :class:`RandomParam`);
* any user callable with the signature ``fn(audio, sample_rate) ->
  audio``, where ``audio`` is an :class:`AudioBuffer`.

:class:`Compose` is callable in the audiomentations idiom::

    with minihost.Compose([
        minihost.Plugin("delay.vst3", sample_rate=48000),
        minihost.Plugin("reverb.vst3", sample_rate=48000),
        minihost.Normalize(-1.0),
    ], tail_seconds=4.0) as fx:
        out = fx(samples, sample_rate=48000)   # returns processed audio
        fx.to_file("in.wav", "out.wav")        # or file-to-file

Design notes
------------
* **Working type is** :class:`AudioBuffer`. numpy is optional and only
  imported when the caller passes a numpy array, uses ``tail_seconds=
  "auto"``, or uses a transform that needs it (:class:`Fade`,
  :class:`AddGaussianNoise`). When ``__call__`` is handed a numpy array
  the result is returned as a numpy array of matching dimensionality
  (1-D in, 1-D out).
* **Sample rate is validated, never silently resampled.** Native
  processors are constructed at a fixed rate; running a pipeline at a
  different rate raises rather than corrupt the signal.
* **Tails are handled once, at the pipeline boundary.** A numeric
  ``tail_seconds`` pads the input with that much silence up front so
  every element rings out into it; ``"auto"`` over-renders and trims
  trailing silence below ``tail_threshold``.
* **Lifetime.** ``Compose`` closes the native processors it holds on
  ``__exit__`` (``close_children=True``, the default), so the whole
  pipeline collapses to a single ``with``. Pass ``close_children=False``
  when a processor is shared with code outside the pipeline.
"""

from __future__ import annotations

import random
from pathlib import Path
from typing import Any, Callable, Sequence, Union

from minihost._core import AudioBuffer, Plugin, PluginBus, PluginChain
from minihost.audio_io import read_audio, resample, write_audio
from minihost.process import process_audio

# Native processors that Compose runs via process_audio.
_PROCESSORS = (Plugin, PluginChain, PluginBus)

Transform = Union[
    Plugin,
    PluginChain,
    PluginBus,
    "Compose",
    "_Wrapper",
    Callable[[Any, float], Any],
]


def _np():
    """Import numpy lazily with a friendly error."""
    try:
        import numpy  # noqa: F401

        return numpy
    except ImportError as e:  # pragma: no cover - exercised only without numpy
        raise ImportError(
            "This minihost.compose feature requires numpy. Install the "
            "numpy extra: 'pip install minihost[numpy]'."
        ) from e


def _coerce(x: Any) -> AudioBuffer:
    """Coerce a transform's return value into an AudioBuffer."""
    if isinstance(x, AudioBuffer):
        return x
    np = _np()
    return AudioBuffer.from_numpy(np.ascontiguousarray(np.asarray(x, dtype=np.float32)))


def _run_processor(
    proc: Any,
    audio: AudioBuffer,
    block_size: int | None,
) -> AudioBuffer:
    """Run a native processor length-preservingly over ``audio``.

    Any tail room has already been padded into ``audio`` by the enclosing
    Compose, so we render with ``tail_seconds=0`` and let latency
    compensation keep the output aligned and same-length.
    """
    return process_audio(
        proc,
        audio,
        tail_seconds=0.0,
        compensate_latency=True,
        block_size=block_size,
    )


def _apply(
    t: Transform,
    audio: AudioBuffer,
    sr: float,
    rng: random.Random,
    block_size: int | None,
) -> AudioBuffer:
    """Apply a single transform to ``audio`` and return the result buffer."""
    if isinstance(t, Compose):
        return t._run(audio, sr, rng)
    if isinstance(t, _PROCESSORS):
        return _run_processor(t, audio, block_size)
    if isinstance(t, _Wrapper):
        return t.apply(audio, sr, rng, block_size)
    if callable(t):
        return _coerce(t(audio, sr))
    raise TypeError(
        f"Transform {t!r} is not a Plugin/PluginChain/PluginBus, a "
        f"Compose, a compose transform, or a callable(audio, sample_rate)."
    )


def _collect_processors(t: Transform):
    """Yield every native processor reachable from ``t`` (recursively)."""
    if isinstance(t, _PROCESSORS):
        yield t
    elif isinstance(t, Compose):
        for child in t.transforms:
            yield from _collect_processors(child)
    elif isinstance(t, _Wrapper):
        for child in t.children():
            yield from _collect_processors(child)


def _collect_closeables(t: Transform):
    """Yield objects with a ``close()`` reachable from ``t`` (processors
    and nested Composes)."""
    if isinstance(t, _PROCESSORS) or isinstance(t, Compose):
        yield t
    if isinstance(t, Compose):
        for child in t.transforms:
            yield from _collect_closeables(child)
    elif isinstance(t, _Wrapper):
        for child in t.children():
            yield from _collect_closeables(child)


class Compose:
    """A callable, ordered pipeline of audio transforms.

    Args:
        transforms: Ordered sequence of transforms (see module docstring
            for what qualifies as a transform).
        tail_seconds: Extra tail handling applied once at the pipeline
            boundary. ``0.0`` (default) preserves length. A positive
            float pads the input with that many seconds of silence up
            front so tails ring out. ``"auto"`` over-renders by
            ``max_tail_seconds`` and trims trailing silence below
            ``tail_threshold`` (requires numpy).
        tail_threshold: Linear-amplitude floor for ``"auto"`` trimming
            (default ``1e-4`` ~ -80 dBFS).
        max_tail_seconds: Over-render length for ``"auto"`` (default
            30 s).
        block_size: Block size threaded into ``process_audio`` for native
            processors. ``None`` uses process_audio's default.
        close_children: When True (default) native processors and nested
            Composes held by this pipeline are closed on ``__exit__`` /
            :meth:`close`.
        shuffle: When True the transform order is randomized on every
            call using the seeded RNG.
        seed: Seed for the pipeline's RNG (used by ``shuffle`` and by the
            stochastic transforms). ``None`` (default) seeds from system
            entropy.
    """

    def __init__(
        self,
        transforms: Sequence[Transform],
        *,
        tail_seconds: float | str = 0.0,
        tail_threshold: float = 1e-4,
        max_tail_seconds: float = 30.0,
        block_size: int | None = None,
        close_children: bool = True,
        shuffle: bool = False,
        seed: int | None = None,
    ) -> None:
        self.transforms: list[Transform] = list(transforms)
        self.tail_seconds = tail_seconds
        self.tail_threshold = float(tail_threshold)
        self.max_tail_seconds = float(max_tail_seconds)
        self.block_size = block_size
        self.close_children = bool(close_children)
        self.shuffle = bool(shuffle)
        self._rng = random.Random(seed)
        self._closed = False

    # -- introspection -------------------------------------------------

    def __len__(self) -> int:
        return len(self.transforms)

    @property
    def sample_rate(self) -> float:
        """The rate inferred from the first native processor.

        Raises if the pipeline holds no native processor (nothing to
        infer a rate from -- pass ``sample_rate`` to ``__call__`` /
        ``to_file`` instead).
        """
        return self._resolve_sr(None)

    def _resolve_sr(self, sample_rate: float | None) -> float:
        if sample_rate is not None:
            return float(sample_rate)
        for proc in self._processors():
            return float(proc.sample_rate)
        raise ValueError(
            "sample_rate is required: the pipeline holds no "
            "Plugin/PluginChain/PluginBus to infer it from, and no "
            "sample_rate was passed."
        )

    def _processors(self):
        for t in self.transforms:
            yield from _collect_processors(t)

    def _check_rates(self, sr: float) -> None:
        for proc in self._processors():
            prate = float(proc.sample_rate)
            if abs(prate - sr) > 1e-6:
                raise ValueError(
                    f"sample_rate mismatch: pipeline is running at {sr} Hz "
                    f"but a {type(proc).__name__} was constructed at "
                    f"{prate} Hz. Reconstruct it at the matching rate "
                    f"(minihost does not silently resample plugins)."
                )

    def _first_input_channels(self) -> int | None:
        for proc in self._processors():
            return int(proc.num_input_channels)
        return None

    # -- tail geometry -------------------------------------------------

    def _tail_frames(self, sr: float) -> tuple[int, bool]:
        """Return ``(pad_frames, do_trim)`` for the configured tail."""
        ts = self.tail_seconds
        if isinstance(ts, str):
            if ts != "auto":
                raise ValueError(f"tail_seconds string must be 'auto', got {ts!r}.")
            return int(self.max_tail_seconds * sr), True
        ts = float(ts)
        if ts < 0:
            raise ValueError(f"tail_seconds must be >= 0, got {ts}.")
        return int(ts * sr), False

    @staticmethod
    def _pad(buf: AudioBuffer, tail_frames: int) -> AudioBuffer:
        """Return a private copy of ``buf`` with ``tail_frames`` of
        trailing silence (a plain copy when ``tail_frames <= 0``)."""
        if tail_frames <= 0:
            return buf.copy()
        out = AudioBuffer(buf.channels, buf.frames + tail_frames)
        out.clear()
        out[:, : buf.frames] = buf
        return out

    def _trim_tail(
        self,
        buf: AudioBuffer,
        min_frames: int,
    ) -> AudioBuffer:
        """Trim trailing frames below ``tail_threshold``, keeping at least
        ``min_frames``."""
        np = _np()
        data = buf.as_ndarray()
        mag = np.max(np.abs(data), axis=0)
        above = np.nonzero(mag > self.tail_threshold)[0]
        end = min_frames if above.size == 0 else max(min_frames, int(above[-1]) + 1)
        end = min(end, buf.frames)
        if end >= buf.frames:
            return buf
        return buf[:, :end].copy()

    # -- execution -----------------------------------------------------

    def _run(
        self,
        buf: AudioBuffer,
        sr: float,
        rng: random.Random,
    ) -> AudioBuffer:
        """Core pipeline: pad, apply transforms in order, optionally trim.

        ``buf`` is treated as read-only; a private working buffer is
        produced by :meth:`_pad` before any transform runs.
        """
        if self._closed:
            raise RuntimeError("Compose pipeline is closed.")
        self._check_rates(sr)

        pad_frames, do_trim = self._tail_frames(sr)
        work = self._pad(buf, pad_frames)

        order = self.transforms
        if self.shuffle:
            order = list(self.transforms)
            rng.shuffle(order)

        for t in order:
            work = _coerce(_apply(t, work, sr, rng, self.block_size))

        if do_trim:
            work = self._trim_tail(work, buf.frames)
        return work

    def __call__(
        self,
        samples: Any = None,
        sample_rate: float | None = None,
    ) -> Any:
        """Process ``samples`` through the pipeline.

        Args:
            samples: Input audio as an :class:`AudioBuffer`, a numpy
                array of shape ``(frames,)`` or ``(channels, frames)``,
                or any 2-D float32 buffer-protocol producer.
            sample_rate: Pipeline sample rate. Inferred from the first
                native processor when omitted; required when the pipeline
                holds only pure-python transforms.

        Returns:
            The processed audio, in the same container family as the
            input: an :class:`AudioBuffer` for AudioBuffer input, else a
            numpy array of matching dimensionality (1-D for 1-D input).
        """
        if samples is None:
            raise ValueError("Compose.__call__ requires input samples.")
        sr = self._resolve_sr(sample_rate)

        if isinstance(samples, AudioBuffer):
            out = self._run(samples, sr, self._rng)
            return out

        np = _np()
        arr = np.asarray(samples, dtype=np.float32)
        was_1d = arr.ndim == 1
        if was_1d:
            arr = arr[np.newaxis, :]
        buf = AudioBuffer.from_numpy(np.ascontiguousarray(arr))
        out = self._run(buf, sr, self._rng)
        result = out.as_ndarray()
        return result[0] if was_1d else result

    def to_file(
        self,
        input_path: str | Path,
        output_path: str | Path,
        *,
        sample_rate: float | None = None,
        bit_depth: int = 24,
        resample_to_pipeline_rate: bool = True,
        duplicate_to_stereo: bool = True,
    ) -> int:
        """Process an audio file through the pipeline and write the result.

        Args:
            input_path: Source audio file (.wav / .flac / ...).
            output_path: Destination file (.wav or .flac).
            sample_rate: Override the pipeline rate; inferred from the
                first native processor when omitted.
            bit_depth: Output bit depth (16/24/32 WAV, 16/24 FLAC).
            resample_to_pipeline_rate: Resample a mismatched input to the
                pipeline rate (default True); otherwise raise on mismatch.
            duplicate_to_stereo: Channel-duplicate a too-narrow input to
                match the first processor's input channel count.

        Returns:
            Number of frames written.
        """
        from minihost.process import _maybe_duplicate_to_match

        sr = self._resolve_sr(sample_rate)
        audio, in_sr = read_audio(input_path)
        if in_sr != sr:
            if not resample_to_pipeline_rate:
                raise ValueError(
                    f"Input is {in_sr} Hz but pipeline is {sr} Hz. Pass "
                    f"resample_to_pipeline_rate=True to convert."
                )
            audio = resample(audio, in_sr, int(sr))
        buf = audio if isinstance(audio, AudioBuffer) else AudioBuffer.from_numpy(audio)

        required = self._first_input_channels()
        if required is not None:
            buf = _maybe_duplicate_to_match(
                buf,
                required,
                duplicate_to_stereo,
                "Input",
            )

        out = self(buf, sample_rate=sr)
        write_audio(output_path, out, int(sr), bit_depth=bit_depth)
        return out.frames

    # -- lifetime ------------------------------------------------------

    def close(self) -> None:
        """Close held processors / nested Composes when
        ``close_children`` is set. Idempotent."""
        if self._closed:
            return
        self._closed = True
        if not self.close_children:
            return
        seen: set[int] = set()
        for t in self.transforms:
            for obj in _collect_closeables(t):
                if id(obj) in seen or obj is self:
                    continue
                seen.add(id(obj))
                try:
                    obj.close()
                except Exception:
                    pass

    def __enter__(self) -> "Compose":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.close()

    def __repr__(self) -> str:
        return f"Compose({self.transforms!r})"


# =====================================================================
# Phase 2 -- pure-python, deterministic transforms
# =====================================================================


class Gain:
    """Apply a fixed gain in decibels."""

    def __init__(self, db: float) -> None:
        self.db = float(db)

    def __call__(self, audio: AudioBuffer, sample_rate: float) -> AudioBuffer:
        out = audio.copy()
        out.apply_gain(10.0 ** (self.db / 20.0))
        return out

    def __repr__(self) -> str:
        return f"Gain(db={self.db})"


class Normalize:
    """Peak-normalize to a target level in dBFS (0 = full scale).

    Silent buffers are passed through unchanged.
    """

    def __init__(self, peak_dbfs: float = -1.0) -> None:
        self.peak_dbfs = float(peak_dbfs)

    def __call__(self, audio: AudioBuffer, sample_rate: float) -> AudioBuffer:
        out = audio.copy()
        peak = out.magnitude()
        if peak > 0.0:
            target = 10.0 ** (self.peak_dbfs / 20.0)
            out.apply_gain(target / peak)
        return out

    def __repr__(self) -> str:
        return f"Normalize(peak_dbfs={self.peak_dbfs})"


class Trim:
    """Keep a time window ``[start, start + duration)`` in seconds.

    ``duration=None`` keeps everything from ``start`` to the end.
    """

    def __init__(self, start: float = 0.0, duration: float | None = None) -> None:
        self.start = float(start)
        self.duration = None if duration is None else float(duration)

    def __call__(self, audio: AudioBuffer, sample_rate: float) -> AudioBuffer:
        n = audio.frames
        s = max(0, min(int(round(self.start * sample_rate)), n))
        if self.duration is None:
            e = n
        else:
            e = max(s, min(s + int(round(self.duration * sample_rate)), n))
        if s == 0 and e == n:
            return audio.copy()
        return audio[:, s:e].copy()

    def __repr__(self) -> str:
        return f"Trim(start={self.start}, duration={self.duration})"


class Fade:
    """Apply a linear fade-in and/or fade-out (lengths in seconds)."""

    def __init__(self, fade_in: float = 0.0, fade_out: float = 0.0) -> None:
        self.fade_in = float(fade_in)
        self.fade_out = float(fade_out)

    def __call__(self, audio: AudioBuffer, sample_rate: float) -> AudioBuffer:
        np = _np()
        data = audio.as_ndarray().copy()
        n = data.shape[1]
        fi = min(int(self.fade_in * sample_rate), n)
        fo = min(int(self.fade_out * sample_rate), n)
        if fi > 0:
            data[:, :fi] *= np.linspace(0.0, 1.0, fi, dtype=np.float32)
        if fo > 0:
            data[:, n - fo :] *= np.linspace(1.0, 0.0, fo, dtype=np.float32)
        return AudioBuffer.from_numpy(data)

    def __repr__(self) -> str:
        return f"Fade(fade_in={self.fade_in}, fade_out={self.fade_out})"


# =====================================================================
# Phase 3 -- stochastic combinators
# =====================================================================


class _Wrapper:
    """Base for transforms that wrap other transforms and consume the
    pipeline RNG. Subclasses implement :meth:`apply` and :meth:`children`.
    """

    def apply(
        self,
        audio: AudioBuffer,
        sr: float,
        rng: random.Random,
        block_size: int | None,
    ) -> AudioBuffer:
        raise NotImplementedError

    def children(self) -> Sequence[Transform]:
        """Wrapped transforms (for RNG-free introspection: rate checks,
        close propagation)."""
        return ()


class Maybe(_Wrapper):
    """Apply ``transform`` with probability ``p``, else pass through."""

    def __init__(self, transform: Transform, p: float = 0.5) -> None:
        if not 0.0 <= p <= 1.0:
            raise ValueError(f"p must be in [0, 1], got {p}.")
        self.transform = transform
        self.p = float(p)

    def apply(self, audio, sr, rng, block_size):
        if rng.random() < self.p:
            return _apply(self.transform, audio, sr, rng, block_size)
        return audio

    def children(self):
        return (self.transform,)

    def __repr__(self) -> str:
        return f"Maybe({self.transform!r}, p={self.p})"


class OneOf(_Wrapper):
    """Apply exactly one of ``transforms``, chosen at random.

    ``weights`` (optional) biases the choice.
    """

    def __init__(
        self,
        transforms: Sequence[Transform],
        weights: Sequence[float] | None = None,
    ) -> None:
        self.transforms = list(transforms)
        if not self.transforms:
            raise ValueError("OneOf requires at least one transform.")
        if weights is not None and len(weights) != len(self.transforms):
            raise ValueError("weights must match the number of transforms.")
        self.weights = None if weights is None else list(weights)

    def apply(self, audio, sr, rng, block_size):
        chosen = rng.choices(self.transforms, weights=self.weights, k=1)[0]
        return _apply(chosen, audio, sr, rng, block_size)

    def children(self):
        return tuple(self.transforms)

    def __repr__(self) -> str:
        return f"OneOf({self.transforms!r})"


class SomeOf(_Wrapper):
    """Apply a random subset of ``transforms``, in their original order.

    ``n`` is the subset size: an int for a fixed count, or a
    ``(min, max)`` tuple for a random count drawn each call.
    """

    def __init__(
        self,
        n: int | tuple[int, int],
        transforms: Sequence[Transform],
    ) -> None:
        self.transforms = list(transforms)
        if not self.transforms:
            raise ValueError("SomeOf requires at least one transform.")
        self.n = n
        if isinstance(n, tuple):
            lo, hi = n
            if not (0 <= lo <= hi <= len(self.transforms)):
                raise ValueError(
                    f"n range {n} out of bounds for "
                    f"{len(self.transforms)} transform(s)."
                )
        elif not (0 <= int(n) <= len(self.transforms)):
            raise ValueError(
                f"n={n} out of bounds for {len(self.transforms)} transform(s)."
            )

    def apply(self, audio, sr, rng, block_size):
        if isinstance(self.n, tuple):
            k = rng.randint(self.n[0], self.n[1])
        else:
            k = int(self.n)
        # Choose k distinct indices, then apply in original order.
        idxs = sorted(rng.sample(range(len(self.transforms)), k))
        out = audio
        for i in idxs:
            out = _apply(self.transforms[i], out, sr, rng, block_size)
        return out

    def children(self):
        return tuple(self.transforms)

    def __repr__(self) -> str:
        return f"SomeOf({self.n}, {self.transforms!r})"


class RandomParam(_Wrapper):
    """Set a plugin parameter to a random value, then process through it.

    The parameter is set to a value drawn uniformly from
    ``[min_value, max_value]`` (normalized parameter units, 0..1) on each
    call, and the plugin is then run over the buffer. ``param`` is a
    parameter name (matched case-insensitively via
    :meth:`Plugin.set_param_by_name`) or an integer parameter index.
    """

    def __init__(
        self,
        plugin: Plugin,
        param: str | int,
        min_value: float = 0.0,
        max_value: float = 1.0,
        *,
        block_size: int | None = None,
    ) -> None:
        if not isinstance(plugin, Plugin):
            raise TypeError("RandomParam wraps a single Plugin.")
        self.plugin = plugin
        self.param = param
        self.min_value = float(min_value)
        self.max_value = float(max_value)
        self.block_size = block_size

    def apply(self, audio, sr, rng, block_size):
        value = rng.uniform(self.min_value, self.max_value)
        if isinstance(self.param, str):
            self.plugin.set_param_by_name(self.param, float(value))
        else:
            self.plugin.set_param(int(self.param), float(value))
        return _run_processor(
            self.plugin,
            audio,
            self.block_size if block_size is None else block_size,
        )

    def children(self):
        return (self.plugin,)

    def __repr__(self) -> str:
        return f"RandomParam({self.param!r}, [{self.min_value}, {self.max_value}])"


class AddGaussianNoise(_Wrapper):
    """Add white Gaussian noise with a random per-call amplitude.

    The amplitude is drawn uniformly from ``[min_amplitude,
    max_amplitude]`` (linear). Requires numpy.
    """

    def __init__(
        self,
        min_amplitude: float = 0.001,
        max_amplitude: float = 0.015,
    ) -> None:
        self.min_amplitude = float(min_amplitude)
        self.max_amplitude = float(max_amplitude)

    def apply(self, audio, sr, rng, block_size):
        np = _np()
        amp = rng.uniform(self.min_amplitude, self.max_amplitude)
        # Draw a fresh numpy Generator seeded from the pipeline RNG so
        # bulk noise stays reproducible without making numpy a hard
        # dependency of the routing combinators.
        gen = np.random.default_rng(rng.getrandbits(63))
        data = audio.as_ndarray()
        noise = (gen.standard_normal(data.shape) * amp).astype(np.float32)
        return AudioBuffer.from_numpy(np.ascontiguousarray(data + noise))

    def __repr__(self) -> str:
        return f"AddGaussianNoise([{self.min_amplitude}, {self.max_amplitude}])"
