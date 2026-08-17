"""Entry points that had no consumer anywhere.

An audit of the C API against its three consumers -- the CLI binaries,
the desktop app and these bindings -- turned up functions that nothing
called and no test exercised. Two of them had no Python equivalent at
all and are exposed now:

  * ``Session.open_desc`` (``mh_session_open_desc``) -- the AudioUnit
    load path through a session's shared format manager. Sessions exist
    so that loading several plugins does not re-register the plugin
    formats each time, and AUs are the format that can only be loaded
    from a descriptor, so the combination is exactly the one worth
    having.
  * ``PluginGraph.set_node_midi`` (``mh_graph_set_node_midi``) -- stages
    MIDI straight onto a plugin node, with no MIDI_INPUT node and no
    edge. An incoming edge takes precedence, which is pinned below.

The AudioUnit case uses the stock Apple units, as tests/test_au_descriptor.py
does, so it runs on any Mac rather than waiting for a configured plugin.
The graph tests need only the default test plugin.
"""

from __future__ import annotations

import os
import platform

import numpy as np
import pytest

import minihost

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

# Stock Apple AudioUnits, present on every macOS install. AUs are
# identified by an id rather than a path, which is why they can only be
# opened from a descriptor.
_STOCK_AU = [
    ("AUDistortion", "AudioUnit:Effects/aufx,dist,appl"),
    ("AUDelay", "AudioUnit:Effects/aufx,dely,appl"),
    ("AULowpass", "AudioUnit:Effects/aufx,lpas,appl"),
]

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)
skip_if_not_macos = pytest.mark.skipif(
    platform.system() != "Darwin", reason="AudioUnits are macOS-only"
)


def _descriptor_xml(name: str, ident: str) -> str:
    return f'<PLUGIN name="{name}" format="AudioUnit" file="{ident}"/>'


BLOCK = 256
NOTE_ON = [(0, 0x90, 60, 100)]


# ---------------------------------------------------------------------------
# Session.open_desc
# ---------------------------------------------------------------------------


@skip_if_not_macos
def test_session_open_desc_loads_an_audiounit():
    """An AU opens through the session's shared format manager.

    The same descriptors `Plugin.from_descriptor` accepts, taken through
    a session instead -- which is the combination that had no consumer.
    """
    session = minihost.Session()
    opened = None
    try:
        for name, ident in _STOCK_AU:
            try:
                opened = session.open_desc(
                    _descriptor_xml(name, ident),
                    sample_rate=48000.0,
                    max_block_size=BLOCK,
                )
                break
            except RuntimeError:
                continue
        if opened is None:
            pytest.skip("no stock Apple AU could be opened on this machine")
        assert opened.num_output_channels >= 1
        assert opened.num_params >= 0
    finally:
        if opened is not None:
            opened.close()
        session.close()


@skip_if_not_macos
def test_session_open_desc_reuses_one_format_manager():
    """Several AUs load from one session, and each outlives it."""
    session = minihost.Session()
    plugins = []
    try:
        for name, ident in _STOCK_AU:
            try:
                plugins.append(
                    session.open_desc(
                        _descriptor_xml(name, ident),
                        sample_rate=48000.0,
                        max_block_size=BLOCK,
                    )
                )
            except RuntimeError:
                continue
        if not plugins:
            pytest.skip("no stock Apple AU could be opened on this machine")
        session.close()  # the plugins do not depend on it afterwards
        for plugin in plugins:
            assert plugin.num_output_channels >= 1
    finally:
        for plugin in plugins:
            plugin.close()


@skip_if_no_plugin
def test_session_open_desc_rejects_a_malformed_descriptor():
    session = minihost.Session()
    try:
        with pytest.raises(RuntimeError, match="descriptor"):
            session.open_desc("<not-a-plugin-description/>")
    finally:
        session.close()


@skip_if_no_plugin
def test_session_outlives_nothing_it_loaded():
    """A plugin keeps working after the session that loaded it is closed.

    The session owns only the format manager, which the plugin no longer
    needs once it is instantiated -- the same guarantee `Session.open`
    documents, checked here for the descriptor path's sibling so the two
    cannot drift.
    """
    session = minihost.Session()
    plugin = session.open(PLUGIN, sample_rate=48000.0, max_block_size=BLOCK)
    session.close()
    try:
        assert plugin.num_params >= 0
        silence = np.zeros((max(plugin.num_input_channels, 1), BLOCK), dtype=np.float32)
        out = np.zeros((plugin.num_output_channels, BLOCK), dtype=np.float32)
        plugin.process_midi(silence, out, NOTE_ON)
    finally:
        plugin.close()


