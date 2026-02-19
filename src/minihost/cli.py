#!/usr/bin/env python3
"""
minihost - Audio plugin hosting CLI.

A command-line interface for loading, inspecting, and rendering audio through
VST3, AudioUnit, and LV2 plugins.

Usage:
    minihost info /path/to/plugin.vst3
    minihost scan /path/to/plugins/
    minihost play /path/to/synth.vst3 --midi 0
    minihost process /path/to/effect.vst3 -i input.wav -o output.wav
    minihost process /path/to/synth.vst3 -m song.mid -o output.wav
    minihost midi
"""

from __future__ import annotations

import argparse
import os
import signal
import sys
import time

import minihost


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


def _print_probe_info(info: dict, json_output: bool = False) -> None:
    """Print probe-level plugin metadata."""
    if json_output:
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


def cmd_info(args: argparse.Namespace) -> int:
    """Show plugin info. With --probe, uses lightweight metadata only."""
    # Probe-only mode: no full load
    if args.probe:
        try:
            info = minihost.probe(args.plugin)
        except RuntimeError as e:
            print(f"Error: {e}", file=sys.stderr)
            return 1
        _print_probe_info(info, json_output=args.json)
        return 0

    # Full mode: load plugin for runtime details
    try:
        plugin = minihost.Plugin(
            args.plugin, sample_rate=args.sample_rate, max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    # Basic info from probe
    try:
        info = minihost.probe(args.plugin)
        if args.json:
            # In JSON mode, merge probe + runtime into one object
            import json

            info["sample_rate"] = plugin.sample_rate
            info["num_params"] = plugin.num_params
            info["num_input_channels"] = plugin.num_input_channels
            info["num_output_channels"] = plugin.num_output_channels
            info["latency_samples"] = plugin.latency_samples
            info["tail_seconds"] = plugin.tail_seconds
            info["supports_double"] = plugin.supports_double
            info["num_programs"] = plugin.num_programs
            print(json.dumps(info, indent=2))
            return 0
        _print_probe_info(info)
    except Exception:
        pass

    print("\nRuntime Info:")
    print(f"  Sample Rate:  {plugin.sample_rate:.0f} Hz")
    print(f"  Parameters:   {plugin.num_params}")
    print(f"  Input Ch:     {plugin.num_input_channels}")
    print(f"  Output Ch:    {plugin.num_output_channels}")
    print(f"  Latency:      {plugin.latency_samples} samples")
    print(f"  Tail:         {plugin.tail_seconds:.3f} s")
    print(f"  Double Prec:  {'yes' if plugin.supports_double else 'no'}")

    # Bus info
    if plugin.num_input_buses > 0:
        print("\nInput Buses:")
        for i in range(plugin.num_input_buses):
            bus = plugin.get_bus_info(True, i)
            flags = "[main]" if bus["is_main"] else ""
            if not bus["is_enabled"]:
                flags += " (disabled)"
            print(f"  [{i}] {bus['name']:<20}  {bus['num_channels']} ch  {flags}")

    if plugin.num_output_buses > 0:
        print("\nOutput Buses:")
        for i in range(plugin.num_output_buses):
            bus = plugin.get_bus_info(False, i)
            flags = "[main]" if bus["is_main"] else ""
            if not bus["is_enabled"]:
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
            args.plugin, sample_rate=args.sample_rate, max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    if args.json:
        import json

        params = []
        for i in range(plugin.num_params):
            info = plugin.get_param_info(i)
            info["index"] = i
            info["value"] = plugin.get_param(i)
            params.append(info)
        print(json.dumps(params, indent=2))
    elif args.verbose:
        print(f"Parameters ({plugin.num_params}):")
        for i in range(plugin.num_params):
            info = plugin.get_param_info(i)
            value = plugin.get_param(i)

            # Build range string
            min_text = plugin.param_to_text(i, 0.0)
            max_text = plugin.param_to_text(i, 1.0)
            default_val = info.get("default_value", 0.0)
            default_text = plugin.param_to_text(i, default_val)

            label = f" {info['label']}" if info["label"] else ""
            print(f"  [{i:3d}] {info['name']}")
            print(f"         Value:   {value:.4f}{label} ({info['current_value_str']})")
            print(f"         Range:   {min_text} .. {max_text}")
            print(f"         Default: {default_val:.4f} ({default_text})")
            flags = []
            if info.get("is_automatable"):
                flags.append("automatable")
            if info.get("is_discrete"):
                flags.append("discrete")
                num_steps = info.get("num_steps")
                if num_steps is not None:
                    flags.append(f"{num_steps} steps")
            if flags:
                print(f"         Flags:   {', '.join(flags)}")
    else:
        print(f"Parameters ({plugin.num_params}):")
        for i in range(plugin.num_params):
            info = plugin.get_param_info(i)
            value = plugin.get_param(i)
            label = f" {info['label']}" if info["label"] else ""
            print(
                f"  [{i:3d}] {info['name']:<30} = {value:.4f}{label} ({info['current_value_str']})"
            )

    return 0


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def _note_name(note_num: int) -> str:
    """Convert MIDI note number to name like C4, D#5."""
    octave = (note_num // 12) - 1
    name = NOTE_NAMES[note_num % 12]
    return f"{name}{octave}"


def _format_midi_msg(timestamp: float, data: bytes) -> str:
    """Format a raw MIDI message as a human-readable one-liner.

    Args:
        timestamp: time in seconds since monitoring started
        data: raw MIDI bytes

    Returns:
        Formatted string for display
    """
    if len(data) == 0:
        return f"{timestamp:7.3f}  (empty)"

    status = data[0]

    # SysEx
    if status == 0xF0:
        hex_str = " ".join(f"{b:02X}" for b in data)
        return f"{timestamp:7.3f}  SysEx          {hex_str}"

    # System real-time / common (single-byte or non-channel)
    if status >= 0xF0:
        hex_str = " ".join(f"{b:02X}" for b in data)
        return f"{timestamp:7.3f}  System         {hex_str}"

    msg_type = status & 0xF0
    channel = (status & 0x0F) + 1

    if msg_type == 0x90 and len(data) >= 3:
        note = data[1]
        vel = data[2]
        if vel == 0:
            return (
                f"{timestamp:7.3f}  Note Off      ch={channel:<2} "
                f"note={_note_name(note)} ({note})  vel=0"
            )
        return (
            f"{timestamp:7.3f}  Note On       ch={channel:<2} "
            f"note={_note_name(note)} ({note})  vel={vel}"
        )

    if msg_type == 0x80 and len(data) >= 3:
        note = data[1]
        vel = data[2]
        return (
            f"{timestamp:7.3f}  Note Off      ch={channel:<2} "
            f"note={_note_name(note)} ({note})  vel={vel}"
        )

    if msg_type == 0xB0 and len(data) >= 3:
        cc = data[1]
        val = data[2]
        return f"{timestamp:7.3f}  CC            ch={channel:<2} cc={cc:<3} val={val}"

    if msg_type == 0xE0 and len(data) >= 3:
        val = data[1] | (data[2] << 7)
        return f"{timestamp:7.3f}  Pitch Bend    ch={channel:<2} val={val}"

    if msg_type == 0xC0 and len(data) >= 2:
        prog = data[1]
        return f"{timestamp:7.3f}  Program       ch={channel:<2} prog={prog}"

    if msg_type == 0xD0 and len(data) >= 2:
        pressure = data[1]
        return f"{timestamp:7.3f}  Ch Pressure   ch={channel:<2} val={pressure}"

    if msg_type == 0xA0 and len(data) >= 3:
        note = data[1]
        pressure = data[2]
        return (
            f"{timestamp:7.3f}  Poly AT       ch={channel:<2} "
            f"note={_note_name(note)} ({note})  val={pressure}"
        )

    # Unknown
    hex_str = " ".join(f"{b:02X}" for b in data)
    return f"{timestamp:7.3f}  Unknown        {hex_str}"


def cmd_midi(args: argparse.Namespace) -> int:
    """List available MIDI ports, or monitor a port."""

    # Monitor mode
    if args.monitor is not None or args.virtual_midi is not None:
        return _cmd_midi_monitor(args)

    # List mode (original behavior)
    inputs = minihost.midi_get_input_ports()
    outputs = minihost.midi_get_output_ports()

    if args.json:
        import json

        print(json.dumps({"inputs": inputs, "outputs": outputs}, indent=2))
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


def _cmd_midi_monitor(args: argparse.Namespace) -> int:
    """Monitor incoming MIDI messages on a port."""
    import queue

    msg_queue: queue.Queue[tuple[float, bytes]] = queue.Queue()
    start_time = time.monotonic()

    def on_midi(data: bytes) -> None:
        elapsed = time.monotonic() - start_time
        msg_queue.put((elapsed, data))

    # Open MIDI input
    try:
        if args.virtual_midi is not None:
            midi_in = minihost.MidiIn.open_virtual(args.virtual_midi, on_midi)
            print(f'Listening on virtual port "{args.virtual_midi}"... Ctrl+C to stop.')
        else:
            port_index = args.monitor
            inputs = minihost.midi_get_input_ports()
            if port_index < 0 or port_index >= len(inputs):
                print(
                    f"Error: MIDI port {port_index} not found. "
                    f"Use 'minihost midi' to list ports.",
                    file=sys.stderr,
                )
                return 1
            port_name = inputs[port_index]["name"]
            midi_in = minihost.MidiIn.open(port_index, on_midi)
            print(f'Listening on port [{port_index}] "{port_name}"... Ctrl+C to stop.')
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    # Monitor loop
    running = True

    def on_signal(sig, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    try:
        while running:
            try:
                timestamp, data = msg_queue.get(timeout=0.1)
                print(f"  {_format_midi_msg(timestamp, data)}")
            except queue.Empty:
                pass
    except KeyboardInterrupt:
        pass
    finally:
        midi_in.close()

    print("\nDone.")
    return 0


def cmd_play(args: argparse.Namespace) -> int:
    """Play plugin with real-time audio and MIDI."""
    try:
        plugin = minihost.Plugin(
            args.plugin, sample_rate=args.sample_rate, max_block_size=args.block_size
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
            print(
                f"Error: MIDI port {midi_port} not found. Use 'minihost midi' to list.",
                file=sys.stderr,
            )
            return 1
        print(f"  MIDI Input: [{midi_port}] {inputs[midi_port]['name']}")

    # Open audio device
    try:
        audio = minihost.AudioDevice(
            plugin,
            sample_rate=args.sample_rate,
            buffer_frames=args.block_size,
            midi_input_port=midi_port,
        )
    except RuntimeError as e:
        print(f"Error opening audio device: {e}", file=sys.stderr)
        return 1

    print("\nAudio Device:")
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
    print("\nPlaying. Press Ctrl+C to stop.")

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


def _load_vstpreset(plugin, path):
    """Load a .vstpreset file into a plugin, with user-facing error handling."""
    from minihost.vstpreset import load_vstpreset

    load_vstpreset(path, plugin)


def _load_midi_events(midi_path, sample_rate):
    """Load MIDI file and convert events to sample-positioned tuples.

    Returns (events_by_sample, total_midi_samples) where events_by_sample
    is a sorted list of (sample_pos, status, data1, data2) tuples.
    """
    from minihost.render import (
        _build_tempo_map,
        _collect_midi_events,
        _event_to_midi_tuple,
        _seconds_to_samples,
        _tick_to_seconds,
    )

    mf = minihost.MidiFile()
    if not mf.load(midi_path):
        raise RuntimeError(f"Failed to load MIDI file: {midi_path}")

    tpq = mf.ticks_per_quarter
    tempo_map = _build_tempo_map(mf)
    all_events = _collect_midi_events(mf)

    # Convert to sample positions
    result = []
    max_sample = 0
    for event in all_events:
        tick = event["tick"]
        seconds = _tick_to_seconds(tick, tempo_map, tpq)
        sample_pos = _seconds_to_samples(seconds, sample_rate)
        midi_tuple = _event_to_midi_tuple(event, sample_pos)
        if midi_tuple:
            result.append(midi_tuple)
            max_sample = max(max_sample, sample_pos)

    result.sort(key=lambda x: x[0])
    return result, max_sample


def cmd_process(args: argparse.Namespace) -> int:
    """Process audio file through plugin (offline)."""
    import numpy as np

    from minihost.audio_io import get_audio_info, read_audio, write_audio
    from minihost.automation import parse_automation_file, parse_param_arg

    # Check output doesn't exist (unless --overwrite)
    if os.path.exists(args.output) and not args.overwrite:
        print(
            f"Error: Output file '{args.output}' already exists. "
            f"Use -y/--overwrite to overwrite.",
            file=sys.stderr,
        )
        return 1

    # --- Determine sample rate and read inputs ---
    input_files = args.input or []
    midi_path = args.midi_input
    has_audio_input = len(input_files) > 0
    has_midi_input = midi_path is not None

    if not has_audio_input and not has_midi_input:
        print(
            "Error: At least one of --input or --midi-input is required.",
            file=sys.stderr,
        )
        return 1

    # Detect sample rate from first audio input, or use CLI default
    detected_sample_rate = args.sample_rate
    if has_audio_input:
        try:
            first_info = get_audio_info(input_files[0])
            detected_sample_rate = first_info["sample_rate"]
        except Exception as e:
            print(f"Error reading input file: {e}", file=sys.stderr)
            return 1

    sample_rate = int(detected_sample_rate)

    # Read all audio inputs
    audio_inputs = []
    if has_audio_input:
        for i, inp_path in enumerate(input_files):
            try:
                data, sr = read_audio(inp_path)
            except Exception as e:
                print(f"Error reading input '{inp_path}': {e}", file=sys.stderr)
                return 1
            if sr != sample_rate:
                print(
                    f"Error: Sample rate mismatch: '{inp_path}' is {sr} Hz, "
                    f"expected {sample_rate} Hz (from first input).",
                    file=sys.stderr,
                )
                return 1
            audio_inputs.append(data)

    # --- Load plugin ---
    main_in_ch = audio_inputs[0].shape[0] if audio_inputs else 2
    sidechain_in = audio_inputs[1] if len(audio_inputs) > 1 else None
    sidechain_ch = sidechain_in.shape[0] if sidechain_in is not None else 0
    out_channels = args.out_channels

    try:
        plugin = minihost.Plugin(
            args.plugin,
            sample_rate=sample_rate,
            max_block_size=args.block_size,
            in_channels=main_in_ch,
            out_channels=out_channels if out_channels else 2,
            sidechain_channels=sidechain_ch,
        )
    except RuntimeError as e:
        print(f"Error loading plugin: {e}", file=sys.stderr)
        return 1

    out_ch = (
        args.out_channels if args.out_channels else max(plugin.num_output_channels, 2)
    )

    # --- Load state / preset ---
    if args.state:
        try:
            with open(args.state, "rb") as f:
                state_data = f.read()
            plugin.set_state(state_data)
            print(f"Loaded state from {args.state}")
        except Exception as e:
            print(f"Warning: Could not load state: {e}", file=sys.stderr)

    if args.vstpreset:
        try:
            _load_vstpreset(plugin, args.vstpreset)
            print(f"Loaded .vstpreset from {args.vstpreset}")
        except (FileNotFoundError, ValueError, RuntimeError) as e:
            print(f"Error loading .vstpreset: {e}", file=sys.stderr)
            return 1

    if args.preset is not None:
        if args.preset < 0 or args.preset >= plugin.num_programs:
            print(
                f"Error: Preset {args.preset} out of range (0-{plugin.num_programs - 1})",
                file=sys.stderr,
            )
            return 1
        plugin.program = args.preset
        print(f"Loaded preset [{args.preset}]: {plugin.get_program_name(args.preset)}")

    # --- Non-realtime mode ---
    if args.non_realtime:
        plugin.non_realtime = True

    # --- Determine total length ---
    midi_events = []
    if has_midi_input:
        try:
            midi_events, midi_max_sample = _load_midi_events(midi_path, sample_rate)
        except RuntimeError as e:
            print(f"Error loading MIDI: {e}", file=sys.stderr)
            return 1

    if has_audio_input:
        total_samples = audio_inputs[0].shape[1]
    elif has_midi_input:
        # Synth mode: MIDI only, add tail for reverb/delay decay
        tail_seconds = args.tail
        total_samples = midi_max_sample + int(tail_seconds * sample_rate)
    else:
        total_samples = 0

    if total_samples == 0:
        print("Error: No audio or MIDI input data to process.", file=sys.stderr)
        return 1

    # --- Parse automation ---
    param_changes = []

    # From automation file
    if args.param_file:
        try:
            param_changes = parse_automation_file(
                args.param_file,
                plugin,
                sample_rate,
                total_samples,
                block_size=args.block_size,
            )
        except (FileNotFoundError, ValueError) as e:
            print(f"Error loading automation file: {e}", file=sys.stderr)
            return 1

    # From --param CLI args (these override / add to file)
    param_overrides = {}
    if args.param:
        for param_str in args.param:
            try:
                param_idx, value = parse_param_arg(param_str, plugin)
            except ValueError as e:
                print(f"Error parsing --param: {e}", file=sys.stderr)
                return 1
            if param_idx in param_overrides:
                print(
                    f"Warning: --param overrides previous value for parameter index {param_idx}",
                    file=sys.stderr,
                )
            param_overrides[param_idx] = value

    # Apply CLI param overrides: set as static values at sample 0.
    # If automation file also sets the same param, warn and remove those entries.
    if param_overrides:
        overridden_indices = set(param_overrides.keys())
        file_params_overridden = {
            idx for (_, idx, _) in param_changes if idx in overridden_indices
        }
        if file_params_overridden:
            print(
                f"Warning: --param overrides automation file for "
                f"{len(file_params_overridden)} parameter(s)",
                file=sys.stderr,
            )
            param_changes = [
                (s, i, v) for (s, i, v) in param_changes if i not in overridden_indices
            ]
        for param_idx, value in param_overrides.items():
            param_changes.append((0, param_idx, value))
        param_changes.sort(key=lambda x: x[0])

    # --- Set transport ---
    if args.bpm:
        plugin.set_transport(bpm=args.bpm, is_playing=True)

    # --- Print summary ---
    latency = plugin.latency_samples
    print(f"Plugin: {args.plugin}")
    print(f"  Sample rate: {sample_rate} Hz")
    print(f"  Block size:  {args.block_size}")
    print(f"  Latency:     {latency} samples")
    if has_audio_input:
        print(f"  Input:       {main_in_ch} ch, {total_samples} samples")
        if sidechain_in is not None:
            print(f"  Sidechain:   {sidechain_ch} ch")
    if has_midi_input:
        print(f"  MIDI events: {len(midi_events)}")
    if param_changes:
        print(f"  Automation:  {len(param_changes)} param change(s)")
    print(f"  Output:      {out_ch} ch -> {args.output}")

    # --- Process loop ---
    block_size = args.block_size
    has_automation = len(param_changes) > 0
    has_sidechain = sidechain_in is not None

    # Pre-allocate output buffer (total_samples + latency for compensation)
    output_total = total_samples + latency
    output_data = np.zeros((out_ch, output_total), dtype=np.float32)

    # Prepare input data (zero-padded if needed)
    if has_audio_input:
        main_input = audio_inputs[0]
        # Pad to output_total length
        if main_input.shape[1] < output_total:
            padded = np.zeros((main_in_ch, output_total), dtype=np.float32)
            padded[:, : main_input.shape[1]] = main_input
            main_input = padded
    else:
        main_input = np.zeros((main_in_ch, output_total), dtype=np.float32)

    if has_sidechain:
        assert sidechain_in is not None
        if sidechain_in.shape[1] < output_total:
            sc_padded = np.zeros((sidechain_ch, output_total), dtype=np.float32)
            sc_padded[:, : sidechain_in.shape[1]] = sidechain_in
            sidechain_in = sc_padded

    # Build per-block event indices for MIDI and automation
    midi_idx = 0
    auto_idx = 0

    for start in range(0, output_total, block_size):
        end = min(start + block_size, output_total)
        bsize = end - start

        in_block = main_input[:, start:end].copy()
        out_block = np.zeros((out_ch, bsize), dtype=np.float32)

        # Collect MIDI events for this block
        block_midi = []
        while midi_idx < len(midi_events):
            sample_pos = midi_events[midi_idx][0]
            if sample_pos >= end:
                break
            offset = max(0, min(sample_pos - start, bsize - 1))
            ev = midi_events[midi_idx]
            block_midi.append((offset, ev[1], ev[2], ev[3]))
            midi_idx += 1

        # Collect automation changes for this block
        block_auto = []
        while auto_idx < len(param_changes):
            sample_pos = param_changes[auto_idx][0]
            if sample_pos >= end:
                break
            offset = max(0, min(sample_pos - start, bsize - 1))
            ac = param_changes[auto_idx]
            block_auto.append((offset, ac[1], ac[2]))
            auto_idx += 1

        # Choose processing path
        if has_sidechain:
            assert sidechain_in is not None
            sc_block = sidechain_in[:, start:end].copy()
            # Apply any static param changes before processing
            for _, param_idx, value in block_auto:
                plugin.set_param(param_idx, value)
            plugin.process_sidechain(in_block, out_block, sc_block)
        elif has_automation or block_auto:
            plugin.process_auto(in_block, out_block, block_midi, block_auto)
        elif block_midi:
            plugin.process_midi(in_block, out_block, block_midi)
        else:
            plugin.process(in_block, out_block)

        output_data[:, start:end] = out_block

    # --- Latency compensation ---
    if latency > 0:
        output_data = output_data[:, latency:]
    else:
        output_data = output_data[:, :total_samples]

    # Trim to original length
    output_data = output_data[:, :total_samples]

    # --- Determine bit depth ---
    bit_depth = args.bit_depth
    if bit_depth is None:
        # Try to match input bit depth
        if has_audio_input:
            try:
                in_info = get_audio_info(input_files[0])
                subtype = in_info.get("subtype", "")
                if "16" in subtype:
                    bit_depth = 16
                elif "32" in subtype or "FLOAT" in subtype:
                    bit_depth = 32
                else:
                    bit_depth = 24
            except Exception:
                bit_depth = 24
        else:
            bit_depth = 24

    # --- Write output ---
    try:
        write_audio(args.output, output_data, sample_rate, bit_depth=bit_depth)
    except Exception as e:
        print(f"Error writing output: {e}", file=sys.stderr)
        return 1

    duration = total_samples / sample_rate
    print(f"Wrote {total_samples} samples ({duration:.2f}s) to {args.output}")
    return 0


def main():
    parser = argparse.ArgumentParser(
        prog="minihost",
        description="Audio plugin hosting CLI",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  minihost info /path/to/plugin.vst3
  minihost info /path/to/plugin.vst3 --probe
  minihost info /path/to/plugin.vst3 --json
  minihost scan /Library/Audio/Plug-Ins/VST3/
  minihost params /path/to/plugin.vst3
  minihost params /path/to/plugin.vst3 --verbose
  minihost midi
  minihost midi -m 0
  minihost play /path/to/synth.vst3 --midi 0
  minihost play /path/to/synth.vst3 --virtual-midi "My Synth"
  minihost process /path/to/effect.vst3 -i input.wav -o output.wav
  minihost process /path/to/effect.vst3 -i input.wav -o output.wav --param "Mix:0.5"
  minihost process /path/to/synth.vst3 -m song.mid -o output.wav --tail 3.0
""",
    )

    # Global options
    parser.add_argument(
        "-r",
        "--sample-rate",
        type=float,
        default=48000,
        help="Sample rate in Hz (default: 48000)",
    )
    parser.add_argument(
        "-b",
        "--block-size",
        type=int,
        default=512,
        help="Block size in samples (default: 512)",
    )

    subparsers = parser.add_subparsers(dest="command", help="Commands")

    # scan
    scan_p = subparsers.add_parser("scan", help="Scan directory for plugins")
    scan_p.add_argument("directory", help="Directory to scan")
    scan_p.add_argument("-j", "--json", action="store_true", help="Output as JSON")
    scan_p.set_defaults(func=cmd_scan)

    # info
    info_p = subparsers.add_parser("info", help="Show plugin info")
    info_p.add_argument("plugin", help="Path to plugin")
    info_p.add_argument("-j", "--json", action="store_true", help="Output as JSON")
    info_p.add_argument(
        "--probe",
        action="store_true",
        help="Lightweight mode: metadata only, no full plugin load",
    )
    info_p.set_defaults(func=cmd_info)

    # params
    params_p = subparsers.add_parser("params", help="List plugin parameters")
    params_p.add_argument("plugin", help="Path to plugin")
    params_p.add_argument("-j", "--json", action="store_true", help="Output as JSON")
    params_p.add_argument(
        "-V",
        "--verbose",
        action="store_true",
        help="Show extended info (ranges, defaults, flags)",
    )
    params_p.set_defaults(func=cmd_params)

    # midi
    midi_p = subparsers.add_parser("midi", help="List or monitor MIDI ports")
    midi_p.add_argument("-j", "--json", action="store_true", help="Output as JSON")
    midi_p.add_argument(
        "-m",
        "--monitor",
        type=int,
        metavar="N",
        default=None,
        help="Monitor MIDI input port N",
    )
    midi_p.add_argument(
        "--virtual-midi",
        type=str,
        metavar="NAME",
        default=None,
        help="Create virtual MIDI input port and monitor it",
    )
    midi_p.set_defaults(func=cmd_midi)

    # play
    play_p = subparsers.add_parser("play", help="Play plugin with real-time audio/MIDI")
    play_p.add_argument("plugin", help="Path to plugin")
    play_p.add_argument(
        "-m", "--midi", type=int, metavar="N", help="Connect to MIDI input port N"
    )
    play_p.add_argument(
        "-v",
        "--virtual-midi",
        type=str,
        metavar="NAME",
        help="Create virtual MIDI input with NAME",
    )
    play_p.set_defaults(func=cmd_play)

    # process
    process_p = subparsers.add_parser(
        "process",
        help="Process audio through plugin (offline)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Basic effect processing
  minihost process /path/to/reverb.vst3 -i input.wav -o output.wav

  # With parameter control
  minihost process /path/to/effect.vst3 -i input.wav -o output.wav \\
    --param "Mix:0.5" --param "Feedback:0.7"

  # With JSON automation file
  minihost process /path/to/effect.vst3 -i input.wav -o output.wav \\
    --param-file automation.json

  # Render synth with MIDI
  minihost process /path/to/synth.vst3 -m song.mid -o output.wav --tail 3.0

  # Sidechain processing (second -i is sidechain)
  minihost process /path/to/compressor.vst3 -i main.wav -i sidechain.wav -o output.wav
""",
    )
    process_p.add_argument("plugin", help="Path to plugin")
    process_p.add_argument(
        "-o",
        "--output",
        required=True,
        help="Output audio file path",
    )
    process_p.add_argument(
        "-i",
        "--input",
        action="append",
        metavar="FILE",
        help="Input audio file (repeatable; second input = sidechain)",
    )
    process_p.add_argument(
        "-m",
        "--midi-input",
        metavar="FILE",
        help="Input MIDI file",
    )
    process_p.add_argument(
        "-t",
        "--tail",
        type=float,
        default=2.0,
        help="Tail length in seconds after MIDI ends (default: 2.0, MIDI-only mode)",
    )
    process_p.add_argument("-s", "--state", help="Load plugin state from file")
    process_p.add_argument("--vstpreset", metavar="FILE", help="Load .vstpreset file")
    process_p.add_argument(
        "-p", "--preset", type=int, metavar="N", help="Load factory preset N"
    )
    process_p.add_argument(
        "--param-file",
        metavar="FILE",
        help="JSON automation file",
    )
    process_p.add_argument(
        "--param",
        action="append",
        metavar="SPEC",
        help='Set parameter: "Name:value" or "Name:value:n" (repeatable)',
    )
    process_p.add_argument(
        "-y",
        "--overwrite",
        action="store_true",
        help="Overwrite output file if it exists",
    )
    process_p.add_argument(
        "--bit-depth",
        type=int,
        default=None,
        choices=[16, 24, 32],
        help="Output bit depth (default: match input or 24)",
    )
    process_p.add_argument(
        "--out-channels",
        type=int,
        default=None,
        metavar="N",
        help="Override output channel count",
    )
    process_p.add_argument(
        "--non-realtime",
        action="store_true",
        help="Enable non-realtime processing mode",
    )
    process_p.add_argument(
        "--bpm",
        type=float,
        default=None,
        help="Set transport BPM (for tempo-synced plugins)",
    )
    process_p.set_defaults(func=cmd_process)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
