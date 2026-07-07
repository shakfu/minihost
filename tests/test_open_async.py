"""Tests for minihost.open_async (background plugin loading).

open_async loads a plugin on a background thread and returns a Future that
resolves to a real Plugin. Safety comes from the native dedicated plugin
thread (which marshals construction/destruction/affine ops), so the returned
plugin is usable and closable from any thread. The Future mechanics are
exercised with monkeypatched stubs; a final gated test loads a real plugin
end-to-end -- including cross-thread close, which used to deadlock.
"""

import concurrent.futures
import os
import threading
import time

import pytest

import minihost
from minihost import open_async


class _SlowStub:
    """Stand-in for Plugin whose construction blocks, to test Future timing."""

    def __init__(self, path, **kwargs):
        time.sleep(0.2)
        self.path = path
        self.kwargs = kwargs


class _RaisingStub:
    def __init__(self, path, **kwargs):
        raise ValueError(f"cannot load {path}")


def test_open_async_returns_future_immediately(monkeypatch):
    monkeypatch.setattr("minihost._core.Plugin", _SlowStub)
    started = time.monotonic()
    fut = open_async("whatever.vst3")
    assert isinstance(fut, concurrent.futures.Future)
    assert (time.monotonic() - started) < 0.15  # does not wait for the load
    assert not fut.done()
    result = fut.result(timeout=5)
    assert isinstance(result, _SlowStub)
    assert result.path == "whatever.vst3"


def test_open_async_result_timeout_then_resolves(monkeypatch):
    monkeypatch.setattr("minihost._core.Plugin", _SlowStub)
    fut = open_async("whatever.vst3")
    with pytest.raises(concurrent.futures.TimeoutError):
        fut.result(timeout=0.01)
    assert fut.result(timeout=5).path == "whatever.vst3"


def test_open_async_error_path_sets_exception(monkeypatch):
    monkeypatch.setattr("minihost._core.Plugin", _RaisingStub)
    fut = open_async("bad.vst3")
    exc = fut.exception(timeout=5)
    assert isinstance(exc, ValueError)
    with pytest.raises(ValueError, match="cannot load bad.vst3"):
        fut.result()


def test_open_async_error_real_invalid_path():
    fut = open_async("/nonexistent/definitely-not-a.vst3")
    with pytest.raises(Exception):
        fut.result(timeout=30)


def test_open_async_forwards_constructor_kwargs(monkeypatch):
    captured = {}

    class _CapturingStub:
        def __init__(self, path, **kwargs):
            captured["path"] = path
            captured["kwargs"] = kwargs

    monkeypatch.setattr("minihost._core.Plugin", _CapturingStub)
    open_async(
        "p.vst3",
        sample_rate=44100.0,
        max_block_size=256,
        in_channels=1,
        out_channels=2,
        sidechain_channels=1,
    ).result(timeout=5)
    assert captured["path"] == "p.vst3"
    assert captured["kwargs"] == {
        "sample_rate": 44100.0,
        "max_block_size": 256,
        "in_channels": 1,
        "out_channels": 2,
        "sidechain_channels": 1,
    }


def test_open_async_loads_on_background_thread(monkeypatch):
    loader_thread = {}

    class _ThreadRecordingStub:
        def __init__(self, path, **kwargs):
            loader_thread["ident"] = threading.get_ident()

    monkeypatch.setattr("minihost._core.Plugin", _ThreadRecordingStub)
    open_async("p.vst3").result(timeout=5)
    assert loader_thread["ident"] != threading.get_ident()


# ---------------------------------------------------------------------------
# End-to-end with a real plugin: load on the background loader thread, then
# use and close from the main thread. This deadlocked before the native
# dedicated plugin thread; it must work now.
# ---------------------------------------------------------------------------

_PLUGIN = os.environ.get("MINIHOST_TEST_PLUGIN")


@pytest.mark.skipif(not _PLUGIN, reason="MINIHOST_TEST_PLUGIN not set")
@pytest.mark.skipif(
    os.environ.get("MINIHOST_MESSAGE_THREAD") == "0",
    reason="native plugin thread disabled; cross-thread async use is unsafe",
)
def test_open_async_real_plugin_load_use_and_close():
    import numpy as np

    fut = open_async(_PLUGIN, sample_rate=48000, max_block_size=512)
    plugin = fut.result(timeout=30)
    try:
        assert isinstance(plugin, minihost.Plugin)
        assert plugin.num_params >= 0
        # Affine control op from the main thread (was the deadlock).
        if plugin.num_params > 0:
            assert isinstance(plugin.get_param_info(0)["name"], str)
        out_ch = max(plugin.num_output_channels, 2)
        in_ch = max(plugin.num_input_channels, 2)
        inp = np.zeros((in_ch, 256), dtype=np.float32)
        out = np.zeros((out_ch, 256), dtype=np.float32)
        plugin.process(inp, out)
        assert np.all(np.isfinite(out))
    finally:
        plugin.close()  # cross-thread close; previously deadlocked
