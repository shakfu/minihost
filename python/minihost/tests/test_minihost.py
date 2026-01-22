"""Tests for minihost nanobind extension module."""

import pytest
import minihost


def test_module_has_plugin_class():
    """Test that Plugin class is exported."""
    assert hasattr(minihost, 'Plugin')


def test_module_has_probe_function():
    """Test that probe function is exported."""
    assert hasattr(minihost, 'probe')
    assert callable(minihost.probe)


def test_plugin_class_has_expected_methods():
    """Test that Plugin class has expected methods."""
    expected_methods = [
        'get_param',
        'set_param',
        'get_param_info',
        'param_to_text',
        'param_from_text',
        'get_state',
        'set_state',
        'set_transport',
        'clear_transport',
        'process',
        'process_midi',
        'process_auto',
        'process_sidechain',
        'reset',
        'get_program_name',
        'get_bus_info',
    ]
    for method in expected_methods:
        assert hasattr(minihost.Plugin, method), f"Plugin missing method: {method}"


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
    assert hasattr(minihost, '__version__')
    assert minihost.__version__ == "0.1.0"


def test_plugin_class_has_expected_properties():
    """Test that Plugin class has expected properties."""
    expected_props = [
        'num_params',
        'num_input_channels',
        'num_output_channels',
        'latency_samples',
        'tail_seconds',
        'bypass',
        'non_realtime',
        'num_programs',
        'program',
        'sidechain_channels',
        'num_input_buses',
        'num_output_buses',
        'sample_rate',
    ]
    for prop in expected_props:
        assert hasattr(minihost.Plugin, prop), f"Plugin missing property: {prop}"


def test_plugin_constructor_docstring():
    """Test that Plugin constructor has documentation."""
    # nanobind doesn't support inspect.signature, so check docstring instead
    doc = minihost.Plugin.__init__.__doc__
    assert doc is not None
    assert 'path' in doc
    assert 'sample_rate' in doc
    assert 'VST3' in doc or 'AudioUnit' in doc


def test_plugin_nonexistent_directory_raises():
    """Test that loading from nonexistent directory raises RuntimeError."""
    with pytest.raises(RuntimeError, match="Failed to open plugin"):
        minihost.Plugin("/no/such/directory/plugin.vst3")


def test_plugin_wrong_extension_raises():
    """Test that loading wrong file type raises RuntimeError."""
    import tempfile
    import os
    # Create a temp file with wrong extension
    with tempfile.NamedTemporaryFile(suffix='.txt', delete=False) as f:
        f.write(b"not a plugin")
        temp_path = f.name
    try:
        with pytest.raises(RuntimeError, match="Failed to open plugin"):
            minihost.Plugin(temp_path)
    finally:
        os.unlink(temp_path)


def test_module_docstring():
    """Test that module has docstring."""
    assert minihost.__doc__ is not None
    assert 'Plugin' in minihost.__doc__


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
    with tempfile.NamedTemporaryFile(suffix='.txt', delete=False) as f:
        f.write(b"not a plugin")
        temp_path = f.name
    try:
        with pytest.raises(RuntimeError, match="Failed to probe plugin"):
            minihost.probe(temp_path)
    finally:
        os.unlink(temp_path)


# Integration tests that require a real plugin - skip if no plugin available
@pytest.fixture
def plugin_path():
    """Get a test plugin path from environment or skip."""
    import os
    path = os.environ.get('MINIHOST_TEST_PLUGIN')
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
            assert 'name' in info
            assert 'label' in info
            assert 'default_value' in info
            assert 'is_automatable' in info

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
        assert 'name' in info
        assert 'vendor' in info
        assert 'version' in info
        assert 'format' in info
        assert 'unique_id' in info
        assert 'accepts_midi' in info
        assert 'produces_midi' in info
        assert 'num_inputs' in info
        assert 'num_outputs' in info

        # Types
        assert isinstance(info['name'], str)
        assert isinstance(info['format'], str)
        assert isinstance(info['accepts_midi'], bool)
        assert isinstance(info['num_inputs'], int)
        assert isinstance(info['num_outputs'], int)

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
            text = info['current_value_str']

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
            assert 'name' in info
            assert 'num_channels' in info
            assert 'is_main' in info
            assert 'is_enabled' in info
            assert isinstance(info['name'], str)
            assert isinstance(info['num_channels'], int)
            assert isinstance(info['is_main'], bool)
            assert isinstance(info['is_enabled'], bool)

        for i in range(num_out_buses):
            info = plugin.get_bus_info(False, i)
            assert isinstance(info, dict)
            assert 'name' in info

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
                sidechain_channels=2
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
