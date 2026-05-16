"""process_audio in-place mode + minihost.Session."""

from __future__ import annotations

import os

import numpy as np
import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN")
    or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
FX_PLUGIN = (
    os.environ.get("MINIHOST_TEST_FX")
    or "/Library/Audio/Plug-Ins/VST3/TAL-Filter-2.vst3"
)
SCAN_DIR = (
    os.environ.get("MINIHOST_TEST_PLUGIN_DIR")
    or os.path.dirname(FX_PLUGIN)
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"plugin not found at {PLUGIN}",
)
skip_if_no_fx = pytest.mark.skipif(
    not os.path.exists(FX_PLUGIN),
    reason=f"effect plugin not found at {FX_PLUGIN}",
)


# ---------------------------------------------------------------------------
# process_audio in-place mode
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_in_place_returns_same_object_as_input():
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 1024)
    for ch in range(src.channels):
        src[ch, 0] = 0.25
    out = minihost.process_audio(
        plugin, src, compensate_latency=False, in_place=True,
    )
    plugin.close()
    assert out is src


@skip_if_no_fx
def test_in_place_matches_out_of_place_result():
    """Running in-place vs. out-of-place on fresh plugins produces
    identical audio output (the input was snapshotted per block before
    the output write, so aliasing the buffer is safe)."""
    src_data = np.zeros((2, 2048), dtype=np.float32)
    src_data[:, 0] = 0.25
    src_data[:, 512] = -0.25

    # Out-of-place baseline.
    p1 = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src1 = minihost.AudioBuffer.from_numpy(src_data)
    ref = minihost.process_audio(p1, src1, compensate_latency=False)
    p1.close()

    # In-place: mutates the input.
    p2 = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src2 = minihost.AudioBuffer.from_numpy(src_data)
    out2 = minihost.process_audio(
        p2, src2, compensate_latency=False, in_place=True,
    )
    p2.close()

    assert np.allclose(ref.as_ndarray(), out2.as_ndarray(), atol=1e-6)


@skip_if_no_fx
def test_in_place_mutates_input_buffer():
    """The point of in-place: src is the output. After processing,
    src's contents have changed (assuming a non-identity plugin and
    a non-silent input)."""
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 1024)
    for ch in range(src.channels):
        src[ch, 0] = 0.5
    original_first_sample = src[0, 0]
    minihost.process_audio(plugin, src, compensate_latency=False, in_place=True)
    plugin.close()
    # TAL-Filter shouldn't pass the impulse through unchanged.
    assert src[0, 0] != original_first_sample or src[0, 5] != 0.0


@skip_if_no_fx
def test_in_place_rejects_non_audiobuffer():
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = np.zeros((plugin.num_input_channels, 512), dtype=np.float32)
    with pytest.raises(TypeError, match="in_place=True requires"):
        minihost.process_audio(plugin, src, in_place=True)
    plugin.close()


@skip_if_no_fx
def test_in_place_rejects_tail_seconds():
    plugin = minihost.Plugin(FX_PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 1024)
    with pytest.raises(ValueError, match="incompatible with tail_seconds"):
        minihost.process_audio(
            plugin, src, tail_seconds=0.5,
            compensate_latency=False, in_place=True,
        )
    plugin.close()


@skip_if_no_plugin
def test_in_place_rejects_channel_mismatch():
    """A synth (0 inputs declared, 2 outputs) won't satisfy the
    matching-channels requirement when wrapped in an AudioBuffer."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000)
    if plugin.num_input_channels == plugin.num_output_channels:
        plugin.close()
        pytest.skip("plugin has matching I/O; cannot exercise mismatch path")
    src = minihost.AudioBuffer(plugin.num_input_channels, 256)
    with pytest.raises(ValueError, match="matching channel counts"):
        minihost.process_audio(plugin, src, compensate_latency=False,
                                in_place=True)
    plugin.close()


# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------


@skip_if_no_fx
def test_session_constructs_and_closes():
    s = minihost.Session()
    s.close()
    # close is idempotent
    s.close()


@skip_if_no_fx
def test_session_open_returns_plugin():
    s = minihost.Session()
    try:
        p = s.open(FX_PLUGIN, sample_rate=48000)
        assert p.num_input_channels == 2
        assert p.num_output_channels == 2
        p.close()
    finally:
        s.close()


@skip_if_no_fx
def test_session_open_twice_reuses_manager():
    """Loading the same plugin twice through one session should
    succeed (and ideally be faster than two independent loads, but
    we only verify correctness here)."""
    s = minihost.Session()
    try:
        p1 = s.open(FX_PLUGIN, sample_rate=48000)
        p2 = s.open(FX_PLUGIN, sample_rate=48000)
        assert p1.num_params == p2.num_params
        p1.close()
        p2.close()
    finally:
        s.close()


@skip_if_no_fx
def test_session_probe_matches_module_probe():
    s = minihost.Session()
    try:
        session_info = s.probe(FX_PLUGIN)
        module_info = minihost.probe(FX_PLUGIN)
        # Same plugin file -> same metadata.
        for key in ("name", "vendor", "format", "unique_id",
                    "num_inputs", "num_outputs"):
            assert session_info[key] == module_info[key], f"{key} differs"
    finally:
        s.close()


@skip_if_no_fx
def test_session_scan_directory_matches_module_scan():
    s = minihost.Session()
    try:
        session_results = s.scan_directory(SCAN_DIR)
        module_results = minihost.scan_directory(SCAN_DIR)
        # Same directory, same plugins discovered.
        session_paths = sorted(r["path"] for r in session_results)
        module_paths = sorted(r["path"] for r in module_results)
        assert session_paths == module_paths
    finally:
        s.close()


def test_session_probe_bad_path_raises():
    s = minihost.Session()
    try:
        with pytest.raises(RuntimeError, match="Session.probe failed"):
            s.probe("/nonexistent/plugin.vst3")
    finally:
        s.close()


@skip_if_no_fx
def test_session_plugin_outlives_session():
    """The C-side claim: an AudioPluginInstance is self-contained
    post-creation, so closing the session does not invalidate plugins
    it created."""
    s = minihost.Session()
    p = s.open(FX_PLUGIN, sample_rate=48000)
    s.close()
    # Plugin still usable.
    inp = np.zeros((2, 256), dtype=np.float32)
    out = np.zeros((2, 256), dtype=np.float32)
    p.process(inp, out)
    p.close()


@skip_if_no_fx
def test_session_context_manager():
    with minihost.Session() as s:
        info = s.probe(FX_PLUGIN)
        assert info["name"]
    # After context exit close is called; calling again is a no-op.
    s.close()
