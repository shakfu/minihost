"""Feedback: plugin parameter values back out to a control surface.

Without this a generated surface is write-only. Load a preset, or move a
control from the plugin's own editor, and every fader on the tablet lies --
it shows where the finger last left it, not where the parameter is.

:class:`OscFeedback` polls the parameters a mapper has bound and sends the
ones that changed. Polling rather than hooking
:meth:`Plugin.set_param_value_callback` is a deliberate choice, and the
reasons are worth stating because the callback looks like the obvious answer:

- **The callback is a single slot.** ``mh_set_param_value_callback`` holds one
  function, and the Python ``Plugin`` binding already occupies it. Taking it
  for feedback would silently break any caller who wanted their own, and
  giving it a multiplexer is machinery in the wrong place.
- **The callback fires on whatever thread changed the parameter**, which
  includes the audio thread -- ``mh_process_auto`` calls
  ``setValueNotifyingHost`` from inside the block. Anything hung off it must
  be lock-free and allocation-free all the way to the socket.
- **A surface cannot use more than about 30 updates a second anyway.** The
  callback's precision is wasted: it would have to be rate-limited back down
  to what polling produces directly.

The cost is that changes faster than the poll interval are not individually
seen -- only the latest value is sent. For a moving fader that is exactly
right, and it is what a rate limiter would have done to the callback stream.
"""

from __future__ import annotations

import threading
from typing import Any, Optional

__all__ = ["OscFeedback"]

#: How often to poll, in seconds. 30 Hz is smooth for a moving fader and
#: cheap: one `get_param` per bound parameter per tick.
DEFAULT_INTERVAL = 1.0 / 30.0

#: How long after a mapper writes a parameter to stay quiet about it.
#:
#: The hazard is a feedback loop with a human in it. The surface sends
#: cutoff=0.5, the host applies it, the poller sees 0.5 and sends it back --
#: harmless when idle, but during a drag the echo arrives a frame behind the
#: finger and fights it. 150 ms is long enough to cover a drag's inter-message
#: gap and short enough that a genuine change arriving just after a touch is
#: not lost for noticeably long.
DEFAULT_SUPPRESS = 0.15

#: Values are 0..1; a change smaller than this is not worth a packet. Chosen
#: below what a 14-bit control can express (1/16383) so no real movement is
#: filtered out.
DEFAULT_EPSILON = 1.0 / 100000.0


class OscFeedback:
    """Send changed parameter values to a surface over OSC.

    Args:
        plugin: The :class:`Plugin` to read from.
        client: An :class:`OscClient` aimed at the surface.
        addresses: ``{address: param_index}`` to watch. Usually
            :meth:`OscMapper.feedback_addresses`, so the same addresses the
            surface sends on are the ones it hears back.
        mapper: Optional mapper whose recent writes should be suppressed, so
            the surface is not sent its own values back mid-drag. Pass the
            mapper receiving from the same surface.
        interval: Poll period in seconds.
        suppress: Seconds to stay quiet about a parameter after the mapper
            wrote it.
        epsilon: Minimum change worth sending.

    Example:
        >>> with minihost.OscClient("192.168.1.40", 9001) as out:
        ...     fb = minihost.OscFeedback(
        ...         plugin, out, mapper.feedback_addresses(), mapper=mapper
        ...     )
        ...     with fb:
        ...         ...  # surface now tracks the plugin
    """

    def __init__(
        self,
        plugin: Any,
        client: Any,
        addresses: dict[str, int],
        mapper: Optional[Any] = None,
        interval: float = DEFAULT_INTERVAL,
        suppress: float = DEFAULT_SUPPRESS,
        epsilon: float = DEFAULT_EPSILON,
    ):
        if interval <= 0:
            raise ValueError(f"interval must be positive, got {interval}")
        if suppress < 0:
            raise ValueError(f"suppress must not be negative, got {suppress}")
        if epsilon < 0:
            raise ValueError(f"epsilon must not be negative, got {epsilon}")

        self._plugin = plugin
        self._client = client
        self._targets = list(addresses.items())
        self._mapper = mapper
        self._interval = interval
        self._suppress = suppress
        self._epsilon = epsilon

        self._last_sent: dict[int, float] = {}
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._sent_count = 0

    # ---- lifecycle ----

    def start(self) -> None:
        """Start polling. No-op if already running."""
        if self._thread is not None:
            return
        self._stop.clear()
        self._thread = threading.Thread(
            target=self._run, name="minihost-osc-feedback", daemon=True
        )
        self._thread.start()

    def stop(self, timeout: float = 2.0) -> None:
        """Stop polling and wait for the thread. No-op if not running."""
        if self._thread is None:
            return
        self._stop.set()
        self._thread.join(timeout=timeout)
        self._thread = None

    def __enter__(self) -> "OscFeedback":
        self.start()
        return self

    def __exit__(self, *args: object) -> None:
        self.stop()

    # ---- introspection ----

    @property
    def sent_count(self) -> int:
        """Messages sent since construction. Useful in tests and diagnostics."""
        return self._sent_count

    @property
    def is_running(self) -> bool:
        return self._thread is not None

    # ---- the poll ----

    def poll_once(self) -> int:
        """Send whatever changed since the last poll. Returns messages sent.

        Public so a caller can drive feedback from their own loop instead of
        this class's thread, and so tests need no timing.
        """
        sent = 0
        for address, param_idx in self._targets:
            if self._mapper is not None and self._mapper.wrote_recently(
                param_idx, self._suppress
            ):
                # Our own echo, or close enough to it. Skip -- but do not
                # record the value, so the change is still sent once the
                # suppression window closes and the surface converges.
                continue

            value = self._plugin.get_param(param_idx)
            previous = self._last_sent.get(param_idx)
            if previous is not None and abs(value - previous) < self._epsilon:
                continue

            try:
                self._client.send(address, float(value))
            except RuntimeError:
                # A closed client, or an address the sender rejects. Neither
                # is worth taking the thread down for; the next tick retries.
                continue

            self._last_sent[param_idx] = value
            sent += 1

        self._sent_count += sent
        return sent

    def _run(self) -> None:
        while not self._stop.wait(self._interval):
            try:
                self.poll_once()
            except Exception:
                # A feedback thread must not be able to take the process down.
                # Every per-message failure is already handled above; this is
                # the backstop for a plugin that raises on get_param.
                pass
