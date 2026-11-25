"""CLAP hosting, against minihost's own CLAP fixtures.

The fixtures (``projects/test_plugin/clap``) share the VST3 fixture's
parameter contract, so the assertions are exact. They need the CLAP builds:
``MINIHOST_TEST_CLAP_FX`` / ``MINIHOST_TEST_CLAP_SYNTH`` if set (CI sets
them), else the newest under ``build*/``.
"""

from __future__ import annotations

import os
import time

import pytest

import minihost

from cli_helpers import find_test_clap

np = pytest.importorskip("numpy")

FX = find_test_clap("MinihostTestFx", "MINIHOST_TEST_CLAP_FX")
SYNTH = find_test_clap("MinihostTestSynth", "MINIHOST_TEST_CLAP_SYNTH")

requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX),
    reason="CLAP MinihostTestFx not built; set MINIHOST_TEST_CLAP_FX or build "
    "with -DMINIHOST_BUILD_TEST_PLUGIN=ON (make fixtures)",
)
requires_synth = pytest.mark.skipif(
    not SYNTH or not os.path.exists(SYNTH),
    reason="CLAP MinihostTestSynth not built; set MINIHOST_TEST_CLAP_SYNTH or "
    "build with -DMINIHOST_BUILD_TEST_PLUGIN=ON (make fixtures)",
)

P_GAIN, P_LATENCY, P_SIDECHAIN, P_SEEN_TEMPO, P_SEEN_FLAGS = range(5)
LATENCY_MAX = 4096
TEMPO_MAX = 1000.0

# clap_transport_flags
HAS_TEMPO, HAS_BEATS, HAS_SECONDS, HAS_TIME_SIG, IS_PLAYING = 1, 2, 4, 8, 16

SR = 48000.0
BLOCK = 512


def _open(path=None, **kw):
    return minihost.Plugin(path or FX, sample_rate=SR, max_block_size=BLOCK, **kw)


def _ramp(channels=2, frames=BLOCK):
    n = np.arange(frames, dtype=np.float32) / frames
    return np.stack([n * (c + 1) * 0.25 for c in range(channels)]).astype(np.float32)


def _await_latency(plugin, want, timeout=5.0):
    """A CLAP plugin reports latency by request_restart; the host restarts
    it on the plugin thread, after the block that asked."""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        plugin.poll_callbacks()
        if plugin.latency_samples == want:
            return True
        time.sleep(0.01)
    return False


# -- discovery ---------------------------------------------------------- #


@requires_fx
def test_probe_reports_the_clap_format_and_main_channels():
    d = minihost.probe(FX)
    assert d["format"] == "CLAP"
    assert d["name"] == "MinihostTestFx"
    assert d["vendor"] == "minihost"
    assert (d["num_inputs"], d["num_outputs"]) == (2, 2)


@requires_synth
def test_probe_marks_the_synth_an_instrument():
    d = minihost.probe(SYNTH)
    assert d["format"] == "CLAP"
    assert (d["num_inputs"], d["num_outputs"]) == (0, 2)
    assert d["accepts_midi"]  # derived from the "instrument" feature


@requires_fx
def test_scan_directory_finds_clap_plugins():
    found = minihost.scan_directory(os.path.dirname(FX))
    clap = {os.path.basename(d["path"]): d["format"] for d in found}
    assert clap.get("MinihostTestFx.clap") == "CLAP"


def test_a_file_that_is_not_a_clap_module_fails_to_open(tmp_path):
    fake = tmp_path / "fake.clap"
    fake.write_bytes(b"not a shared library")
    with pytest.raises(RuntimeError):
        minihost.Plugin(str(fake), sample_rate=SR, max_block_size=BLOCK)


# -- audio -------------------------------------------------------------- #


@requires_fx
def test_unity_gain_is_bit_exact_passthrough():
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
        assert np.array_equal(out, src * np.float32(0.5))
    finally:
        plugin.close()


@requires_fx
def test_automation_lands_on_the_scheduled_sample():
    plugin = _open()
    try:
        src = np.ones((2, BLOCK), dtype=np.float32)
        out = np.zeros_like(src)
        plugin.process_auto(src, out, [], [(100, P_GAIN, 0.5), (300, P_GAIN, 0.25)])
        assert np.all(out[:, :100] == 1.0)
        assert np.all(out[:, 100:300] == 0.5)
        assert np.all(out[:, 300:] == 0.25)
    finally:
        plugin.close()


@requires_fx
def test_sidechain_port_reaches_the_plugin():
    plugin = _open(sidechain_channels=2)
    try:
        assert plugin.sidechain_channels == 2
        plugin.set_param(P_SIDECHAIN, 1.0)
        main = np.zeros((2, BLOCK), dtype=np.float32)
        side = _ramp()
        out = np.zeros_like(main)
        plugin.process_sidechain(main, out, side)
        assert np.array_equal(out, side)
    finally:
        plugin.close()


@requires_fx
def test_sidechain_is_silent_when_not_requested():
    plugin = _open()
    try:
        plugin.set_param(P_SIDECHAIN, 1.0)
        src = _ramp()
        out = np.zeros_like(src)
        plugin.process(src, out)
        assert np.array_equal(out, src)
    finally:
        plugin.close()


