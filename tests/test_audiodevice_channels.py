"""Regression tests: AudioDevice must not index past its conversion buffers.

Pre-fix, MH_AudioDevice allocated its non-interleaved input_buffers /
output_buffers with exactly `dev->channels` channel pointers (the *device's*
channel count), then handed those pointer tables straight to mh_process /
mh_chain_process. Those functions read the plugin's input-channel count and
write its output-channel count, neither of which is bounded by the device's.

Any configuration where the plugin has more channels than the device therefore
walked off the end of a `float**` on the audio thread. The two reachable
routes were:

  * AudioDevice(plugin, output_channels=N) with N below the plugin's count --
    a documented, supported override.
  * The device negotiating fewer channels than requested (mono-only hardware),
    since dev->channels is read back from the *actual* device.

Reproduction pre-fix: AudioDevice(stereo_plugin, output_channels=1).start()
segfaulted the interpreter (SIGSEGV) within one audio callback.

The fix sizes the buffers to max(device channels, plugin inputs, plugin
outputs) and feeds silence to any input channel the device does not supply.

These tests open a real audio device, so they skip cleanly when no plugin or
no audio hardware is available (CI runners typically have neither).
"""

from __future__ import annotations

import os
import time

import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)

# How long to let the audio thread run. The overrun fired on the first
# callback, so this only has to be long enough for one buffer to be served.
RUN_SECONDS = 0.3


def _open_plugin():
    return minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)


def _run_device(plugin, **device_kwargs):
    """Open, start, run briefly, and stop an AudioDevice.

    Returns the device's negotiated channel count, or None if no audio
    hardware is available (the environment simply cannot host the test).
    """
    try:
        device = minihost.AudioDevice(plugin, **device_kwargs)
    except RuntimeError as e:
        pytest.skip(f"no usable audio device: {e}")
        return None
    try:
        device.start()
        time.sleep(RUN_SECONDS)
        device.stop()
    finally:
        del device
    return True


@skip_if_no_plugin
@pytest.mark.parametrize("output_channels", [1, 2, 4])
def test_device_narrower_or_wider_than_plugin_does_not_overrun(output_channels):
    """The heart of the regression: a device channel count that disagrees
    with the plugin's must not corrupt memory. output_channels=1 is the
    case that reliably segfaulted pre-fix for any stereo-output plugin.
    """
    plugin = _open_plugin()
    try:
        assert _run_device(plugin, output_channels=output_channels)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_device_default_channels_still_works():
    """output_channels=0 (the default) means 'use the plugin's count'. This
    path was always safe; it guards against the fix regressing the common case.
    """
    plugin = _open_plugin()
    try:
        assert _run_device(plugin)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_device_reports_its_own_channel_count_not_the_buffer_count():
    """The internal buffers are widened to cover the plugin, but the public
    `channels` property must keep reporting what the *device* exchanges --
    write_input() interleaves against it, so widening it would corrupt the
    input ring buffer's frame layout.
    """
    plugin = _open_plugin()
    try:
        try:
            device = minihost.AudioDevice(plugin, output_channels=1)
        except RuntimeError as e:
            pytest.skip(f"no usable audio device: {e}")
        try:
            assert device.channels == 1
        finally:
            del device
    finally:
        plugin.close()


@skip_if_no_plugin
def test_duplex_capture_with_narrow_device_does_not_overrun():
    """The capture (duplex) branch de-interleaves into the same buffers and
    was equally exposed. Skips if the machine has no input device.
    """
    plugin = _open_plugin()
    try:
        assert _run_device(plugin, output_channels=1, capture=True)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_enabled_input_ring_with_narrow_device_does_not_overrun():
    """enable_input() installs a ring-buffer reader as the input callback.
    It fills only the device's own channels, so any extra channels the plugin
    reads must be zero-filled by the callback rather than left unallocated.
    """
    plugin = _open_plugin()
    try:
        try:
            device = minihost.AudioDevice(plugin, output_channels=1)
        except RuntimeError as e:
            pytest.skip(f"no usable audio device: {e}")
        try:
            device.enable_input()
            device.start()
            time.sleep(RUN_SECONDS)
            device.stop()
            device.disable_input()
        finally:
            del device
    finally:
        plugin.close()
