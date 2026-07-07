"""Async plugin loading for minihost.

``open_async`` loads a plugin on a background thread and returns a ``Future``
that resolves to a ``Plugin``. This is safe because the native layer owns a
dedicated JUCE plugin thread: construction, destruction, and thread-affine
control operations are marshaled onto it regardless of which thread calls
them, so the returned ``Plugin`` can be built on the loader thread and then
used and closed from any other thread. (Set ``MINIHOST_MESSAGE_THREAD=0`` to
disable that thread; async loading then reverts to being unsafe.)
"""

from __future__ import annotations

import concurrent.futures
import threading


def open_async(
    path: str,
    sample_rate: float = 48000.0,
    max_block_size: int = 512,
    in_channels: int = 2,
    out_channels: int = 2,
    sidechain_channels: int = 0,
) -> concurrent.futures.Future:
    """Load a plugin asynchronously, off the calling thread.

    Returns a ``concurrent.futures.Future`` that resolves to a ``Plugin``.
    The heavy load runs on a background thread so the caller is not blocked;
    the resulting plugin is a normal ``Plugin`` usable from any thread.

    Note that loads are serialized on the native plugin thread, so this gives
    non-blocking (not parallel) loading. Useful for large sample-library
    plugins that take seconds to construct::

        future = minihost.open_async("/path/to/heavy_synth.vst3")
        # ... do other work ...
        plugin = future.result()   # blocks until ready
        plugin.process(in_buf, out_buf)
        plugin.close()

    Args:
        path: Path to the plugin (.vst3, .component, .lv2).
        sample_rate: Sample rate in Hz.
        max_block_size: Maximum audio block size in samples.
        in_channels: Requested input channels.
        out_channels: Requested output channels.
        sidechain_channels: Sidechain input channels (0 to disable).

    Returns:
        A Future whose ``.result()`` is the loaded ``Plugin``.
    """
    from minihost._core import Plugin

    future: concurrent.futures.Future = concurrent.futures.Future()

    def _loader() -> None:
        try:
            future.set_result(
                Plugin(
                    path,
                    sample_rate=sample_rate,
                    max_block_size=max_block_size,
                    in_channels=in_channels,
                    out_channels=out_channels,
                    sidechain_channels=sidechain_channels,
                )
            )
        except Exception as exc:  # noqa: BLE001 - propagated via the Future
            future.set_exception(exc)

    threading.Thread(target=_loader, daemon=True).start()
    return future