# -- latency and restart ------------------------------------------------ #


@requires_fx
def test_latency_is_applied_at_once_and_reported_after_restart():
    plugin = _open()
    try:
        delay = 64
        plugin.set_param(P_LATENCY, delay / LATENCY_MAX)
        src = _ramp()
        out = np.zeros_like(src)
        plugin.process(src, out)

        assert np.all(out[:, :delay] == 0.0)
        assert np.array_equal(out[:, delay:], src[:, : BLOCK - delay])
        assert _await_latency(plugin, delay), plugin.latency_samples
    finally:
        plugin.close()


@requires_fx
def test_parameters_survive_the_restart():
    plugin = _open()
    try:
        plugin.set_param(P_GAIN, 0.5)
        plugin.set_param(P_LATENCY, 64 / LATENCY_MAX)
        silence = np.zeros((2, BLOCK), dtype=np.float32)
        plugin.process(silence, np.zeros_like(silence))
        assert _await_latency(plugin, 64)

        src = np.zeros((2, BLOCK), dtype=np.float32)
        src[:, 0] = 1.0
        out = np.zeros_like(src)
        plugin.process(src, out)
        expected = np.zeros_like(src)
        expected[:, 64] = 0.5
        assert np.array_equal(out, expected)
    finally:
        plugin.close()


@requires_fx
def test_chain_reports_the_clap_latency():
    plugin = _open()
    try:
        chain = minihost.PluginChain([plugin])
        plugin.set_param(P_LATENCY, 100 / LATENCY_MAX)
        silence = np.zeros((2, BLOCK), dtype=np.float32)
        chain.process(silence, np.zeros_like(silence))
        assert _await_latency(plugin, 100)
        assert chain.latency_samples == 100
    finally:
        plugin.close()


@requires_fx
def test_reset_clears_the_delay_line():
    # 600 samples of delay: an impulse at 0 would emerge at 88 of block 2.
    plugin = _open()
    try:
        plugin.set_param(P_LATENCY, 600 / LATENCY_MAX)
        src = np.zeros((2, BLOCK), dtype=np.float32)
        src[:, 0] = 1.0
        plugin.process(src, np.zeros_like(src))
        plugin.reset()
        out = np.zeros_like(src)
        plugin.process(np.zeros_like(src), out)
        assert not out.any()
    finally:
        plugin.close()


@requires_fx
def test_sample_rate_change_reactivates():
    plugin = _open()
    try:
        plugin.sample_rate = 44100.0
        src = _ramp()
        out = np.zeros_like(src)
        plugin.process(src, out)
        assert np.array_equal(out, src)
    finally:
        plugin.close()


# -- parameters --------------------------------------------------------- #


@requires_fx
def test_parameter_info_comes_from_the_plugin():
    plugin = _open()
    try:
        assert plugin.num_params == 5
        info = [plugin.get_param_info(i) for i in range(plugin.num_params)]
        assert [i["name"] for i in info] == [
            "Gain",
            "Latency",
            "Sidechain Mix",
            "Seen Tempo",
            "Seen Transport Flags",
        ]
        assert [i["id"] for i in info] == ["0", "1", "2", "3", "4"]
        assert info[P_GAIN]["default_value"] == 1.0
        assert info[P_LATENCY]["num_steps"] == LATENCY_MAX + 1
        assert info[P_GAIN]["num_steps"] == 0
        assert info[P_GAIN]["is_automatable"]
        assert not info[P_SEEN_TEMPO]["is_automatable"]
    finally:
        plugin.close()


@requires_fx
def test_parameter_text_goes_through_the_plugin():
    plugin = _open()
    try:
        assert plugin.param_to_text(P_LATENCY, 64 / LATENCY_MAX) == "64 smp"
        assert plugin.param_to_text(P_GAIN, 0.25) == "0.25"
        assert plugin.param_from_text(P_GAIN, "0.75") == pytest.approx(0.75)
    finally:
        plugin.close()


@requires_fx
def test_stepped_parameter_snaps_to_an_integer():
    plugin = _open()
    try:
        plugin.set_param(P_LATENCY, 64.4 / LATENCY_MAX)
        assert plugin.get_param(P_LATENCY) * LATENCY_MAX == pytest.approx(64.0)
    finally:
        plugin.close()


@requires_fx
def test_read_only_parameter_ignores_the_host():
    plugin = _open()
    try:
        plugin.set_param(P_SEEN_TEMPO, 0.5)
        assert plugin.get_param(P_SEEN_TEMPO) == 0.0
    finally:
        plugin.close()


