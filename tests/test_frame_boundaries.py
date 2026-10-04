"""Frame-count edges for every Plugin process method: 0, max_block_size, and
one past it. Uses the FX fixture at unity gain, so a full block must come
back bit-exact."""

from __future__ import annotations

import pytest

from cli_helpers import find_test_plugin

import minihost

np = pytest.importorskip("numpy")

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")

pytestmark = pytest.mark.skipif(
    not FX,
    reason="MinihostTestFx not built; set MINIHOST_TEST_PLUGIN_FX or "
    "build with -DMINIHOST_BUILD_TEST_PLUGIN=ON",
)

BLOCK = 256


def _call(plugin, method, n):
    """Run `method` over n frames of a ramp; return (input, output)."""
    dtype = np.float64 if method == "process_double" else np.float32
    src = (np.arange(2 * n, dtype=dtype).reshape(2, n) + 1) / (2 * n + 1)
    out = np.zeros_like(src)
    side = np.zeros((2, n), dtype=np.float32)
    if method == "process_midi":
        plugin.process_midi(src, out, [])
    elif method == "process_auto":
        plugin.process_auto(src, out, [], [])
    elif method == "process_sidechain":
        plugin.process_sidechain(src, out, side)
    elif method == "process_sidechain_midi":
        plugin.process_sidechain_midi(src, out, side, [])
    else:
        getattr(plugin, method)(src, out)
    if method == "process_double" and not plugin.supports_double:
        # A float-only plugin is processed through float32.
        src = src.astype(np.float32).astype(np.float64)
    return src, out


METHODS = [
    "process",
    "process_midi",
    "process_auto",
    "process_sidechain",
    "process_sidechain_midi",
    "process_double",
]


@pytest.fixture
def fx():
    plugin = minihost.Plugin(
        FX, sample_rate=48000, max_block_size=BLOCK, sidechain_channels=2
    )
    yield plugin
    plugin.close()


@pytest.mark.parametrize("method", METHODS)
def test_zero_frames_is_a_no_op(fx, method):
    src, out = _call(fx, method, 0)
    assert out.shape == (2, 0)
    # The plugin still works afterwards.
    src, out = _call(fx, method, BLOCK)
    assert np.array_equal(out, src)


@pytest.mark.parametrize("method", METHODS)
def test_exactly_max_block_size_is_processed_in_full(fx, method):
    src, out = _call(fx, method, BLOCK)
    assert np.array_equal(out, src)


@pytest.mark.parametrize("method", METHODS)
def test_one_past_max_block_size_is_rejected(fx, method):
    with pytest.raises((ValueError, RuntimeError)):
        _call(fx, method, BLOCK + 1)
