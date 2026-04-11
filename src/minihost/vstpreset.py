"""Parser for Steinberg .vstpreset files.

The .vstpreset format is a binary container storing VST3 plugin state.
Layout:
    [Header: 48 bytes]
    [Data area: variable, contains chunk blobs]
    [Chunk list: at offset stored in header]

Header (48 bytes):
    0..3    char[4]    Magic 'VST3'
    4..7    int32_LE   Version (1)
    8..39   char[32]   Processor class ID (ASCII FUID)
    40..47  int64_LE   Offset to chunk list

Chunk list:
    0..3    char[4]    'List'
    4..7    int32_LE   Entry count
    8..     Entry[N]   20 bytes each: 4-byte ID, 8-byte offset, 8-byte size

Chunk IDs:
    'Comp' = component (processor) state
    'Cont' = controller state
    'Info' = XML metadata
"""

from __future__ import annotations

import struct
from pathlib import Path

_MAGIC = b"VST3"
_CHUNK_LIST_ID = b"List"
_COMPONENT_STATE_ID = b"Comp"
_CONTROLLER_STATE_ID = b"Cont"
_HEADER_SIZE = 48
_ENTRY_SIZE = 20  # 4 + 8 + 8


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
    path = Path(path)
    if not path.exists():
        raise FileNotFoundError(f"Preset file not found: {path}")

    data = path.read_bytes()

    if len(data) < _HEADER_SIZE:
        raise ValueError(
            f"File too small to be a .vstpreset ({len(data)} bytes, "
            f"need at least {_HEADER_SIZE})"
        )

    # Parse header
    magic = data[0:4]
    if magic != _MAGIC:
        raise ValueError(f"Invalid .vstpreset magic: {magic!r} (expected {_MAGIC!r})")

    (version,) = struct.unpack_from("<i", data, 4)
    if version != 1:
        raise ValueError(f"Unsupported .vstpreset version: {version}")

    class_id_bytes = data[8:40]
    class_id = class_id_bytes.rstrip(b"\x00").decode("ascii", errors="replace")

    (list_offset,) = struct.unpack_from("<q", data, 40)

    if list_offset < _HEADER_SIZE or list_offset >= len(data):
        raise ValueError(
            f"Invalid chunk list offset: {list_offset} (file size: {len(data)})"
        )

    # Parse chunk list
    if list_offset + 8 > len(data):
        raise ValueError("Chunk list header truncated")

    list_magic = data[list_offset : list_offset + 4]
    if list_magic != _CHUNK_LIST_ID:
        raise ValueError(
            f"Invalid chunk list magic: {list_magic!r} (expected {_CHUNK_LIST_ID!r})"
        )

    (entry_count,) = struct.unpack_from("<i", data, list_offset + 4)
    if entry_count < 0 or entry_count > 128:
        raise ValueError(f"Invalid chunk entry count: {entry_count}")

    entries_start = list_offset + 8
    entries_end = entries_start + entry_count * _ENTRY_SIZE
    if entries_end > len(data):
        raise ValueError("Chunk list entries truncated")

    # Extract chunks
    component_state = None
    controller_state = None

    for i in range(entry_count):
        offset = entries_start + i * _ENTRY_SIZE
        chunk_id = data[offset : offset + 4]
        (chunk_offset,) = struct.unpack_from("<q", data, offset + 4)
        (chunk_size,) = struct.unpack_from("<q", data, offset + 12)

        if chunk_offset < 0 or chunk_size < 0:
            continue
        if chunk_offset + chunk_size > len(data):
            raise ValueError(
                f"Chunk '{chunk_id.decode('ascii', errors='replace')}' "
                f"extends beyond file (offset={chunk_offset}, size={chunk_size}, "
                f"file_size={len(data)})"
            )

        if chunk_id == _COMPONENT_STATE_ID:
            component_state = data[chunk_offset : chunk_offset + chunk_size]
        elif chunk_id == _CONTROLLER_STATE_ID:
            controller_state = data[chunk_offset : chunk_offset + chunk_size]

    return VstPreset(
        class_id=class_id,
        component_state=component_state,
        controller_state=controller_state,
    )


def load_vstpreset(path: str | Path, plugin) -> None:
    """Load a .vstpreset file into a plugin.

    Extracts the component state from the preset and applies it
    via plugin.set_state().

    Args:
        path: Path to the .vstpreset file.
        plugin: A minihost.Plugin instance.

    Raises:
        FileNotFoundError: If the file does not exist.
        ValueError: If the file is not a valid .vstpreset or has no state.
        RuntimeError: If set_state() fails.
    """
    preset = read_vstpreset(path)

    if preset.component_state is None:
        raise ValueError(f"Preset file has no component state ('Comp' chunk): {path}")

    plugin.set_state(preset.component_state)


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
    if not class_id:
        raise ValueError("class_id must be a non-empty string")
    if component_state is None:
        raise ValueError("component_state must not be None")

    # Header: magic + version + 32-byte class id + placeholder list offset
    class_id_bytes = class_id.encode("ascii", errors="replace")
    class_id_bytes = class_id_bytes[:32].ljust(32, b"\x00")

    # Data area: component state first, then optional controller state
    data_area = bytes(component_state)
    comp_offset = _HEADER_SIZE
    comp_size = len(component_state)

    entries: list[tuple[bytes, int, int]] = [(_COMPONENT_STATE_ID, comp_offset, comp_size)]

    if controller_state is not None:
        cont_offset = comp_offset + comp_size
        cont_size = len(controller_state)
        data_area += bytes(controller_state)
        entries.append((_CONTROLLER_STATE_ID, cont_offset, cont_size))

    list_offset = _HEADER_SIZE + len(data_area)

    header = (
        _MAGIC
        + struct.pack("<i", 1)
        + class_id_bytes
        + struct.pack("<q", list_offset)
    )
    assert len(header) == _HEADER_SIZE

    chunk_list = _CHUNK_LIST_ID + struct.pack("<i", len(entries))
    for chunk_id, offset, size in entries:
        chunk_list += chunk_id
        chunk_list += struct.pack("<q", offset)
        chunk_list += struct.pack("<q", size)

    Path(path).write_bytes(header + data_area + chunk_list)


def save_vstpreset(
    path: str | Path,
    plugin,
    class_id: str | None = None,
) -> None:
    """Save a plugin's current state as a .vstpreset file.

    Args:
        path: Destination file path.
        plugin: A minihost.Plugin instance.
        class_id: Processor class ID. If None, uses the plugin's
            probed unique_id (suitable only for round-trips through
            minihost's own loader, not for cross-DAW compatibility).

    Raises:
        RuntimeError: If plugin.get_state() fails.
        OSError: If the file cannot be written.
    """
    state = plugin.get_state()
    if class_id is None:
        class_id = "minihost_unknown"
    write_vstpreset(path, class_id, state)
