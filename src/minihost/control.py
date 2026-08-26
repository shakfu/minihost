"""Control-surface mappers: MIDI and OSC into plugin parameter writes.

Translate incoming control events -- MIDI CC from a USB surface, OSC from a
tablet -- into plugin parameter changes or user callbacks. Both mappers are
callable and are designed to be passed straight to the thing that receives
for them: :class:`MidiMapper` to :meth:`minihost.MidiIn.open`,
:class:`OscMapper` to :meth:`minihost.OscServer.open`.

Most modern USB MIDI control surfaces (Novation Launch Control, Akai
MIDIMix, Korg nanoKONTROL, Behringer X-Touch, MIDI Fighter Twister,
Arturia BeatStep, etc.) emit standard MIDI CC messages and need no
HID-layer support; they appear as standard MIDI input ports.

The two share a resolution core (:class:`_Binding`) that is deliberately
keyed on a **normalized float in 0..1 plus a source identity**, not on CC
numbers or OSC addresses. Each transport converts to that unit at its own
edge -- MIDI divides by 127, OSC passes its float32 through -- which is what
keeps the curve and range logic in one place and makes another transport (Web
MIDI, a WebSocket) an adapter rather than a rewrite. See section 7 of
docs/dev/osc_and_touch.md for why that is worth a little indirection.

Resolution is the visible difference between them. A 7-bit CC gives 128
steps, which is audibly stepped on a filter cutoff; OSC carries float32 and
is not quantized at all.
"""

from __future__ import annotations

import re
import threading
import time
from typing import Any, Callable, Optional

from minihost._core import Plugin, osc_address_matches, osc_is_valid_address

# Names match common DAW (Bitwig / Live / Reaper) conventions:
#   linear -- straight 1:1 mapping
#   exp    -- more resolution near the low end (sensitive bottom)
#             implemented as v -> v^2 (convex)
#   log    -- more resolution near the high end (sensitive top)
#             implemented as v -> 1 - (1 - v)^2 (concave)
_VALID_CURVES = ("linear", "exp", "log")


def _apply_curve(unit: float, curve: str) -> float:
    """Shape a 0..1 value. Shared by every transport, which is the point."""
    if curve == "exp":
        return unit * unit
    if curve == "log":
        inv = 1.0 - unit
        return 1.0 - inv * inv
    return unit


def slug(text: str) -> str:
    """An OSC-safe name for a parameter.

    Mirrors ``py2tosc.surface.slug`` exactly, because a generated layout and
    this mapper have to spell an address the same way or the control is
    silently dead -- nothing is logged at either end. The rules: split on
    runs of alphanumerics, lowercase the first, capitalize the rest, and
    drop everything else (OSC reserves ``#*,?[]{}`` and forbids spaces).

    ``tests/test_osc_mapper.py`` asserts the two implementations agree on a
    corpus of awkward names whenever py2tosc is installed.
    """
    words = re.findall(r"[A-Za-z0-9]+", text)
    if not words:
        return "parameter"
    return words[0].lower() + "".join(word.capitalize() for word in words[1:])


#: The characters that make an OSC address a *pattern* rather than a plain
#: address. Checked before matching so an ordinary unmapped message does not
#: scan the whole table.
_WILDCARD_CHARS = "*?[]{}"


def _has_wildcard(address: str) -> bool:
    return any(c in address for c in _WILDCARD_CHARS)


