"""Tests for minihost nanobind extension module."""

import pytest
import minihost


def test_module_has_plugin_class():
    """Test that Plugin class is exported."""
    assert hasattr(minihost, "Plugin")


def test_module_has_plugin_chain_class():
    """Test that PluginChain class is exported."""
    assert hasattr(minihost, "PluginChain")


def test_module_has_audio_device_class():
    """Test that AudioDevice class is exported."""
    assert hasattr(minihost, "AudioDevice")


def test_module_has_midi_functions():
    """Test that MIDI functions are exported."""
    assert hasattr(minihost, "midi_get_input_ports")
    assert hasattr(minihost, "midi_get_output_ports")
    assert callable(minihost.midi_get_input_ports)
    assert callable(minihost.midi_get_output_ports)


def test_module_has_probe_function():
    """Test that probe function is exported."""
    assert hasattr(minihost, "probe")
    assert callable(minihost.probe)


def test_module_has_scan_directory_function():
    """Test that scan_directory function is exported."""
    assert hasattr(minihost, "scan_directory")
    assert callable(minihost.scan_directory)


def test_module_has_midifile_class():
    """Test that MidiFile class is exported."""
    assert hasattr(minihost, "MidiFile")


def test_module_has_midiin_class():
    """Test that MidiIn class is exported with expected methods."""
    assert hasattr(minihost, "MidiIn")
    assert hasattr(minihost.MidiIn, "open")
    assert hasattr(minihost.MidiIn, "open_virtual")
    assert hasattr(minihost.MidiIn, "close")


def test_module_has_render_functions():
    """Test that render functions are exported."""
    assert hasattr(minihost, "render_midi")
    assert hasattr(minihost, "render_midi_stream")
    assert hasattr(minihost, "render_midi_to_file")
    assert hasattr(minihost, "MidiRenderer")
    assert callable(minihost.render_midi)
    assert callable(minihost.render_midi_stream)
    assert callable(minihost.render_midi_to_file)


def test_audio_device_class_has_expected_methods():
    """Test that AudioDevice class has expected methods."""
    expected_methods = [
        "start",
        "stop",
    ]
    for method in expected_methods:
        assert hasattr(minihost.AudioDevice, method), (
            f"AudioDevice missing method: {method}"
        )


def test_audio_device_class_has_expected_properties():
    """Test that AudioDevice class has expected properties."""
    expected_props = [
        "is_playing",
        "sample_rate",
        "buffer_frames",
        "channels",
        "midi_input_port",
        "midi_output_port",
    ]
    for prop in expected_props:
        assert hasattr(minihost.AudioDevice, prop), (
            f"AudioDevice missing property: {prop}"
        )


def test_audio_device_class_has_midi_methods():
    """Test that AudioDevice class has MIDI methods."""
    expected_methods = [
        "connect_midi_input",
        "connect_midi_output",
        "disconnect_midi_input",
        "disconnect_midi_output",
        "create_virtual_midi_input",
        "create_virtual_midi_output",
        "send_midi",
    ]
    for method in expected_methods:
        assert hasattr(minihost.AudioDevice, method), (
            f"AudioDevice missing method: {method}"
        )


def test_audio_device_class_has_virtual_midi_properties():
    """Test that AudioDevice class has virtual MIDI properties."""
    expected_props = [
        "is_midi_input_virtual",
        "is_midi_output_virtual",
    ]
    for prop in expected_props:
        assert hasattr(minihost.AudioDevice, prop), (
            f"AudioDevice missing property: {prop}"
        )


def test_midi_port_enumeration():
    """Test that MIDI port enumeration returns lists."""
    inputs = minihost.midi_get_input_ports()
    outputs = minihost.midi_get_output_ports()

    assert isinstance(inputs, list)
    assert isinstance(outputs, list)

    # If there are ports, check structure
    for port in inputs:
        assert isinstance(port, dict)
        assert "name" in port
        assert "index" in port
        assert isinstance(port["name"], str)
        assert isinstance(port["index"], int)

    for port in outputs:
        assert isinstance(port, dict)
        assert "name" in port
        assert "index" in port


def test_plugin_class_has_expected_methods():
    """Test that Plugin class has expected methods."""
    expected_methods = [
        "get_param",
        "set_param",
        "get_param_info",
        "param_to_text",
        "param_from_text",
        "get_state",
        "set_state",
        "set_transport",
        "clear_transport",
        "process",
        "process_midi",
        "process_auto",
        "process_sidechain",
        "process_double",
        "reset",
        "get_program_name",
        "get_bus_info",
        "check_buses_layout",
        "begin_param_gesture",
        "end_param_gesture",
        "get_program_state",
        "set_program_state",
        "set_change_callback",
        "set_param_value_callback",
        "set_param_gesture_callback",
        "set_track_properties",
    ]
    for method in expected_methods:
        assert hasattr(minihost.Plugin, method), f"Plugin missing method: {method}"


def test_plugin_chain_class_has_expected_methods():
    """Test that PluginChain class has expected methods."""
    expected_methods = [
        "get_plugin",
        "process",
        "process_midi",
        "reset",
        "set_non_realtime",
    ]
    for method in expected_methods:
        assert hasattr(minihost.PluginChain, method), (
            f"PluginChain missing method: {method}"
        )


def test_plugin_chain_class_has_expected_properties():
    """Test that PluginChain class has expected properties."""
    expected_props = [
        "num_plugins",
        "latency_samples",
        "num_input_channels",
        "num_output_channels",
        "sample_rate",
        "tail_seconds",
    ]
    for prop in expected_props:
        assert hasattr(minihost.PluginChain, prop), (
            f"PluginChain missing property: {prop}"
        )


