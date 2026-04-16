"""Async plugin loading for minihost."""

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
    """Load a plugin asynchronously in a background thread.

    Returns a ``concurrent.futures.Future`` that resolves to a ``Plugin``
    instance.  Useful for large sample-library plugins that take seconds
    to load.

    Args:
        path: Path to the plugin (.vst3, .component, .lv2).
        sample_rate: Sample rate in Hz.
        max_block_size: Maximum audio block size in samples.
        in_channels: Requested input channels.
        out_channels: Requested output channels.
        sidechain_channels: Sidechain input channels (0 to disable).

    Returns:
        A Future whose ``.result()`` is the loaded ``Plugin``.

    Example::

        future = minihost.open_async("/path/to/heavy_synth.vst3")
        # ... do other work ...
        plugin = future.result()  # blocks until ready
    """
    from minihost._core import Plugin

    future: concurrent.futures.Future = concurrent.futures.Future()

    def _loader() -> None:
        try:
            plugin = Plugin(
                path,
                sample_rate=sample_rate,
                max_block_size=max_block_size,
                in_channels=in_channels,
                out_channels=out_channels,
                sidechain_channels=sidechain_channels,
            )
            future.set_result(plugin)
        except Exception as exc:
            future.set_exception(exc)

    thread = threading.Thread(target=_loader, daemon=True)
    thread.start()

    return future