class _Binding:
    """One resolved parameter target: where it goes and how the value maps.

    Holds a parameter index rather than a name: resolution happens once at
    map time so the hot path is a dict lookup and some arithmetic, and so a
    bad name fails at the call that made the mistake rather than silently on
    the first incoming event.
    """

    __slots__ = ("param_name", "param_idx", "value_range", "curve")

    def __init__(
        self,
        param_name: str,
        param_idx: int,
        value_range: tuple[float, float],
        curve: str,
    ):
        self.param_name = param_name
        self.param_idx = param_idx
        self.value_range = value_range
        self.curve = curve

    def resolve(self, unit: float) -> float:
        """Map a normalized 0..1 input through the curve and value_range.

        The input is already normalized, whatever the transport: this is the
        one function that must not know whether the value arrived as a 7-bit
        CC, a 14-bit pair, or an OSC float.
        """
        if unit < 0.0:
            unit = 0.0
        elif unit > 1.0:
            unit = 1.0
        lo, hi = self.value_range
        return lo + _apply_curve(unit, self.curve) * (hi - lo)


def _validate_curve(curve: str) -> str:
    if curve not in _VALID_CURVES:
        raise ValueError(f"curve must be one of {list(_VALID_CURVES)}, got {curve!r}")
    return curve


def _validate_range(value_range) -> tuple[float, float]:
    if not (isinstance(value_range, (list, tuple)) and len(value_range) == 2):
        raise ValueError(f"value_range must be (lo, hi), got {value_range!r}")
    return (float(value_range[0]), float(value_range[1]))


class _ParamWriter:
    """The half of a mapper that owns a plugin and writes parameters.

    Split out because it is identical for every transport and because the
    choice it encodes -- device ring when one is bound, direct write when not
    -- is the thing that must not be duplicated and drift.
    """

    def __init__(
        self,
        plugin: Plugin,
        device: Optional[Any] = None,
        plugin_index: int = 0,
    ):
        self._plugin = plugin
        self._device = device
        self._plugin_index = plugin_index
        self._lock = threading.RLock()
        # When each parameter was last written *by this mapper*, so the
        # feedback direction can tell its own echo from a genuine change the
        # plugin made. See OscFeedback: sending a value back to the surface
        # that just sent it fights the finger during a drag.
        self._last_write: dict[int, float] = {}

    def bind_device(self, device: Optional[Any], plugin_index: int = 0) -> None:
        """Route parameter writes through an :class:`AudioDevice`.

        Without a device, a write calls :meth:`Plugin.set_param`, which takes
        the plugin's state mutex and sets the value underneath whatever the
        audio thread is doing -- so the change lands at an undefined point,
        and the receiving thread can block behind an offline caller holding
        that mutex. With one, the write goes onto the device's lock-free
        control queue and the audio thread applies it at the start of the
        next block through the sample-accurate process entry point.

        Bind whenever a device is running. The unbound path remains correct
        for offline use, where there is no audio thread to race.

        Pass ``None`` to unbind. Thread-safe: a single reassignment is
        atomic under the GIL.
        """
        self._plugin_index = plugin_index
        self._device = device

    def _write_param(self, param_idx: int, value: float, source: object = None) -> None:
        """Apply one parameter write, through the device when bound.

        ``source`` identifies who originated the write. It is unused today
        and carried anyway, because the feedback direction needs it to avoid
        echoing a value back to the surface that just sent it -- and
        threading it through later would mean changing every call site.

        A full control queue is dropped rather than raised: this runs on a
        receive thread (MIDI input or OSC socket), where an exception would
        escape into a C callback, and the value it carries is superseded by
        the next turn of the fader anyway.
        """
        self._last_write[param_idx] = time.monotonic()

        device = self._device
        if device is None:
            self._plugin.set_param(param_idx, value)
            return
        try:
            device.send_param_control(param_idx, value, self._plugin_index)
        except RuntimeError:
            pass

    def wrote_recently(self, param_idx: int, within: float) -> bool:
        """Did this mapper write ``param_idx`` in the last ``within`` seconds?

        Read by the feedback direction to suppress its own echo. A plain dict
        read of a float; no lock, because a stale answer either way costs one
        skipped or one extra feedback message.
        """
        last = self._last_write.get(param_idx)
        return last is not None and (time.monotonic() - last) < within


class _NoteMapping:
    __slots__ = ("callback",)

    def __init__(self, callback: Callable[[int], None]):
        self.callback = callback