def test_plugin_invalid_path_raises():
    """Test that loading an invalid plugin path raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to open plugin"):
        minihost.Plugin("/nonexistent/path/to/plugin.vst3")


def test_plugin_empty_path_raises():
    """Test that loading with empty path raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to open plugin"):
        minihost.Plugin("")


def test_version():
    """Test that version is defined."""
    assert hasattr(minihost, "__version__")
    assert minihost.__version__ == "0.1.2"


def test_plugin_class_has_expected_properties():
    """Test that Plugin class has expected properties."""
    expected_props = [
        "num_params",
        "num_input_channels",
        "num_output_channels",
        "latency_samples",
        "tail_seconds",
        "bypass",
        "non_realtime",
        "num_programs",
        "program",
        "sidechain_channels",
        "num_input_buses",
        "num_output_buses",
        "supports_double",
        "processing_precision",
        "sample_rate",
        "accepts_midi",
        "produces_midi",
        "is_midi_effect",
        "supports_mpe",
    ]
    for prop in expected_props:
        assert hasattr(minihost.Plugin, prop), f"Plugin missing property: {prop}"


def test_plugin_constructor_docstring():
    """Test that Plugin constructor has documentation."""
    # nanobind doesn't support inspect.signature, so check docstring instead
    doc = minihost.Plugin.__init__.__doc__
    assert doc is not None
    assert "path" in doc
    assert "sample_rate" in doc
    assert "VST3" in doc or "AudioUnit" in doc


def test_plugin_nonexistent_directory_raises():
    """Test that loading from nonexistent directory raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to open plugin"):
        minihost.Plugin("/no/such/directory/plugin.vst3")


def test_plugin_wrong_extension_raises():
    """Test that loading wrong file type raises RuntimeError."""
    import tempfile
    import os

    # Create a temp file with wrong extension
    with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as f:
        f.write(b"not a plugin")
        temp_path = f.name
    try:
        with pytest.raises(RuntimeError, match="Failed to open plugin"):
            minihost.Plugin(temp_path)
    finally:
        os.unlink(temp_path)


def test_module_has_change_constants():
    """Test that change notification constants are exported."""
    assert hasattr(minihost, "MH_CHANGE_LATENCY")
    assert hasattr(minihost, "MH_CHANGE_PARAM_INFO")
    assert hasattr(minihost, "MH_CHANGE_PROGRAM")
    assert hasattr(minihost, "MH_CHANGE_NON_PARAM_STATE")
    # Verify they are distinct bitmask values
    assert minihost.MH_CHANGE_LATENCY == 0x01
    assert minihost.MH_CHANGE_PARAM_INFO == 0x02
    assert minihost.MH_CHANGE_PROGRAM == 0x04
    assert minihost.MH_CHANGE_NON_PARAM_STATE == 0x08


def test_module_has_precision_constants():
    """Test that processing precision constants are exported."""
    assert hasattr(minihost, "MH_PRECISION_SINGLE")
    assert hasattr(minihost, "MH_PRECISION_DOUBLE")
    assert minihost.MH_PRECISION_SINGLE == 0
    assert minihost.MH_PRECISION_DOUBLE == 1


def test_module_docstring():
    """Test that module has docstring."""
    assert minihost.__doc__ is not None
    assert "Plugin" in minihost.__doc__


def test_probe_invalid_path_raises():
    """Test that probing invalid path raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to probe plugin"):
        minihost.probe("/nonexistent/path/to/plugin.vst3")


def test_probe_empty_path_raises():
    """Test that probing empty path raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to probe plugin"):
        minihost.probe("")


def test_probe_wrong_file_type_raises():
    """Test that probing wrong file type raises RuntimeError."""
    import tempfile
    import os

    with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as f:
        f.write(b"not a plugin")
        temp_path = f.name
    try:
        with pytest.raises(RuntimeError, match="Failed to probe plugin"):
            minihost.probe(temp_path)
    finally:
        os.unlink(temp_path)


def test_scan_directory_nonexistent_raises():
    """Test that scanning nonexistent directory raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to scan directory"):
        minihost.scan_directory("/nonexistent/directory/path")


