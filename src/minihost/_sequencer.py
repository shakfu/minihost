"""Note generation for `sequencer` project nodes.

A port of the desktop app's `projects/minihost_desktop/src/sequencer.h`. It
must stay bit-identical to it: an offline render of a project in Python and
in the desktop app produces the same notes at the same samples. Each step's
note, velocity and on/off decision is hashed from (seed, step), so any sample
range can be generated on its own.
"""

from __future__ import annotations

import math
from dataclasses import dataclass

_MASK = (1 << 64) - 1

SCALES: dict[str, tuple[int, ...]] = {
    "major": (0, 2, 4, 5, 7, 9, 11),
    "minor": (0, 2, 3, 5, 7, 8, 10),
    "dorian": (0, 2, 3, 5, 7, 9, 10),
    "major_pentatonic": (0, 2, 4, 7, 9),
    "minor_pentatonic": (0, 3, 5, 7, 10),
    "chromatic": tuple(range(12)),
}


@dataclass
class SequencerParams:
    """Fields and defaults of the desktop `SequencerParams`."""

    steps: int = 16
    rate: float = 0.25  # step length in beats
    root: int = 48
    scale: str = "minor"
    octaves: int = 2
    density: float = 0.7
    gate: float = 0.5
    vel_min: int = 70
    vel_max: int = 110
    mutate: float = 0.0
    seed: int = 1
    channel: int = 1  # 1-16


def _clamp(v, lo, hi):
    return max(lo, min(hi, v))


def clamp_params(p: SequencerParams) -> None:
    """Clamp every field to its range, as `clampSequencerParams` does."""
    p.steps = _clamp(p.steps, 1, 64)
    p.rate = _clamp(p.rate, 1.0 / 16.0, 4.0) if math.isfinite(p.rate) else 0.25
    p.root = _clamp(p.root, 0, 127)
    p.octaves = _clamp(p.octaves, 1, 4)
    p.density = _clamp(p.density, 0.0, 1.0) if math.isfinite(p.density) else 0.7
    p.gate = _clamp(p.gate, 0.01, 1.0) if math.isfinite(p.gate) else 0.5
    p.vel_min = _clamp(p.vel_min, 1, 127)
    p.vel_max = _clamp(p.vel_max, p.vel_min, 127)
    p.mutate = _clamp(p.mutate, 0.0, 1.0) if math.isfinite(p.mutate) else 0.0
    p.channel = _clamp(p.channel, 1, 16)


def seed_from_json(value: float) -> int:
    """The desktop's seed conversion: `(uint32)(int64) fmod(|v|, 2^32)`."""
    return int(math.fmod(abs(value), 4294967296.0))


def _splitmix(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & _MASK
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & _MASK
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & _MASK
    return x ^ (x >> 31)


def _unit(seed: int, key: int, salt: int) -> float:
    """Uniform in [0, 1)."""
    h = _splitmix(_splitmix(seed) ^ _splitmix((key * 8 + salt) & _MASK))
    return (h >> 11) * 2.0**-53


def _llround(x: float) -> int:
    """`std::llround`: half away from zero (Python's round() is half-even)."""
    r = math.floor(x)
    if x - r >= 0.5:
        r += 1
    return int(r)


def step(p: SequencerParams, i: int) -> tuple[bool, int, int]:
    """(on, note, velocity) of step `i`, as `sequencerStep` computes it."""
    steps = max(1, p.steps)
    key = i % steps
    if p.mutate > 0.0 and _unit(p.seed, i & _MASK, 7) < p.mutate:
        key = steps + i  # this occurrence only
    on = _unit(p.seed, key, 1) < p.density
    degrees = SCALES.get(p.scale, SCALES["minor"])
    count = len(degrees)
    rng = count * max(1, p.octaves)
    d = min(rng - 1, int(_unit(p.seed, key, 2) * rng))
    note = _clamp(p.root + 12 * (d // count) + degrees[d % count], 0, 127)
    lo = _clamp(min(p.vel_min, p.vel_max), 1, 127)
    hi = _clamp(max(p.vel_min, p.vel_max), 1, 127)
    vel = min(hi, lo + int(_unit(p.seed, key, 3) * (hi - lo + 1)))
    return on, note, vel


def events(
    p: SequencerParams, sample_rate: float, bpm: float, s0: int, s1: int
) -> list[tuple[int, int, int, int]]:
    """(sample, status, data1, data2) for every note-on and note-off at an
    absolute sample in [s0, s1), in time order, as `sequencerEvents` emits."""
    out: list[tuple[int, int, int, int]] = []
    if bpm <= 0.0 or p.rate <= 0.0 or s1 <= s0:
        return out
    step_len = p.rate * 60.0 * sample_rate / bpm
    if step_len < 1.0:
        return out
    gate = _clamp(p.gate, 0.01, 1.0)
    ch = _clamp(p.channel, 1, 16) - 1
    i = max(0, math.floor(s0 / step_len - gate) - 1)
    while True:
        on_at = _llround(i * step_len)
        if on_at >= s1:
            break
        on, note, vel = step(p, i)
        if on:
            off_at = _llround((i + gate) * step_len)
            if on_at >= s0:
                out.append((on_at, 0x90 | ch, note, vel))
            if s0 <= off_at < s1:
                out.append((off_at, 0x80 | ch, note, 0))
        i += 1
    return out
