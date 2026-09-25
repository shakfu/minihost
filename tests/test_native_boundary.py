"""Validation at the Python -> native boundary, against the exact FX fixture.

An audit of the bindings found callers' tuples and values reaching the C
layer unchecked:

  * A short automation tuple (``(100, 0)``) segfaulted: the tuple was indexed
    past its end. Reachable from the public ``process_audio``.
  * NaN parameter values reached the plugin: ``jlimit`` passes NaN through.
  * Unsorted automation was applied late, and unsorted MIDI was dropped once
    automation split the block.
  * MIDI past the block end was clamped without automation, dropped with it.
  * Out-of-range parameter and chain-slot indices were ignored without error.
  * MIDI processor params and ``AudioDevice.send_midi`` bytes were not
    range-checked; ``midi_out_capacity`` had no upper bound.

The fixture's gain parameter is linear (0.5 halves the signal) and it passes
MIDI through unchanged, so each assertion is an exact value.
"""

from __future__ import annotations

import math
import os

import pytest

import minihost
from cli_helpers import find_test_plugin
from device_helpers import skip_if_no_audio_device

np = pytest.importorskip("numpy")

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")

requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX),
    reason="MinihostTestFx not built; set MINIHOST_TEST_PLUGIN_FX or build "
    "with -DMINIHOST_BUILD_TEST_PLUGIN=ON",
)

P_GAIN = 0
SR = 48000.0
BLOCK = 512
NAN = float("nan")


@pytest.fixture
def fx():
    plugin = minihost.Plugin(FX, sample_rate=SR, max_block_size=BLOCK)
    plugin.set_param(P_GAIN, 1.0)
    yield plugin
    plugin.close()


def _ones():
    return np.ones((2, BLOCK), dtype=np.float32)


def _graph(plugin):
    g = minihost.PluginGraph(BLOCK, SR)
    a = g.add_input(2)
    node = g.add_plugin(plugin)
    o = g.add_output(2)
    g.connect(a, node)
    g.connect(node, o)
    g.compile()
    return g, node


# -------------------------------------------------------------------- #
# Tuple shape: raised, not a segfault                                   #
# -------------------------------------------------------------------- #


@requires_fx
@pytest.mark.parametrize("entry", [(100, 0), (100,), (), (100, 0, 0.5, 1)])
def test_plugin_automation_tuple_of_wrong_length_raises(fx, entry):
    # A chain-shaped 4-tuple used to be read as (offset, plugin, param).
    src = _ones()
    with pytest.raises(ValueError, match="automation entry must be a tuple"):
        fx.process_auto(src, np.zeros_like(src), [], [entry])


@requires_fx
def test_chain_automation_tuple_of_wrong_length_raises(fx):
    chain = minihost.PluginChain([fx])
    src = _ones()
    with pytest.raises(ValueError, match="automation entry must be a tuple"):
        chain.process_auto(src, np.zeros_like(src), [], [(0, 0, 0)])


@requires_fx
def test_process_audio_short_automation_tuple_raises(fx):
    with pytest.raises(ValueError, match="automation entry"):
        minihost.process_audio(fx, _ones(), param_changes=[(100, 0)])


# -------------------------------------------------------------------- #
# Indices                                                               #
# -------------------------------------------------------------------- #


@requires_fx
def test_plugin_automation_bad_param_index_raises(fx):
    src = _ones()
    with pytest.raises(ValueError, match="param_index"):
        fx.process_auto(src, np.zeros_like(src), [], [(0, fx.num_params, 0.5)])


@requires_fx
def test_chain_automation_bad_plugin_index_raises(fx):
    chain = minihost.PluginChain([fx])
    src = _ones()
    with pytest.raises(ValueError, match="plugin_index"):
        chain.process_auto(src, np.zeros_like(src), [], [(0, 1, 0, 0.5)])


@requires_fx
def test_graph_automation_bad_param_index_raises(fx):
    g, node = _graph(fx)
    with pytest.raises(ValueError, match="param_index"):
        g.set_node_automation(node, [(0, 999, 0.5)])


@requires_fx
def test_graph_automation_on_non_plugin_node_raises(fx):
    g, _ = _graph(fx)
    with pytest.raises(RuntimeError, match="bad node id"):
        g.set_node_automation(0, [(0, 0, 0.5)])  # node 0 is the audio input


# -------------------------------------------------------------------- #
# Non-finite values                                                     #
# -------------------------------------------------------------------- #


@requires_fx
@pytest.mark.parametrize("value", [NAN, math.inf, -math.inf])
def test_set_param_rejects_non_finite(fx, value):
    with pytest.raises(ValueError, match="finite"):
        fx.set_param(P_GAIN, value)
    assert fx.get_param(P_GAIN) == 1.0


