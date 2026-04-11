"""Tests for minihost.vstpreset module."""

import struct

import pytest

from minihost.vstpreset import (
    VstPreset,
    load_vstpreset,
    read_class_id_from_bundle,
    read_vstpreset,
    save_vstpreset,
    write_vstpreset,
)


def _make_bundle(tmp_path, moduleinfo_text):
    """Create a fake .vst3 bundle directory containing the given
    moduleinfo.json text. Returns the bundle path."""
    bundle = tmp_path / "Plugin.vst3"
    resources = bundle / "Contents" / "Resources"
    resources.mkdir(parents=True)
    (resources / "moduleinfo.json").write_text(moduleinfo_text)
    return bundle


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


class TestWriteVstPreset:
    def test_round_trip_component_only(self, tmp_path):
        path = tmp_path / "out.vstpreset"
        state = b"\xde\xad\xbe\xef" * 32
        write_vstpreset(path, "A" * 32, state)

        preset = read_vstpreset(path)
        assert preset.class_id == "A" * 32
        assert preset.component_state == state
        assert preset.controller_state is None

    def test_round_trip_with_controller(self, tmp_path):
        path = tmp_path / "out.vstpreset"
        comp = b"component_state_bytes"
        cont = b"controller_state_bytes"
        write_vstpreset(path, "B" * 32, comp, cont)

        preset = read_vstpreset(path)
        assert preset.component_state == comp
        assert preset.controller_state == cont

    def test_class_id_truncation(self, tmp_path):
        path = tmp_path / "out.vstpreset"
        long_id = "X" * 100  # longer than 32
        write_vstpreset(path, long_id, b"data")

        preset = read_vstpreset(path)
        assert preset.class_id == "X" * 32

    def test_class_id_padding(self, tmp_path):
        path = tmp_path / "out.vstpreset"
        short_id = "short"
        write_vstpreset(path, short_id, b"data")

        preset = read_vstpreset(path)
        # class_id after reading strips trailing NULs
        assert preset.class_id == "short"

    def test_empty_class_id_raises(self, tmp_path):
        with pytest.raises(ValueError, match="non-empty"):
            write_vstpreset(tmp_path / "x.vstpreset", "", b"data")

    def test_none_state_raises(self, tmp_path):
        with pytest.raises(ValueError, match="component_state"):
            write_vstpreset(tmp_path / "x.vstpreset", "A" * 32, None)  # type: ignore[arg-type]

    def test_empty_state(self, tmp_path):
        path = tmp_path / "out.vstpreset"
        write_vstpreset(path, "A" * 32, b"")

        preset = read_vstpreset(path)
        assert preset.component_state == b""

    def test_large_state(self, tmp_path):
        path = tmp_path / "out.vstpreset"
        state = bytes(range(256)) * 500  # 128000 bytes
        write_vstpreset(path, "A" * 32, state)

        preset = read_vstpreset(path)
        assert preset.component_state == state


class TestSaveVstPreset:
    def test_saves_plugin_state(self, tmp_path):
        from unittest.mock import MagicMock

        plugin = MagicMock()
        plugin.get_state.return_value = b"state_from_plugin"

        path = tmp_path / "out.vstpreset"
        save_vstpreset(path, plugin, class_id="A" * 32)

        plugin.get_state.assert_called_once()
        preset = read_vstpreset(path)
        assert preset.component_state == b"state_from_plugin"
        assert preset.class_id == "A" * 32

    def test_default_class_id_requires_vst3_path(self, tmp_path):
        """save_vstpreset() with class_id=None must raise for non-VST3 plugins.

        The .vstpreset format is VST3-only; without an explicit class_id we
        need a real VST3 bundle to read the FUID from. A MagicMock plugin
        whose .path is itself a Mock cannot satisfy that contract.
        """
        from unittest.mock import MagicMock

        plugin = MagicMock()
        plugin.path = "/some/effect.au"  # not a .vst3
        plugin.get_state.return_value = b"data"

        path = tmp_path / "out.vstpreset"
        with pytest.raises(ValueError, match="VST3-only"):
            save_vstpreset(path, plugin)  # no class_id

    def test_default_class_id_missing_moduleinfo(self, tmp_path):
        """save_vstpreset() with class_id=None for a .vst3 path without
        a moduleinfo.json must raise with a helpful message."""
        from unittest.mock import MagicMock

        # Pretend the plugin's path ends in .vst3, but the bundle dir
        # doesn't actually exist -- so moduleinfo.json lookup will fail.
        plugin = MagicMock()
        plugin.path = str(tmp_path / "fake.vst3")
        plugin.get_state.return_value = b"data"

        out = tmp_path / "out.vstpreset"
        with pytest.raises(ValueError, match="Could not auto-detect"):
            save_vstpreset(out, plugin)

    def test_default_class_id_from_moduleinfo(self, tmp_path):
        """save_vstpreset() with class_id=None reads the FUID from a real
        moduleinfo.json fixture."""
        from unittest.mock import MagicMock

        # Build a minimal valid VST3 bundle layout with moduleinfo.json
        bundle = tmp_path / "FakePlugin.vst3"
        resources = bundle / "Contents" / "Resources"
        resources.mkdir(parents=True)
        (resources / "moduleinfo.json").write_text(
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "ABCDEF0123456789ABCDEF0123456789",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )

        plugin = MagicMock()
        plugin.path = str(bundle)
        plugin.get_state.return_value = b"data"

        out = tmp_path / "out.vstpreset"
        save_vstpreset(out, plugin)

        preset = read_vstpreset(out)
        assert preset.class_id == "ABCDEF0123456789ABCDEF0123456789"
        assert preset.component_state == b"data"

    def test_round_trip_load_after_save(self, tmp_path):
        """Save then load a preset through minihost's own loader."""
        from unittest.mock import MagicMock

        # Save side
        src_plugin = MagicMock()
        src_plugin.get_state.return_value = b"plugin_state_xyz"
        path = tmp_path / "round.vstpreset"
        save_vstpreset(path, src_plugin, class_id="ROUNDTRIP" + "_" * 23)

        # Load side
        dst_plugin = MagicMock()
        load_vstpreset(path, dst_plugin)
        dst_plugin.set_state.assert_called_once_with(b"plugin_state_xyz")