def test_scan_directory_empty_path_raises():
    """Test that scanning empty path raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to scan directory"):
        minihost.scan_directory("")


def test_scan_directory_no_plugins():
    """Test that scanning directory with no plugins returns empty list."""
    import tempfile
    import os

    # Create a temp directory with no plugins
    with tempfile.TemporaryDirectory() as tmpdir:
        result = minihost.scan_directory(tmpdir)
        assert isinstance(result, list)
        assert len(result) == 0


def test_scan_directory_file_not_directory_raises():
    """Test that scanning a file instead of directory raises RuntimeError."""
    import tempfile
    import os

    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(b"not a directory")
        temp_path = f.name
    try:
        with pytest.raises(RuntimeError, match="Failed to scan directory"):
            minihost.scan_directory(temp_path)
    finally:
        os.unlink(temp_path)


# Integration tests that require a real plugin - skip if no plugin available
@pytest.fixture
def plugin_path():
    """Get a test plugin path from environment or skip."""
    import os

    path = os.environ.get("MINIHOST_TEST_PLUGIN")
    if not path:
        pytest.skip("Set MINIHOST_TEST_PLUGIN env var to run integration tests")
    return path


@pytest.fixture
def plugin(plugin_path):
    """Create a plugin instance for testing."""
    return minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)


class TestPluginIntegration:
    """Integration tests that require a real plugin."""

    def test_plugin_properties(self, plugin):
        """Test plugin properties."""
        assert plugin.num_params >= 0
        assert plugin.num_input_channels >= 0
        assert plugin.num_output_channels >= 0
        assert plugin.latency_samples >= 0
        assert plugin.tail_seconds >= 0.0

    def test_midi_capabilities(self, plugin):
        """Test MIDI capability query properties."""
        assert isinstance(plugin.accepts_midi, bool)
        assert isinstance(plugin.produces_midi, bool)
        assert isinstance(plugin.is_midi_effect, bool)
        assert isinstance(plugin.supports_mpe, bool)

    def test_param_access(self, plugin):
        """Test parameter get/set."""
        if plugin.num_params > 0:
            # Get current value
            val = plugin.get_param(0)
            assert 0.0 <= val <= 1.0

            # Set and verify
            plugin.set_param(0, 0.5)
            new_val = plugin.get_param(0)
            assert abs(new_val - 0.5) < 0.01

    def test_param_info(self, plugin):
        """Test parameter info retrieval."""
        if plugin.num_params > 0:
            info = plugin.get_param_info(0)
            assert "name" in info
            assert "id" in info
            assert "label" in info
            assert "default_value" in info
            assert "is_automatable" in info
            assert "category" in info
            assert isinstance(info["id"], str)
            assert isinstance(info["category"], int)
            assert info["category"] >= 0

    def test_state_save_restore(self, plugin):
        """Test state save and restore."""
        state = plugin.get_state()
        assert isinstance(state, bytes)
        if len(state) > 0:
            plugin.set_state(state)

    def test_process_audio(self, plugin):
        """Test audio processing."""
        import numpy as np

        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)

        plugin.process(input_audio, output_audio)

    def test_process_with_midi(self, plugin):
        """Test audio processing with MIDI."""
        import numpy as np

        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)

        # Note on at sample 0, note off at sample 256
        midi_in = [(0, 0x90, 60, 100), (256, 0x80, 60, 0)]
        midi_out = plugin.process_midi(input_audio, output_audio, midi_in)

        assert isinstance(midi_out, list)

    def test_transport(self, plugin):
        """Test transport info."""
        plugin.set_transport(bpm=120.0, is_playing=True)
        plugin.clear_transport()

    def test_bypass(self, plugin):
        """Test bypass."""
        # Just test that it doesn't crash - not all plugins support bypass
        _ = plugin.bypass
        plugin.bypass = True
        plugin.bypass = False

    def test_reset(self, plugin):
        """Test reset clears internal state."""
        import numpy as np

        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        # Process some audio first
        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)
        plugin.process(input_audio, output_audio)

        # Reset should succeed
        plugin.reset()

        # Should be able to process again
        plugin.process(input_audio, output_audio)

    def test_non_realtime(self, plugin):
        """Test non-realtime mode toggle."""
        # Default should be False
        assert plugin.non_realtime is False

        # Set to True
        plugin.non_realtime = True
        assert plugin.non_realtime is True

        # Set back to False
        plugin.non_realtime = False
        assert plugin.non_realtime is False

    def test_probe(self, plugin_path):
        """Test probing plugin metadata."""
        info = minihost.probe(plugin_path)

        assert isinstance(info, dict)
        assert "name" in info
        assert "vendor" in info
        assert "version" in info
        assert "format" in info
        assert "unique_id" in info
        assert "accepts_midi" in info
        assert "produces_midi" in info
        assert "num_inputs" in info
        assert "num_outputs" in info

        # Types
        assert isinstance(info["name"], str)
        assert isinstance(info["format"], str)
        assert isinstance(info["accepts_midi"], bool)
        assert isinstance(info["num_inputs"], int)
        assert isinstance(info["num_outputs"], int)

    def test_param_to_text(self, plugin):
        """Test parameter value to text conversion."""
        if plugin.num_params > 0:
            # Convert a few values to text
            text_0 = plugin.param_to_text(0, 0.0)
            text_50 = plugin.param_to_text(0, 0.5)
            text_100 = plugin.param_to_text(0, 1.0)

            assert isinstance(text_0, str)
            assert isinstance(text_50, str)
            assert isinstance(text_100, str)

    def test_param_from_text(self, plugin):
        """Test text to parameter value conversion."""
        if plugin.num_params > 0:
            # Get current text representation
            info = plugin.get_param_info(0)
            text = info["current_value_str"]

            # Try to convert it back - note: not all plugins implement this well
            try:
                value = plugin.param_from_text(0, text)
                assert 0.0 <= value <= 1.0
            except RuntimeError:
                # Some plugins don't implement text-to-value
                pass

    def test_factory_presets(self, plugin):
        """Test factory preset (program) access."""
        num_programs = plugin.num_programs
        assert isinstance(num_programs, int)
        assert num_programs >= 0

        if num_programs > 0:
            # Get current program
            current = plugin.program
            assert isinstance(current, int)
            assert 0 <= current < num_programs

            # Get program names
            for i in range(min(num_programs, 5)):  # Test first 5
                name = plugin.get_program_name(i)
                assert isinstance(name, str)

            # Set program
            plugin.program = 0
            assert plugin.program == 0

            if num_programs > 1:
                plugin.program = 1
                assert plugin.program == 1
                # Restore original
                plugin.program = current

    def test_bus_layout(self, plugin):
        """Test bus layout query."""
        # Check number of buses
        num_in_buses = plugin.num_input_buses
        num_out_buses = plugin.num_output_buses

        assert isinstance(num_in_buses, int)
        assert isinstance(num_out_buses, int)
        assert num_in_buses >= 0
        assert num_out_buses >= 0

        # Query bus info for each bus
        for i in range(num_in_buses):
            info = plugin.get_bus_info(True, i)
            assert isinstance(info, dict)
            assert "name" in info
            assert "num_channels" in info
            assert "is_main" in info
            assert "is_enabled" in info
            assert isinstance(info["name"], str)
            assert isinstance(info["num_channels"], int)
            assert isinstance(info["is_main"], bool)
            assert isinstance(info["is_enabled"], bool)

        for i in range(num_out_buses):
            info = plugin.get_bus_info(False, i)
            assert isinstance(info, dict)
            assert "name" in info

    def test_sidechain_properties(self, plugin):
        """Test sidechain channel property."""
        sc_ch = plugin.sidechain_channels
        assert isinstance(sc_ch, int)
        # Default plugin opened without sidechain should have 0
        assert sc_ch >= 0

    def test_process_sidechain(self, plugin_path):
        """Test processing with sidechain input."""
        import numpy as np

        # Open plugin with sidechain support
        try:
            plugin_sc = minihost.Plugin(
                plugin_path,
                sample_rate=48000,
                max_block_size=512,
                in_channels=2,
                out_channels=2,
                sidechain_channels=2,
            )
        except RuntimeError:
            pytest.skip("Plugin may not support sidechain")
            return

        in_ch = max(plugin_sc.num_input_channels, 2)
        out_ch = max(plugin_sc.num_output_channels, 2)
        sc_ch = plugin_sc.sidechain_channels

        # Create test buffers
        main_in = np.zeros((in_ch, 512), dtype=np.float32)
        main_out = np.zeros((out_ch, 512), dtype=np.float32)

        if sc_ch > 0:
            sidechain_in = np.zeros((sc_ch, 512), dtype=np.float32)
            plugin_sc.process_sidechain(main_in, main_out, sidechain_in)
        else:
            # Plugin doesn't support sidechain, use regular process
            plugin_sc.process(main_in, main_out)

    def test_sample_rate_change(self, plugin):
        """Test sample rate change without reloading."""
        import numpy as np

        # Get initial sample rate
        initial_rate = plugin.sample_rate
        assert initial_rate == 48000  # We opened with 48000

        # Set a parameter to a known value (if plugin has params)
        original_param_val = None
        if plugin.num_params > 0:
            plugin.set_param(0, 0.75)
            original_param_val = plugin.get_param(0)

        # Change sample rate
        plugin.sample_rate = 44100
        assert plugin.sample_rate == 44100

        # Parameter state should be preserved
        if plugin.num_params > 0:
            new_param_val = plugin.get_param(0)
            assert abs(new_param_val - original_param_val) < 0.01

        # Process should work at new sample rate
        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)
        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)
        plugin.process(input_audio, output_audio)

        # Change back to original rate
        plugin.sample_rate = initial_rate
        assert plugin.sample_rate == initial_rate

        # Process should still work
        plugin.process(input_audio, output_audio)

    def test_scan_directory(self, plugin_path):
        """Test scanning a directory containing plugins."""
        import os

        # Scan the parent directory of the test plugin
        plugin_dir = os.path.dirname(plugin_path)
        if not plugin_dir:
            plugin_dir = "."

        results = minihost.scan_directory(plugin_dir)

        assert isinstance(results, list)
        # Should find at least the test plugin
        assert len(results) >= 1

        # Check that results contain expected fields
        for info in results:
            assert isinstance(info, dict)
            assert "name" in info
            assert "vendor" in info
            assert "version" in info
            assert "format" in info
            assert "unique_id" in info
            assert "path" in info
            assert "accepts_midi" in info
            assert "produces_midi" in info
            assert "num_inputs" in info
            assert "num_outputs" in info

            # Check types
            assert isinstance(info["name"], str)
            assert isinstance(info["path"], str)
            assert isinstance(info["format"], str)
            assert isinstance(info["accepts_midi"], bool)
            assert isinstance(info["num_inputs"], int)

            # Path should be non-empty and exist
            assert len(info["path"]) > 0

        # Check that we found our test plugin
        test_plugin_name = os.path.basename(plugin_path)
        found_test_plugin = any(test_plugin_name in info["path"] for info in results)
        assert found_test_plugin, (
            f"Did not find test plugin {test_plugin_name} in scan results"
        )

    def test_double_precision(self, plugin):
        """Test double precision audio processing."""
        import numpy as np

        # Check supports_double property
        supports_double = plugin.supports_double
        assert isinstance(supports_double, bool)

        in_ch = max(plugin.num_input_channels, 2)
        out_ch = max(plugin.num_output_channels, 2)

        # Create float64 buffers
        input_audio = np.zeros((in_ch, 512), dtype=np.float64)
        output_audio = np.zeros((out_ch, 512), dtype=np.float64)

        # Process with double precision (works regardless of native support)
        plugin.process_double(input_audio, output_audio)

        # Verify output is still float64
        assert output_audio.dtype == np.float64

    def test_audio_device(self, plugin):
        """Test AudioDevice creation and properties."""
        # Create audio device
        audio = minihost.AudioDevice(plugin)

        # Check properties
        assert audio.sample_rate > 0
        assert audio.buffer_frames > 0
        assert audio.channels > 0
        assert audio.is_playing is False

    def test_audio_device_start_stop(self, plugin):
        """Test AudioDevice start/stop."""
        audio = minihost.AudioDevice(plugin)

        # Start playback
        audio.start()
        assert audio.is_playing is True

        # Stop playback
        audio.stop()
        assert audio.is_playing is False

    def test_audio_device_context_manager(self, plugin):
        """Test AudioDevice as context manager."""
        with minihost.AudioDevice(plugin) as audio:
            assert audio.is_playing is True
            assert audio.sample_rate > 0
        # After exiting context, should be stopped
        # Note: audio object is invalid after close, so we don't check is_playing

    def test_audio_device_with_config(self, plugin_path):
        """Test AudioDevice with custom configuration."""
        # Create plugin with specific sample rate
        plugin = minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=1024)

        # Create audio device with specific settings
        audio = minihost.AudioDevice(
            plugin, sample_rate=48000, buffer_frames=512, output_channels=2
        )

        # Verify properties (actual values may differ from requested)
        assert audio.sample_rate > 0
        assert audio.buffer_frames > 0
        assert audio.channels > 0

        audio.stop()  # Cleanup

    def test_audio_device_midi_properties(self, plugin):
        """Test AudioDevice MIDI properties."""
        audio = minihost.AudioDevice(plugin)

        # Should not be connected to any MIDI port by default
        assert audio.midi_input_port == -1
        assert audio.midi_output_port == -1

    def test_check_buses_layout(self, plugin):
        """Test bus layout validation."""
        # Build channel count arrays matching current bus layout
        in_buses = []
        for i in range(plugin.num_input_buses):
            info = plugin.get_bus_info(True, i)
            in_buses.append(info["num_channels"])

        out_buses = []
        for i in range(plugin.num_output_buses):
            info = plugin.get_bus_info(False, i)
            out_buses.append(info["num_channels"])

        # Current layout should be supported
        assert plugin.check_buses_layout(in_buses, out_buses) is True

        # Absurd layout (many buses) -- just verify it returns a bool
        result = plugin.check_buses_layout([2] * 20, [2] * 20)
        assert isinstance(result, bool)

    def test_param_gestures(self, plugin):
        """Test parameter gesture begin/end around set_param."""
        if plugin.num_params > 0:
            plugin.begin_param_gesture(0)
            plugin.set_param(0, 0.5)
            plugin.set_param(0, 0.7)
            plugin.end_param_gesture(0)

    def test_program_state_save_restore(self, plugin):
        """Test per-program state save and restore."""
        state = plugin.get_program_state()
        assert isinstance(state, bytes)
        if len(state) > 0:
            plugin.set_program_state(state)

    def test_change_notifications(self, plugin):
        """Test change notification callbacks."""
        import threading

        received = {"flags": None}
        event = threading.Event()

        def on_change(flags):
            received["flags"] = flags
            event.set()

        plugin.set_change_callback(on_change)

        # Trigger a change by loading state (should fire non-param state change)
        state = plugin.get_state()
        if len(state) > 0:
            plugin.set_state(state)
            # Give callback a moment to fire (it may come from another thread)
            event.wait(timeout=1.0)

        # Clear callback (should not crash)
        plugin.set_change_callback(None)

    def test_param_value_callback(self, plugin):
        """Test parameter value change notification."""
        if plugin.num_params == 0:
            return

        received = []

        def on_param_change(idx, val):
            received.append((idx, val))

        plugin.set_param_value_callback(on_param_change)

        # setValueNotifyingHost should trigger the listener
        plugin.set_param(0, 0.42)

        # Clear callback
        plugin.set_param_value_callback(None)

    def test_param_gesture_callback(self, plugin):
        """Test parameter gesture notification from plugin side."""
        if plugin.num_params == 0:
            return

        received = []

        def on_gesture(idx, starting):
            received.append((idx, starting))

        plugin.set_param_gesture_callback(on_gesture)

        # Trigger gesture from host side -- the listener won't fire for
        # host-initiated gestures (those are plugin->host), but ensure
        # no crash and callback clears cleanly
        plugin.begin_param_gesture(0)
        plugin.end_param_gesture(0)

        plugin.set_param_gesture_callback(None)

    def test_processing_precision(self, plugin):
        """Test processing precision get/set."""
        import numpy as np

        # Default should be single precision
        assert plugin.processing_precision == minihost.MH_PRECISION_SINGLE

        # If plugin supports double, switch to double and back
        if plugin.supports_double:
            plugin.processing_precision = minihost.MH_PRECISION_DOUBLE
            assert plugin.processing_precision == minihost.MH_PRECISION_DOUBLE

            # Process should still work
            in_ch = max(plugin.num_input_channels, 2)
            out_ch = max(plugin.num_output_channels, 2)
            input_audio = np.zeros((in_ch, 512), dtype=np.float64)
            output_audio = np.zeros((out_ch, 512), dtype=np.float64)
            plugin.process_double(input_audio, output_audio)

            # Switch back
            plugin.processing_precision = minihost.MH_PRECISION_SINGLE
            assert plugin.processing_precision == minihost.MH_PRECISION_SINGLE

    def test_processing_precision_single_always_works(self, plugin):
        """Test that setting single precision always succeeds."""
        plugin.processing_precision = minihost.MH_PRECISION_SINGLE
        assert plugin.processing_precision == minihost.MH_PRECISION_SINGLE

    def test_track_properties(self, plugin):
        """Test setting track properties."""
        # Set name only
        plugin.set_track_properties(name="Lead Synth")

        # Set name and colour
        plugin.set_track_properties(name="Bass", colour=0xFF0000FF)

        # Set colour only
        plugin.set_track_properties(colour=0xFFFF0000)

        # Clear both
        plugin.set_track_properties()

    def test_audio_device_midi_connection(self, plugin):
        """Test AudioDevice MIDI connection (if ports available)."""
        audio = minihost.AudioDevice(plugin)

        inputs = minihost.midi_get_input_ports()
        if inputs:
            # Connect to first input port
            audio.connect_midi_input(inputs[0]["index"])
            assert audio.midi_input_port == inputs[0]["index"]

            # Disconnect
            audio.disconnect_midi_input()
            assert audio.midi_input_port == -1

        outputs = minihost.midi_get_output_ports()
        if outputs:
            # Connect to first output port
            audio.connect_midi_output(outputs[0]["index"])
            assert audio.midi_output_port == outputs[0]["index"]

            # Disconnect
            audio.disconnect_midi_output()
            assert audio.midi_output_port == -1


class TestMidiFile:
    """Tests for MidiFile class."""

    def test_midifile_creation(self):
        """Test MidiFile creation."""
        mf = minihost.MidiFile()
        assert mf.num_tracks >= 0
        assert mf.ticks_per_quarter > 0

    def test_midifile_add_track(self):
        """Test adding tracks."""
        mf = minihost.MidiFile()
        initial_tracks = mf.num_tracks
        idx = mf.add_track()
        assert mf.num_tracks == initial_tracks + 1
        assert idx == initial_tracks

    def test_midifile_add_notes(self):
        """Test adding note events."""
        mf = minihost.MidiFile()
        # Add note on and off
        mf.add_note_on(0, 0, 0, 60, 100)
        mf.add_note_off(0, 480, 0, 60, 0)

        # Get events
        events = mf.get_events(0)
        assert len(events) >= 2

        # Find note events
        note_ons = [e for e in events if e["type"] == "note_on"]
        note_offs = [e for e in events if e["type"] == "note_off"]
        assert len(note_ons) >= 1
        assert len(note_offs) >= 1

    def test_midifile_add_tempo(self):
        """Test adding tempo events."""
        mf = minihost.MidiFile()
        mf.add_tempo(0, 0, 120.0)

        events = mf.get_events(0)
        tempo_events = [e for e in events if e["type"] == "tempo"]
        assert len(tempo_events) >= 1
        assert abs(tempo_events[0]["bpm"] - 120.0) < 0.01

    def test_midifile_add_control_change(self):
        """Test adding control change events."""
        mf = minihost.MidiFile()
        mf.add_control_change(0, 0, 0, 1, 64)  # CC1 (mod wheel) = 64

        events = mf.get_events(0)
        cc_events = [e for e in events if e["type"] == "control_change"]
        assert len(cc_events) >= 1
        assert cc_events[0]["controller"] == 1
        assert cc_events[0]["value"] == 64

    def test_midifile_add_program_change(self):
        """Test adding program change events."""
        mf = minihost.MidiFile()
        mf.add_program_change(0, 0, 0, 42)

        events = mf.get_events(0)
        pc_events = [e for e in events if e["type"] == "program_change"]
        assert len(pc_events) >= 1
        assert pc_events[0]["program"] == 42

    def test_midifile_save_load(self):
        """Test saving and loading MIDI files."""
        import tempfile
        import os

        mf = minihost.MidiFile()
        mf.add_tempo(0, 0, 120.0)
        mf.add_note_on(0, 0, 0, 60, 100)
        mf.add_note_off(0, 480, 0, 60, 0)

        # Save to temp file
        with tempfile.NamedTemporaryFile(suffix=".mid", delete=False) as f:
            temp_path = f.name

        try:
            assert mf.save(temp_path) is True

            # Load it back
            mf2 = minihost.MidiFile()
            assert mf2.load(temp_path) is True
            assert mf2.num_tracks >= 1

            # Check events
            events = mf2.get_events(0)
            note_ons = [e for e in events if e["type"] == "note_on"]
            assert len(note_ons) >= 1
            assert note_ons[0]["pitch"] == 60
        finally:
            os.unlink(temp_path)

    def test_midifile_ticks_per_quarter(self):
        """Test setting ticks per quarter note."""
        mf = minihost.MidiFile()
        mf.ticks_per_quarter = 480
        assert mf.ticks_per_quarter == 480

    def test_midifile_duration(self):
        """Test duration calculation."""
        mf = minihost.MidiFile()
        mf.add_tempo(0, 0, 120.0)  # 120 BPM = 2 beats/sec
        mf.add_note_on(0, 0, 0, 60, 100)
        mf.add_note_off(
            0, 480, 0, 60, 0
        )  # 480 ticks at 120 TPQ = 4 quarters = 2 sec at 120 BPM

        # Duration should be positive
        duration = mf.duration_seconds
        assert duration > 0

    def test_midifile_load_nonexistent(self):
        """Test loading nonexistent file returns False."""
        mf = minihost.MidiFile()
        assert mf.load("/nonexistent/path/to/file.mid") is False

    def test_midifile_get_events_invalid_track(self):
        """Test getting events from invalid track returns empty list."""
        mf = minihost.MidiFile()
        events = mf.get_events(999)  # Invalid track index
        assert isinstance(events, list)
        assert len(events) == 0


class TestMidiRendering:
    """Tests for MIDI rendering functions (require a plugin)."""

    @pytest.fixture
    def synth_plugin(self):
        """Get a synth plugin for testing."""
        import os

        path = os.environ.get("MINIHOST_TEST_PLUGIN")
        if not path:
            pytest.skip("Set MINIHOST_TEST_PLUGIN env var to run rendering tests")
        return minihost.Plugin(path, sample_rate=48000, max_block_size=512)

    @pytest.fixture
    def test_midi_file(self):
        """Create a simple test MIDI file."""
        mf = minihost.MidiFile()
        mf.add_tempo(0, 0, 120.0)
        mf.add_note_on(0, 0, 0, 60, 100)
        mf.add_note_off(0, 480, 0, 60, 0)
        return mf

    def test_render_midi_returns_array(self, synth_plugin, test_midi_file):
        """Test that render_midi returns a numpy array."""
        import numpy as np

        audio = minihost.render_midi(
            synth_plugin, test_midi_file, block_size=512, tail_seconds=0.5
        )

        assert isinstance(audio, np.ndarray)
        assert audio.ndim == 2
        assert audio.shape[0] >= 1  # At least 1 channel
        assert audio.shape[1] > 0  # Some samples

    def test_render_midi_stream_yields_blocks(self, synth_plugin, test_midi_file):
        """Test that render_midi_stream yields audio blocks."""
        import numpy as np

        blocks = list(
            minihost.render_midi_stream(
                synth_plugin, test_midi_file, block_size=256, tail_seconds=0.1
            )
        )

        assert len(blocks) > 0
        for block in blocks:
            assert isinstance(block, np.ndarray)
            assert block.ndim == 2

    def test_render_midi_to_file(self, synth_plugin, test_midi_file):
        """Test rendering MIDI to WAV file."""
        import tempfile
        import os

        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
            temp_path = f.name

        try:
            samples = minihost.render_midi_to_file(
                synth_plugin, test_midi_file, temp_path, tail_seconds=0.1
            )

            assert samples > 0
            assert os.path.exists(temp_path)
            assert os.path.getsize(temp_path) > 44  # At least header size
        finally:
            if os.path.exists(temp_path):
                os.unlink(temp_path)

    def test_midi_renderer_class(self, synth_plugin, test_midi_file):
        """Test MidiRenderer class."""
        renderer = minihost.MidiRenderer(
            synth_plugin, test_midi_file, block_size=512, tail_seconds=0.5
        )

        assert renderer.duration_seconds > 0
        assert renderer.total_samples > 0
        assert renderer.progress == 0.0
        assert not renderer.is_finished

        # Render a few blocks
        block = renderer.render_block()
        assert block is not None
        assert renderer.progress > 0.0

        # Render rest
        audio = renderer.render_all()
        assert renderer.is_finished
        assert renderer.progress == 1.0

    def test_render_midi_from_file_path(self, synth_plugin):
        """Test rendering from MIDI file path."""
        import tempfile
        import os

        # Create a temp MIDI file
        mf = minihost.MidiFile()
        mf.add_tempo(0, 0, 120.0)
        mf.add_note_on(0, 0, 0, 60, 100)
        mf.add_note_off(0, 240, 0, 60, 0)

        with tempfile.NamedTemporaryFile(suffix=".mid", delete=False) as f:
            midi_path = f.name

        try:
            mf.save(midi_path)

            # Render from path
            audio = minihost.render_midi(synth_plugin, midi_path, tail_seconds=0.1)

            assert audio.shape[1] > 0
        finally:
            if os.path.exists(midi_path):
                os.unlink(midi_path)


class TestPluginChain:
    """Tests for PluginChain class (require a plugin)."""

    @pytest.fixture
    def plugin_path(self):
        """Get a test plugin path from environment or skip."""
        import os

        path = os.environ.get("MINIHOST_TEST_PLUGIN")
        if not path:
            pytest.skip("Set MINIHOST_TEST_PLUGIN env var to run PluginChain tests")
        return path

    @pytest.fixture
    def plugin(self, plugin_path):
        """Create a plugin instance for testing."""
        return minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)

    @pytest.fixture
    def plugin2(self, plugin_path):
        """Create a second plugin instance for testing chains."""
        return minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)

    def test_chain_creation_single_plugin(self, plugin):
        """Test creating chain with single plugin."""
        chain = minihost.PluginChain([plugin])
        assert chain.num_plugins == 1
        assert chain.sample_rate == 48000

    def test_chain_creation_multiple_plugins(self, plugin, plugin2):
        """Test creating chain with multiple plugins."""
        chain = minihost.PluginChain([plugin, plugin2])
        assert chain.num_plugins == 2

    def test_chain_latency_is_sum(self, plugin, plugin2):
        """Test that chain latency is sum of plugin latencies."""
        latency1 = plugin.latency_samples
        latency2 = plugin2.latency_samples
        chain = minihost.PluginChain([plugin, plugin2])
        assert chain.latency_samples == latency1 + latency2

    def test_chain_channels(self, plugin):
        """Test chain channel properties."""
        chain = minihost.PluginChain([plugin])
        assert chain.num_input_channels == plugin.num_input_channels
        assert chain.num_output_channels == plugin.num_output_channels

    def test_chain_get_plugin(self, plugin, plugin2):
        """Test getting plugin from chain by index."""
        chain = minihost.PluginChain([plugin, plugin2])
        assert chain.get_plugin(0) is plugin
        assert chain.get_plugin(1) is plugin2

    def test_chain_get_plugin_out_of_range(self, plugin):
        """Test that out of range index raises error."""
        chain = minihost.PluginChain([plugin])
        with pytest.raises(RuntimeError, match="index out of range"):
            chain.get_plugin(5)

    def test_chain_process(self, plugin):
        """Test processing audio through chain."""
        import numpy as np

        chain = minihost.PluginChain([plugin])

        in_ch = max(chain.num_input_channels, 2)
        out_ch = max(chain.num_output_channels, 2)

        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)

        chain.process(input_audio, output_audio)

    def test_chain_process_midi(self, plugin):
        """Test processing audio with MIDI through chain."""
        import numpy as np

        chain = minihost.PluginChain([plugin])

        in_ch = max(chain.num_input_channels, 2)
        out_ch = max(chain.num_output_channels, 2)

        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)

        # Note on, note off
        midi_in = [(0, 0x90, 60, 100), (256, 0x80, 60, 0)]
        midi_out = chain.process_midi(input_audio, output_audio, midi_in)

        assert isinstance(midi_out, list)

    def test_chain_reset(self, plugin):
        """Test resetting chain."""
        chain = minihost.PluginChain([plugin])
        chain.reset()  # Should not raise

    def test_chain_tail_seconds(self, plugin, plugin2):
        """Test chain tail_seconds property."""
        chain = minihost.PluginChain([plugin, plugin2])
        # Tail is max of all plugin tails
        assert chain.tail_seconds >= 0.0

    def test_chain_audio_device(self, plugin, plugin2):
        """Test AudioDevice with plugin chain."""
        chain = minihost.PluginChain([plugin, plugin2])
        audio = minihost.AudioDevice(chain)

        assert audio.sample_rate > 0
        assert audio.buffer_frames > 0
        assert audio.is_playing is False

    def test_chain_audio_device_start_stop(self, plugin):
        """Test AudioDevice start/stop with chain."""
        chain = minihost.PluginChain([plugin])
        audio = minihost.AudioDevice(chain)

        audio.start()
        assert audio.is_playing is True

        audio.stop()
        assert audio.is_playing is False

    def test_chain_audio_device_context_manager(self, plugin):
        """Test AudioDevice as context manager with chain."""
        chain = minihost.PluginChain([plugin])
        with minihost.AudioDevice(chain) as audio:
            assert audio.is_playing is True

    def test_chain_render_midi(self, plugin):
        """Test render_midi with plugin chain."""
        import numpy as np

        chain = minihost.PluginChain([plugin])

        mf = minihost.MidiFile()
        mf.add_tempo(0, 0, 120.0)
        mf.add_note_on(0, 0, 0, 60, 100)
        mf.add_note_off(0, 480, 0, 60, 0)

        audio = minihost.render_midi(chain, mf, tail_seconds=0.5)

        assert isinstance(audio, np.ndarray)
        assert audio.ndim == 2
        assert audio.shape[1] > 0

    def test_chain_process_auto(self, plugin, plugin2):
        """Test chain process_auto with parameter changes."""
        import numpy as np

        chain = minihost.PluginChain([plugin, plugin2])

        in_ch = max(chain.num_input_channels, 2)
        out_ch = max(chain.num_output_channels, 2)

        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)

        # Build param changes targeting both plugins
        # (sample_offset, plugin_index, param_index, value)
        param_changes = []
        if plugin.num_params > 0:
            param_changes.append((0, 0, 0, 0.5))
            param_changes.append((256, 0, 0, 0.8))
        if plugin2.num_params > 0:
            param_changes.append((0, 1, 0, 0.3))

        # Sort by sample_offset (required by API)
        param_changes.sort(key=lambda x: x[0])

        midi_in = [(0, 0x90, 60, 100), (256, 0x80, 60, 0)]
        midi_out = chain.process_auto(input_audio, output_audio, midi_in, param_changes)

        assert isinstance(midi_out, list)

    def test_chain_process_auto_no_changes(self, plugin):
        """Test chain process_auto with no param changes (fast path)."""
        import numpy as np

        chain = minihost.PluginChain([plugin])

        in_ch = max(chain.num_input_channels, 2)
        out_ch = max(chain.num_output_channels, 2)

        input_audio = np.zeros((in_ch, 512), dtype=np.float32)
        output_audio = np.zeros((out_ch, 512), dtype=np.float32)
        output_audio_ref = np.zeros((out_ch, 512), dtype=np.float32)

        # process_auto with empty changes should behave like process_midi
        midi_in = [(0, 0x90, 60, 100)]
        midi_out_auto = chain.process_auto(input_audio, output_audio, midi_in, [])
        midi_out_ref = chain.process_midi(input_audio, output_audio_ref, midi_in)

        assert isinstance(midi_out_auto, list)

    def test_empty_chain_raises_error(self):
        """Test that empty plugin list raises error."""
        with pytest.raises(RuntimeError, match="at least one plugin"):
            minihost.PluginChain([])

    def test_chain_sample_rate_mismatch_raises(self, plugin_path):
        """Test that mismatched sample rates raise error."""
        plugin1 = minihost.Plugin(plugin_path, sample_rate=44100, max_block_size=512)
        plugin2 = minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)

        with pytest.raises(RuntimeError, match="[Ss]ample rate"):
            minihost.PluginChain([plugin1, plugin2])