@requires_fx
def test_set_param_by_name_rejects_nan(fx):
    name = fx.get_param_info(P_GAIN)["name"]
    with pytest.raises(ValueError, match="finite"):
        fx.set_param_by_name(name, NAN)


@requires_fx
def test_automation_rejects_nan(fx):
    src = _ones()
    out = np.zeros_like(src)
    with pytest.raises(ValueError, match="finite"):
        fx.process_auto(src, out, [], [(0, P_GAIN, NAN)])
    g, node = _graph(fx)
    with pytest.raises(ValueError, match="finite"):
        g.set_node_automation(node, [(0, P_GAIN, NAN)])


@requires_fx
def test_morph_rejects_nan(fx):
    snap = fx.morph_capture()
    bad = list(snap)
    bad[P_GAIN] = NAN
    with pytest.raises(ValueError, match="finite"):
        fx.morph_apply(bad)
    with pytest.raises(ValueError, match="finite"):
        fx.morph(snap, snap, NAN)
    assert fx.get_param(P_GAIN) == 1.0  # nothing half-applied


def test_morph_lerp_rejects_nan():
    with pytest.raises(ValueError, match="non-finite"):
        minihost.morph.lerp([0.0, NAN], [1.0, 1.0], 0.5)
    with pytest.raises(ValueError, match="non-finite"):
        minihost.morph.lerp([0.0], [1.0], [NAN])


@requires_fx
def test_param_to_text_rejects_nan(fx):
    with pytest.raises(ValueError, match="finite"):
        fx.param_to_text(P_GAIN, NAN)


# -------------------------------------------------------------------- #
# Ordering and offsets under automation                                 #
# -------------------------------------------------------------------- #


@requires_fx
def test_unsorted_automation_lands_at_its_offsets(fx):
    src = _ones()
    out = np.zeros_like(src)
    fx.process_auto(src, out, [], [(300, P_GAIN, 0.25), (100, P_GAIN, 0.5)])
    assert np.allclose(out[:, :100], 1.0, atol=1e-6)
    assert np.allclose(out[:, 100:300], 0.5, atol=1e-6)
    assert np.allclose(out[:, 300:], 0.25, atol=1e-6)


@requires_fx
def test_unsorted_graph_automation_lands_at_its_offsets(fx):
    g, node = _graph(fx)
    src = _ones()
    out = np.zeros_like(src)
    g.set_node_automation(node, [(300, P_GAIN, 0.25), (100, P_GAIN, 0.5)])
    g.render_block([src], [out], BLOCK)
    assert np.allclose(out[:, 100:300], 0.5, atol=1e-6)
    assert np.allclose(out[:, 300:], 0.25, atol=1e-6)


@requires_fx
def test_unsorted_midi_survives_automation(fx):
    src = _ones()
    events = [(300, 0x90, 60, 100), (100, 0x90, 62, 100)]
    got = fx.process_auto(src, np.zeros_like(src), events, [(200, P_GAIN, 1.0)])
    assert sorted(got) == sorted(events)


@requires_fx
def test_unsorted_chain_midi_survives_automation(fx):
    chain = minihost.PluginChain([fx])
    src = _ones()
    events = [(300, 0x90, 60, 100), (100, 0x90, 62, 100)]
    got = chain.process_auto(src, np.zeros_like(src), events, [(200, 0, P_GAIN, 1.0)])
    assert sorted(got) == sorted(events)


@requires_fx
def test_midi_past_block_end_is_clamped_with_or_without_automation(fx):
    src = _ones()
    events = [(BLOCK, 0x90, 60, 100), (BLOCK + 100, 0x90, 61, 100)]
    want = [(BLOCK - 1, 0x90, 60, 100), (BLOCK - 1, 0x90, 61, 100)]
    plain = fx.process_midi(src, np.zeros_like(src), events)
    auto = fx.process_auto(src, np.zeros_like(src), events, [(10, P_GAIN, 1.0)])
    assert plain == want
    assert auto == want


# -------------------------------------------------------------------- #
# Capacity and processor params                                         #
# -------------------------------------------------------------------- #


@requires_fx
@pytest.mark.parametrize("cap", [0, 65537, 2**30])
def test_midi_out_capacity_is_bounded(fx, cap):
    src = _ones()
    with pytest.raises(ValueError, match="midi_out_capacity"):
        fx.process_auto(src, np.zeros_like(src), [], [], midi_out_capacity=cap)


@pytest.mark.parametrize(
    "params, match",
    [
        (dict(op=0, min_note=200), "min_note"),
        (dict(op=0, min_note=72, max_note=60), "min_note"),
        (dict(op=0, channel_mask=-1), "channel_mask"),
        (dict(op=1, transpose_semitones=128), "transpose_semitones"),
        (dict(op=1, transpose_semitones=2**40), "transpose_semitones"),
        (dict(op=2, velocity_gamma=NAN), "velocity_gamma"),
        (dict(op=2, velocity_gamma=0.0), "velocity_gamma"),
        (dict(op=3), "MH_MidiOp"),
    ],
)
def test_midi_processor_params_are_range_checked(params, match):
    g = minihost.PluginGraph(64, SR)
    with pytest.raises((RuntimeError, ValueError), match=match):
        g.add_midi_processor(params)


