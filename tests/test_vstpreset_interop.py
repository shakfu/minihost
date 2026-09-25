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
from minihost.vstpreset import read_class_id_from_bundle

PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)

skip_if_no_plugin = pytest.mark.skipif(
    not os.path.exists(PLUGIN),
    reason=f"test plugin not found at {PLUGIN}",
)


def _class_id():
    """The plugin's own class id, as any host writing a preset for it would.

    load_vstpreset refuses a preset whose class id differs from the plugin's.
    A plugin without moduleinfo.json cannot be checked, so a placeholder does.
    """
    try:
        return read_class_id_from_bundle(PLUGIN)
    except (ValueError, RuntimeError, FileNotFoundError):
        return "A" * 32


def _open():
    return minihost.Plugin(PLUGIN, sample_rate=48000, max_block_size=512)


def _rejects_bad_state(plugin) -> bool:
    """Whether this plugin reports a state it cannot use as a failure.

    A JUCE-built VST3 cannot: its wrapper's `readFromUnknownStream` hands any
    bytes it does not recognise to `setStateInformation`, which has no way to
    report failure, and returns kResultTrue. The vendor name used to stand in
    for this and was wrong in both directions -- Renoise Redux is not
    JUCE-built and also accepts anything -- so ask the plugin.

    Not the assertion the test makes: that one goes through `load_vstpreset`,
    the layer under test, and checks the error survives it. This is only the
    condition that makes the assertion meaningful.
    """
    try:
        plugin.set_state(_core.vst3_state_join(b"definitely not component state"))
    except RuntimeError:
        return True
    return False


def _state_round_trips(plugin, idx) -> bool:
    """Whether a parameter value survives the plugin's own get_state/set_state.

    That pair is the measuring instrument for every test below: they set a
    parameter, push the state through a file, and read the value back. A
    plugin whose restore does not carry the value can say nothing about the
    file layer under test.

    Renoise Redux is such a plugin on the default message thread. It builds
    an NSWindow inside setStateInformation, which AppKit refuses anywhere but
    the main thread, so the restore either fails outright or leaves the
    parameter at the plugin's default. `MINIHOST_MESSAGE_THREAD=main` runs
    control calls on the main thread and Redux then round-trips correctly,
    at the price of every minihost call having to come from there.
    """
    want = plugin.get_param(idx)
    snapshot = plugin.get_state()
    plugin.set_param(idx, 0.5 if want != 0.5 else 0.1)
    _commit_params(plugin)
    try:
        plugin.set_state(snapshot)
    except RuntimeError:
        return False
    return plugin.get_param(idx) == pytest.approx(want, abs=1e-4)


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
    """Index of a parameter we can set, read back, *and* that the plugin stores.

    Reading the value back is not enough. Renoise Redux's parameter 0 is a
    Preset selector: it reports whatever is written to it and is absent from
    IComponent::getState, so it reads 0 after any restore and these tests
    measured the plugin's state model instead of the preset code. Requiring
    the state snapshot to move as well picks a parameter a round trip can
    carry -- Redux's parameter 1 and every one after it qualifies.

    Leaves the chosen parameter committed to the processor (see
    _commit_params), so a state snapshot taken straight after this call
    contains the value the caller just set.
    """
    for i in range(plugin.num_params):
        before = plugin.get_param(i)
        baseline = plugin.get_state()
        plugin.set_param(i, 0.25 if before > 0.5 else 0.75)
        if plugin.get_param(i) == before:
            continue
        _commit_params(plugin)
        if plugin.get_state() != baseline:
            return i
        plugin.set_param(i, before)
        _commit_params(plugin)
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
        if not _state_round_trips(plugin, idx):
            pytest.skip("plugin's own state round trip does not carry a value here")
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
        if not _state_round_trips(plugin, idx):
            pytest.skip("plugin's own state round trip does not carry a value here")
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
        if not _state_round_trips(plugin, idx):
            pytest.skip("plugin's own state round trip does not carry a value here")
        want = plugin.get_param(idx)

        # Exactly what a foreign host writes: the raw IComponent chunk.
        component, controller = _core.vst3_state_split(plugin.get_state())
        path = tmp_path / "foreign.vstpreset"
        minihost.write_vstpreset(path, _class_id(), component, controller)

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
        if not _state_round_trips(plugin, idx):
            pytest.skip("plugin's own state round trip does not carry a value here")
        want = plugin.get_param(idx)

        path = tmp_path / "legacy.vstpreset"
        minihost.write_vstpreset(path, _class_id(), plugin.get_state())

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
    if os.environ.get("MINIHOST_MESSAGE_THREAD") == "main":
        # The probe below hands the plugin bytes it cannot parse. On the main
        # thread a plugin's own error handling runs: Renoise Redux blocks
        # there indefinitely, presumably on a dialog no one can answer.
        pytest.skip("corrupt-state probe can block a plugin in main-thread mode")
    plugin = _open()
    try:
        if not _rejects_bad_state(plugin):
            pytest.skip(
                "plugin accepts any state chunk, so it cannot report a bad preset"
            )
        path = tmp_path / "corrupt.vstpreset"
        minihost.write_vstpreset(path, _class_id(), b"definitely not component state")

        with pytest.raises(RuntimeError):
            minihost.load_vstpreset(path, plugin)
    finally:
        plugin.close()
