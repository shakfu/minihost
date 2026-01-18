"""Tests for minihost nanobind extension module."""

import pytest
import minihost


def test_module_has_plugin_class():
    """Test that Plugin class is exported."""
    assert hasattr(minihost, 'Plugin')


def test_plugin_class_has_expected_methods():
    """Test that Plugin class has expected methods."""
    expected_methods = [
        'get_param',
        'set_param',
        'get_param_info',
        'get_state',
        'set_state',
        'set_transport',
        'clear_transport',
        'process',
        'process_midi',
        'process_auto',
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
