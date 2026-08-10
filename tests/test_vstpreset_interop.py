"""Integration tests for .vstpreset interchange against a real VST3 plugin.

Two defects motivated these:

  * `save_vstpreset` wrote JUCE's own `<VST3PluginState>` container into the
    file's `Comp` chunk. The format specifies the *raw* VST3 component state
    there, so the resulting file was readable only by minihost -- any other
    host would hand those bytes to `IComponent::setState` and get garbage.
  * `load_vstpreset` did the mirror image: it fed a real preset's raw chunk
    straight to `set_state()`. JUCE's hosted `setStateInformation` begins with
    `if (auto head = getXmlFromBinary(...))` and simply returns when the data
    is not its own container -- so loading a third-party preset silently did
    nothing while reporting success.

The silent part was compounded by `mh_set_state` always returning 1; that is
fixed separately, and is what makes a failed restore raise here.

`tests/test_vstpreset.py` covers the parsing/serialisation with mocks. These
tests use an actual plugin so the chunks are real and the round-trip is proven
end to end.
"""

from __future__ import annotations

import os

import pytest

import minihost
from minihost import _core

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


def _open():
    return minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)


def _commit_params(plugin):
    """Push one silent block so pending parameter changes reach the processor.

    VST3 separates the edit controller from the processor. A host-side
    set_param lands on IEditController immediately -- get_param reads it back
    at once -- but it only reaches IComponent through the parameter-change
    queue that rides along with a process call. IComponent::getState is
    exactly the chunk a .vstpreset stores, so without an intervening block it
    still reports the *pre-change* value.

    Plugins that mirror the value eagerly (Dexed, the default test plugin,
    among them) hide this entirely. Against one that does not, these tests
    failed while snapshotting rather than in the preset logic they exist to
    cover. minihost is not doing anything wrong here: it sets the value via
    JUCE's setValueNotifyingHost, and VST3 offers no guarantee that a
    parameter is visible to the processor before it has processed.
    """
    frames = 64
    inp = minihost.AudioBuffer(max(plugin.num_input_channels, 1), frames)
    out = minihost.AudioBuffer(max(plugin.num_output_channels, 1), frames)
    plugin.process_midi(inp, out, [])


def _first_automatable_param(plugin):
    """Index of a parameter whose value we can set and read back.

    Leaves the chosen parameter committed to the processor (see
    _commit_params), so a state snapshot taken straight after this call
    contains the value the caller just set.
    """
    for i in range(plugin.num_params):
        before = plugin.get_param(i)
        plugin.set_param(i, 0.25 if before > 0.5 else 0.75)
        if plugin.get_param(i) != before:
            _commit_params(plugin)
            return i
    return None


# --- state container conversion --------------------------------------- #


@skip_if_no_plugin
def test_split_extracts_raw_component_chunk():
    """vst3_state_split must unwrap JUCE's container, not return it."""
    plugin = _open()
    try:
        state = plugin.get_state()
        assert state[:4] == b"VC2!", "expected a JUCE-wrapped state blob"

        component, _controller = _core.vst3_state_split(state)
        assert component is not None
        assert component != state, "split returned the wrapper, not the chunk"
        assert len(component) < len(state)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_split_join_round_trips_through_the_plugin():
    plugin = _open()
    try:
        idx = _first_automatable_param(plugin)
        if idx is None:
            pytest.skip("plugin exposes no settable parameter")
        want = plugin.get_param(idx)

        component, controller = _core.vst3_state_split(plugin.get_state())
        rebuilt = _core.vst3_state_join(component, controller)

        plugin.set_param(idx, 0.5 if want != 0.5 else 0.1)
        plugin.set_state(rebuilt)
        assert plugin.get_param(idx) == pytest.approx(want, abs=1e-4)
    finally:
        plugin.close()


def test_split_rejects_non_juce_blobs():
    """A raw component chunk (what a foreign preset holds) is not a host blob.

    This is the discrimination `load_vstpreset` relies on to tell a modern
    spec-shaped preset from a legacy minihost-written one.
    """
    with pytest.raises(RuntimeError, match="VC2!"):
        _core.vst3_state_split(b"raw component chunk from another host")


