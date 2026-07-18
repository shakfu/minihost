"""Parameter preset morphing (A/B interpolation) for minihost.

A *snapshot* is a plain list of normalized parameter values (each in
``[0, 1]``), one entry per plugin parameter, as produced by :func:`capture`.
:func:`lerp` linearly interpolates two snapshots so you can blend or sweep
between two presets along a single control -- useful for sound-design
exploration and for automating a whole patch from one macro.

Morphing operates on the *normalized per-parameter values*, not on opaque
VST/AU state blobs (``get_state`` / ``set_state``): those are not
interpolatable, so a meaningful A/B morph must go through the parameters.

Only continuous parameters interpolate sensibly. Stepped / boolean / enum
parameters will pass through intermediate normalized values during a morph;
the host plugin quantizes them, so the audible result may jump rather than
glide. Pass ``indices`` to :func:`lerp` (via ``t`` as a per-parameter
sequence) or simply exclude such parameters from the snapshots you morph if
that matters for your use.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, List, Sequence, Union

if TYPE_CHECKING:
    from minihost._core import Plugin

Snapshot = List[float]

# t may be a single blend amount applied to every parameter, or a
# per-parameter sequence of blend amounts (same length as the snapshots).
Blend = Union[float, Sequence[float]]


def _clamp01(v: float) -> float:
    if v < 0.0:
        return 0.0
    if v > 1.0:
        return 1.0
    return v


def capture(plugin: "Plugin") -> Snapshot:
    """Return a snapshot of every parameter's current normalized value."""
    return [plugin.get_param(i) for i in range(plugin.num_params)]


def apply(plugin: "Plugin", snapshot: Sequence[float]) -> None:
    """Set every parameter from ``snapshot`` (values clamped to [0, 1]).

    Raises ``ValueError`` if the snapshot length does not match the plugin's
    parameter count.
    """
    n = plugin.num_params
    if len(snapshot) != n:
        raise ValueError(
            f"snapshot has {len(snapshot)} values but plugin has {n} parameters"
        )
    for i, v in enumerate(snapshot):
        plugin.set_param(i, _clamp01(float(v)))


def lerp(a: Sequence[float], b: Sequence[float], t: Blend) -> Snapshot:
    """Linearly interpolate two snapshots: ``a + (b - a) * t``.

    ``t`` is either a scalar blend amount applied to all parameters, or a
    per-parameter sequence (same length as ``a`` and ``b``). ``t = 0`` returns
    ``a``, ``t = 1`` returns ``b``. Results are clamped to ``[0, 1]`` so that
    extrapolated ``t`` (outside ``[0, 1]``) still yields valid normalized
    values. Raises ``ValueError`` on length mismatch.
    """
    if len(a) != len(b):
        raise ValueError(f"snapshots differ in length ({len(a)} vs {len(b)})")
    if isinstance(t, (int, float)):
        tf = float(t)
        return [_clamp01(x + (y - x) * tf) for x, y in zip(a, b)]

    ts = list(t)
    if len(ts) != len(a):
        raise ValueError(
            f"per-parameter t has {len(ts)} values but snapshots have {len(a)}"
        )
    return [_clamp01(x + (y - x) * float(ti)) for x, y, ti in zip(a, b, ts)]


def morph(
    plugin: "Plugin", a: Sequence[float], b: Sequence[float], t: Blend
) -> Snapshot:
    """Interpolate snapshots ``a`` and ``b`` at ``t`` and apply to ``plugin``.

    Convenience wrapper over :func:`lerp` + :func:`apply`. Returns the applied
    (clamped) snapshot so callers can inspect or reuse it.
    """
    result = lerp(a, b, t)
    apply(plugin, result)
    return result
