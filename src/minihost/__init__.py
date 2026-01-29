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
    AudioDevice,
    probe,
    scan_directory,
    midi_get_input_ports,
    midi_get_output_ports,
)

__all__ = [
    "Plugin",
    "AudioDevice",
    "probe",
    "scan_directory",
    "midi_get_input_ports",
    "midi_get_output_ports",
]
__version__ = "0.1.0"