def test_split_rejects_a_plugins_own_juce_blob():
    """A JUCE-built *plugin* returns its own copyXmlToBinary blob from
    IComponent::getState, so a leading 'VC2!' alone must not be treated as a
    host state blob -- the <VST3PluginState> root is what distinguishes them.
    """
    # A JUCE container whose root element is something else entirely.
    inner = _core.vst3_state_join(b"payload")
    nested = _core.vst3_state_join(inner)  # inner becomes the *component* chunk
    component, _ = _core.vst3_state_split(nested)
    assert component == inner
    # `inner` itself is a host blob, but a plugin's own blob would not be;
    # verify the root-tag check is what is doing the work.
    with pytest.raises(RuntimeError, match="root element"):
        _core.vst3_state_split(
            b"VC2!" + (24).to_bytes(4, "little") + b"<Dexed a='1'></Dexed>" + b"\x00"
        )


# --- file-level round trip -------------------------------------------- #


@skip_if_no_plugin
def test_saved_preset_holds_the_raw_chunk_not_juce_wrapper(tmp_path):
    """The whole point of the fix: the file must be spec-shaped."""
    plugin = _open()
    try:
        path = tmp_path / "out.vstpreset"
        try:
            minihost.save_vstpreset(path, plugin)
        except ValueError as e:
            if "class_id" in str(e):
                pytest.skip("plugin predates VST3 SDK 3.7.5 (no moduleinfo.json)")
            raise

        preset = minihost.read_vstpreset(path)
        expected, _ = _core.vst3_state_split(plugin.get_state())
        assert preset.component_state == expected
        # And it must NOT be the host wrapper we used to write.
        assert preset.component_state != plugin.get_state()
    finally:
        plugin.close()


@skip_if_no_plugin
def test_save_then_load_restores_parameters(tmp_path):
    plugin = _open()
    try:
        idx = _first_automatable_param(plugin)
        if idx is None:
            pytest.skip("plugin exposes no settable parameter")
        want = plugin.get_param(idx)

        path = tmp_path / "round.vstpreset"
        try:
            minihost.save_vstpreset(path, plugin)
        except ValueError as e:
            if "class_id" in str(e):
                pytest.skip("plugin predates VST3 SDK 3.7.5 (no moduleinfo.json)")
            raise

        plugin.set_param(idx, 0.5 if want != 0.5 else 0.1)
        minihost.load_vstpreset(path, plugin)
        assert plugin.get_param(idx) == pytest.approx(want, abs=1e-4)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_foreign_style_preset_loads(tmp_path):
    """The load-side half of the interop bug.

    A preset written by any other VST3 host holds the *raw* component chunk.
    Pre-fix, minihost handed that straight to JUCE's set_state, which ignored
    it and reported success -- the plugin silently kept its previous patch.
    This builds exactly such a file and requires the state to actually arrive.
    """
    plugin = _open()
    try:
        idx = _first_automatable_param(plugin)
        if idx is None:
            pytest.skip("plugin exposes no settable parameter")
        want = plugin.get_param(idx)

        # Exactly what a foreign host writes: the raw IComponent chunk.
        component, controller = _core.vst3_state_split(plugin.get_state())
        path = tmp_path / "foreign.vstpreset"
        minihost.write_vstpreset(path, "A" * 32, component, controller)

        plugin.set_param(idx, 0.5 if want != 0.5 else 0.1)
        assert plugin.get_param(idx) != pytest.approx(want, abs=1e-4)

        minihost.load_vstpreset(path, plugin)
        assert plugin.get_param(idx) == pytest.approx(want, abs=1e-4), (
            "preset did not reach the plugin"
        )
    finally:
        plugin.close()


@skip_if_no_plugin
def test_legacy_minihost_preset_still_loads(tmp_path):
    """Files written by older minihost versions put a whole JUCE blob in the
    Comp chunk. Those must keep working.
    """
    plugin = _open()
    try:
        idx = _first_automatable_param(plugin)
        if idx is None:
            pytest.skip("plugin exposes no settable parameter")
        want = plugin.get_param(idx)

        path = tmp_path / "legacy.vstpreset"
        minihost.write_vstpreset(path, "A" * 32, plugin.get_state())

        plugin.set_param(idx, 0.5 if want != 0.5 else 0.1)
        minihost.load_vstpreset(path, plugin)
        assert plugin.get_param(idx) == pytest.approx(want, abs=1e-4)
    finally:
        plugin.close()


@skip_if_no_plugin
def test_loading_a_corrupt_preset_raises_rather_than_silently_doing_nothing(tmp_path):
    """The failure mode that started all this: a preset whose chunk the plugin
    cannot use must surface as an error, not a successful no-op.
    """
    plugin = _open()
    try:
        path = tmp_path / "corrupt.vstpreset"
        minihost.write_vstpreset(path, "A" * 32, b"definitely not component state")

        with pytest.raises(RuntimeError):
            minihost.load_vstpreset(path, plugin)
    finally:
        plugin.close()
