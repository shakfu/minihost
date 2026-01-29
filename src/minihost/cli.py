#!/usr/bin/env python3
"""
minihost - Audio plugin hosting CLI.

A command-line interface for loading, inspecting, and rendering audio through
VST3 and AudioUnit plugins.

Usage:
    minihost probe /path/to/plugin.vst3
    minihost scan /path/to/plugins/
    minihost play /path/to/synth.vst3 --midi 0
    minihost render /path/to/synth.vst3 song.mid output.wav
    minihost midi-ports
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import time
from typing import Optional

import minihost


def cmd_probe(args: argparse.Namespace) -> int:
    """Probe plugin metadata without full instantiation."""
    try:
        info = minihost.probe(args.plugin)
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    if args.json:
        import json
        print(json.dumps(info, indent=2))
    else:
        print(f"Name:      {info['name']}")
        print(f"Vendor:    {info['vendor']}")
        print(f"Version:   {info['version']}")
        print(f"Format:    {info['format']}")
        print(f"Unique ID: {info['unique_id']}")
        print(f"Inputs:    {info['num_inputs']}")
        print(f"Outputs:   {info['num_outputs']}")
        print(f"MIDI In:   {'yes' if info['accepts_midi'] else 'no'}")
        print(f"MIDI Out:  {'yes' if info['produces_midi'] else 'no'}")

    return 0


def cmd_scan(args: argparse.Namespace) -> int:
    """Scan directory for plugins."""
    try:
        results = minihost.scan_directory(args.directory)
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    if args.json:
        import json
        print(json.dumps(results, indent=2))
    else:
        for i, info in enumerate(results, 1):
            print(f"[{i}] {info['name']} ({info['format']}) - {info['path']}")
        print(f"\nFound {len(results)} plugin(s)")

    return 0


def cmd_info(args: argparse.Namespace) -> int:
    """Show detailed plugin info."""
    try:
        plugin = minihost.Plugin(
            args.plugin,
            sample_rate=args.sample_rate,
            max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    # Basic info from probe
    try:
        info = minihost.probe(args.plugin)
        print(f"Name:      {info['name']}")
        print(f"Vendor:    {info['vendor']}")
        print(f"Version:   {info['version']}")
        print(f"Format:    {info['format']}")
        print(f"Unique ID: {info['unique_id']}")
        print(f"MIDI In:   {'yes' if info['accepts_midi'] else 'no'}")
        print(f"MIDI Out:  {'yes' if info['produces_midi'] else 'no'}")
    except Exception:
        pass

    print(f"\nRuntime Info:")
    print(f"  Sample Rate:  {plugin.sample_rate:.0f} Hz")
    print(f"  Parameters:   {plugin.num_params}")
    print(f"  Input Ch:     {plugin.num_input_channels}")
    print(f"  Output Ch:    {plugin.num_output_channels}")
    print(f"  Latency:      {plugin.latency_samples} samples")
    print(f"  Tail:         {plugin.tail_seconds:.3f} s")
    print(f"  Double Prec:  {'yes' if plugin.supports_double else 'no'}")

    # Bus info
    if plugin.num_input_buses > 0:
        print(f"\nInput Buses:")
        for i in range(plugin.num_input_buses):
            bus = plugin.get_bus_info(True, i)
            flags = "[main]" if bus['is_main'] else ""
            if not bus['is_enabled']:
                flags += " (disabled)"
            print(f"  [{i}] {bus['name']:<20}  {bus['num_channels']} ch  {flags}")

    if plugin.num_output_buses > 0:
        print(f"\nOutput Buses:")
        for i in range(plugin.num_output_buses):
            bus = plugin.get_bus_info(False, i)
            flags = "[main]" if bus['is_main'] else ""
            if not bus['is_enabled']:
                flags += " (disabled)"
            print(f"  [{i}] {bus['name']:<20}  {bus['num_channels']} ch  {flags}")

    # Factory presets
    if plugin.num_programs > 0:
        print(f"\nFactory Presets: {plugin.num_programs}")
        current = plugin.program
        for i in range(min(plugin.num_programs, 10)):
            name = plugin.get_program_name(i)
            marker = " (current)" if i == current else ""
            print(f"  [{i}] {name}{marker}")
        if plugin.num_programs > 10:
            print(f"  ... and {plugin.num_programs - 10} more")

    return 0


def cmd_params(args: argparse.Namespace) -> int:
    """List plugin parameters."""
    try:
        plugin = minihost.Plugin(
            args.plugin,
            sample_rate=args.sample_rate,
            max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    if args.json:
        import json
        params = []
        for i in range(plugin.num_params):
            info = plugin.get_param_info(i)
            info['index'] = i
            info['value'] = plugin.get_param(i)
            params.append(info)
        print(json.dumps(params, indent=2))
    else:
        print(f"Parameters ({plugin.num_params}):")
        for i in range(plugin.num_params):
            info = plugin.get_param_info(i)
            value = plugin.get_param(i)
            label = f" {info['label']}" if info['label'] else ""
            print(f"  [{i:3d}] {info['name']:<30} = {value:.4f}{label} ({info['current_value_str']})")

    return 0


def cmd_midi_ports(args: argparse.Namespace) -> int:
    """List available MIDI ports."""
    inputs = minihost.midi_get_input_ports()
    outputs = minihost.midi_get_output_ports()

    if args.json:
        import json
        print(json.dumps({'inputs': inputs, 'outputs': outputs}, indent=2))
    else:
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

    return 0


def cmd_play(args: argparse.Namespace) -> int:
    """Play plugin with real-time audio and MIDI."""
    try:
        plugin = minihost.Plugin(
            args.plugin,
            sample_rate=args.sample_rate,
            max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error loading plugin: {e}", file=sys.stderr)
        return 1

    print(f"Loaded: {args.plugin}")
    print(f"  Inputs: {plugin.num_input_channels}")
    print(f"  Outputs: {plugin.num_output_channels}")
    print(f"  Parameters: {plugin.num_params}")

    # Determine MIDI configuration
    midi_port = -1
    if args.midi is not None:
        midi_port = args.midi
        inputs = minihost.midi_get_input_ports()
        if midi_port >= len(inputs):
            print(f"Error: MIDI port {midi_port} not found. Use 'minihost midi-ports' to list.", file=sys.stderr)
            return 1
        print(f"  MIDI Input: [{midi_port}] {inputs[midi_port]['name']}")

    # Open audio device
    try:
        audio = minihost.AudioDevice(
            plugin,
            sample_rate=args.sample_rate,
            buffer_frames=args.block_size,
            midi_input_port=midi_port
        )
    except RuntimeError as e:
        print(f"Error opening audio device: {e}", file=sys.stderr)
        return 1

    print(f"\nAudio Device:")
    print(f"  Sample rate: {audio.sample_rate:.0f} Hz")
    print(f"  Buffer: {audio.buffer_frames} frames")
    print(f"  Channels: {audio.channels}")

    # Create virtual MIDI port if requested
    if args.virtual_midi:
        try:
            audio.create_virtual_midi_input(args.virtual_midi)
            print(f"  Virtual MIDI: '{args.virtual_midi}'")
        except RuntimeError as e:
            print(f"Warning: Could not create virtual MIDI port: {e}", file=sys.stderr)

    # Setup signal handler
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

    if args.midi is None and not args.virtual_midi:
        print("(No MIDI input. Use --midi N or --virtual-midi NAME)")

    try:
        while running:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass

    audio.stop()
    print("Done.")
    return 0


def cmd_render(args: argparse.Namespace) -> int:
    """Render MIDI file through plugin."""
    import numpy as np

    # Load plugin
    try:
        plugin = minihost.Plugin(
            args.plugin,
            sample_rate=args.sample_rate,
            max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error loading plugin: {e}", file=sys.stderr)
        return 1

    # Load state if provided
    if args.state:
        try:
            with open(args.state, 'rb') as f:
                state_data = f.read()
            plugin.set_state(state_data)
            print(f"Loaded state from {args.state}")
        except Exception as e:
            print(f"Warning: Could not load state: {e}", file=sys.stderr)

    # Load preset if provided
    if args.preset is not None:
        if args.preset < 0 or args.preset >= plugin.num_programs:
            print(f"Error: Preset {args.preset} out of range (0-{plugin.num_programs-1})", file=sys.stderr)
            return 1
        plugin.program = args.preset
        print(f"Loaded preset [{args.preset}]: {plugin.get_program_name(args.preset)}")

    print(f"Plugin: {args.plugin}")
    print(f"  Sample rate: {plugin.sample_rate:.0f} Hz")
    print(f"  Channels: {plugin.num_output_channels}")

    # Render
    print(f"Rendering: {args.midi} -> {args.output}")

    try:
        renderer = minihost.MidiRenderer(
            plugin,
            args.midi,
            block_size=args.block_size,
            tail_seconds=args.tail
        )
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    print(f"  Duration: {renderer.duration_seconds:.2f}s ({renderer.total_samples} samples)")

    # Render with progress
    if args.output.lower().endswith('.wav'):
        # Use render_midi_to_file for WAV output
        try:
            samples = minihost.render_midi_to_file(
                plugin,
                args.midi,
                args.output,
                block_size=args.block_size,
                tail_seconds=args.tail,
                bit_depth=args.bit_depth
            )
            print(f"Wrote {samples} samples to {args.output}")
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            return 1
    else:
        # Raw float32 output
        try:
            audio = minihost.render_midi(
                plugin,
                args.midi,
                block_size=args.block_size,
                tail_seconds=args.tail
            )
            # Save as raw float32 interleaved
            interleaved = audio.T.flatten()
            interleaved.tofile(args.output)
            print(f"Wrote {audio.shape[1]} samples to {args.output} (raw float32)")
        except Exception as e:
            print(f"Error: {e}", file=sys.stderr)
            return 1

    return 0


def cmd_process(args: argparse.Namespace) -> int:
    """Process audio file through plugin (offline)."""
    import numpy as np

    # Load plugin
    try:
        plugin = minihost.Plugin(
            args.plugin,
            sample_rate=args.sample_rate,
            max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error loading plugin: {e}", file=sys.stderr)
        return 1

    # Load state if provided
    if args.state:
        try:
            with open(args.state, 'rb') as f:
                state_data = f.read()
            plugin.set_state(state_data)
            print(f"Loaded state from {args.state}")
        except Exception as e:
            print(f"Warning: Could not load state: {e}", file=sys.stderr)

    in_ch = max(plugin.num_input_channels, 2)
    out_ch = max(plugin.num_output_channels, 2)

    # Read input (raw float32 interleaved)
    try:
        input_data = np.fromfile(args.input, dtype=np.float32)
    except Exception as e:
        print(f"Error reading input: {e}", file=sys.stderr)
        return 1

    total_samples = len(input_data) // in_ch
    input_data = input_data[:total_samples * in_ch].reshape(-1, in_ch).T  # (channels, samples)

    print(f"Processing {total_samples} samples ({in_ch} -> {out_ch} channels)")

    # Process in blocks
    output_blocks = []
    for start in range(0, total_samples, args.block_size):
        end = min(start + args.block_size, total_samples)
        block_size = end - start

        in_block = input_data[:, start:end].astype(np.float32)
        out_block = np.zeros((out_ch, block_size), dtype=np.float32)

        if args.double and plugin.supports_double:
            plugin.process_double(in_block.astype(np.float64),
                                 out_block.view(np.float64))
        else:
            plugin.process(in_block, out_block)

        output_blocks.append(out_block)

    output_data = np.concatenate(output_blocks, axis=1)

    # Write output (raw float32 interleaved)
    interleaved = output_data.T.flatten()
    interleaved.tofile(args.output)

    print(f"Wrote {output_data.shape[1]} samples to {args.output}")
    return 0


def main():
    parser = argparse.ArgumentParser(
        prog='minihost',
        description='Audio plugin hosting CLI',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  minihost probe /path/to/plugin.vst3
  minihost scan /Library/Audio/Plug-Ins/VST3/
  minihost info /path/to/plugin.vst3
  minihost params /path/to/plugin.vst3
  minihost midi-ports
  minihost play /path/to/synth.vst3 --midi 0
  minihost play /path/to/synth.vst3 --virtual-midi "My Synth"
  minihost render /path/to/synth.vst3 song.mid output.wav
  minihost render /path/to/synth.vst3 song.mid output.wav --preset 5
  minihost process /path/to/effect.vst3 input.raw output.raw
"""
    )

    # Global options
    parser.add_argument('-r', '--sample-rate', type=float, default=48000,
                       help='Sample rate in Hz (default: 48000)')
    parser.add_argument('-b', '--block-size', type=int, default=512,
                       help='Block size in samples (default: 512)')

    subparsers = parser.add_subparsers(dest='command', help='Commands')

    # probe
    probe_p = subparsers.add_parser('probe', help='Get plugin metadata')
    probe_p.add_argument('plugin', help='Path to plugin')
    probe_p.add_argument('-j', '--json', action='store_true', help='Output as JSON')
    probe_p.set_defaults(func=cmd_probe)

    # scan
    scan_p = subparsers.add_parser('scan', help='Scan directory for plugins')
    scan_p.add_argument('directory', help='Directory to scan')
    scan_p.add_argument('-j', '--json', action='store_true', help='Output as JSON')
    scan_p.set_defaults(func=cmd_scan)

    # info
    info_p = subparsers.add_parser('info', help='Show detailed plugin info')
    info_p.add_argument('plugin', help='Path to plugin')
    info_p.set_defaults(func=cmd_info)

    # params
    params_p = subparsers.add_parser('params', help='List plugin parameters')
    params_p.add_argument('plugin', help='Path to plugin')
    params_p.add_argument('-j', '--json', action='store_true', help='Output as JSON')
    params_p.set_defaults(func=cmd_params)

    # midi-ports
    midi_p = subparsers.add_parser('midi-ports', help='List available MIDI ports')
    midi_p.add_argument('-j', '--json', action='store_true', help='Output as JSON')
    midi_p.set_defaults(func=cmd_midi_ports)

    # play
    play_p = subparsers.add_parser('play', help='Play plugin with real-time audio/MIDI')
    play_p.add_argument('plugin', help='Path to plugin')
    play_p.add_argument('-m', '--midi', type=int, metavar='N',
                       help='Connect to MIDI input port N')
    play_p.add_argument('-v', '--virtual-midi', type=str, metavar='NAME',
                       help='Create virtual MIDI input with NAME')
    play_p.set_defaults(func=cmd_play)

    # render
    render_p = subparsers.add_parser('render', help='Render MIDI file through plugin')
    render_p.add_argument('plugin', help='Path to plugin (synth/instrument)')
    render_p.add_argument('midi', help='Input MIDI file')
    render_p.add_argument('output', help='Output audio file (.wav or raw)')
    render_p.add_argument('-s', '--state', help='Load plugin state from file')
    render_p.add_argument('-p', '--preset', type=int, metavar='N',
                         help='Load factory preset N')
    render_p.add_argument('-t', '--tail', type=float, default=2.0,
                         help='Tail length in seconds (default: 2.0)')
    render_p.add_argument('--bit-depth', type=int, default=24, choices=[16, 24, 32],
                         help='WAV bit depth (default: 24)')
    render_p.set_defaults(func=cmd_render)

    # process
    process_p = subparsers.add_parser('process', help='Process audio file offline')
    process_p.add_argument('plugin', help='Path to plugin (effect)')
    process_p.add_argument('input', help='Input audio file (raw float32)')
    process_p.add_argument('output', help='Output audio file (raw float32)')
    process_p.add_argument('-s', '--state', help='Load plugin state from file')
    process_p.add_argument('-d', '--double', action='store_true',
                          help='Use double precision if supported')
    process_p.set_defaults(func=cmd_process)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