class MidiMapper(_ParamWriter):
    """Map incoming MIDI events to plugin parameter writes or callbacks.

    Designed to be the callback for :meth:`minihost.MidiIn.open` -- the
    instance is callable and dispatches each incoming MIDI message
    according to its mappings.

    Mutations (``map_cc``, ``map_note``, ``unmap_*``, ``clear``) are
    safe to call from another thread while the MIDI callback fires;
    a single internal lock guards all mapping state. The lock hold is
    a few hash-table ops -- negligible relative to MIDI inter-event time.

    Example:
        >>> plugin = minihost.Plugin("/path/to/synth.vst3", sample_rate=48000)
        >>> mapper = minihost.MidiMapper(plugin)
        >>> mapper.map_cc(channel=0, cc=7,  param="Volume")
        >>> mapper.map_cc(channel=0, cc=10, param="Pan",
        ...               value_range=(-1.0, 1.0))
        >>> mapper.map_cc(channel=0, cc=74, param="Cutoff", curve="exp")
        >>>
        >>> # Pad triggers send notes to the plugin via the AudioDevice
        >>> with minihost.AudioDevice(plugin) as audio:
        ...     mapper.map_note(channel=0, note=36,
        ...                     callback=lambda vel: audio.send_midi(0x90, 60, vel))
        ...     with minihost.MidiIn.open(0, mapper):
        ...         input("Press Enter to stop...\\n")
    """

    def __init__(
        self,
        plugin: Plugin,
        on_unmapped: Optional[Callable[[bytes], None]] = None,
        device: Optional[Any] = None,
        plugin_index: int = 0,
    ):
        """Create a mapper bound to a plugin.

        Args:
            plugin: The :class:`Plugin` to receive parameter writes from
                CC mappings. Parameter names are resolved at mapping time
                via :meth:`Plugin.find_param`.
            on_unmapped: Optional callback invoked for any MIDI event
                that isn't matched by a CC or note mapping. Receives the
                raw MIDI bytes. Useful for forwarding non-controller
                events (e.g., keyboard notes from a hybrid controller)
                onward to the plugin via ``audio_device.send_midi``.
            device: Optional :class:`AudioDevice`. When given, parameter
                writes are queued on the device rather than written
                through :meth:`Plugin.set_param`, so they are applied by
                the audio thread at a block boundary instead of racing
                a running ``processBlock``. See :meth:`bind_device`.
            plugin_index: Chain slot the mappings address, when ``device``
                was opened on a :class:`PluginChain`. Ignored otherwise.
        """
        super().__init__(plugin, device=device, plugin_index=plugin_index)
        self._cc: dict[tuple[int, int], _Binding] = {}
        # Keyed on the MSB controller number; the LSB lives at cc + 32.
        self._cc14: dict[tuple[int, int], _Binding] = {}
        # Last MSB and LSB seen per 14-bit pair. Written and read on the MIDI
        # thread; guarded by the same lock as the mappings so a concurrent
        # unmap cannot leave a half-updated entry behind.
        self._cc14_state: dict[tuple[int, int], list[int]] = {}
        self._note: dict[tuple[int, int], _NoteMapping] = {}
        self._on_unmapped = on_unmapped

    # ---- mapping configuration ----

    def map_cc(
        self,
        channel: int,
        cc: int,
        param: str,
        value_range: tuple[float, float] = (0.0, 1.0),
        curve: str = "linear",
    ) -> None:
        """Map a MIDI CC to a plugin parameter.

        Args:
            channel: MIDI channel (0-15).
            cc: CC number (0-127).
            param: Plugin parameter name (case-insensitive lookup via
                :meth:`Plugin.find_param`). Resolved immediately;
                ``ValueError`` if not found.
            value_range: ``(low, high)`` tuple. The 0..127 CC value is
                rescaled into this range. Defaults to ``(0.0, 1.0)``
                which matches the plugin's normalized parameter convention.
            curve: One of ``"linear"`` (default), ``"exp"`` (more
                resolution at low values; useful for filter cutoffs),
                or ``"log"`` (more resolution at high values).

        Raises:
            ValueError: On invalid channel/cc/curve, or if ``param``
                is not a known parameter of the plugin.
        """
        if not (0 <= channel <= 15):
            raise ValueError(f"channel must be 0-15, got {channel}")
        if not (0 <= cc <= 127):
            raise ValueError(f"cc must be 0-127, got {cc}")
        _validate_curve(curve)
        value_range = _validate_range(value_range)

        # Resolve the parameter name now -- fail fast if it's wrong, before
        # the user opens the MIDI port and starts receiving events.
        param_idx = self._plugin.find_param(param)
        with self._lock:
            self._reject_cc_conflict(channel, cc)
            self._cc[(channel, cc)] = _Binding(
                param_name=param,
                param_idx=param_idx,
                value_range=value_range,
                curve=curve,
            )

    def map_cc14(
        self,
        channel: int,
        cc: int,
        param: str,
        value_range: tuple[float, float] = (0.0, 1.0),
        curve: str = "linear",
    ) -> None:
        """Map a 14-bit MIDI CC pair to a plugin parameter.

        A plain CC carries 7 bits, so 128 steps across the parameter's whole
        range. On a filter cutoff that is audibly stepped. The MIDI spec's
        answer is to pair controller ``n`` (0-31), carrying the high 7 bits,
        with controller ``n + 32``, carrying the low 7 -- 16384 steps.

        Args:
            channel: MIDI channel (0-15).
            cc: The **MSB** controller number, 0-31. The LSB is taken from
                ``cc + 32`` automatically; do not map that separately.
            param: Plugin parameter name, resolved immediately.
            value_range: ``(low, high)``, as for :meth:`map_cc`.
            curve: As for :meth:`map_cc`.

        Raises:
            ValueError: On an invalid channel, controller number or curve, or
                if either controller of the pair is already mapped as a plain
                CC. That last check matters: without it a stray ``map_cc`` on
                ``cc + 32`` would shadow the LSB and the fader would move in
                coarse steps with nothing to say why.
        """
        if not (0 <= channel <= 15):
            raise ValueError(f"channel must be 0-15, got {channel}")
        if not (0 <= cc <= 31):
            raise ValueError(
                f"cc must be 0-31 for a 14-bit pair, got {cc}. The MSB "
                f"controller is 0-31 and its LSB is that number plus 32, so "
                f"only the first 32 controllers can be paired."
            )
        _validate_curve(curve)
        value_range = _validate_range(value_range)

        param_idx = self._plugin.find_param(param)
        with self._lock:
            self._reject_cc14_conflict(channel, cc)
            self._cc14[(channel, cc)] = _Binding(
                param_name=param,
                param_idx=param_idx,
                value_range=value_range,
                curve=curve,
            )
            self._cc14_state.setdefault((channel, cc), [-1, 0])

    def _reject_cc_conflict(self, channel: int, cc: int) -> None:
        """Refuse a 7-bit mapping that would shadow half of a 14-bit pair."""
        if (channel, cc) in self._cc14:
            raise ValueError(
                f"cc {cc} on channel {channel} is already the MSB of a 14-bit "
                f"pair; unmap it with unmap_cc14 first"
            )
        if 32 <= cc <= 63 and (channel, cc - 32) in self._cc14:
            raise ValueError(
                f"cc {cc} on channel {channel} is already the LSB of the "
                f"14-bit pair whose MSB is cc {cc - 32}; unmap it with "
                f"unmap_cc14 first"
            )

    def _reject_cc14_conflict(self, channel: int, cc: int) -> None:
        """Refuse a 14-bit pair whose halves are already 7-bit mappings."""
        for occupied, role in ((cc, "MSB"), (cc + 32, "LSB")):
            if (channel, occupied) in self._cc:
                raise ValueError(
                    f"cc {occupied} on channel {channel} is already mapped as "
                    f"a plain CC and would be the {role} of this 14-bit pair; "
                    f"unmap it with unmap_cc first"
                )

    def map_note(
        self,
        channel: int,
        note: int,
        callback: Callable[[int], None],
    ) -> None:
        """Map a MIDI note-on event to a user callback.

        The callback is invoked with the velocity (1-127) when a
        note-on event for ``(channel, note)`` is received. Note-off
        events and zero-velocity note-ons are NOT dispatched (treat
        zero-velocity note-on as note-off, the standard convention).

        Args:
            channel: MIDI channel (0-15).
            note: MIDI note number (0-127).
            callback: Callable receiving the velocity (1-127). Common
                pattern: ``lambda vel: audio.send_midi(0x90, 60, vel)``
                to forward the pad press as a note-on at a different pitch.

        Raises:
            ValueError: On invalid channel or note number.
        """
        if not (0 <= channel <= 15):
            raise ValueError(f"channel must be 0-15, got {channel}")
        if not (0 <= note <= 127):
            raise ValueError(f"note must be 0-127, got {note}")
        with self._lock:
            self._note[(channel, note)] = _NoteMapping(callback=callback)

    def unmap_cc(self, channel: int, cc: int) -> None:
        """Remove a CC mapping. No-op if not currently mapped."""
        with self._lock:
            self._cc.pop((channel, cc), None)

    def unmap_cc14(self, channel: int, cc: int) -> None:
        """Remove a 14-bit mapping by its MSB controller number.

        No-op if not currently mapped.
        """
        with self._lock:
            self._cc14.pop((channel, cc), None)
            self._cc14_state.pop((channel, cc), None)

    def unmap_note(self, channel: int, note: int) -> None:
        """Remove a note mapping. No-op if not currently mapped."""
        with self._lock:
            self._note.pop((channel, note), None)

    def clear(self) -> None:
        """Remove all CC and note mappings."""
        with self._lock:
            self._cc.clear()
            self._cc14.clear()
            self._cc14_state.clear()
            self._note.clear()

    def set_on_unmapped(self, callback: Optional[Callable[[bytes], None]]) -> None:
        """Replace the unmapped-event fallback callback.

        Useful when the forwarding target (e.g. an ``AudioDevice``) isn't
        available until after the mapper has been constructed and
        configured. Pass ``None`` to disable the fallback.

        Thread-safe: a single reassignment is atomic under the GIL.
        """
        self._on_unmapped = callback

    # ---- introspection ----

    @property
    def cc_mappings(self) -> dict[tuple[int, int], str]:
        """Snapshot of CC mappings as ``{(channel, cc): param_name}``."""
        with self._lock:
            return {k: v.param_name for k, v in self._cc.items()}

    @property
    def cc14_mappings(self) -> dict[tuple[int, int], str]:
        """Snapshot of 14-bit mappings as ``{(channel, msb_cc): param_name}``.

        The LSB controller is ``msb_cc + 32`` and is not listed separately.
        """
        with self._lock:
            return {k: v.param_name for k, v in self._cc14.items()}

    @property
    def note_mappings(self) -> set[tuple[int, int]]:
        """Set of currently-mapped ``(channel, note)`` pairs."""
        with self._lock:
            return set(self._note.keys())

    # ---- MidiIn callback interface ----

    def __call__(self, data: bytes) -> None:
        """Dispatch a MIDI event. The signature matches the callback
        contract for :meth:`MidiIn.open` / :meth:`MidiIn.open_virtual`.

        Empty messages, system messages (status >= 0xF0), and short
        messages are silently dropped (after offering them to
        ``on_unmapped`` if configured).
        """
        if not data:
            return
        status = data[0]
        msg_type = status & 0xF0
        channel = status & 0x0F

        # CC: status 0xB0-0xBF, 3 bytes total
        if msg_type == 0xB0 and len(data) >= 3:
            cc, value = data[1], data[2]
            with self._lock:
                cc_map = self._cc.get((channel, cc))
                # A controller is the MSB of a pair, the LSB of one, or a
                # plain CC -- never two of those, because map_cc and map_cc14
                # reject the overlap rather than letting one shadow the other.
                pair_key = None
                if cc_map is None:
                    if (channel, cc) in self._cc14:
                        pair_key, is_msb = (channel, cc), True
                    elif 32 <= cc <= 63 and (channel, cc - 32) in self._cc14:
                        pair_key, is_msb = (channel, cc - 32), False

                unit = None
                if pair_key is not None:
                    state = self._cc14_state.setdefault(pair_key, [-1, 0])
                    if is_msb:
                        state[0] = value
                    else:
                        state[1] = value
                    msb, lsb = state
                    # An LSB before any MSB has no coarse position to refine.
                    # Emitting it alone would read as msb=0 and slam the
                    # parameter to the bottom of its range, so it is held
                    # until an MSB gives it meaning.
                    if msb >= 0:
                        unit = ((msb << 7) | lsb) / 16383.0
                    binding = self._cc14[pair_key]

            if cc_map is not None:
                # The 7-bit-to-unit conversion is the MIDI edge's job; the
                # binding sees a normalized float like every other transport.
                self._write_param(
                    cc_map.param_idx, cc_map.resolve(value / 127.0), source=self
                )
                return

            if pair_key is not None:
                if unit is not None:
                    self._write_param(
                        binding.param_idx, binding.resolve(unit), source=self
                    )
                return

        # Note-on: status 0x90-0x9F, 3 bytes total, velocity > 0
        # (zero-velocity note-on = note-off by convention; not dispatched)
        elif msg_type == 0x90 and len(data) >= 3 and data[2] > 0:
            note, velocity = data[1], data[2]
            with self._lock:
                note_map = self._note.get((channel, note))
            if note_map is not None:
                note_map.callback(velocity)
                return

        if self._on_unmapped is not None:
            self._on_unmapped(data)


