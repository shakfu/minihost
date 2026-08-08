"""End-to-end proof that sidechain audio reaches the plugin's detector.

`tests/test_sidechain_channels.py` pins the *accounting* (H1): which bus each
count refers to, and that buffers sized by the reported counts are accepted.
That is structural -- it shows minihost writes the sidechain at the channel
offset where JUCE places bus 1, but not that the plugin receives it.

This closes that gap behaviourally, which needs a plugin that (a) exposes a
sidechain bus and (b) audibly acts on it by default once switched to external.
Most do not: several MeldaProduction plugins expose the bus but ignore it
without enabling sidechain modulation in their own UI, and FabFilter Pro-G's
gate sits open at default settings so nothing changes either way. FabFilter
Pro-C 3 does respond, so it is the reference here.

Set MINIHOST_TEST_SIDECHAIN_PLUGIN (plus the two param indices) to use a
different compressor; otherwise these skip.
"""

from __future__ import annotations

import math
import os

import pytest

import minihost

# Path, the "sidechain source" parameter, and the normalised value selecting
# an external sidechain on that parameter.
PLUGIN = os.environ.get(
    "MINIHOST_TEST_SIDECHAIN_PLUGIN",
    "/Library/Audio/Plug-Ins/VST3/FabFilter Pro-C 3.vst3",
)
SC_SOURCE_PARAM = int(os.environ.get("MINIHOST_TEST_SIDECHAIN_PARAM", "21"))
SC_EXTERNAL = float(os.environ.get("MINIHOST_TEST_SIDECHAIN_EXTERNAL", str(1 / 3)))
SC_INTERNAL = 0.0

SR = 48000
N = 2048
F_MAIN = 440.0
F_SIDE = 80.0

skip_if_absent = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=(
        f"sidechain reference plugin not found at {PLUGIN}; set "
        f"MINIHOST_TEST_SIDECHAIN_PLUGIN to a compressor with an external sidechain"
    ),
)


def _render(sc_amplitude: float, source: float) -> float:
    """Output RMS with a 440 Hz main tone and an 80 Hz sidechain tone."""
    plugin = minihost.Plugin(
        PLUGIN, sample_rate=SR, max_block_size=N, sidechain_channels=2
    )
    try:
        if plugin.sidechain_channels <= 0:
            pytest.skip(f"{PLUGIN} exposes no sidechain bus")
        plugin.set_param(SC_SOURCE_PARAM, source)

        main_in = minihost.AudioBuffer(plugin.num_input_channels, N)
        main_out = minihost.AudioBuffer(plugin.num_output_channels, N)
        sidechain = minihost.AudioBuffer(plugin.sidechain_channels, N)
        for ch in range(main_in.channels):
            for i in range(N):
                main_in[ch, i] = 0.3 * math.sin(2 * math.pi * F_MAIN * i / SR)
        for ch in range(sidechain.channels):
            for i in range(N):
                sidechain[ch, i] = sc_amplitude * math.sin(
                    2 * math.pi * F_SIDE * i / SR
                )

        # Several blocks so the level detector settles.
        for _ in range(60):
            plugin.process_sidechain(main_in, main_out, sidechain)
        return (sum(main_out[0, i] ** 2 for i in range(N)) / N) ** 0.5
    finally:
        plugin.close()


@skip_if_absent
def test_external_sidechain_ducks_the_main_signal():
    """The behavioural proof: a loud external sidechain must reduce output.

    If the sidechain landed in scratch channels -- the pre-H1 behaviour, where
    it was written past the channels the plugin reads -- these would be
    identical. Measured -7.3 dB on the reference plugin.
    """
    quiet = _render(0.0, SC_EXTERNAL)
    loud = _render(0.95, SC_EXTERNAL)
    assert quiet > 0, "plugin produced silence; is it licensed?"

    change_db = 20 * math.log10(loud / quiet)
    assert change_db < -1.0, (
        f"a loud external sidechain changed the output by only {change_db:+.2f} dB "
        f"({quiet:.5f} -> {loud:.5f}); the sidechain signal is not reaching the "
        f"plugin's detector"
    )


@skip_if_absent
def test_internal_sidechain_ignores_the_external_bus():
    """Control. With the source set to internal the same external audio must
    make no difference -- otherwise the ducking above could be an artefact of
    the main path rather than evidence about sidechain routing.
    """
    quiet = _render(0.0, SC_INTERNAL)
    loud = _render(0.95, SC_INTERNAL)
    assert quiet > 0

    change_db = 20 * math.log10(loud / quiet)
    assert abs(change_db) < 0.5, (
        f"external sidechain audio changed the output by {change_db:+.2f} dB "
        f"even with the sidechain source set to internal"
    )
