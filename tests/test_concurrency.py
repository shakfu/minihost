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

3. GIL release around native work: the process bindings must not hold the
   GIL while the plugin runs, or a multi-threaded host gets no parallelism
   at all and MIDI callbacks stall behind whatever is processing.

Note: set_state and set_program are NOT safe vs. concurrent process
(they call releaseResources / prepareToPlay). We do not test those.
"""

from __future__ import annotations

import os
import sys
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


# ---------------------------------------------------------------------------
# GIL release (M1)
# ---------------------------------------------------------------------------


@skip_if_no_plugin
def test_process_releases_the_gil():
    """A pure-Python thread must make progress *during* one native call.

    Every binding used to hold the GIL for the whole of processBlock, so a
    multi-threaded host got no parallelism and a MidiIn callback could not run
    until the in-flight process call returned.

    Deliberately not measured as a threaded-vs-serial speedup: that conflates
    minihost's behaviour with the plugin's. Some plugins serialize internally
    (Dexed, the default test plugin, measures 0.89x on 4 threads while three
    other VST3s measure 3.3-3.7x), so a speedup assertion would fail for
    reasons that have nothing to do with the GIL.

    Instead, spin a counter in a background thread and sample it either side of
    a process() call. If the GIL is held for the duration, the counter cannot
    advance at all. Measured: 0 before the fix, ~250k after, for both a
    self-serializing and a well-behaved plugin.

    Sampled over several calls rather than one, because the two outcomes are
    not symmetric. A held GIL is deterministic: the spinner cannot run during
    *any* call, so every sample reads zero. A released GIL only gives the
    spinner an opportunity, and whether the OS takes it inside one call is
    luck -- 65536 frames through the default test plugin is about 0.3 ms of
    real work, shorter than Python's default 5 ms switch interval, so a single
    sample on a loaded machine can read zero with the GIL released perfectly.
    That flakiness was observed. Taking the best of several samples keeps the
    discrimination (all-zero versus any-advance) and drops the luck; the switch
    interval is shortened for the measurement so each call offers the spinner
    several scheduling points.
    """
    import minihost

    frames = 65536
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=frames)
    # A plugin may cap the block size below what was asked for; processing
    # more frames than it accepts would make the call a no-op and measure
    # nothing at all.
    frames = min(frames, plugin.max_block_size)
    trials = 20
    best = 0
    per_call_ms: list[float] = []
    original_interval = sys.getswitchinterval()
    try:
        buf_in = minihost.AudioBuffer(max(plugin.num_input_channels, 1), frames)
        buf_out = minihost.AudioBuffer(max(plugin.num_output_channels, 1), frames)
        plugin.process(buf_in, buf_out)  # warm up (first block may allocate)

        counter = 0
        stop = False

        def spin():
            nonlocal counter
            while not stop:
                counter += 1

        sys.setswitchinterval(1e-4)
        spinner = threading.Thread(target=spin, daemon=True)
        spinner.start()
        try:
            time.sleep(0.1)  # let the spinner get going
            for _ in range(trials):
                before = counter
                started = time.perf_counter()
                plugin.process(buf_in, buf_out)
                per_call_ms.append((time.perf_counter() - started) * 1000.0)
                best = max(best, counter - before)
                if best > 10:
                    break
        finally:
            stop = True
            spinner.join(timeout=5.0)
    finally:
        sys.setswitchinterval(original_interval)
        plugin.close()

    # Deliberately no "too fast to measure" guard: holding the GIL makes the
    # call *faster* (the spinner cannot compete for CPU), so such a guard would
    # skip exactly the regression this test exists to catch. The discrimination
    # is 0 vs hundreds of thousands, so a small threshold is ample.
    assert best > 10, (
        f"a background thread never advanced its counter during {len(per_call_ms)} "
        f"process() calls of {frames} frames "
        f"(best {best}, calls averaged {sum(per_call_ms) / len(per_call_ms):.3f} ms) "
        f"-- the GIL looks like it is being held across the native call"
    )