class OscMapper(_ParamWriter):
    """Map incoming OSC messages to plugin parameter writes or callbacks.

    Designed to be the callback for :meth:`minihost.OscServer.open` -- the
    instance is callable with the ``(address, args)`` signature that receiver
    delivers.

    The difference from :class:`MidiMapper` that matters is resolution. A CC
    carries 7 bits, so 128 steps; OSC carries float32 and this class does not
    quantize it. On a filter cutoff that gap is audible.

    Two ways to bind. :meth:`map_address` names one address and one
    parameter. :meth:`bind_all` binds every automatable parameter at once
    under a prefix, addressing each by its slugged name -- which is what a
    generated surface wants, and what makes the layout and the host agree
    without a hand-written table.

    Wildcards are handled: an OSC sender may address with ``*``, ``?``,
    ``[a-z]`` or ``{a,b}``, and matching is delegated to JUCE's own
    ``OSCAddressPattern`` rather than reimplemented here, so both ends of a
    connection agree by construction.

    Note that this is the *name-addressed* path, and it runs in Python. For
    plain parameter automation :meth:`AudioDevice.connect_osc` is the better
    tool: it parses ``/mh/param/<index>`` in C and takes neither a lock nor
    the GIL, where every message through this class costs a GIL acquisition.
    Use this when you want names, curves, ranges, or callbacks.

    Example:
        >>> plugin = minihost.Plugin("/path/to/synth.vst3", sample_rate=48000)
        >>> with minihost.AudioDevice(plugin) as audio:
        ...     mapper = minihost.OscMapper(plugin, device=audio)
        ...     mapper.bind_all()                       # /mh/param/<slug>
        ...     mapper.map_address("/fx/mix", "Dry Wet", curve="exp")
        ...     with minihost.OscServer.open(9000, mapper):
        ...         input("Press Enter to stop...\n")
    """

    def __init__(
        self,
        plugin: Plugin,
        device: Optional[Any] = None,
        plugin_index: int = 0,
        on_unmapped: Optional[Callable[[str, list[float]], None]] = None,
    ):
        """Create a mapper bound to a plugin.

        Args:
            plugin: The :class:`Plugin` to receive parameter writes.
            device: Optional :class:`AudioDevice`. When given, writes are
                queued on the device rather than written through
                :meth:`Plugin.set_param`. See :meth:`bind_device`.
            plugin_index: Chain slot the mappings address, when ``device``
                was opened on a :class:`PluginChain`. Ignored otherwise.
            on_unmapped: Optional callback for any message that matches no
                mapping, receiving ``(address, args)``. Useful for transport
                messages and anything else this class does not handle.
        """
        super().__init__(plugin, device=device, plugin_index=plugin_index)
        self._by_address: dict[str, _Binding] = {}
        self._on_unmapped = on_unmapped

    # ---- mapping configuration ----

    def map_address(
        self,
        address: str,
        param: str,
        value_range: tuple[float, float] = (0.0, 1.0),
        curve: str = "linear",
    ) -> None:
        """Map an OSC address to a plugin parameter.

        Args:
            address: The OSC address, e.g. ``/mh/param/cutoff``. Must be a
                valid OSC address pattern; validated here because the
                alternative failure is silent, a control that simply never
                arrives with nothing logged at either end.
            param: Plugin parameter name (case-insensitive lookup via
                :meth:`Plugin.find_param`). Resolved immediately;
                ``ValueError`` if not found.
            value_range: ``(low, high)``. The incoming 0..1 float is rescaled
                into this range. Defaults to ``(0.0, 1.0)``, which matches the
                plugin's own normalized convention.
            curve: One of ``"linear"`` (default), ``"exp"`` (more resolution
                at low values; useful for filter cutoffs), or ``"log"``.

        Raises:
            ValueError: On an invalid address, range or curve, or if ``param``
                is not a known parameter of the plugin.
        """
        if not osc_is_valid_address(address):
            raise ValueError(
                f"{address!r} is not a valid OSC address; it must begin with "
                "'/' and contain no spaces or the reserved characters #*,?[]{}"
            )
        _validate_curve(curve)
        value_range = _validate_range(value_range)

        param_idx = self._plugin.find_param(param)
        with self._lock:
            self._by_address[address] = _Binding(
                param_name=param,
                param_idx=param_idx,
                value_range=value_range,
                curve=curve,
            )

    def bind_all(
        self,
        prefix: str = "/mh/param",
        automatable_only: bool = True,
        curve: str = "linear",
        numeric: bool = True,
    ) -> int:
        """Bind every parameter at once, addressed by slugged name.

        This is the counterpart to a generated surface: both sides derive the
        address from the parameter name by the same rule, so no table has to
        be written down or kept in step.

        Duplicate names are numbered (``bypass``, ``bypass2``, ...) exactly as
        ``py2tosc.surface.unique`` does, because real plugins repeat names --
        a compressor with three parameters called "Bypass" is ordinary -- and
        two parameters sharing one address would make the second unreachable.

        Args:
            prefix: Address prefix. Each parameter is bound at
                ``<prefix>/<slug>``.
            automatable_only: Skip parameters the plugin reports as not
                automatable. These are usually meters and readouts, which a
                surface should not be writing to.
            curve: Curve applied to every binding.
            numeric: Also bind ``<prefix>/<index>`` for each parameter, so
                one port accepts the same numeric addressing that
                :meth:`AudioDevice.connect_osc` parses natively. On by
                default because the alternative is a surprise: a sender that
                works against ``connect_osc`` would silently do nothing here.

        Returns:
            The number of parameters bound (not the number of addresses,
            which is twice that when ``numeric`` is set).
        """
        _validate_curve(curve)
        prefix = prefix.rstrip("/")

        seen: dict[str, int] = {}
        bound = 0
        for index in range(self._plugin.num_params):
            info = self._plugin.get_param_info(index)
            if automatable_only and not info.get("is_automatable", True):
                continue

            name = slug(info["name"])
            seen[name] = seen.get(name, 0) + 1
            if seen[name] > 1:
                name = f"{name}{seen[name]}"

            addresses = [f"{prefix}/{name}"]
            if numeric:
                addresses.append(f"{prefix}/{index}")

            added = False
            for address in addresses:
                if not osc_is_valid_address(address):
                    continue
                with self._lock:
                    self._by_address[address] = _Binding(
                        param_name=info["name"],
                        param_idx=index,
                        value_range=(0.0, 1.0),
                        curve=curve,
                    )
                added = True
            if added:
                bound += 1
        return bound

    def unmap_address(self, address: str) -> None:
        """Remove an address mapping. No-op if not currently mapped."""
        with self._lock:
            self._by_address.pop(address, None)

    def clear(self) -> None:
        """Remove all address mappings."""
        with self._lock:
            self._by_address.clear()

    def set_on_unmapped(
        self, callback: Optional[Callable[[str, list[float]], None]]
    ) -> None:
        """Replace the unmapped-message fallback. Pass ``None`` to disable."""
        self._on_unmapped = callback

    # ---- introspection ----

    @property
    def addresses(self) -> dict[str, str]:
        """Snapshot of mappings as ``{address: param_name}``."""
        with self._lock:
            return {k: v.param_name for k, v in self._by_address.items()}

    def feedback_addresses(self) -> dict[str, int]:
        """One address per bound parameter, for the feedback direction.

        :meth:`bind_all` binds each parameter twice by default, by name and
        by index. Echoing both would double the traffic to no benefit, so the
        name form wins and the numeric one is dropped -- a surface built from
        a generated layout is listening on the names.
        """
        with self._lock:
            chosen: dict[int, str] = {}
            for address, binding in self._by_address.items():
                tail = address.rsplit("/", 1)[-1]
                existing = chosen.get(binding.param_idx)
                if existing is None or (
                    tail.isdigit() is False and existing != address
                ):
                    # Prefer a non-numeric tail; take the first otherwise.
                    if existing is None or existing.rsplit("/", 1)[-1].isdigit():
                        chosen[binding.param_idx] = address
        return {address: idx for idx, address in chosen.items()}

    # ---- OscServer callback interface ----

    def __call__(self, address: str, args: list[float]) -> None:
        """Dispatch one OSC message.

        The signature matches the callback contract for
        :meth:`OscServer.open`. A message with no arguments, or one matching
        no mapping, is offered to ``on_unmapped`` if configured and otherwise
        dropped.
        """
        if args:
            with self._lock:
                binding = self._by_address.get(address)

            # Exact hit is the common case and costs one dict lookup. Only a
            # miss pays for pattern matching, and only when the sender
            # actually used a wildcard -- an ordinary unmapped address does
            # not scan the table.
            if binding is not None:
                self._write_param(
                    binding.param_idx, binding.resolve(args[0]), source=self
                )
                return

            if _has_wildcard(address):
                with self._lock:
                    matched = [
                        b
                        for bound, b in self._by_address.items()
                        if osc_address_matches(address, bound)
                    ]
                if matched:
                    # A pattern legitimately addresses many parameters at
                    # once -- "/mh/param/*" to reset a page, say -- so every
                    # match is written, not just the first.
                    for binding in matched:
                        self._write_param(
                            binding.param_idx, binding.resolve(args[0]), source=self
                        )
                    return

        if self._on_unmapped is not None:
            self._on_unmapped(address, args)
