"""
minihost - Python bindings for audio plugin hosting.

A minimal audio plugin host library supporting VST3 and AudioUnit plugins.

Example usage:
    >>> import numpy as np
    >>> import minihost
    >>>
    >>> # Load a plugin
    >>> plugin = minihost.Plugin("/path/to/plugin.vst3", sample_rate=48000)
    >>>
    >>> # Check plugin info
    >>> print(f"Parameters: {plugin.num_params}")
    >>> print(f"Latency: {plugin.latency_samples} samples")
    >>>
    >>> # Process audio
    >>> input_audio = np.zeros((2, 512), dtype=np.float32)
    >>> output_audio = np.zeros((2, 512), dtype=np.float32)
    >>> plugin.process(input_audio, output_audio)
    >>>
    >>> # Process with MIDI
    >>> midi_events = [(0, 0x90, 60, 100), (256, 0x80, 60, 0)]  # Note on/off
    >>> midi_out = plugin.process_midi(input_audio, output_audio, midi_events)
    >>>
    >>> # Set transport for tempo-synced plugins
    >>> plugin.set_transport(bpm=120.0, is_playing=True)
    >>>
    >>> # Save/restore state
    >>> state = plugin.get_state()
    >>> plugin.set_state(state)
"""

from minihost._core import (
    Plugin,
    PluginChain,
    AudioDevice,
    MidiFile,
    probe,
    scan_directory,
    midi_get_input_ports,
    midi_get_output_ports,
    MH_CHANGE_LATENCY,
    MH_CHANGE_PARAM_INFO,
    MH_CHANGE_PROGRAM,
    MH_CHANGE_NON_PARAM_STATE,
    MH_PRECISION_SINGLE,
    MH_PRECISION_DOUBLE,
)

from minihost.render import (
    render_midi,
    render_midi_stream,
    render_midi_to_file,
    MidiRenderer,
)

__all__ = [
    # Core classes
    "Plugin",
    "PluginChain",
    "AudioDevice",
    "MidiFile",
    # Plugin discovery
    "probe",
    "scan_directory",
    # MIDI ports
    "midi_get_input_ports",
    "midi_get_output_ports",
    # MIDI rendering
    "render_midi",
    "render_midi_stream",
    "render_midi_to_file",
    "MidiRenderer",
    # Change notification constants
    "MH_CHANGE_LATENCY",
    "MH_CHANGE_PARAM_INFO",
    "MH_CHANGE_PROGRAM",
    "MH_CHANGE_NON_PARAM_STATE",
    # Processing precision constants
    "MH_PRECISION_SINGLE",
    "MH_PRECISION_DOUBLE",
]
__version__ = "0.1.1"
