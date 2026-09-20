"""Signal-level tests against minihost's own deterministic plugin.

These are the checks the suite could not previously make: every assertion
here is an exact value, because the fixture (``projects/test_plugin``) is
specified to produce one. They need the effect build, pointed at by
``MINIHOST_TEST_PLUGIN_FX``; CI builds it and sets the variable.

Parameter indices are the fixture's contract -- see TestPluginProcessor.h.
"""

from __future__ import annotations

import os
import time

import pytest

import minihost

np = pytest.importorskip("numpy")

FX = os.environ.get("MINIHOST_TEST_PLUGIN_FX")

requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX),
    reason="set MINIHOST_TEST_PLUGIN_FX to the MinihostTestFx build",
)

P_GAIN, P_LATENCY, P_SIDECHAIN = 0, 1, 2

SR = 48000.0
BLOCK = 512


def _open(**kw):
    return minihost.Plugin(FX, sample_rate=SR, max_block_size=BLOCK, **kw)


def _ramp(channels=2, frames=BLOCK):
    """Distinct, non-repeating values per channel, so a misrouted or
    duplicated channel cannot pass by coincidence."""
    n = np.arange(frames, dtype=np.float32) / frames
    return np.stack([n * (c + 1) * 0.25 for c in range(channels)]).astype(np.float32)


@requires_fx
def test_unity_gain_is_bit_exact_passthrough():
    """No tolerance: anything the host does to the samples on the way in or
    out shows up here, and nothing legitimately should."""
    plugin = _open()
    try:
        src = _ramp()
        out = np.zeros_like(src)
        plugin.process(src, out)
        assert np.array_equal(out, src)
    finally:
        plugin.close()


@requires_fx
def test_gain_parameter_scales_exactly():
    plugin = _open()
    try:
        plugin.set_param(P_GAIN, 0.5)
        src = _ramp()
        out = np.zeros_like(src)
        plugin.process(src, out)
        assert np.allclose(out, src * 0.5, atol=0, rtol=1e-6)
    finally:
        plugin.close()


@requires_fx
def test_automation_takes_effect_at_the_sample_it_is_scheduled_for():
    """The point of ``process_auto``: a change at offset k applies from k,
    not at the block boundary either side of it."""
    plugin = _open()
    try:
        plugin.set_param(P_GAIN, 1.0)
        src = np.ones((2, BLOCK), dtype=np.float32)
        out = np.zeros_like(src)
        k = 128
        plugin.process_auto(src, out, [], [(k, P_GAIN, 0.25)])

        assert np.allclose(out[:, :k], 1.0, atol=1e-6)
        assert np.allclose(out[:, k:], 0.25, atol=1e-6)
    finally:
        plugin.close()


@requires_fx
def test_two_automation_points_in_one_block_both_land():
    plugin = _open()
    try:
        plugin.set_param(P_GAIN, 1.0)
        src = np.ones((2, BLOCK), dtype=np.float32)
        out = np.zeros_like(src)
        plugin.process_auto(src, out, [], [(100, P_GAIN, 0.5), (300, P_GAIN, 0.25)])

        assert np.allclose(out[:, :100], 1.0, atol=1e-6)
        assert np.allclose(out[:, 100:300], 0.5, atol=1e-6)
        assert np.allclose(out[:, 300:], 0.25, atol=1e-6)
    finally:
        plugin.close()


# The fixture's latency parameter is an integer 0..kMaxLatencySamples,
# exposed normalised like every VST3 parameter.
_LATENCY_MAX = 4096


@requires_fx
def test_latency_parameter_delays_the_signal_by_exactly_that_many_samples():
    plugin = _open()
    try:
        delay = 64
        plugin.set_param(P_LATENCY, delay / _LATENCY_MAX)

        src = _ramp(frames=BLOCK)
        out = np.zeros_like(src)
        plugin.process(src, out)

        # First `delay` samples are the delay line's zeroed initial state;
        # the rest is the input, shifted.
        assert np.allclose(out[:, :delay], 0.0, atol=0)
        assert np.allclose(out[:, delay:], src[:, : BLOCK - delay], atol=0)
    finally:
        plugin.close()


def _await_latency(plugin, want, timeout=5.0):
    """Wait for a latency change the plugin reported to reach the host.

    A VST3 reports one by calling restartComponent, which JUCE's host side
    defers to the message thread, so it arrives after the block that caused
    it rather than during. `poll_callbacks` is what delivers it on macOS,
    where JUCE binds its queue to the main run loop; on Linux and Windows
    the plugin thread has already done it and the call is a no-op.
    """
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        plugin.poll_callbacks()
        if plugin.latency_samples == want:
            return True
        time.sleep(0.01)
    return False


@requires_fx
def test_reported_latency_matches_the_delay_actually_applied():
    """A host that trusts `latency_samples` without the plugin honouring it
    compensates by the wrong amount, so the two must agree."""
    plugin = _open()
    try:
        delay = 256
        plugin.set_param(P_LATENCY, delay / _LATENCY_MAX)

        src = _ramp(frames=BLOCK)
        out = np.zeros_like(src)
        plugin.process(src, out)  # the block that applies it and reports it

        assert _await_latency(plugin, delay), (
            f"plugin reported latency never reached the host "
            f"(still {plugin.latency_samples}, wanted {delay})"
        )
        assert np.allclose(out[:, :delay], 0.0, atol=0)
        assert np.allclose(out[:, delay:], src[:, : BLOCK - delay], atol=0)
    finally:
        plugin.close()


@requires_fx
def test_sidechain_bus_reaches_the_plugin():
    """A silent main input and a non-silent sidechain: whatever comes out is
    the sidechain, so a sidechain that never arrives is unmistakable."""
    plugin = _open(sidechain_channels=2)
    try:
        if plugin.sidechain_channels != 2:
            pytest.skip("sidechain bus not enabled on this build")
        plugin.set_param(P_SIDECHAIN, 1.0)

        main = np.zeros((2, BLOCK), dtype=np.float32)
        side = _ramp()
        out = np.zeros_like(main)
        plugin.process_sidechain(main, out, side)

        assert np.allclose(out, side, atol=1e-6)
    finally:
        plugin.close()


@requires_fx
def test_midi_passes_through_with_its_offsets_intact():
    plugin = _open()
    try:
        src = np.zeros((2, BLOCK), dtype=np.float32)
        out = np.zeros_like(src)
        sent = [(0, 0x90, 60, 100), (128, 0x80, 60, 0), (400, 0x90, 67, 64)]
        got = plugin.process_midi(src, out, sent)

        assert [tuple(e) for e in got] == sent
    finally:
        plugin.close()
