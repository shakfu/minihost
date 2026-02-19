"""Tests for minihost.vstpreset module."""

import struct

import pytest

from minihost.vstpreset import VstPreset, read_vstpreset, load_vstpreset


def _build_vstpreset(class_id="A" * 32, component_state=b"", controller_state=None):
    """Build a minimal valid .vstpreset file in memory.

    Returns the bytes of the complete file.
    """
    # Header
    magic = b"VST3"
    version = struct.pack("<i", 1)
    fuid = class_id.encode("ascii").ljust(32, b"\x00")[:32]
    list_offset_placeholder = struct.pack("<q", 0)  # patched later

    header = magic + version + fuid + list_offset_placeholder
    assert len(header) == 48

    # Data area: component state, then optional controller state
    data_area = component_state
    comp_offset = 48
    comp_size = len(component_state)

    entries = [(b"Comp", comp_offset, comp_size)]

    if controller_state is not None:
        cont_offset = comp_offset + comp_size
        cont_size = len(controller_state)
        data_area += controller_state
        entries.append((b"Cont", cont_offset, cont_size))

    # Chunk list
    list_offset = 48 + len(data_area)
    chunk_list = b"List"
    chunk_list += struct.pack("<i", len(entries))
    for chunk_id, offset, size in entries:
        chunk_list += chunk_id
        chunk_list += struct.pack("<q", offset)
        chunk_list += struct.pack("<q", size)

    # Patch list offset in header
    header = header[:40] + struct.pack("<q", list_offset)

    return header + data_area + chunk_list


class TestReadVstPreset:
    def test_minimal_preset(self, tmp_path):
        state = b"\x01\x02\x03\x04"
        data = _build_vstpreset(component_state=state)
        path = tmp_path / "test.vstpreset"
        path.write_bytes(data)

        preset = read_vstpreset(path)

        assert preset.class_id == "A" * 32
        assert preset.component_state == state
        assert preset.controller_state is None

    def test_with_controller_state(self, tmp_path):
        comp_state = b"comp_data_here"
        cont_state = b"cont_data_here"
        data = _build_vstpreset(component_state=comp_state, controller_state=cont_state)
        path = tmp_path / "test.vstpreset"
        path.write_bytes(data)

        preset = read_vstpreset(path)

        assert preset.component_state == comp_state
        assert preset.controller_state == cont_state

    def test_class_id_extraction(self, tmp_path):
        class_id = "41424344454647484142434445464748"
        data = _build_vstpreset(class_id=class_id, component_state=b"\x00")
        path = tmp_path / "test.vstpreset"
        path.write_bytes(data)

        preset = read_vstpreset(path)
        assert preset.class_id == class_id

    def test_empty_component_state(self, tmp_path):
        data = _build_vstpreset(component_state=b"")
        path = tmp_path / "test.vstpreset"
        path.write_bytes(data)

        preset = read_vstpreset(path)
        assert preset.component_state == b""

    def test_large_state(self, tmp_path):
        state = bytes(range(256)) * 100  # 25600 bytes
        data = _build_vstpreset(component_state=state)
        path = tmp_path / "test.vstpreset"
        path.write_bytes(data)

        preset = read_vstpreset(path)
        assert preset.component_state == state

    def test_file_not_found(self):
        with pytest.raises(FileNotFoundError):
            read_vstpreset("/nonexistent/path/preset.vstpreset")

    def test_invalid_magic(self, tmp_path):
        path = tmp_path / "bad.vstpreset"
        path.write_bytes(b"NOPE" + b"\x00" * 100)

        with pytest.raises(ValueError, match="Invalid .vstpreset magic"):
            read_vstpreset(path)

    def test_file_too_small(self, tmp_path):
        path = tmp_path / "tiny.vstpreset"
        path.write_bytes(b"VST3")

        with pytest.raises(ValueError, match="too small"):
            read_vstpreset(path)

    def test_invalid_version(self, tmp_path):
        data = b"VST3" + struct.pack("<i", 99) + b"\x00" * 40
        path = tmp_path / "bad_version.vstpreset"
        path.write_bytes(data)

        with pytest.raises(ValueError, match="Unsupported .vstpreset version"):
            read_vstpreset(path)

    def test_invalid_list_offset(self, tmp_path):
        # Valid header but list offset points beyond file
        data = b"VST3" + struct.pack("<i", 1) + b"\x00" * 32 + struct.pack("<q", 99999)
        path = tmp_path / "bad_offset.vstpreset"
        path.write_bytes(data)

        with pytest.raises(ValueError, match="Invalid chunk list offset"):
            read_vstpreset(path)

    def test_invalid_list_magic(self, tmp_path):
        # Valid header, list offset points to data but wrong magic
        header = b"VST3" + struct.pack("<i", 1) + b"\x00" * 32 + struct.pack("<q", 48)
        bad_list = b"XXXX" + struct.pack("<i", 0)
        data = header + bad_list
        path = tmp_path / "bad_list.vstpreset"
        path.write_bytes(data)

        with pytest.raises(ValueError, match="Invalid chunk list magic"):
            read_vstpreset(path)


class TestLoadVstPreset:
    def test_applies_state_to_plugin(self, tmp_path):
        from unittest.mock import MagicMock

        state = b"test_state_data"
        data = _build_vstpreset(component_state=state)
        path = tmp_path / "test.vstpreset"
        path.write_bytes(data)

        plugin = MagicMock()

        load_vstpreset(path, plugin)

        plugin.set_state.assert_called_once_with(state)

    def test_raises_if_no_component_state(self, tmp_path):
        from unittest.mock import MagicMock

        # Build a preset with empty chunk list (no Comp entry)
        header = b"VST3" + struct.pack("<i", 1) + b"\x00" * 32 + struct.pack("<q", 48)
        chunk_list = b"List" + struct.pack("<i", 0)
        data = header + chunk_list
        path = tmp_path / "no_comp.vstpreset"
        path.write_bytes(data)

        plugin = MagicMock()

        with pytest.raises(ValueError, match="no component state"):
            load_vstpreset(path, plugin)


class TestVstPresetDataclass:
    def test_attributes(self):
        preset = VstPreset(
            class_id="test_id",
            component_state=b"comp",
            controller_state=b"cont",
        )
        assert preset.class_id == "test_id"
        assert preset.component_state == b"comp"
        assert preset.controller_state == b"cont"

    def test_none_states(self):
        preset = VstPreset(class_id="x", component_state=None, controller_state=None)
        assert preset.component_state is None
        assert preset.controller_state is None
