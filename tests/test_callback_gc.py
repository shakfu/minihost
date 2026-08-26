"""Types holding Python callbacks must be reachable by the cycle collector.

`Plugin`, `AudioDevice` and `MidiIn` each hold a Python reference in a C++
member. Python's collector cannot see such an edge unless the type provides
`tp_traverse`, so any cycle running through one was uncollectable -- the
objects lived until the process exited, keeping a native plugin instance, an
open plugin bundle and (for a device) an audio stream with them.

The cycle is not exotic. A callback defined at module scope reaches its
module's `__dict__` through `__globals__`, and that dict normally holds the
plugin as well, so plain script or REPL usage closes it::

    p = minihost.Plugin(...)
    p.set_param_value_callback(lambda i, v: print(i, v))   # p -> lambda -> globals -> p

`AudioDevice` was a second, subtler case: it pinned its plugin with
`nb::keep_alive`, which records the edge in a nanobind side table that the
collector cannot walk either. It now holds the reference in a traversed
member instead.

Each test builds the cycle explicitly rather than relying on module scope, and
detects collection with a sentinel whose ``__del__`` fires only when the cycle
is actually reclaimed.
"""

from __future__ import annotations

import gc
import os

import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN), reason=f"test plugin not found at {PLUGIN}"
)


class Sentinel:
    """Records its own collection, so a cycle's fate is observable."""

    def __init__(self, freed: list[str], name: str):
        self._freed = freed
        self._name = name

    def __del__(self):
        self._freed.append(self._name)


def _collect():
    # Two passes: the first may only run finalizers, the second reclaims.
    gc.collect()
    gc.collect()


def _flags_have_gc(tp) -> bool:
    """Py_TPFLAGS_HAVE_GC. nanobind sets it exactly when a type supplies a
    Py_tp_traverse slot, so this is a direct read of whether the fix is in."""
    return bool(tp.__flags__ & (1 << 14))


def test_types_holding_callbacks_participate_in_gc():
    """The mechanism, checked directly.

    Without this flag the collector never even calls tp_traverse, so every
    behavioural test below would be testing nothing.
    """
    for tp in (
        minihost.Plugin,
        minihost.AudioDevice,
        minihost.MidiIn,
        minihost.PluginChain,
        minihost.PluginBus,
        minihost.PluginGraph,
        minihost.OscServer,
    ):
        assert _flags_have_gc(tp), f"{tp.__name__} is not GC-tracked"


@skip_if_no_plugin
def test_a_plugin_in_a_callback_cycle_is_collected():
    freed: list[str] = []

    def build() -> None:
        # Everything stays inside this frame. Note that the cycle must not be
        # dismantled by hand on the way out: `del holder` would empty the
        # closure cell that `on_param` holds, breaking the cycle before the
        # collector ever sees it and making this test pass either way.
        holder: dict[str, object] = {"sentinel": Sentinel(freed, "plugin-cycle")}
        plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)

        def on_param(index, value):
            holder  # plugin -> on_param -> cell -> holder -> plugin

        holder["plugin"] = plugin
        plugin.set_param_value_callback(on_param)

    build()
    _collect()

    assert freed == ["plugin-cycle"], (
        "the plugin's callback cycle was not collected; tp_traverse is not "
        "reporting the callback to the GC"
    )


@skip_if_no_plugin
def test_all_three_plugin_callbacks_are_traversed():
    """change, param-value and gesture holders each need visiting."""
    for setter in (
        "set_change_callback",
        "set_param_value_callback",
        "set_param_gesture_callback",
    ):
        freed: list[str] = []

        def build(setter=setter) -> None:
            holder: dict[str, object] = {"sentinel": Sentinel(freed, setter)}
            plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)

            def callback(*args):
                holder

            holder["plugin"] = plugin
            getattr(plugin, setter)(callback)

        build()
        _collect()

        assert freed == [setter], f"{setter} holder is not traversed"


@skip_if_no_plugin
def test_a_device_holding_a_plugin_in_a_cycle_is_collected():
    """The device's reference to its plugin must be visible to the GC.

    This is the case `nb::keep_alive` could not serve: the edge existed but
    lived in nanobind's internal table, so the cycle
    device -> plugin -> callback -> holder -> device was never broken.
    """
    freed: list[str] = []

    def build() -> None:
        holder: dict[str, object] = {"sentinel": Sentinel(freed, "device-cycle")}
        plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
        device = minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256)

        def on_param(index, value):
            holder

        holder["device"] = device
        plugin.set_param_value_callback(on_param)

    build()
    _collect()

    assert freed == ["device-cycle"], "the device/plugin cycle was not collected"


