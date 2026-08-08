"""High-level Python wrappers around the C .vstpreset reader/writer.

The binary format and chunk layout are documented in
``projects/libminihost/minihost_vstpreset.h``. This module is a thin
ergonomic layer over ``mh_vstpreset_read`` / ``mh_vstpreset_write``
exposed through the nanobind binding (``_core.vstpreset_read`` /
``_core.vstpreset_write``).
"""

from __future__ import annotations

from pathlib import Path


class VstPreset:
    """Parsed .vstpreset file.

    Attributes:
        class_id: The processor component's FUID (32-char ASCII string).
        component_state: Raw bytes of the processor state ('Comp' chunk),
            or None if not present.
        controller_state: Raw bytes of the controller state ('Cont' chunk),
            or None if not present.
    """

    def __init__(
        self,
        class_id: str,
        component_state: bytes | None,
        controller_state: bytes | None,
    ):
        self.class_id = class_id
        self.component_state = component_state
        self.controller_state = controller_state


def read_vstpreset(path: str | Path) -> VstPreset:
    """Read and parse a .vstpreset file.

    Args:
        path: Path to the .vstpreset file.

    Returns:
        VstPreset with extracted class_id and state blobs.

    Raises:
        FileNotFoundError: If the file does not exist.
        ValueError: If the file is not a valid .vstpreset.
    """
    from . import _core

    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Preset file not found: {path}")

    try:
        class_id, comp, cont = _core.vstpreset_read(str(path))
    except RuntimeError as e:
        raise ValueError(str(e)) from e

    return VstPreset(class_id=class_id, component_state=comp, controller_state=cont)


def _is_juce_host_state(blob: bytes) -> bool:
    """True if ``blob`` is a JUCE VST3 *host* state blob rather than a raw
    VST3 component chunk.

    Both can begin with JUCE's ``VC2!`` marker -- a JUCE-built plugin returns
    its own ``copyXmlToBinary`` blob from ``IComponent::getState`` -- so the
    marker alone is not enough. ``vst3_state_split`` additionally requires the
    ``<VST3PluginState>`` root that only JUCE's host writes, which is what
    separates the two.
    """
    from . import _core

    try:
        _core.vst3_state_split(blob)
    except RuntimeError:
        return False
    return True


def load_vstpreset(path: str | Path, plugin) -> None:
    """Load a .vstpreset file into a plugin.

    The file's ``Comp`` (and ``Cont``, if present) chunks hold raw VST3
    component / controller state. JUCE's hosted ``set_state`` does not accept
    those directly -- it wants its own ``<VST3PluginState>`` container -- so
    they are rewrapped on the way in. Without that step JUCE silently ignores
    the data and the plugin keeps its previous patch.

    Files written by older minihost versions stored a whole JUCE blob in the
    ``Comp`` chunk; those are detected and passed through unchanged so existing
    presets keep loading.

    Args:
        path: Path to the .vstpreset file.
        plugin: A minihost.Plugin instance.

    Raises:
        FileNotFoundError: If the file does not exist.
        ValueError: If the file is not a valid .vstpreset or has no state.
        RuntimeError: If set_state() fails (including the plugin rejecting it).
    """
    from . import _core

    preset = read_vstpreset(path)

    if preset.component_state is None:
        raise ValueError(f"Preset file has no component state ('Comp' chunk): {path}")

    if _is_juce_host_state(preset.component_state):
        # Legacy minihost-written preset: the chunk is already a JUCE blob.
        plugin.set_state(preset.component_state)
    else:
        plugin.set_state(
            _core.vst3_state_join(preset.component_state, preset.controller_state)
        )


