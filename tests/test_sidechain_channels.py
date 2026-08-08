"""Sidechain channel accounting.

`p->inCh` was set from JUCE's `getTotalNumInputChannels()`, which already sums
every enabled input bus -- including the sidechain. `p->sidechainCh` then
counted the sidechain a second time, so for a compressor opened as
`mh_open_ex(main_in=2, sidechain=2)`:

  * `MH_Info.num_input_ch` reported 4, not 2, so callers had to hand
    `process_sidechain` a 4-channel "main" buffer *plus* a redundant 2-channel
    sidechain buffer;
  * `mh_process_sidechain` wrote the sidechain at channels [inCh, inCh+scCh) =
    [4, 6), while the plugin's sidechain bus lives at [2, 4). The sidechain
    signal landed in scratch channels the plugin never reads, and the real
    sidechain bus received whatever happened to be in `main_in[2..3]`.

The fix splits the count in two: `mainInCh` (bus 0 -- what the caller supplies
and what `num_input_ch` reports) and `totalInCh` (every input channel, used
only to size the internal buffer). C ABI 2.4.0 documents the changed meaning.

Most of this needs a plugin with an actual sidechain bus. `_sidechain_plugin`
skips when the configured test plugin has none -- note that
`Plugin(..., sidechain_channels=2)` succeeds regardless, reporting
`sidechain_channels == 0`, so "it opened" is not evidence of a sidechain.
"""

from __future__ import annotations

import os

import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


def _sidechain_plugin():
    plugin = minihost.Plugin(
        PLUGIN, sample_rate=48000, max_block_size=512, sidechain_channels=2
    )
    if plugin.sidechain_channels <= 0:
        plugin.close()
        pytest.skip(
            f"{PLUGIN} has no sidechain bus; set MINIHOST_TEST_PLUGIN to a "
            f"plugin that does (e.g. a compressor) to exercise these"
        )
    return plugin


# --- the invariant that was violated ---------------------------------- #


@skip_if_no_plugin
def test_num_input_channels_is_the_main_bus_only():
    """The core regression. Pre-fix this reported main + sidechain."""
    plugin = _sidechain_plugin()
    try:
        main_bus = plugin.get_bus_info(True, 0)["num_channels"]
        assert plugin.num_input_channels == main_bus, (
            f"num_input_channels ({plugin.num_input_channels}) must be the main "
            f"bus width ({main_bus}), not the sum over all input buses"
        )
    finally:
        plugin.close()


@skip_if_no_plugin
def test_sidechain_channels_reports_the_sidechain_bus():
    plugin = _sidechain_plugin()
    try:
        assert plugin.num_input_buses >= 2
        assert plugin.sidechain_channels == plugin.get_bus_info(True, 1)["num_channels"]
    finally:
        plugin.close()


@skip_if_no_plugin
def test_main_and_sidechain_counts_are_disjoint():
    """Their sum should be the plugin's real total input width -- neither
    double-counted nor overlapping.
    """
    plugin = _sidechain_plugin()
    try:
        total = sum(
            plugin.get_bus_info(True, i)["num_channels"]
            for i in range(plugin.num_input_buses)
        )
        assert plugin.num_input_channels + plugin.sidechain_channels <= total
        assert plugin.num_input_channels < total, (
            "main input width must be strictly less than the total once a "
            "sidechain bus is enabled"
        )
    finally:
        plugin.close()


# --- what callers actually pass --------------------------------------- #


@skip_if_no_plugin
def test_process_sidechain_accepts_buffers_sized_by_the_reported_counts():
    """Pre-fix, main_in sized to num_input_channels was rejected with
    "Main input has 2 channel(s) but plugin requires at least 4".
    """
    plugin = _sidechain_plugin()
    try:
        n = 256
        main_in = minihost.AudioBuffer(plugin.num_input_channels, n)
        main_out = minihost.AudioBuffer(plugin.num_output_channels, n)
        sidechain = minihost.AudioBuffer(plugin.sidechain_channels, n)
        plugin.process_sidechain(main_in, main_out, sidechain)  # must not raise
    finally:
        plugin.close()


@skip_if_no_plugin
def test_plain_process_takes_only_the_main_input_width():
    """A sidechain-configured plugin must still process through the normal
    path with a main-width input buffer; the sidechain bus is fed silence.
    """
    plugin = _sidechain_plugin()
    try:
        n = 256
        buf_in = minihost.AudioBuffer(plugin.num_input_channels, n)
        buf_out = minihost.AudioBuffer(plugin.num_output_channels, n)
        plugin.process(buf_in, buf_out)  # must not raise
    finally:
        plugin.close()


@skip_if_no_plugin
def test_opening_without_sidechain_reports_none():
    """enableAllBuses() turns on a sidechain bus even when none was requested,
    which is exactly how the total-vs-main confusion leaked into plugins the
    caller never asked to sidechain. num_input_channels must still be the main
    bus, and sidechain_channels must be 0.
    """
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    try:
        assert plugin.sidechain_channels == 0
        if plugin.num_input_buses > 0:
            assert (
                plugin.num_input_channels
                == plugin.get_bus_info(True, 0)["num_channels"]
            )
    finally:
        plugin.close()
