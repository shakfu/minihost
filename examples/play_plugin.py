#!/usr/bin/env python3
"""
Example: Real-time plugin playback with MIDI support.

Usage:
    python play_plugin.py /path/to/plugin.vst3
    python play_plugin.py --midi 0 /path/to/synth.vst3
    python play_plugin.py --virtual-midi "My Synth" /path/to/synth.vst3

    # Or use environment variable:
    export MINIHOST_PLUGIN=/path/to/plugin.vst3
    python play_plugin.py

Options:
    --midi N              Connect to MIDI input port N
    --virtual-midi NAME   Create virtual MIDI input with NAME
    --list-midi           List available MIDI ports and exit
    --info                Show plugin info and exit
    --sample-rate N       Set sample rate (default: 48000)
    --buffer N            Set buffer size in frames (default: 512)
"""

import argparse
import os
import sys
import time
import signal

import minihost


def list_midi_ports():
    """List all available MIDI ports."""
    inputs = minihost.midi_get_input_ports()
    outputs = minihost.midi_get_output_ports()

    print("MIDI Input Ports:")
    if inputs:
        for port in inputs:
            print(f"  [{port['index']}] {port['name']}")
    else:
        print("  (none)")

    print("\nMIDI Output Ports:")
    if outputs:
        for port in outputs:
            print(f"  [{port['index']}] {port['name']}")
    else:
        print("  (none)")


def show_plugin_info(path):
    """Show plugin metadata without full instantiation."""
    try:
        info = minihost.probe(path)
        print(f"Plugin: {info['name']}")
        print(f"Vendor: {info['vendor']}")
        print(f"Version: {info['version']}")
        print(f"Format: {info['format']}")
        print(f"Unique ID: {info['unique_id']}")
        print(f"Inputs: {info['num_inputs']}")
        print(f"Outputs: {info['num_outputs']}")
        print(f"Accepts MIDI: {info['accepts_midi']}")
        print(f"Produces MIDI: {info['produces_midi']}")
    except Exception as e:
        print(f"Error probing plugin: {e}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="Play an audio plugin with real-time audio and MIDI support.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument(
        "plugin",
        nargs="?",
        help="Path to plugin (.vst3 or .component). "
             "Can also be set via MINIHOST_PLUGIN environment variable."
    )
    parser.add_argument(
        "--midi", "-m",
        type=int,
        default=None,
        metavar="N",
        help="Connect to MIDI input port N"
    )
    parser.add_argument(
        "--virtual-midi", "-v",
        type=str,
        default=None,
        metavar="NAME",
        help="Create a virtual MIDI input port with NAME"
    )
    parser.add_argument(
        "--list-midi", "-l",
        action="store_true",
        help="List available MIDI ports and exit"
    )
    parser.add_argument(
        "--info", "-i",
        action="store_true",
        help="Show plugin info and exit"
    )
    parser.add_argument(
        "--sample-rate", "-r",
        type=int,
        default=48000,
        metavar="N",
        help="Sample rate (default: 48000)"
    )
    parser.add_argument(
        "--buffer", "-b",
        type=int,
        default=512,
        metavar="N",
        help="Buffer size in frames (default: 512)"
    )

    args = parser.parse_args()

    # Handle --list-midi (doesn't need plugin)
    if args.list_midi:
        list_midi_ports()
        return

    # Get plugin path from argument or environment
    plugin_path = args.plugin or os.environ.get("MINIHOST_PLUGIN")

    if not plugin_path:
        print("Error: No plugin specified.", file=sys.stderr)
        print("Provide plugin path as argument or set MINIHOST_PLUGIN environment variable.",
              file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(plugin_path):
        print(f"Error: Plugin not found: {plugin_path}", file=sys.stderr)
        sys.exit(1)

    # Handle --info
    if args.info:
        show_plugin_info(plugin_path)
        return

    # Load the plugin
    print(f"Loading: {plugin_path}")
    try:
        plugin = minihost.Plugin(
            plugin_path,
            sample_rate=args.sample_rate,
            max_block_size=args.buffer,
            in_channels=2,
            out_channels=2
        )
    except Exception as e:
        print(f"Error loading plugin: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"  Inputs: {plugin.num_input_channels}")
    print(f"  Outputs: {plugin.num_output_channels}")
    print(f"  Parameters: {plugin.num_params}")
    print(f"  Latency: {plugin.latency_samples} samples")

    # Determine MIDI configuration
    midi_input_port = -1
    if args.midi is not None:
        midi_input_port = args.midi
        inputs = minihost.midi_get_input_ports()
        if midi_input_port >= len(inputs):
            print(f"Error: MIDI port {midi_input_port} not found. "
                  f"Use --list-midi to see available ports.", file=sys.stderr)
            sys.exit(1)
        print(f"  MIDI Input: [{midi_input_port}] {inputs[midi_input_port]['name']}")

    # Open audio device
    print(f"\nOpening audio device...")
    try:
        audio = minihost.AudioDevice(
            plugin,
            sample_rate=args.sample_rate,
            buffer_frames=args.buffer,
            midi_input_port=midi_input_port
        )
    except Exception as e:
        print(f"Error opening audio device: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"  Sample rate: {audio.sample_rate:.0f} Hz")
    print(f"  Buffer: {audio.buffer_frames} frames")
    print(f"  Channels: {audio.channels}")

    # Create virtual MIDI port if requested
    if args.virtual_midi:
        try:
            audio.create_virtual_midi_input(args.virtual_midi)
            print(f"  Virtual MIDI: '{args.virtual_midi}'")
        except Exception as e:
            print(f"Warning: Could not create virtual MIDI port: {e}", file=sys.stderr)

    # Setup signal handler for clean shutdown
    running = True

    def on_signal(sig, frame):
        nonlocal running
        running = False
        print("\nStopping...")

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    # Start audio
    audio.start()
    print(f"\nPlaying. Press Ctrl+C to stop.")

    if not args.midi and not args.virtual_midi:
        print("(No MIDI input configured. Use --midi N or --virtual-midi NAME)")

    # Main loop
    try:
        while running:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass

    # Cleanup
    audio.stop()
    print("Done.")


if __name__ == "__main__":
    main()
