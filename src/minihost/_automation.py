"""Parameter automation in project files: parsing and expansion.

The desktop app has the same code (``projects/minihost_desktop/src/project.cpp``,
``expandAutomation``); both renderers must send a plugin the same parameter
changes, so a change here needs the same change there.

A lane is ``{"node": id, "param": index or name, "points": [[seconds,
value], ...]}``. Values are normalized (0..1). Between two points the value
ramps linearly, sent as a change every ``STEP`` samples; before the first
point it holds the first value, after the last it holds the last.
"""

from __future__ import annotations

import math
from bisect import bisect_left
from dataclasses import dataclass
from typing import Any

from ._sequencer import _llround

STEP = 64


@dataclass(frozen=True)
class Lane:
    node: str
    param: int | str
    points: list[tuple[float, float]]


def parse(raw: Any, plugin_ids: set[str]) -> list[Lane]:
    """Validate the project's ``automation`` list. Raises ValueError."""
    if not isinstance(raw, list):
        raise ValueError("automation must be a list")
    lanes = []
    for i, item in enumerate(raw):
        where = f"automation[{i}]"
        if not isinstance(item, dict):
            raise ValueError(f"{where} must be an object")
        node = item.get("node")
        if not isinstance(node, str) or node not in plugin_ids:
            raise ValueError(f"{where}: node {node!r} is not a plugin node")
        param = item.get("param")
        if isinstance(param, bool) or not isinstance(param, (int, str)):
            raise ValueError(f"{where}: param must be an index or a name")
        if isinstance(param, int) and param < 0:
            raise ValueError(f"{where}: param index must be >= 0")
        points = item.get("points")
        if not isinstance(points, list) or not points:
            raise ValueError(f"{where}: points must be a non-empty list")
        parsed = []
        for p in points:
            ok = (
                isinstance(p, list)
                and len(p) == 2
                and all(
                    isinstance(v, (int, float)) and not isinstance(v, bool) for v in p
                )
            )
            if not ok or not all(math.isfinite(v) for v in p):
                raise ValueError(f"{where}: each point must be [seconds, value]")
            t, v = float(p[0]), float(p[1])
            if t < 0.0:
                raise ValueError(f"{where}: point time must be >= 0")
            if not 0.0 <= v <= 1.0:
                raise ValueError(f"{where}: point value must be in [0, 1]")
            parsed.append((t, v))
        parsed.sort(key=lambda tv: tv[0])  # stable: equal times keep file order
        lanes.append(Lane(node, param, parsed))
    return lanes


def resolve_param(plugin: Any, param: int | str) -> int:
    """The parameter index a lane names. Raises ValueError."""
    n = plugin.num_params
    if isinstance(param, int):
        if param >= n:
            raise ValueError(f"param index {param} out of range (plugin has {n})")
        return param
    # Case-insensitive, first match: as `Plugin.find_param` and `--param`.
    try:
        return plugin.find_param(param)
    except RuntimeError:
        raise ValueError(f"no parameter named {param!r}") from None


def expand(
    points: list[tuple[float, float]], sample_rate: float
) -> list[tuple[int, float]]:
    """(sample, value) changes, sorted by sample."""
    at = [(_llround(t * sample_rate), v) for t, v in points]
    out: list[tuple[int, float]] = []
    if at[0][0] > 0:
        out.append((0, at[0][1]))
    for i, (s0, v0) in enumerate(at):
        if i + 1 == len(at):
            out.append((s0, v0))
            break
        s1, v1 = at[i + 1]
        s = s0
        while s < s1:
            out.append((s, v0 + (v1 - v0) * (s - s0) / (s1 - s0)))
            s += STEP
    return out


def block_changes(
    lanes: list[tuple[int, list[int], list[float]]], start: int, end: int
) -> list[tuple[int, int, float]]:
    """One node's changes in [start, end), offsets local to `start`.

    `lanes` holds (param_index, samples, values) per lane, in file order;
    changes at one offset keep that order.
    """
    out: list[tuple[int, int, float]] = []
    for param, samples, values in lanes:
        lo, hi = bisect_left(samples, start), bisect_left(samples, end)
        out.extend((samples[k] - start, param, values[k]) for k in range(lo, hi))
    out.sort(key=lambda c: c[0])
    return out
