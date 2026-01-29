#!/usr/bin/env python3
"""
Basic minihost usage examples.

Set MINIHOST_PLUGIN environment variable to a synth plugin path to run these examples.
"""

import os
import sys
import time

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

import minihost


def get_plugin_path():
    """Get plugin path from environment."""
    path = os.environ.get("MINIHOST_PLUGIN")
    if not path:
        print("Set MINIHOST_PLUGIN environment variable to a plugin path")
        print("Example: export MINIHOST_PLUGIN=/path/to/synth.vst3")
        sys.exit(1)
    return path


def example_probe():
    """Probe plugin metadata without loading."""
    print("=== Probe Plugin ===")
    path = get_plugin_path()

    info = minihost.probe(path)
    print(f"Name: {info['name']}")
    print(f"Vendor: {info['vendor']}")
    print(f"Format: {info['format']}")
    print(f"MIDI: {info['accepts_midi']}")
    print()


def example_offline_processing():
    """Process audio offline (non-realtime)."""
    print("=== Offline Processing ===")

    if not HAS_NUMPY:
        print("(skipped - numpy not installed)")
        print()
        return

    path = get_plugin_path()

    # Load plugin
    plugin = minihost.Plugin(path, sample_rate=48000, max_block_size=512)
    print(f"Loaded: {plugin.num_params} parameters")

    # Create buffers
    input_audio = np.zeros((2, 512), dtype=np.float32)
    output_audio = np.zeros((2, 512), dtype=np.float32)

    # Send a note and process
    midi_events = [
        (0, 0x90, 60, 100),    # Note on: C4, velocity 100
        (256, 0x80, 60, 0),    # Note off after 256 samples
    ]
    plugin.process_midi(input_audio, output_audio, midi_events)

    # Check output
    peak = np.max(np.abs(output_audio))
    print(f"Peak output level: {peak:.4f}")
    print()


def example_realtime_playback():
    """Real-time audio playback."""
    print("=== Real-time Playback ===")
    path = get_plugin_path()

    plugin = minihost.Plugin(path, sample_rate=48000)

    # Use context manager for automatic start/stop
    with minihost.AudioDevice(plugin) as audio:
        print(f"Playing at {audio.sample_rate:.0f} Hz")

        # Play a chord using send_midi (queues events to audio thread)
        audio.send_midi(0x90, 60, 80)   # C4 note on
        audio.send_midi(0x90, 64, 80)   # E4 note on
        audio.send_midi(0x90, 67, 80)   # G4 note on

        time.sleep(1.0)

        # Release notes
        audio.send_midi(0x80, 60, 0)    # C4 note off
        audio.send_midi(0x80, 64, 0)    # E4 note off
        audio.send_midi(0x80, 67, 0)    # G4 note off

        time.sleep(0.5)

    print("Done")
    print()


def example_midi_ports():
    """List and use MIDI ports."""
    print("=== MIDI Ports ===")

    inputs = minihost.midi_get_input_ports()
    outputs = minihost.midi_get_output_ports()

    print(f"Found {len(inputs)} input(s), {len(outputs)} output(s)")

    for port in inputs:
        print(f"  Input: [{port['index']}] {port['name']}")

    for port in outputs:
        print(f"  Output: [{port['index']}] {port['name']}")

    print()


def example_virtual_midi():
    """Create virtual MIDI port."""
    print("=== Virtual MIDI ===")
    path = get_plugin_path()

    plugin = minihost.Plugin(path, sample_rate=48000)
    audio = minihost.AudioDevice(plugin)

    try:
        audio.create_virtual_midi_input("minihost Example")
        print("Created virtual MIDI input: 'minihost Example'")
        print("Connect a MIDI source to this port...")

        audio.start()
        time.sleep(3.0)
        audio.stop()

    except RuntimeError as e:
        print(f"Virtual MIDI not supported on this platform: {e}")

    print()


def example_parameters():
    """Access plugin parameters."""
    print("=== Parameters ===")
    path = get_plugin_path()

    plugin = minihost.Plugin(path, sample_rate=48000)

    # List first 5 parameters
    for i in range(min(5, plugin.num_params)):
        info = plugin.get_param_info(i)
        value = plugin.get_param(i)
        print(f"  [{i}] {info['name']}: {value:.3f} ({info['current_value_str']})")

    print()


def example_state():
    """Save and restore plugin state."""
    print("=== State Save/Restore ===")
    path = get_plugin_path()

    plugin = minihost.Plugin(path, sample_rate=48000)

    # Modify a parameter
    if plugin.num_params > 0:
        original = plugin.get_param(0)
        plugin.set_param(0, 0.75)
        print(f"Changed param 0: {original:.3f} -> {plugin.get_param(0):.3f}")

    # Save state
    state = plugin.get_state()
    print(f"Saved state: {len(state)} bytes")

    # Reset parameter
    if plugin.num_params > 0:
        plugin.set_param(0, 0.0)
        print(f"Reset param 0: {plugin.get_param(0):.3f}")

    # Restore state
    plugin.set_state(state)
    if plugin.num_params > 0:
        print(f"Restored param 0: {plugin.get_param(0):.3f}")

    print()


if __name__ == "__main__":
    print("minihost Examples")
    print("=" * 40)
    print()

    # Examples that don't need a plugin
    example_midi_ports()

    # Examples that need a plugin
    try:
        example_probe()
        example_parameters()
        example_state()
        example_offline_processing()
        example_realtime_playback()
        # example_virtual_midi()  # Uncomment to test virtual MIDI
    except SystemExit:
        pass