# ---------------------------------------------------------------------------
# PluginGraph.set_node_midi
# ---------------------------------------------------------------------------


def _instrument_graph(plugin):
    """A graph of one instrument node feeding one audio output."""
    graph = minihost.PluginGraph(BLOCK, 48000.0)
    node = graph.add_plugin(plugin)
    out = graph.add_output(plugin.num_output_channels)
    graph.connect(node, out)
    return graph, node, out


@skip_if_no_plugin
def test_set_node_midi_drives_a_plugin_without_a_midi_node():
    """Staging on the node alone must sound, with no MIDI_INPUT node."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000.0, max_block_size=BLOCK)
    try:
        if not plugin.accepts_midi:
            pytest.skip("test plugin accepts no MIDI")
        if plugin.num_input_channels > 0:
            pytest.skip("test plugin wants audio input; wiring is topology-specific")

        graph, node, _ = _instrument_graph(plugin)
        graph.compile()
        buffer = np.zeros((plugin.num_output_channels, BLOCK), dtype=np.float32)
        graph.set_node_midi(node, NOTE_ON)
        graph.render_block([], [buffer], BLOCK)
        graph.close()
    finally:
        plugin.close()

    assert np.max(np.abs(buffer)) > 1e-6, "note-on staged on the node produced silence"


@skip_if_no_plugin
def test_set_node_midi_matches_a_midi_input_edge():
    """The two ways of delivering MIDI must produce the same audio."""
    events = [(0, 0x90, 64, 100)]

    direct = minihost.Plugin(PLUGIN, sample_rate=48000.0, max_block_size=BLOCK)
    try:
        if not direct.accepts_midi or direct.num_input_channels > 0:
            pytest.skip("test plugin is not a MIDI-only instrument")
        graph, node, _ = _instrument_graph(direct)
        graph.compile()
        staged = np.zeros((direct.num_output_channels, BLOCK), dtype=np.float32)
        graph.set_node_midi(node, events)
        graph.render_block([], [staged], BLOCK)
        graph.close()
    finally:
        direct.close()

    edged_plugin = minihost.Plugin(PLUGIN, sample_rate=48000.0, max_block_size=BLOCK)
    try:
        graph, node, _ = _instrument_graph(edged_plugin)
        source = graph.add_midi_input()
        graph.connect_midi(source, node)
        graph.compile()
        edged = np.zeros((edged_plugin.num_output_channels, BLOCK), dtype=np.float32)
        graph.set_midi_input_events(source, events)
        graph.render_block([], [edged], BLOCK)
        graph.close()
    finally:
        edged_plugin.close()

    assert np.max(np.abs(edged)) > 1e-6, "reference render is silent"
    assert np.allclose(staged, edged, atol=1e-5)


@skip_if_no_plugin
def test_a_midi_edge_takes_precedence_over_staged_node_events():
    """Documented rule: with an edge present, staged events are ignored."""
    plugin = minihost.Plugin(PLUGIN, sample_rate=48000.0, max_block_size=BLOCK)
    try:
        if not plugin.accepts_midi or plugin.num_input_channels > 0:
            pytest.skip("test plugin is not a MIDI-only instrument")
        graph, node, _ = _instrument_graph(plugin)
        source = graph.add_midi_input()
        graph.connect_midi(source, node)
        graph.compile()

        buffer = np.zeros((plugin.num_output_channels, BLOCK), dtype=np.float32)
        # Edge says nothing this block; the node is staged with a note.
        graph.set_midi_input_events(source, [])
        graph.set_node_midi(node, NOTE_ON)
        graph.render_block([], [buffer], BLOCK)
        graph.close()
    finally:
        plugin.close()

    assert np.max(np.abs(buffer)) <= 1e-6, (
        "staged events sounded even though a MIDI edge was connected"
    )


@skip_if_no_plugin
def test_set_node_midi_rejects_a_non_plugin_node():
    graph = minihost.PluginGraph(BLOCK, 48000.0)
    audio_in = graph.add_input(1)
    audio_out = graph.add_output(1)
    graph.connect(audio_in, audio_out)
    try:
        with pytest.raises(RuntimeError, match="not a plugin"):
            graph.set_node_midi(audio_in, NOTE_ON)
    finally:
        graph.close()