def test_set_midi_processor_params_rejects_what_add_rejects():
    g = minihost.PluginGraph(64, SR)
    node = g.add_midi_processor(dict(op=1, transpose_semitones=12))
    with pytest.raises(RuntimeError, match="out of range"):
        g.set_midi_processor_params(node, dict(op=0, min_note=200))


# -------------------------------------------------------------------- #
# AudioDevice: checked on the caller's thread, not dropped on the       #
# audio thread                                                          #
# -------------------------------------------------------------------- #


@pytest.fixture
def device(fx):
    try:
        dev = minihost.AudioDevice(fx)
    except RuntimeError as e:
        pytest.skip(f"no usable audio device: {e}")
    yield dev
    del dev


@requires_fx
@skip_if_no_audio_device
@pytest.mark.parametrize(
    "msg, match",
    [
        ((0x190, 60, 100), "status"),
        ((0x40, 60, 100), "status"),
        ((0x90, 128, 100), "data1"),
        ((0x90, 60, -1), "data2"),
    ],
)
def test_send_midi_is_range_checked(device, msg, match):
    # static_cast<unsigned char> sent 0x190 as 0x90.
    with pytest.raises(ValueError, match=match):
        device.send_midi(*msg)


@requires_fx
@skip_if_no_audio_device
def test_send_param_checks_target_and_value(device, fx):
    with pytest.raises(ValueError, match="param_index"):
        device.send_param(fx.num_params, 0.5)
    with pytest.raises(ValueError, match="plugin_index"):
        device.send_param(P_GAIN, 0.5, plugin_index=1)
    with pytest.raises(ValueError, match="finite"):
        device.send_param_control(P_GAIN, NAN)
    device.send_param(P_GAIN, 0.5)  # a valid target still queues


# -------------------------------------------------------------------- #
# Control surfaces run on a receive thread: they drop, not raise        #
# -------------------------------------------------------------------- #


@requires_fx
def test_mapper_drops_non_finite_writes(fx):
    mapper = minihost.MidiMapper(fx)
    mapper._write_param(P_GAIN, NAN)
    mapper._write_param(P_GAIN, math.inf)
    assert fx.get_param(P_GAIN) == 1.0
    assert not mapper.wrote_recently(P_GAIN, within=60.0)


# -------------------------------------------------------------------- #
# Open arguments                                                        #
# -------------------------------------------------------------------- #


@requires_fx
@pytest.mark.parametrize(
    "sample_rate, max_block_size, match",
    [
        (NAN, 512, "sample_rate"),
        (0.0, 512, "sample_rate"),
        (-48000.0, 512, "sample_rate"),
        (math.inf, 512, "sample_rate"),
        (SR, 0, "max_block_size"),
        (SR, -1, "max_block_size"),
        (SR, 2**31 - 1, "max_block_size"),
    ],
)
def test_open_rejects_invalid_rate_and_block_size(sample_rate, max_block_size, match):
    # A rate of 0 or NaN opened and rendered NaN; -48000 rendered; block size
    # 0 opened unusable, and 2**31-1 reserved about 34 GB.
    with pytest.raises(RuntimeError, match=match):
        minihost.Plugin(FX, sample_rate=sample_rate, max_block_size=max_block_size)


@requires_fx
def test_sample_rate_setter_rejects_inf(fx):
    with pytest.raises(RuntimeError):
        fx.sample_rate = math.inf
    assert fx.sample_rate == SR


# -------------------------------------------------------------------- #
# One instance, two positions                                          #
# -------------------------------------------------------------------- #
#
# A plugin placed twice in one container is processed twice per block, so
# its state advances twice: PluginChain([p, p]) with a 100-sample delay
# reported 200 samples but differed from two real instances by up to 1.51.


@pytest.fixture
def fx2():
    plugin = minihost.Plugin(FX, sample_rate=SR, max_block_size=BLOCK)
    yield plugin
    plugin.close()


@requires_fx
def test_chain_rejects_the_same_plugin_twice(fx, fx2):
    with pytest.raises(RuntimeError, match="same instance"):
        minihost.PluginChain([fx, fx2, fx])
    assert minihost.PluginChain([fx, fx2]).num_plugins == 2


