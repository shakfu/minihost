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

from minihost import _core

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

    Results come back at parameter precision (float32), which is what a
    plugin holds: ``get_param`` returns a float and ``set_param`` takes
    one. Snapshots from :func:`capture` therefore round-trip exactly, but
    a hand-written double literal such as ``0.2`` comes back as its
    float32 neighbour.

    The arithmetic itself lives in the C library (``mh_morph_lerp`` and
    ``mh_morph_lerp_per_param``), which this delegates to. It used to be
    written out again here in Python: the same interpolation and the same
    clamping in two languages, with nothing checking that they agreed, so
    either could have drifted unnoticed. The length checks stay in Python
    because the messages name the offending lengths.
    """
    if len(a) != len(b):
        raise ValueError(f"snapshots differ in length ({len(a)} vs {len(b)})")

    if isinstance(t, (int, float)):
        return _core.morph_lerp([float(x) for x in a], [float(x) for x in b], float(t))

    ts = [float(x) for x in t]
    if len(ts) != len(a):
        raise ValueError(
            f"per-parameter t has {len(ts)} values but snapshots have {len(a)}"
        )
    return _core.morph_lerp_per_param([float(x) for x in a], [float(x) for x in b], ts)


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
