"""Inputs that were accepted and then silently did the wrong thing.

* Output rows past the plugin's channel count kept their old contents.
* A NaN or inf bus branch gain was stored and turned the output into NaN.
* A NUL byte in a path truncated it: ``Plugin("a\\x00b")`` opened ``a``.
* ``AudioBuffer(-3, 5)`` was clamped to ``(1, 5)``.
"""

from __future__ import annotations

import math
import os

import pytest

import minihost
from cli_helpers import find_test_plugin

np = pytest.importorskip("numpy")

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")

requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX),
    reason="MinihostTestFx not built; set MINIHOST_TEST_PLUGIN_FX or build "
    "with -DMINIHOST_BUILD_TEST_PLUGIN=ON",
)

FRAMES = 64
STALE = 7.0


def _plugin(**kw):
    return minihost.Plugin(FX, sample_rate=48000, max_block_size=512, **kw)


def _ones(rows, dtype=np.float32):
    return np.ones((rows, FRAMES), dtype=dtype)


# -- extra output rows ---------------------------------------------------------


PLUGIN_CALLS = {
    "process": lambda p, i, o: p.process(i, o),
    "process_midi": lambda p, i, o: p.process_midi(i, o, []),
    "process_auto": lambda p, i, o: p.process_auto(i, o, [], []),
    "process_sidechain": lambda p, i, o: p.process_sidechain(i, o, _ones(2)),
    "process_sidechain_midi": lambda p, i, o: p.process_sidechain_midi(
        i, o, _ones(2), []
    ),
}


@requires_fx
@pytest.mark.parametrize("call", PLUGIN_CALLS.values(), ids=PLUGIN_CALLS.keys())
def test_plugin_zeroes_extra_output_rows(call):
    with _plugin(sidechain_channels=2) as p:
        n = p.num_output_channels
        out = np.full((n + 2, FRAMES), STALE, dtype=np.float32)
        call(p, _ones(p.num_input_channels), out)
    np.testing.assert_array_equal(out[:n], 1.0)
    np.testing.assert_array_equal(out[n:], 0.0)


@requires_fx
def test_plugin_process_double_zeroes_extra_output_rows():
    with _plugin() as p:
        n = p.num_output_channels
        out = np.full((n + 2, FRAMES), STALE, dtype=np.float64)
        p.process_double(_ones(p.num_input_channels, np.float64), out)
    np.testing.assert_array_equal(out[n:], 0.0)


CHAIN_CALLS = {
    "process": lambda c, i, o: c.process(i, o),
    "process_midi": lambda c, i, o: c.process_midi(i, o, []),
    "process_auto": lambda c, i, o: c.process_auto(i, o, [], []),
}


@requires_fx
@pytest.mark.parametrize("call", CHAIN_CALLS.values(), ids=CHAIN_CALLS.keys())
def test_chain_zeroes_extra_output_rows(call):
    with _plugin() as a, _plugin() as b:
        chain = minihost.PluginChain([a, b])
        n = chain.num_output_channels
        out = np.full((n + 2, FRAMES), STALE, dtype=np.float32)
        call(chain, _ones(chain.num_input_channels), out)
        chain.close()
    np.testing.assert_array_equal(out[n:], 0.0)


BUS_CALLS = {
    "process": lambda b, i, o: b.process(i, o),
    "process_midi": lambda b, i, o: b.process_midi(i, o, []),
}


@requires_fx
@pytest.mark.parametrize("call", BUS_CALLS.values(), ids=BUS_CALLS.keys())
def test_bus_zeroes_extra_output_rows(call):
    with _plugin() as p:
        chain = minihost.PluginChain([p])
        bus = minihost.PluginBus(2, 2, 512)
        bus.add_branch(chain)
        out = np.full((4, FRAMES), STALE, dtype=np.float32)
        call(bus, _ones(2), out)
        bus.close()
        chain.close()
    np.testing.assert_array_equal(out[:2], 1.0)
    np.testing.assert_array_equal(out[2:], 0.0)


# -- bus branch gain -----------------------------------------------------------


@requires_fx
@pytest.mark.parametrize("bad", [math.nan, math.inf, -math.inf])
def test_bus_rejects_non_finite_gain(bad):
    with _plugin() as p:
        chain = minihost.PluginChain([p])
        bus = minihost.PluginBus(2, 2, 512)
        try:
            with pytest.raises(ValueError, match="gain must be a finite number"):
                bus.add_branch(chain, bad)
            assert bus.num_branches == 0

            bus.add_branch(chain, 0.5)
            with pytest.raises(ValueError, match="gain must be a finite number"):
                bus.set_branch_gain(0, bad)
            assert bus.get_branch_gain(0) == 0.5
        finally:
            bus.close()
            chain.close()


# -- NUL bytes in paths --------------------------------------------------------


@pytest.mark.parametrize(
    "call",
    [
        lambda path: minihost.Plugin(path, sample_rate=48000, max_block_size=512),
        lambda path: minihost.probe(path),
        lambda path: minihost._core.audio_read(path),
        lambda path: minihost.write_audio(path, np.zeros((1, 8), np.float32), 48000),
        lambda path: minihost.MidiFile().load(path),
        lambda path: minihost.MidiFile().save(path),
    ],
    ids=[
        "Plugin",
        "probe",
        "audio_read",
        "write_audio",
        "MidiFile.load",
        "MidiFile.save",
    ],
)
def test_path_with_nul_byte_is_rejected(call, tmp_path):
    # The prefix exists, so truncation would reach (or overwrite) a real file.
    prefix = tmp_path / "real.wav"
    prefix.write_bytes(b"")
    with pytest.raises(ValueError, match="null byte"):
        call(str(prefix) + "\x00suffix.wav")
    assert prefix.read_bytes() == b""


# -- AudioBuffer sizes ---------------------------------------------------------


@pytest.mark.parametrize("cls", [minihost.AudioBuffer, minihost.AudioBufferD])
@pytest.mark.parametrize("shape", [(-3, 5), (2, -1)])
def test_audiobuffer_rejects_negative_sizes(cls, shape):
    with pytest.raises(ValueError, match="must be >= 0"):
        cls(*shape)