@requires_fx
def test_transport_reaches_the_plugin():
    # The fixture echoes the tempo and flags it saw as output parameter
    # events, so this also covers the plugin-to-host parameter path.
    plugin = _open()
    try:
        silence = np.zeros((2, BLOCK), dtype=np.float32)
        plugin.process(silence, np.zeros_like(silence))
        assert plugin.get_param(P_SEEN_TEMPO) == 0.0  # no transport set

        plugin.set_transport(bpm=133.0, position_beats=2.0)
        plugin.process(silence, np.zeros_like(silence))
        assert plugin.get_param(P_SEEN_TEMPO) * TEMPO_MAX == pytest.approx(
            133.0, abs=1e-3
        )
        flags = round(plugin.get_param(P_SEEN_FLAGS) * 255)
        assert flags == HAS_TEMPO | HAS_BEATS | HAS_SECONDS | HAS_TIME_SIG | IS_PLAYING

        plugin.clear_transport()
        plugin.process(silence, np.zeros_like(silence))
        assert plugin.get_param(P_SEEN_FLAGS) == 0.0
    finally:
        plugin.close()


@requires_fx
def test_plugin_parameter_changes_reach_the_value_callback():
    plugin = _open()
    try:
        seen = []
        plugin.set_param_value_callback(
            lambda index, value: seen.append((index, value))
        )
        plugin.set_transport(bpm=120.0)
        silence = np.zeros((2, BLOCK), dtype=np.float32)
        plugin.process(silence, np.zeros_like(silence))
        plugin.poll_callbacks()
        tempos = [v for i, v in seen if i == P_SEEN_TEMPO]
        assert tempos and tempos[-1] * TEMPO_MAX == pytest.approx(120.0, abs=1e-3)
    finally:
        plugin.close()


# -- state -------------------------------------------------------------- #


@requires_fx
def test_state_round_trips_between_instances():
    a, b = _open(), _open()
    try:
        a.set_param(P_GAIN, 0.25)
        a.set_param(P_LATENCY, 32 / LATENCY_MAX)
        b.set_state(a.get_state())
        assert b.get_param(P_GAIN) == pytest.approx(0.25)
        assert b.get_param(P_LATENCY) * LATENCY_MAX == pytest.approx(32.0)
    finally:
        a.close()
        b.close()


@requires_fx
def test_state_includes_a_change_not_yet_processed():
    # set_param only queues the change for the next block; get_state must
    # flush it to the plugin first or the saved state is stale.
    plugin = _open()
    try:
        plugin.set_param(P_GAIN, 0.125)
        other = _open()
        try:
            other.set_state(plugin.get_state())
            assert other.get_param(P_GAIN) == pytest.approx(0.125)
        finally:
            other.close()
    finally:
        plugin.close()


@requires_fx
def test_a_rejected_state_is_reported():
    plugin = _open()
    try:
        with pytest.raises(RuntimeError):
            plugin.set_state(b"not this plugin's state")
        assert plugin.get_param(P_GAIN) == 1.0
    finally:
        plugin.close()


# -- MIDI --------------------------------------------------------------- #


@requires_fx
def test_midi_passes_through_a_midi_dialect_port():
    plugin = _open()
    try:
        src = np.zeros((2, BLOCK), dtype=np.float32)
        sent = [
            (0, 0x90, 60, 100),
            (1, 0xB0, 7, 99),
            (2, 0xE0, 0, 64),
            (3, 0xD0, 40, 0),
            (128, 0x80, 60, 0),
            (400, 0xC0, 5, 0),
        ]
        got = plugin.process_midi(src, np.zeros_like(src), sent)
        assert [tuple(e) for e in got] == sent
    finally:
        plugin.close()


@requires_synth
def test_notes_round_trip_through_the_clap_note_dialect():
    # The synth prefers CLAP note events: note on/off are converted to them
    # and back; everything else stays MIDI.
    plugin = _open(SYNTH)
    try:
        out = np.zeros((2, BLOCK), dtype=np.float32)
        sent = [(0, 0x91, 69, 127), (10, 0xB0, 1, 5), (400, 0x81, 69, 64)]
        got = plugin.process_midi(np.zeros((0, BLOCK), dtype=np.float32), out, sent)
        assert [tuple(e) for e in got] == sent
    finally:
        plugin.close()


@requires_synth
def test_synth_renders_the_note_it_was_sent():
    plugin = _open(SYNTH)
    try:
        out = np.zeros((2, BLOCK), dtype=np.float32)
        on, off = 0, 400
        plugin.process_midi(
            np.zeros((0, BLOCK), dtype=np.float32),
            out,
            [(on, 0x90, 69, 127), (off, 0x80, 69, 0)],
        )
        delta = 2.0 * np.pi * 440.0 / SR
        expected = (0.25 * np.sin(np.arange(off) * delta)).astype(np.float32)
        assert np.allclose(out[0, :off], expected, atol=1e-6)
        assert np.array_equal(out[0], out[1])
        assert not out[:, off:].any()
    finally:
        plugin.close()


# -- module lifetime ---------------------------------------------------- #


@requires_fx
def test_module_outlives_one_of_two_instances():
    a, b = _open(), _open()
    a.close()
    try:
        src = _ramp()
        out = np.zeros_like(src)
        b.process(src, out)
        assert np.array_equal(out, src)
    finally:
        b.close()
    # Last instance gone: the module was deinitialised. It must init again.
    c = _open()
    try:
        out = np.zeros_like(src)
        c.process(src, out)
        assert np.array_equal(out, src)
    finally:
        c.close()