def write_vstpreset(
    path: str | Path,
    class_id: str,
    component_state: bytes,
    controller_state: bytes | None = None,
) -> None:
    """Write a .vstpreset file.

    Args:
        path: Destination file path.
        class_id: Processor class ID (up to 32 ASCII characters).
            For round-trip compatibility through minihost's own loader only the
            component_state is needed, but the class_id is still written so
            external tools can identify the target plugin. It will be truncated
            or padded with NUL bytes to exactly 32 bytes.
        component_state: Raw processor state bytes (from plugin.get_state()).
        controller_state: Optional raw controller state bytes.

    Raises:
        ValueError: If class_id is empty or component_state is None.
        OSError: If the file cannot be written.
    """
    from . import _core

    if not class_id:
        raise ValueError("class_id must be a non-empty string")
    if component_state is None:
        raise ValueError("component_state must not be None")

    try:
        _core.vstpreset_write(
            str(path),
            class_id,
            bytes(component_state),
            None if controller_state is None else bytes(controller_state),
        )
    except RuntimeError as e:
        # mh_vstpreset_write reports IO errors via err_buf; translate.
        raise OSError(str(e)) from e


def read_class_id_from_bundle(vst3_path: str | Path) -> str:
    """Read the processor class ID (FUID) from a VST3 bundle's moduleinfo.json.

    Looks for ``<vst3_path>/Contents/Resources/moduleinfo.json`` and returns
    the 32-character uppercase hex CID of the first entry whose ``Category``
    is ``"Audio Module Class"`` (the processor component).

    Args:
        vst3_path: Path to the .vst3 bundle directory.

    Returns:
        A 32-character uppercase hex string.

    Raises:
        ValueError: If moduleinfo.json is missing (plugin predates VST3 SDK
            3.7.5), malformed, or contains no Audio Module Class entry.
    """
    from . import _core

    try:
        return _core.vstpreset_read_class_id_from_bundle(str(vst3_path))
    except RuntimeError as e:
        raise ValueError(str(e)) from e


def save_vstpreset(
    path: str | Path,
    plugin,
    class_id: str | None = None,
) -> None:
    """Save a plugin's current state as a .vstpreset file.

    The plugin's state is unwrapped from JUCE's ``<VST3PluginState>`` container
    into the raw component / controller chunks the format actually specifies,
    so the resulting file is readable by other VST3 hosts and by the plugin's
    own preset browser. (Writing JUCE's container into the ``Comp`` chunk, as
    earlier versions did, produced a file only minihost could read.)

    Args:
        path: Destination file path.
        plugin: A minihost.Plugin instance.
        class_id: Processor class ID (32-char hex FUID). If None, the FUID
            is read from the plugin bundle's moduleinfo.json. This requires
            the plugin to be VST3 and built against VST3 SDK 3.7.5 or newer.
            For older plugins, or for non-VST3 formats (.vstpreset is a
            VST3-only format), pass class_id explicitly or use
            ``load_vstpreset`` to inherit one from an existing preset.

    Raises:
        ValueError: If class_id is None and cannot be auto-detected, or if the
            plugin's state is not in JUCE's VST3 container format (i.e. it is
            not a JUCE-hosted VST3 plugin, so there is nothing to unwrap).
        RuntimeError: If plugin.get_state() fails.
        OSError: If the file cannot be written.
    """
    from . import _core

    state = plugin.get_state()
    try:
        component_state, controller_state = _core.vst3_state_split(state)
    except RuntimeError as e:
        raise ValueError(
            f"Cannot write a .vstpreset for this plugin: {e}. '.vstpreset' is a "
            f"VST3-only format and the state must come from a JUCE-hosted VST3 "
            f"plugin."
        ) from e
    if component_state is None:
        raise ValueError(
            "Plugin state carries no component chunk; nothing to write to 'Comp'."
        )
    if class_id is None:
        plugin_path = getattr(plugin, "path", "") or ""
        if not plugin_path.lower().endswith(".vst3"):
            raise ValueError(
                "save_vstpreset requires class_id for non-VST3 plugins; "
                "'.vstpreset' is a VST3-only format. Pass class_id explicitly."
            )
        try:
            class_id = read_class_id_from_bundle(plugin_path)
        except ValueError as e:
            raise ValueError(
                f"Could not auto-detect VST3 class_id for {plugin_path!r}: {e}. "
                f"Pass class_id explicitly, or use load_vstpreset() to inherit "
                f"one from an existing .vstpreset file."
            ) from e
    write_vstpreset(path, class_id, component_state, controller_state)
