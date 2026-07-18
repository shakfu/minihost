"""Tier 1 features: peak normalization, progress callbacks, declarative chains.

Plugin-free unit tests for the bits that don't need a real plugin:
* _normalize_peak helper math
* load_chain spec parsing / validation
* _ProgressBar smoke test

Plugin-backed tests live in test_process_audio.py and pick up the new
kwargs there.
"""

from __future__ import annotations

import json
import math
import os
import sys
import tempfile

import pytest

import minihost
from minihost.process import _normalize_peak


# ---------------------------------------------------------------------------
# _normalize_peak (no plugin required)
# ---------------------------------------------------------------------------


def test_normalize_peak_scales_to_0_dbfs():
    buf = minihost.AudioBuffer(1, 4)
    buf[0, 0] = 0.5
    buf[0, 1] = -0.25
    buf[0, 2] = 0.1
    buf[0, 3] = 0.0
    _normalize_peak(buf, target_dbfs=0.0)
    assert buf.magnitude() == pytest.approx(1.0, rel=1e-5)
    # ratios preserved
    assert buf[0, 1] == pytest.approx(-0.5, rel=1e-5)


def test_normalize_peak_scales_to_negative_dbfs():
    buf = minihost.AudioBuffer(1, 2)
    buf[0, 0] = 0.5
    buf[0, 1] = -0.5
    _normalize_peak(buf, target_dbfs=-6.0)
    expected = 10.0 ** (-6.0 / 20.0)
    assert buf.magnitude() == pytest.approx(expected, rel=1e-4)


def test_normalize_peak_silent_buffer_is_noop():
    buf = minihost.AudioBuffer(2, 16)
    _normalize_peak(buf, target_dbfs=0.0)
    assert buf.magnitude() == 0.0


# ---------------------------------------------------------------------------
# progress_callback hook on process_audio (no plugin needed if we use a
# null-channel-count source... we actually still need a Plugin to call
# process. Skip if unavailable.)
# ---------------------------------------------------------------------------


PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


@skip_if_no_plugin
def test_process_audio_progress_callback_called_monotonically():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    calls: list[tuple[int, int]] = []
    minihost.process_audio(
        plugin,
        src,
        compensate_latency=False,
        progress_callback=lambda c, t: calls.append((c, t)),
    )
    assert calls, "progress_callback should be invoked at least once"
    # monotonic non-decreasing current
    for prev, nxt in zip(calls, calls[1:]):
        assert prev[0] <= nxt[0]
        assert prev[1] == nxt[1]  # total is stable
    # final call hits the total
    assert calls[-1][0] == calls[-1][1] == 4800


@skip_if_no_plugin
def test_process_audio_normalize_brings_peak_to_target():
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    src = minihost.AudioBuffer(plugin.num_input_channels, 4800)
    # Inject some signal so the plugin produces non-zero output. We poke
    # an impulse on every input channel.
    for ch in range(plugin.num_input_channels):
        src[ch, 0] = 0.1
    out = minihost.process_audio(
        plugin,
        src,
        compensate_latency=False,
        normalize=-3.0,
    )
    if out.magnitude() == 0.0:
        pytest.skip("plugin produced silence for this input")
    expected = 10.0 ** (-3.0 / 20.0)
    assert out.magnitude() == pytest.approx(expected, rel=1e-3)


# ---------------------------------------------------------------------------
# load_chain (spec parsing only — exercises file IO + validation paths
# without needing a real plugin)
# ---------------------------------------------------------------------------


def _write(tmp_path, name, content):
    p = tmp_path / name
    p.write_text(content)
    return str(p)


def test_load_chain_rejects_missing_file(tmp_path):
    with pytest.raises(FileNotFoundError):
        minihost.load_chain(str(tmp_path / "nope.json"))


def test_load_chain_rejects_unknown_extension(tmp_path):
    path = _write(tmp_path, "spec.toml", "irrelevant")
    with pytest.raises(ValueError, match="Unsupported chain spec extension"):
        minihost.load_chain(path)


def test_load_chain_rejects_non_mapping_top_level(tmp_path):
    path = _write(tmp_path, "spec.json", "[1, 2, 3]")
    with pytest.raises(ValueError, match="mapping at the top level"):
        minihost.load_chain(path)


def test_load_chain_rejects_empty_plugins_list(tmp_path):
    path = _write(tmp_path, "spec.json", json.dumps({"plugins": []}))
    with pytest.raises(ValueError, match="non-empty 'plugins' list"):
        minihost.load_chain(path)


def test_load_chain_rejects_plugin_without_path(tmp_path):
    spec = {"plugins": [{"params": {}}]}
    path = _write(tmp_path, "spec.json", json.dumps(spec))
    with pytest.raises(ValueError, match="must specify a 'path'"):
        minihost.load_chain(path)


def test_load_chain_rejects_multiple_state_sources(tmp_path):
    spec = {
        "plugins": [
            {"path": "/nonexistent.vst3", "preset": 0, "vstpreset": "x.vstpreset"},
        ]
    }
    path = _write(tmp_path, "spec.json", json.dumps(spec))
    with pytest.raises(ValueError, match="multiple state sources"):
        minihost.load_chain(path)


def test_load_chain_yaml_requires_pyyaml(tmp_path, monkeypatch):
    path = _write(tmp_path, "spec.yaml", "plugins:\n  - path: /x.vst3\n")
    # Force the yaml import inside _load_spec to fail by stripping it
    # from sys.modules and shadowing it.
    monkeypatch.setitem(sys.modules, "yaml", None)
    with pytest.raises(ImportError, match="PyYAML"):
        minihost.load_chain(path)


@skip_if_no_plugin
def test_load_chain_constructs_chain_from_json(tmp_path):
    spec = {
        "sample_rate": 48000,
        "block_size": 512,
        "plugins": [
            {"path": PLUGIN},
            {"path": PLUGIN},
        ],
    }
    path = _write(tmp_path, "spec.json", json.dumps(spec))
    chain = minihost.load_chain(path)
    try:
        assert chain.num_plugins == 2
        # Plugin refs are kept alive on the chain.
        assert hasattr(chain, "_owned_plugins")
        assert len(chain._owned_plugins) == 2
    finally:
        chain.close()


# ---------------------------------------------------------------------------
# _ProgressBar (CLI internal helper; smoke test)
# ---------------------------------------------------------------------------


def test_progress_bar_disabled_writes_nothing(capsys):
    from minihost.cli import _ProgressBar

    bar = _ProgressBar("x", enabled=False)
    bar(50, 100)
    bar.finish()
    err = capsys.readouterr().err
    assert err == ""


def test_progress_bar_enabled_writes_to_stderr(capsys):
    from minihost.cli import _ProgressBar

    bar = _ProgressBar("render", enabled=True)
    bar(0, 100)
    bar(50, 100)
    bar(100, 100)
    bar.finish()
    err = capsys.readouterr().err
    assert "render" in err
    assert "100%" in err
    assert err.endswith("\n")