class TestReadClassIdFromBundle:
    """Tests for the moduleinfo.json scanner."""

    def test_basic(self, tmp_path):
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "ABCDEF0123456789ABCDEF0123456789",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        assert read_class_id_from_bundle(bundle) == "ABCDEF0123456789ABCDEF0123456789"

    def test_picks_audio_module_class_not_controller(self, tmp_path):
        """The scanner must skip the controller class and pick the
        processor (Audio Module Class) entry, regardless of order."""
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "1111111111111111CCCCCCCCCCCCCCCC",\n'
            '      "Category": "Component Controller Class"\n'
            '    },\n'
            '    {\n'
            '      "CID": "2222222222222222AAAAAAAAAAAAAAAA",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        assert read_class_id_from_bundle(bundle) == "2222222222222222AAAAAAAAAAAAAAAA"

    def test_tolerates_trailing_commas(self, tmp_path):
        """Real-world moduleinfo.json files emit JSON5-style trailing
        commas (e.g., Dexed, Strokes). The scanner must tolerate them."""
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "ABCDEF019182FAEB4447534244657864",\n'
            '      "Category": "Audio Module Class",\n'
            '      "Sub Categories": [\n'
            '        "Instrument",\n'
            '        "Synth",\n'
            '      ],\n'
            '    },\n'
            '  ],\n'
            '}\n'
        )
        assert read_class_id_from_bundle(bundle) == "ABCDEF019182FAEB4447534244657864"

    def test_uppercases_lowercase_hex(self, tmp_path):
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "abcdef0123456789abcdef0123456789",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        assert read_class_id_from_bundle(bundle) == "ABCDEF0123456789ABCDEF0123456789"

    def test_cid_before_category(self, tmp_path):
        """Key order should not matter -- CID may appear before Category."""
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "Name": "Foo",\n'
            '      "CID": "DEADBEEFDEADBEEFDEADBEEFDEADBEEF",\n'
            '      "Vendor": "Acme",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        assert read_class_id_from_bundle(bundle) == "DEADBEEFDEADBEEFDEADBEEFDEADBEEF"

    def test_picks_first_audio_module_class(self, tmp_path):
        """If a bundle exposes multiple audio plugins, take the first."""
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",\n'
            '      "Category": "Audio Module Class"\n'
            '    },\n'
            '    {\n'
            '      "CID": "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        assert read_class_id_from_bundle(bundle) == "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

    def test_missing_moduleinfo_raises(self, tmp_path):
        bundle = tmp_path / "Plugin.vst3"
        bundle.mkdir()  # bundle exists but no moduleinfo.json
        with pytest.raises(ValueError, match="moduleinfo.json not found"):
            read_class_id_from_bundle(bundle)

    def test_bundle_path_not_a_directory_raises(self, tmp_path):
        with pytest.raises(ValueError, match="moduleinfo.json not found"):
            read_class_id_from_bundle(tmp_path / "nonexistent.vst3")

    def test_no_audio_module_class_raises(self, tmp_path):
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "1111111111111111CCCCCCCCCCCCCCCC",\n'
            '      "Category": "Component Controller Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        with pytest.raises(ValueError, match="Audio Module Class"):
            read_class_id_from_bundle(bundle)

    def test_invalid_cid_length_raises(self, tmp_path):
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "TOO_SHORT",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        with pytest.raises(ValueError, match="32 hex characters"):
            read_class_id_from_bundle(bundle)

    def test_non_hex_cid_raises(self, tmp_path):
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        with pytest.raises(ValueError, match="32 hex characters"):
            read_class_id_from_bundle(bundle)

    def test_no_classes_key_raises(self, tmp_path):
        bundle = _make_bundle(tmp_path, '{"Name": "Foo"}\n')
        with pytest.raises(ValueError, match='"Classes" key'):
            read_class_id_from_bundle(bundle)

    def test_bundle_path_with_trailing_slash(self, tmp_path):
        """Trailing slashes on the bundle path should be tolerated."""
        bundle = _make_bundle(tmp_path,
            '{\n'
            '  "Classes": [\n'
            '    {\n'
            '      "CID": "ABCDEF0123456789ABCDEF0123456789",\n'
            '      "Category": "Audio Module Class"\n'
            '    }\n'
            '  ]\n'
            '}\n'
        )
        assert read_class_id_from_bundle(str(bundle) + "/") == \
            "ABCDEF0123456789ABCDEF0123456789"
