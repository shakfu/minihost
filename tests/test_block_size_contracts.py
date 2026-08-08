"""Block-size and sample-rate contracts across device, chain and graph.

Three findings (H3 / H4 / M10 in REVIEW.md) shared one root cause: the layers
that own the block size had no way to ask a plugin what it could handle, so
none of them validated, and every mismatch surfaced as a per-block failure with
an unhelpful message -- or no message at all.

  * `PluginChain` hard-coded `max_block_size = 8192`, advertising a ceiling no
    member could honour. A caller sizing blocks against it passed the Python
    shape check and then failed inside `mh_process` with "Chain process failed",
    naming neither the real limit nor the plugin imposing it.
  * `AudioDevice` never compared the device period to the plugin's limit. Every
    `mh_process` returned 0, the return value was ignored, and because the
    output buffers are allocated once and never cleared the device replayed the
    previous block forever -- a buzz, with no error anywhere.
  * `PluginGraph` documented that plugin nodes must match its sample rate and
    block size but checked neither: a rate mismatch rendered silently at the
    wrong rate.

`mh_get_max_block_size` (C ABI 2.3.0, additive) is the shared primitive; these
tests pin the behaviour each layer now derives from it.
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


def _open(max_block_size=512, sample_rate=48000):
    return minihost.Plugin(
        PLUGIN, sample_rate=sample_rate, max_block_size=max_block_size
    )


# --- the shared primitive --------------------------------------------- #


@skip_if_no_plugin
def test_plugin_reports_its_max_block_size():
    plugin = _open(max_block_size=256)
    try:
        assert plugin.max_block_size == 256
    finally:
        plugin.close()


# --- H4: chain derives its limit from its members --------------------- #


@skip_if_no_plugin
def test_chain_max_block_size_is_the_minimum_across_plugins():
    a = _open(max_block_size=512)
    b = _open(max_block_size=256)
    try:
        chain = minihost.PluginChain([a, b])
        assert chain.max_block_size == 256, (
            "chain must advertise a ceiling every member can honour"
        )
    finally:
        a.close()
        b.close()


@skip_if_no_plugin
def test_chain_rejects_an_oversized_block_up_front():
    """Pre-fix this passed validation (against the hard-coded 8192) and then
    failed deep inside with a message naming neither limit nor plugin.
    """
    a = _open(max_block_size=256)
    b = _open(max_block_size=256)
    try:
        chain = minihost.PluginChain([a, b])
        inp = minihost.AudioBuffer(chain.num_input_channels or 1, 2048)
        out = minihost.AudioBuffer(chain.num_output_channels or 1, 2048)
        with pytest.raises(RuntimeError, match="256"):
            chain.process(inp, out)
    finally:
        a.close()
        b.close()


@skip_if_no_plugin
def test_chain_accepts_a_block_at_its_limit():
    a = _open(max_block_size=256)
    b = _open(max_block_size=512)
    try:
        chain = minihost.PluginChain([a, b])
        n = chain.max_block_size
        inp = minihost.AudioBuffer(chain.num_input_channels or 1, n)
        out = minihost.AudioBuffer(chain.num_output_channels or 1, n)
        chain.process(inp, out)  # must not raise
    finally:
        a.close()
        b.close()


# --- M10: graph validates its documented preconditions ---------------- #


@skip_if_no_plugin
def test_graph_rejects_a_plugin_with_too_small_a_block_size():
    plugin = _open(max_block_size=256)
    try:
        graph = minihost.PluginGraph(max_block_size=1024, sample_rate=48000)
        with pytest.raises(RuntimeError, match="max_block_size"):
            graph.add_plugin(plugin)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_graph_rejects_a_plugin_at_the_wrong_sample_rate():
    """Pre-fix this rendered silently at the wrong rate."""
    plugin = _open(max_block_size=512, sample_rate=48000)
    try:
        graph = minihost.PluginGraph(max_block_size=512, sample_rate=44100)
        with pytest.raises(RuntimeError, match="sample rate"):
            graph.add_plugin(plugin)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_graph_accepts_a_matching_plugin():
    plugin = _open(max_block_size=512, sample_rate=48000)
    try:
        graph = minihost.PluginGraph(max_block_size=512, sample_rate=48000)
        assert graph.add_plugin(plugin) >= 0
    finally:
        plugin.close()


# --- H3: audio device validates against the plugin -------------------- #


@skip_if_no_plugin
def test_audio_device_refuses_a_plugin_that_cannot_span_the_device_period():
    """Pre-fix the device opened happily and then played a repeating buzz,
    because every process call was refused and the stale output buffer was
    re-interleaved each callback.
    """
    plugin = _open(max_block_size=64)
    try:
        with pytest.raises(RuntimeError) as excinfo:
            minihost.AudioDevice(plugin, buffer_frames=1024)
        message = str(excinfo.value)
        # The message must name both numbers and what to change.
        assert "64" in message
        assert "1024" in message
        assert "max_block_size" in message
    finally:
        plugin.close()


@skip_if_no_plugin
def test_audio_device_accepts_a_plugin_sized_to_the_device_period():
    """The common case -- plugin and device sized alike -- must keep working.

    The conversion buffers carry 2x headroom internally; validating against
    that rather than the actual period would reject this.
    """
    plugin = _open(max_block_size=512)
    try:
        try:
            device = minihost.AudioDevice(plugin, buffer_frames=512)
        except RuntimeError as e:
            pytest.skip(f"no usable audio device: {e}")
        try:
            device.start()
            time.sleep(0.2)
            device.stop()
        finally:
            del device
    finally:
        plugin.close()