@skip_if_no_plugin
def test_the_device_still_keeps_its_plugin_alive():
    """Replacing keep_alive must not weaken the lifetime guarantee.

    The device holds a raw MH_Plugin*; if dropping the caller's reference
    freed the plugin, processing would read freed memory.
    """
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    device = minihost.AudioDevice(plugin, sample_rate=48000, buffer_frames=256)

    del plugin
    _collect()

    # The device is still the only owner; using it must be safe.
    device.start()
    device.stop()
    assert device.sample_rate > 0


@skip_if_no_plugin
def test_a_chain_device_keeps_its_chain_alive():
    a = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    chain = minihost.PluginChain([a])
    device = minihost.AudioDevice(chain, sample_rate=48000, buffer_frames=256)

    del a, chain
    _collect()

    device.start()
    device.stop()
    assert device.channels > 0


def test_a_midi_in_callback_cycle_is_collected():
    """MidiIn holds its callback the same way, and needs the same treatment."""
    ports = minihost.midi_get_input_ports()
    if not ports:
        pytest.skip("no MIDI input ports available")

    freed: list[str] = []

    def build() -> None:
        holder: dict[str, object] = {"sentinel": Sentinel(freed, "midi-cycle")}

        def on_midi(data):
            holder

        midi_in = minihost.MidiIn.open(0, on_midi)
        holder["midi_in"] = midi_in

    build()
    _collect()

    assert freed == ["midi-cycle"], "the MidiIn callback cycle was not collected"


@skip_if_no_plugin
def test_no_callback_means_no_cycle_to_begin_with():
    """Control: the acyclic case was never broken and must stay that way."""
    freed: list[str] = []
    sentinel = Sentinel(freed, "acyclic")

    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    del plugin, sentinel
    _collect()

    assert freed == ["acyclic"]


@skip_if_no_plugin
def test_a_chain_in_a_cycle_is_collected():
    """PluginChain held its plugin list with keep_alive, same invisible edge."""
    freed: list[str] = []

    def build() -> None:
        holder: dict[str, object] = {"sentinel": Sentinel(freed, "chain-cycle")}
        plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
        chain = minihost.PluginChain([plugin])

        def on_change(flags):
            holder

        holder["chain"] = chain
        plugin.set_change_callback(on_change)

    build()
    _collect()

    assert freed == ["chain-cycle"], "the chain cycle was not collected"


@skip_if_no_plugin
def test_a_graph_in_a_cycle_is_collected():
    """PluginGraph.add_plugin accumulated the same kind of edge."""
    freed: list[str] = []

    def build() -> None:
        holder: dict[str, object] = {"sentinel": Sentinel(freed, "graph-cycle")}
        plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
        graph = minihost.PluginGraph(512, 48000.0)
        graph.add_plugin(plugin)

        def on_change(flags):
            holder

        holder["graph"] = graph
        plugin.set_change_callback(on_change)

    build()
    _collect()

    assert freed == ["graph-cycle"], "the graph cycle was not collected"


@skip_if_no_plugin
def test_a_graph_still_keeps_its_plugins_alive():
    """add_plugin must still pin the plugin: the graph holds a raw pointer."""
    graph = minihost.PluginGraph(512, 48000.0)
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    node = graph.add_plugin(plugin)

    del plugin
    _collect()

    # Touching the node must not reach freed memory.
    assert node >= 0
    assert graph.num_nodes >= 1


@skip_if_no_plugin
def test_a_chain_still_keeps_its_plugins_alive():
    """The chain holds raw MH_Plugin*; the list must outlive the caller's ref."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)
    chain = minihost.PluginChain([plugin])

    del plugin
    _collect()

    assert chain.num_plugins == 1


def test_an_osc_server_callback_cycle_is_collected():
    """OscServer holds its callback in a C++ member, same as MidiIn."""
    freed: list[str] = []

    def build() -> None:
        holder: dict[str, object] = {"sentinel": Sentinel(freed, "osc-cycle")}

        def on_osc(address, args):
            holder

        server = minihost.OscServer.open(0, on_osc)
        holder["server"] = server

    build()
    _collect()

    assert freed == ["osc-cycle"], "the OSC server callback cycle was not collected"
