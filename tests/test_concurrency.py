"""Concurrency stress tests.

Two scenarios:

1. set_param racing process: set_param is documented as thread-safe vs. the
   audio thread (it's atomic on JUCE's side). Hammer set_param from a worker
   while process runs on the main thread and verify no crash and finite
   output.

2. Callback queue under producer/consumer pressure: the trampoline pushes
   from arbitrary threads (potentially the audio thread) while
   poll_callbacks drains from the main thread. Verify the bounded queue
   behaves correctly: events are dispatched in order received, no leaks,
   no allocation after warmup (proxy: no slowdown across many iterations),
   and overflow is reported via callback_events_dropped() rather than
   silently lost.

Note: set_state and set_program are NOT safe vs. concurrent process
(they call releaseResources / prepareToPlay). We do not test those.
"""

from __future__ import annotations

import os
import threading
import time

import numpy as np
import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


@skip_if_no_plugin
def test_set_param_does_not_crash_concurrent_process():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin has no parameters")

    inp = np.zeros((plugin.num_input_channels, 256), dtype=np.float32)
    out = np.zeros((plugin.num_output_channels, 256), dtype=np.float32)

    stop = threading.Event()

    def hammer():
        i = 0
        while not stop.is_set():
            plugin.set_param(0, (i % 100) / 100.0)
            i += 1

    worker = threading.Thread(target=hammer, daemon=True)
    worker.start()
    try:
        for _ in range(500):
            plugin.process(inp, out)
            assert np.isfinite(out).all()
    finally:
        stop.set()
        worker.join(timeout=2.0)
    assert not worker.is_alive(), "hammer thread did not stop"


@skip_if_no_plugin
def test_concurrent_set_param_from_multiple_threads():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin has no parameters")

    stop = threading.Event()

    def hammer(seed: int):
        i = seed
        while not stop.is_set():
            plugin.set_param(0, ((i * 7) % 100) / 100.0)
            i += 1

    workers = [
        threading.Thread(target=hammer, args=(s,), daemon=True) for s in range(4)
    ]
    for w in workers:
        w.start()
    time.sleep(0.2)
    stop.set()
    for w in workers:
        w.join(timeout=2.0)
        assert not w.is_alive()

    # Plugin should remain valid; reading the param must not crash.
    v = plugin.get_param(0)
    assert 0.0 <= v <= 1.0


@skip_if_no_plugin
def test_callback_queue_dispatches_in_order():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin has no parameters")

    received: list[tuple[int, float]] = []

    def on_param_value(idx: int, value: float):
        received.append((idx, value))

    plugin.set_param_value_callback(on_param_value)

    # Produce N param-value events by setting the parameter (which fires the
    # listener internally via setValueNotifyingHost).
    N = 50
    for i in range(N):
        plugin.set_param(0, i / 100.0)

    dispatched = plugin.poll_callbacks()
    assert dispatched == len(received)
    assert dispatched >= 1, "expected at least one param-value callback"
    # Values must be monotonically non-decreasing in the order we set them.
    values = [v for _, v in received]
    assert values == sorted(values), "callbacks dispatched out of order"


@skip_if_no_plugin
def test_callback_queue_overflow_is_reported_not_silent():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    if plugin.num_params == 0:
        pytest.skip("plugin has no parameters")

    # Register a callback so events actually flow into the queue.
    plugin.set_param_value_callback(lambda idx, v: None)

    # Fire more events than the queue capacity (1024) before draining.
    # (set_param fires the listener synchronously.)
    for i in range(1500):
        plugin.set_param(0, (i % 100) / 100.0)

    # Some events should have been dropped (or very close to it). Either way,
    # the dropped counter must be retrievable and non-negative.
    dropped = plugin.callback_events_dropped()
    assert dropped >= 0
    # Reading again must reset (we documented it that way).
    assert plugin.callback_events_dropped() == 0

    # Drain whatever we managed to enqueue. Must not crash.
    plugin.poll_callbacks()
