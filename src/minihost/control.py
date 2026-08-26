"""MIDI control-surface mapper.

Translate incoming MIDI events from a USB MIDI control surface (knobs,
faders, pads) into plugin parameter changes or user callbacks.
:class:`MidiMapper` is callable, designed to be passed directly as the
callback to :meth:`minihost.MidiIn.open` or
:meth:`minihost.MidiIn.open_virtual`.

Most modern USB MIDI control surfaces (Novation Launch Control, Akai
MIDIMix, Korg nanoKONTROL, Behringer X-Touch, MIDI Fighter Twister,
Arturia BeatStep, etc.) emit standard MIDI CC messages and need no
HID-layer support; they appear as standard MIDI input ports.
"""

from __future__ import annotations

import threading
from typing import Any, Callable, Optional

from minihost._core import Plugin

# Names match common DAW (Bitwig / Live / Reaper) conventions:
#   linear -- straight 1:1 mapping
#   exp    -- more resolution near the low end (sensitive bottom)
#             implemented as v -> v^2 (convex)
#   log    -- more resolution near the high end (sensitive top)
#             implemented as v -> 1 - (1 - v)^2 (concave)
_VALID_CURVES = ("linear", "exp", "log")


class _CCMapping:
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

    def normalize(self, midi_value: int) -> float:
        """Convert a 0..127 CC value through the curve and value_range."""
        n = midi_value / 127.0
        if self.curve == "exp":
            n = n * n
        elif self.curve == "log":
            inv = 1.0 - n
            n = 1.0 - inv * inv
        # else: linear
        lo, hi = self.value_range
        return lo + n * (hi - lo)


class _NoteMapping:
    __slots__ = ("callback",)

    def __init__(self, callback: Callable[[int], None]):
        self.callback = callback


class MidiMapper:
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
        self._plugin = plugin
        self._cc: dict[tuple[int, int], _CCMapping] = {}
        self._note: dict[tuple[int, int], _NoteMapping] = {}
        self._on_unmapped = on_unmapped
        self._device = device
        self._plugin_index = plugin_index
        self._lock = threading.RLock()

    def bind_device(self, device: Optional[Any], plugin_index: int = 0) -> None:
        """Route parameter writes through an :class:`AudioDevice`.

        Without a device, a CC write calls :meth:`Plugin.set_param`, which
        takes the plugin's state mutex and sets the value underneath
        whatever the audio thread is doing -- so the change lands at an
        undefined point, and the MIDI thread can block behind an offline
        caller holding that mutex. With one, the write goes onto the
        device's lock-free control queue and the audio thread applies it
        at the start of the next block through the sample-accurate
        process entry point.

        Bind whenever a device is running. The unbound path remains
        correct for offline use, where there is no audio thread to race.

        Pass ``None`` to unbind. Thread-safe: a single reassignment is
        atomic under the GIL.
        """
        self._plugin_index = plugin_index
        self._device = device

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
        if curve not in _VALID_CURVES:
            raise ValueError(
                f"curve must be one of {list(_VALID_CURVES)}, got {curve!r}"
            )

        # Resolve the parameter name now -- fail fast if it's wrong, before
        # the user opens the MIDI port and starts receiving events.
        param_idx = self._plugin.find_param(param)
        with self._lock:
            self._cc[(channel, cc)] = _CCMapping(
                param_name=param,
                param_idx=param_idx,
                value_range=value_range,
                curve=curve,
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

    def unmap_note(self, channel: int, note: int) -> None:
        """Remove a note mapping. No-op if not currently mapped."""
        with self._lock:
            self._note.pop((channel, note), None)

    def clear(self) -> None:
        """Remove all CC and note mappings."""
        with self._lock:
            self._cc.clear()
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
    def note_mappings(self) -> set[tuple[int, int]]:
        """Set of currently-mapped ``(channel, note)`` pairs."""
        with self._lock:
            return set(self._note.keys())

    # ---- parameter writes ----

    def _write_param(self, param_idx: int, value: float) -> None:
        """Apply one parameter write, through the device when bound.

        A full control queue is dropped rather than raised: this runs on the
        MIDI input thread, where an exception would escape into the callback,
        and the value it carries is superseded by the next turn of the fader
        anyway.
        """
        device = self._device
        if device is None:
            self._plugin.set_param(param_idx, value)
            return
        try:
            device.send_param_control(param_idx, value, self._plugin_index)
        except RuntimeError:
            pass

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
            if cc_map is not None:
                self._write_param(cc_map.param_idx, cc_map.normalize(value))
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