@requires_fx
def test_bus_rejects_a_repeated_chain_or_shared_plugin(fx, fx2):
    bus = minihost.PluginBus(2, 2, BLOCK, SR)
    chain = minihost.PluginChain([fx])
    bus.add_branch(chain, 1.0)
    with pytest.raises(RuntimeError, match="already a branch"):
        bus.add_branch(chain, 1.0)
    with pytest.raises(RuntimeError, match="already in another branch"):
        bus.add_branch(minihost.PluginChain([fx2, fx]), 1.0)
    bus.add_branch(minihost.PluginChain([fx2]), 1.0)
    assert bus.num_branches == 2


@requires_fx
def test_graph_rejects_the_same_plugin_twice(fx):
    g = minihost.PluginGraph(BLOCK, SR)
    g.add_plugin(fx)
    with pytest.raises(RuntimeError, match="already a node"):
        g.add_plugin(fx)


@requires_fx
def test_load_vstpreset_refuses_another_plugins_preset(tmp_path, fx):
    # A preset for another plugin loaded without complaint, writing foreign
    # state into this one.
    from minihost.vstpreset import load_vstpreset, save_vstpreset

    synth_path = find_test_plugin("MinihostTestSynth", "MINIHOST_TEST_PLUGIN_SYNTH")
    if not synth_path:
        pytest.skip("MinihostTestSynth not built")
    synth = minihost.Plugin(
        synth_path, sample_rate=SR, max_block_size=BLOCK, in_channels=0
    )
    try:
        path = tmp_path / "fx.vstpreset"
        save_vstpreset(path, fx)
        load_vstpreset(path, fx)  # its own preset still loads
        with pytest.raises(ValueError, match="is for class"):
            load_vstpreset(path, synth)
    finally:
        synth.close()


# -------------------------------------------------------------------- #
# Bus and chain                                                         #
# -------------------------------------------------------------------- #


@requires_fx
def test_muted_branch_still_receives_midi():
    # Gain 0 skipped the branch entirely: a note-off sent while muted never
    # arrived, and the note rang on once unmuted.
    synth_path = find_test_plugin("MinihostTestSynth", "MINIHOST_TEST_PLUGIN_SYNTH")
    if not synth_path:
        pytest.skip("MinihostTestSynth not built")
    synth = minihost.Plugin(
        synth_path, sample_rate=SR, max_block_size=BLOCK, in_channels=0
    )
    try:
        chain = minihost.PluginChain([synth])
        bus = minihost.PluginBus(
            chain.num_input_channels, chain.num_output_channels, BLOCK, SR
        )
        bus.add_branch(chain, 1.0)
        x = np.zeros((max(1, chain.num_input_channels), BLOCK), np.float32)
        y = np.zeros((chain.num_output_channels, BLOCK), np.float32)
        bus.process_midi(x, y, [(0, 0x90, 60, 100)])
        assert np.abs(y).max() > 0.01
        bus.set_branch_gain(0, 0.0)
        bus.process_midi(x, y, [(0, 0x80, 60, 0)])
        assert np.abs(y).max() == 0.0  # muted: not summed
        bus.set_branch_gain(0, 1.0)
        for _ in range(20):
            bus.process_midi(x, y, [])
        assert np.abs(y).max() < 1e-6  # the note-off arrived
    finally:
        synth.close()


@requires_fx
def test_bus_rejects_a_branch_with_a_smaller_block(fx):
    bus = minihost.PluginBus(2, 2, BLOCK * 2, SR)
    with pytest.raises(RuntimeError, match="max block size"):
        bus.add_branch(minihost.PluginChain([fx]), 1.0)


@requires_fx
def test_sample_rate_cannot_change_under_a_container(fx):
    # The chain cached the rate it checked; a change underneath went unseen.
    chain = minihost.PluginChain([fx])
    with pytest.raises(RuntimeError, match="in use by a chain"):
        fx.sample_rate = 96000.0
    chain.close()
    fx.sample_rate = 96000.0  # free again once the chain lets go
    assert fx.sample_rate == 96000.0


@requires_fx
def test_automation_offset_past_the_block_is_rejected(fx):
    # The loop stopped at the block end, so such a change was never applied.
    src = _ones()
    with pytest.raises(ValueError, match="sample_offset"):
        fx.process_auto(src, np.zeros_like(src), [], [(BLOCK, P_GAIN, 0.25)])
    chain = minihost.PluginChain([fx])
    with pytest.raises(ValueError, match="sample_offset"):
        chain.process_auto(src, np.zeros_like(src), [], [(BLOCK, 0, P_GAIN, 0.25)])


@requires_fx
def test_graph_automation_past_the_block_applies_next_block(fx):
    g, node = _graph(fx)
    src = _ones()
    out = np.zeros_like(src)
    g.set_node_automation(node, [(BLOCK + 10, P_GAIN, 0.25)])
    g.render_block([src], [out], BLOCK)
    assert np.allclose(out, 1.0, atol=1e-6)  # not this block
    g.render_block([src], [out], BLOCK)
    assert np.allclose(out, 0.25, atol=1e-6)  # but not lost either
