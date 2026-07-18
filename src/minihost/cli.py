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
from pathlib import Path
import threading
from typing import Optional
import signal
import sys
import time

import minihost


class _ProgressBar:
    """Simple stderr progress bar driven by an (current, total) callback.

    Disabled when ``enabled`` is False. ``__call__`` matches the
    ``progress_callback`` signature used by
    :func:`minihost.process_audio` / :func:`minihost.render_midi_to_file`,
    so an instance can be passed directly.
    """

    def __init__(self, label: str, enabled: bool):
        self._label = label
        self._enabled = bool(enabled)
        self._last_pct = -1
        self._finished = False

    def __call__(self, current: int, total: int) -> None:
        if not self._enabled or total <= 0:
            return
        pct = min(100, int(100 * current / total))
        if pct == self._last_pct:
            return
        self._last_pct = pct
        bar_len = 30
        filled = int(bar_len * current / total)
        bar = "#" * filled + "-" * (bar_len - filled)
        sys.stderr.write(f"\r  {self._label} [{bar}] {pct:3d}%")
        sys.stderr.flush()

    def finish(self) -> None:
        if self._enabled and not self._finished:
            sys.stderr.write("\n")
            sys.stderr.flush()
            self._finished = True


def cmd_scan(args: argparse.Namespace) -> int:
    """Scan directory for plugins. Uses the persistent scan cache by
    default (only new/changed plugins are probed); --no-cache forces a
    full uncached scan and --refresh re-probes every plugin."""
    try:
        if args.no_cache:
            results = minihost.scan_directory(args.directory)
        else:
            from minihost import plugincache

            def _progress(done: int, total: int, _path: str) -> None:
                sys.stderr.write(f"\rProbing {done}/{total}...")
                sys.stderr.flush()
                if done == total:
                    sys.stderr.write("\r" + " " * 24 + "\r")
                    sys.stderr.flush()

            results = plugincache.scan(
                args.directory,
                refresh=args.refresh,
                on_progress=None if args.json else _progress,
            )
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


def cmd_cache(args: argparse.Namespace) -> int:
    """Manage and query the persistent plugin-scan cache."""
    from minihost import plugincache

    action = args.cache_action
    if action == "path":
        print(plugincache.cache_file())
        return 0

    if action == "stats":
        s = plugincache.stats()
        if args.json:
            import json

            print(json.dumps(s, indent=2))
        else:
            print(f"Cache:  {s['path']}")
            print(f"Exists: {'yes' if s['exists'] else 'no'}")
            print(f"Total:  {s['total']}  (ok: {s['ok']}, error: {s['error']})")
        return 0

    if action == "clear":
        plugincache.clear()
        print("Cache cleared.")
        return 0

    if action == "prune":
        n = plugincache.prune()
        print(f"Pruned {n} missing plugin(s).")
        return 0

    if action == "list":
        results = plugincache.query(
            format=args.format,
            name_contains=args.name,
            vendor_contains=args.vendor,
            accepts_midi=True if args.midi_in else None,
            produces_midi=True if args.midi_out else None,
        )
        if args.json:
            import json

            print(json.dumps(results, indent=2))
        else:
            for i, d in enumerate(results, 1):
                print(f"[{i}] {d['name']} ({d['format']}) - {d['path']}")
            print(f"\n{len(results)} plugin(s) in cache")
        return 0

    print(f"Error: unknown cache action {action!r}", file=sys.stderr)
    return 1


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
    # Probe-only mode: no full load. Served from the scan cache (probing +
    # caching on a miss / stale fingerprint) unless --no-cache is given.
    if args.probe:
        try:
            if args.no_cache:
                info = minihost.probe(args.plugin)
            else:
                from minihost import plugincache

                info = plugincache.info(args.plugin, refresh=args.refresh)
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


def cmd_presets(args: argparse.Namespace) -> int:
    """List plugin factory presets, and optionally save current state as .vstpreset.

    Modes:
      - Default: list all factory presets.
      - --save FILE: load the plugin (optionally applying --program, --state, or
        --vstpreset input), then write the current plugin state to FILE as a
        .vstpreset.
    """
    # Listing mode can use probe() to fetch class_id but still needs to load
    # the plugin for program names / count, so just load once.
    try:
        plugin = minihost.Plugin(
            args.plugin,
            sample_rate=args.sample_rate,
            max_block_size=args.block_size,
        )
    except RuntimeError as e:
        print(f"Error loading plugin: {e}", file=sys.stderr)
        return 1

    # Determine class_id for writing: either from an input .vstpreset (to
    # preserve identity) or auto-detected from the plugin bundle's
    # moduleinfo.json. There is no silent fallback -- if neither path
    # produces a real FUID we hard-error rather than write a broken file.
    class_id: str | None = None

    # Apply inputs, if any, before saving.
    if args.state:
        try:
            with open(args.state, "rb") as f:
                plugin.set_state(f.read())
        except (OSError, RuntimeError) as e:
            print(f"Error loading state '{args.state}': {e}", file=sys.stderr)
            return 1

    if args.load_vstpreset:
        try:
            from minihost.vstpreset import read_vstpreset

            preset = read_vstpreset(args.load_vstpreset)
            if preset.component_state is None:
                raise ValueError("preset has no component state")
            plugin.set_state(preset.component_state)
            class_id = preset.class_id
        except (FileNotFoundError, ValueError, RuntimeError) as e:
            print(
                f"Error loading .vstpreset '{args.load_vstpreset}': {e}",
                file=sys.stderr,
            )
            return 1

    if args.program is not None:
        if plugin.num_programs == 0:
            print("Error: plugin has no factory presets.", file=sys.stderr)
            return 1
        if args.program < 0 or args.program >= plugin.num_programs:
            print(
                f"Error: program {args.program} out of range "
                f"(0-{plugin.num_programs - 1})",
                file=sys.stderr,
            )
            return 1
        plugin.program = args.program

    # Save mode
    if args.save:
        if os.path.exists(args.save) and not args.overwrite:
            print(
                f"Error: Output file '{args.save}' already exists. "
                f"Use -y/--overwrite to overwrite.",
                file=sys.stderr,
            )
            return 1

        # Auto-detect class_id from the plugin bundle if we didn't inherit
        # one from --load-vstpreset.
        if class_id is None:
            try:
                from minihost.vstpreset import read_class_id_from_bundle

                class_id = read_class_id_from_bundle(args.plugin)
            except ValueError as e:
                print(
                    f"Error: cannot determine VST3 class_id for "
                    f"'{args.plugin}': {e}\n"
                    f"Use --load-vstpreset to inherit a class_id from an "
                    f"existing .vstpreset, or pre-build the file in code "
                    f"with an explicit class_id.",
                    file=sys.stderr,
                )
                return 1

        try:
            from minihost.vstpreset import save_vstpreset

            save_vstpreset(args.save, plugin, class_id=class_id)
        except (OSError, ValueError, RuntimeError) as e:
            print(f"Error writing '{args.save}': {e}", file=sys.stderr)
            return 1
        print(f"Wrote {args.save}")
        return 0

    # Listing mode
    if args.json:
        import json

        presets = []
        current = plugin.program
        for i in range(plugin.num_programs):
            presets.append(
                {
                    "index": i,
                    "name": plugin.get_program_name(i),
                    "is_current": i == current,
                }
            )
        print(json.dumps({"count": plugin.num_programs, "presets": presets}, indent=2))
        return 0

    if plugin.num_programs == 0:
        print(f"{args.plugin}: no factory presets")
        return 0

    print(f"Factory Presets: {plugin.num_programs}")
    current = plugin.program
    for i in range(plugin.num_programs):
        name = plugin.get_program_name(i)
        marker = " (current)" if i == current else ""
        print(f"  [{i}] {name}{marker}")
    return 0


def _morph_capture_source(
    plugin: "minihost.Plugin", program: int | None, state: str | None, label: str
) -> list[float]:
    """Resolve a snapshot source onto the plugin, then capture its params.

    ``state`` (a file path) takes precedence over ``program`` (a factory
    program index); if neither is given the plugin's current values are
    captured. Returns the normalized snapshot via the native
    ``Plugin.morph_capture`` binding.
    """
    if state:
        with open(state, "rb") as f:
            plugin.set_state(f.read())
    elif program is not None:
        if program < 0 or program >= plugin.num_programs:
            raise ValueError(
                f"snapshot {label} program {program} out of range "
                f"(plugin has {plugin.num_programs})"
            )
        plugin.program = program
    return plugin.morph_capture()


def cmd_morph(args: argparse.Namespace) -> int:
    """Interpolate between two parameter snapshots (A/B morph).

    Mirrors the ``morph`` command in the C/C++ front-ends: capture snapshots
    A and B from factory programs or saved state files, blend them at ``-t``,
    print an A/B/blend table (or JSON), and optionally apply and save the
    result. Uses the native ``Plugin.morph_capture`` / ``morph_apply``
    bindings and ``minihost.lerp_params`` for the interpolation.
    """
    try:
        plugin = minihost.Plugin(
            args.plugin, sample_rate=args.sample_rate, max_block_size=args.block_size
        )
    except RuntimeError as e:
        print(f"Error loading plugin: {e}", file=sys.stderr)
        return 1

    try:
        n = plugin.num_params
        if n == 0:
            print("Error: plugin has no parameters to morph.", file=sys.stderr)
            return 1

        a_program, b_program = args.a_program, args.b_program
        a_state, b_state = args.a_state, args.b_state

        # Default sources: factory programs 0 and 1 when nothing is given.
        if (
            a_program is None
            and b_program is None
            and a_state is None
            and b_state is None
        ):
            if plugin.num_programs >= 2:
                a_program, b_program = 0, 1
            else:
                print(
                    "Error: no snapshot sources given and plugin has < 2 "
                    "factory programs. Pass --a-program/--b-program or "
                    "--a-state/--b-state.",
                    file=sys.stderr,
                )
                return 1

        try:
            a = _morph_capture_source(plugin, a_program, a_state, "A")
            b = _morph_capture_source(plugin, b_program, b_state, "B")
        except (OSError, ValueError, RuntimeError) as e:
            print(f"Error: {e}", file=sys.stderr)
            return 1

        blend = minihost.lerp_params(a, b, args.blend)

        if args.json:
            import json

            out = {
                "blend": args.blend,
                "num_params": n,
                "params": [
                    {"index": i, "a": a[i], "b": b[i], "blend": blend[i]}
                    for i in range(n)
                ],
            }
            print(json.dumps(out, indent=2))
        else:
            print(
                f"Morph between A and B at t={args.blend:.3f} ({n} params)",
                file=sys.stderr,
            )
            print(f"{'idx':<4} {'name':<28} {'A':>9} {'B':>9} {'blend':>9}")
            for i in range(n):
                name = plugin.get_param_info(i)["name"]
                print(f"{i:<4} {name:<28} {a[i]:>9.4f} {b[i]:>9.4f} {blend[i]:>9.4f}")

        # Apply and optionally persist the morphed snapshot.
        if args.apply or args.save:
            plugin.morph_apply(blend)
            print("Applied morphed snapshot to plugin.", file=sys.stderr)
            if args.save:
                try:
                    with open(args.save, "wb") as f:
                        f.write(plugin.get_state())
                    print(f"Saved morphed state to {args.save}", file=sys.stderr)
                except OSError as e:
                    print(
                        f"Warning: failed to save state to {args.save}: {e}",
                        file=sys.stderr,
                    )
        return 0
    finally:
        plugin.close()


def cmd_devices(args: argparse.Namespace) -> int:
    """List available audio input/output devices."""
    try:
        playback = minihost.audio_get_playback_devices()
        capture = minihost.audio_get_capture_devices()
    except RuntimeError as e:
        print(f"Error enumerating audio devices: {e}", file=sys.stderr)
        return 1

    if args.json:
        import json

        print(json.dumps({"playback": playback, "capture": capture}, indent=2))
        return 0

    print("Audio Playback (Output) Devices:")
    if playback:
        for dev in playback:
            marker = " (default)" if dev.get("is_default") else ""
            print(f"  [{dev['index']}] {dev['name']}{marker}")
    else:
        print("  (none)")

    print("\nAudio Capture (Input) Devices:")
    if capture:
        for dev in capture:
            marker = " (default)" if dev.get("is_default") else ""
            print(f"  [{dev['index']}] {dev['name']}{marker}")
    else:
        print("  (none)")
    return 0


def _resolve_audio_device_arg(value, devices, kind):
    """Resolve a CLI device argument (int index or substring name match).

    Args:
        value: the raw CLI string (or None)
        devices: list of device dicts from audio_get_*_devices()
        kind: "playback" or "capture" (for error messages)

    Returns:
        An integer index (>= 0) on success, or -1 if value is None.

    Raises:
        ValueError: if the value cannot be resolved.
    """
    if value is None:
        return -1

    # Try integer index first
    try:
        idx = int(value)
    except ValueError:
        idx = None

    if idx is not None:
        if idx < 0 or idx >= len(devices):
            raise ValueError(
                f"{kind} device index {idx} out of range "
                f"(0-{len(devices) - 1 if devices else 'none'})"
            )
        return idx

    # Fall back to substring name match (case-insensitive)
    needle = value.lower()
    matches = [d for d in devices if needle in d["name"].lower()]
    if not matches:
        raise ValueError(f"No {kind} device matches '{value}'")
    if len(matches) > 1:
        names = ", ".join(f"[{m['index']}] {m['name']}" for m in matches)
        raise ValueError(
            f"Ambiguous {kind} device '{value}' matches: {names}. "
            f"Use an explicit index."
        )
    return matches[0]["index"]


def _load_map_file(path: str, mapper) -> int:
    """Load CC mappings from a JSON file into ``mapper``.

    Format:
        {
          "mappings": [
            {"channel": 0, "cc": 7,  "param": "Volume"},
            {"channel": 0, "cc": 10, "param": "Pan", "value_range": [-1.0, 1.0]},
            {"channel": 0, "cc": 74, "param": "Cutoff", "curve": "exp"}
          ]
        }

    Required fields per entry: ``channel``, ``cc``, ``param``.
    Optional fields: ``value_range`` (default ``[0.0, 1.0]``), ``curve``
    (default ``"linear"``; one of ``linear``, ``exp``, ``log``).

    Returns the number of mappings loaded. Raises ``ValueError`` on a
    malformed file or unknown parameter name.
    """
    import json

    with open(path) as f:
        try:
            data = json.load(f)
        except json.JSONDecodeError as e:
            raise ValueError(f"--map-file {path!r}: invalid JSON: {e}") from e

    if not isinstance(data, dict) or "mappings" not in data:
        raise ValueError(f"--map-file {path!r}: must contain a 'mappings' array")
    mappings = data["mappings"]
    if not isinstance(mappings, list):
        raise ValueError(f"--map-file {path!r}: 'mappings' must be a list")

    for i, entry in enumerate(mappings):
        if not isinstance(entry, dict):
            raise ValueError(f"--map-file {path!r}: mappings[{i}] must be an object")
        try:
            channel = entry["channel"]
            cc = entry["cc"]
            param = entry["param"]
        except KeyError as e:
            raise ValueError(
                f"--map-file {path!r}: mappings[{i}] missing required field {e}"
            ) from None

        vr_raw = entry.get("value_range", [0.0, 1.0])
        if not (isinstance(vr_raw, (list, tuple)) and len(vr_raw) == 2):
            raise ValueError(
                f"--map-file {path!r}: mappings[{i}].value_range must be [lo, hi]"
            )
        value_range = (float(vr_raw[0]), float(vr_raw[1]))

        curve = entry.get("curve", "linear")

        # Delegates the channel/cc/curve/param-existence validation to map_cc.
        mapper.map_cc(
            channel=int(channel),
            cc=int(cc),
            param=str(param),
            value_range=value_range,
            curve=str(curve),
        )

    return len(mappings)


def _parse_map_spec(spec: str) -> tuple[int, int, str, tuple[float, float], str]:
    """Parse one --map argument string into (channel, cc, param, value_range, curve).

    Format: 'channel:cc:param[:lo:hi[:curve]]'. Caller is responsible for
    further validation (e.g. checking the parameter exists on the plugin
    via MidiMapper.map_cc, which raises if not found).
    """
    parts = spec.split(":")
    if len(parts) not in (3, 5, 6):
        raise ValueError(
            f"--map expects 'channel:cc:param[:lo:hi[:curve]]' "
            f"(3, 5, or 6 colon-separated fields), got {spec!r}"
        )
    try:
        channel = int(parts[0])
        cc = int(parts[1])
    except ValueError as e:
        raise ValueError(f"--map: channel and cc must be integers, got {spec!r}") from e

    param = parts[2]
    if not param:
        raise ValueError(f"--map: param name must be non-empty, got {spec!r}")

    if len(parts) >= 5:
        try:
            lo = float(parts[3])
            hi = float(parts[4])
        except ValueError as e:
            raise ValueError(f"--map: lo and hi must be numbers, got {spec!r}") from e
        value_range = (lo, hi)
    else:
        value_range = (0.0, 1.0)

    curve = parts[5] if len(parts) == 6 else "linear"
    return channel, cc, param, value_range, curve


def _collect_play_midi_events(midi_file_path: str, sample_rate: float):
    """Read a MIDI file and return (events, total_samples) where events is
    a list of (sample_offset, status, data1, data2) sorted by sample_offset.

    Reuses the helpers in ``minihost.render`` so tempo handling matches
    the offline renderer.
    """
    from minihost.render import (
        _build_tempo_map,
        _collect_midi_events,
        _event_to_midi_tuple,
        _seconds_to_samples,
        _tick_to_seconds,
    )

    mf = minihost.MidiFile()
    if not mf.load(midi_file_path):
        raise RuntimeError(f"Failed to load MIDI file: {midi_file_path}")

    tempo_map = _build_tempo_map(mf)
    raw = _collect_midi_events(mf)
    tpq = mf.ticks_per_quarter

    events = []
    last_sample = 0
    for ev in raw:
        seconds = _tick_to_seconds(ev["tick"], tempo_map, tpq)
        sample_pos = _seconds_to_samples(seconds, sample_rate)
        tup = _event_to_midi_tuple(ev, sample_pos)
        if tup is not None:
            events.append(tup)
            if sample_pos > last_sample:
                last_sample = sample_pos

    events.sort(key=lambda t: t[0])
    return events, last_sample


# CC 123 = All Notes Off, sent on every channel at loop wrap to silence
# any sustained notes from the previous iteration before re-triggering.
_ALL_NOTES_OFF_CC = 123


def _midi_loop_thread(audio, midi_file_path, sample_rate, stop_event):
    """Schedule MIDI events from a file at wall-clock-correct times,
    looping from the start when the file ends.

    Calls ``audio.send_midi(status, data1, data2)`` for each event. Uses
    ``time.monotonic()`` for pacing. Latency is bounded by the granularity
    of ``stop_event.wait()`` (a few milliseconds), acceptable for musical
    timing at typical buffer sizes.

    The stop_event is checked between every event and during waits, so
    Ctrl+C / SIGTERM is responsive.
    """
    try:
        events, last_sample = _collect_play_midi_events(midi_file_path, sample_rate)
    except (RuntimeError, FileNotFoundError) as e:
        print(f"loop-midi: {e}", file=sys.stderr)
        return
    if not events:
        return

    # Loop length: extend slightly past the last event so the loop has a
    # natural decay before retriggering.
    loop_samples = last_sample + int(0.25 * sample_rate)

    while not stop_event.is_set():
        loop_start = time.monotonic()
        for sample_pos, status, d1, d2 in events:
            if stop_event.is_set():
                return
            target = loop_start + sample_pos / sample_rate
            now = time.monotonic()
            if target > now:
                if stop_event.wait(target - now):
                    return
            audio.send_midi(int(status), int(d1), int(d2))

        # Wait the remaining loop duration.
        loop_end = loop_start + loop_samples / sample_rate
        now = time.monotonic()
        if loop_end > now:
            if stop_event.wait(loop_end - now):
                return

        # Send All Notes Off on every channel before the next iteration.
        # 0xB0..0xBF = CC on channels 0..15.
        for ch in range(16):
            audio.send_midi(0xB0 | ch, _ALL_NOTES_OFF_CC, 0)


def _audio_loop_thread(audio, audio_file_path, sample_rate, stop_event):
    """Feed audio file data into the AudioDevice's input ring buffer at
    real-time pace, looping from the start when the file ends.

    Caller must have already enabled the input ring buffer
    (``audio.enable_input()``). The file is auto-resampled to the device's
    sample rate.
    """
    try:
        data, file_sr = minihost.read_audio(audio_file_path)
    except (FileNotFoundError, RuntimeError) as e:
        print(f"loop-audio: {e}", file=sys.stderr)
        return

    if int(file_sr) != int(sample_rate):
        data = minihost.resample(data, int(file_sr), int(sample_rate))

    block_size = 1024
    # Pace at slightly less than real time so the ring buffer stays fed
    # without overflowing (mirrors the README example).
    block_period = (block_size / sample_rate) * 0.9

    while not stop_event.is_set():
        for start in range(0, data.frames, block_size):
            if stop_event.is_set():
                return
            end = min(start + block_size, data.frames)
            chunk = data[:, start:end]
            audio.write_input(chunk)
            if stop_event.wait(block_period):
                return


def cmd_play(args: argparse.Namespace) -> int:
    """Play plugin with real-time audio and MIDI."""
    capture = getattr(args, "input", False) or False
    loop_midi_path = getattr(args, "loop_midi", None)
    loop_audio_path = getattr(args, "loop_audio", None)

    if loop_audio_path and capture:
        print(
            "Error: --loop-audio and --input both write to the input "
            "ring buffer. Use one or the other.",
            file=sys.stderr,
        )
        return 1

    # Resolve audio device selection (index or name substring)
    playback_device_index = -1
    capture_device_index = -1
    try:
        if getattr(args, "playback_device", None) is not None:
            playback_device_index = _resolve_audio_device_arg(
                args.playback_device,
                minihost.audio_get_playback_devices(),
                "playback",
            )
        if getattr(args, "capture_device", None) is not None:
            if not capture:
                print(
                    "Error: --capture-device requires --input (duplex mode).",
                    file=sys.stderr,
                )
                return 1
            capture_device_index = _resolve_audio_device_arg(
                args.capture_device,
                minihost.audio_get_capture_devices(),
                "capture",
            )
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

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

    # Build CC->parameter mapper from --map specs, if any.
    # When a mapper is active we route the MIDI port through Python via
    # MidiIn instead of letting AudioDevice consume it directly: the
    # mapper translates mapped CCs to set_param calls on the plugin and
    # forwards everything else (notes, unmapped CCs) to the plugin via
    # AudioDevice.send_midi so the user can still play notes.
    midi_mapper: Optional[minihost.MidiMapper] = None
    map_specs = getattr(args, "map", None) or []
    map_file = getattr(args, "map_file", None)
    if map_specs or map_file:
        midi_mapper = minihost.MidiMapper(plugin)
        if map_file:
            try:
                _load_map_file(map_file, midi_mapper)
            except (ValueError, RuntimeError, FileNotFoundError) as e:
                print(f"Error loading --map-file: {e}", file=sys.stderr)
                return 1
        try:
            for spec in map_specs:
                channel, cc, param, value_range, curve = _parse_map_spec(spec)
                midi_mapper.map_cc(
                    channel=channel,
                    cc=cc,
                    param=param,
                    value_range=value_range,
                    curve=curve,
                )
        except (ValueError, RuntimeError) as e:
            # ValueError from _parse_map_spec; RuntimeError from
            # Plugin.find_param via MidiMapper.map_cc on unknown param.
            print(f"Error parsing --map: {e}", file=sys.stderr)
            return 1
        print(f"  CC mappings: {len(midi_mapper.cc_mappings)}")
        for (ch, cc), pname in sorted(midi_mapper.cc_mappings.items()):
            print(f"    ch={ch} cc={cc:<3} -> {pname}")

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
    elif midi_mapper is not None and not args.virtual_midi:
        print(
            "Error: --map requires --midi N or --virtual-midi NAME so the "
            "mapper has an input source.",
            file=sys.stderr,
        )
        return 1

    # Determine MIDI output configuration
    midi_out_port = -1
    if args.midi_out is not None:
        midi_out_port = args.midi_out
        outputs = minihost.midi_get_output_ports()
        if midi_out_port >= len(outputs):
            print(
                f"Error: MIDI output port {midi_out_port} not found. Use 'minihost midi' to list.",
                file=sys.stderr,
            )
            return 1
        print(f"  MIDI Output: [{midi_out_port}] {outputs[midi_out_port]['name']}")

    # When a mapper is active, AudioDevice must NOT also open the MIDI
    # input port -- the port is owned by the standalone MidiIn that drives
    # the mapper. The mapper's on_unmapped forwards events back into the
    # plugin via AudioDevice.send_midi, set up after the device is created.
    audio_midi_input_port = -1 if midi_mapper is not None else midi_port

    # Open audio device (duplex mode if --input)
    try:
        audio = minihost.AudioDevice(
            plugin,
            sample_rate=args.sample_rate,
            buffer_frames=args.block_size,
            midi_input_port=audio_midi_input_port,
            midi_output_port=midi_out_port,
            capture=capture,
            playback_device_index=playback_device_index,
            capture_device_index=capture_device_index,
        )
    except RuntimeError as e:
        print(f"Error opening audio device: {e}", file=sys.stderr)
        return 1

    mode = "Duplex (capture + playback)" if capture else "Playback"
    print(f"\nAudio Device ({mode}):")
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

    # Create virtual MIDI output port if requested
    if args.virtual_midi_out:
        try:
            audio.create_virtual_midi_output(args.virtual_midi_out)
            print(f"  Virtual MIDI Output: '{args.virtual_midi_out}'")
        except RuntimeError as e:
            print(
                f"Warning: Could not create virtual MIDI output port: {e}",
                file=sys.stderr,
            )

    # Wire up MidiIn -> mapper -> forwarder. The mapper translates mapped
    # CCs to plugin.set_param; on_unmapped forwards everything else (notes,
    # unmapped CCs) into the plugin's MIDI queue via audio.send_midi.
    # SysEx and longer messages aren't forwarded (send_midi takes 3 bytes).
    midi_in_handle = None
    if midi_mapper is not None:

        def _forward_unmapped(data: bytes) -> None:
            n = len(data)
            if n >= 3:
                audio.send_midi(data[0], data[1], data[2])
            elif n == 2:
                audio.send_midi(data[0], data[1], 0)
            elif n == 1:
                audio.send_midi(data[0], 0, 0)

        midi_mapper.set_on_unmapped(_forward_unmapped)

        if midi_port >= 0:
            try:
                midi_in_handle = minihost.MidiIn.open(midi_port, midi_mapper)
            except RuntimeError as e:
                print(f"Error opening MIDI input: {e}", file=sys.stderr)
                return 1
        elif args.virtual_midi:
            try:
                midi_in_handle = minihost.MidiIn.open_virtual(
                    args.virtual_midi, midi_mapper
                )
            except RuntimeError as e:
                print(f"Error creating virtual MIDI input: {e}", file=sys.stderr)
                return 1

    # Setup signal handler
    running = True

    def on_signal(sig, frame):
        nonlocal running
        running = False
        print("\nStopping...")

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)

    # If --loop-audio is set, prepare the input ring buffer before
    # starting the audio device so the first block doesn't underrun.
    if loop_audio_path:
        try:
            audio.enable_input()
        except RuntimeError as e:
            print(f"Error enabling input ring buffer: {e}", file=sys.stderr)
            return 1
        print(f"  Loop audio: {loop_audio_path}")
    if loop_midi_path:
        print(f"  Loop MIDI:  {loop_midi_path}")

    # Start audio
    audio.start()
    print("\nPlaying. Press Ctrl+C to stop.")

    # Spawn loop-MIDI / loop-audio threads. They use a single stop_event
    # so the main loop can break them out cleanly on Ctrl+C.
    loop_stop = threading.Event()
    loop_threads: list[threading.Thread] = []
    if loop_midi_path:
        t = threading.Thread(
            target=_midi_loop_thread,
            args=(audio, loop_midi_path, audio.sample_rate, loop_stop),
            name="minihost-loop-midi",
            daemon=True,
        )
        t.start()
        loop_threads.append(t)
    if loop_audio_path:
        t = threading.Thread(
            target=_audio_loop_thread,
            args=(audio, loop_audio_path, audio.sample_rate, loop_stop),
            name="minihost-loop-audio",
            daemon=True,
        )
        t.start()
        loop_threads.append(t)

    no_midi_in = args.midi is None and not args.virtual_midi and not loop_midi_path
    no_midi_out = args.midi_out is None and not args.virtual_midi_out
    has_audio_in = capture or bool(loop_audio_path)
    if no_midi_in or no_midi_out or not has_audio_in:
        hints = []
        if not has_audio_in:
            hints.append("audio input (--input / --loop-audio PATH)")
        if no_midi_in:
            hints.append(
                "MIDI input (--midi N / --virtual-midi NAME / --loop-midi PATH)"
            )
        if no_midi_out:
            hints.append("MIDI output (--midi-out N / --virtual-midi-out NAME)")
        print(f"(No {', '.join(hints)})")

    try:
        while running:
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass

    # Stop the loop threads first so they don't try to send_midi /
    # write_input into a stopped device.
    loop_stop.set()
    for t in loop_threads:
        t.join(timeout=2.0)
    audio.stop()
    if midi_in_handle is not None:
        midi_in_handle.close()
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


def _process_single_file(
    plugin,
    input_path,
    output_path,
    args,
    *,
    reset=False,
    expected_sample_rate=None,
    expected_channels=None,
    allow_resample=True,
):
    """Process a single audio file through a plugin. Returns 0 on success, 1 on error.

    Thin wrapper around :func:`minihost.process_audio_to_file` that adds
    the batch-mode invariants (sample-rate / channel-count consistency
    across files) and translates exceptions into the int return contract
    expected by :func:`_cmd_process_batch`.

    The plugin should already be loaded and configured with state/presets.
    If reset=True, plugin.reset() is called before processing.
    expected_sample_rate/expected_channels: validate input matches (for batch mode).
    allow_resample: if True, resample mismatched files instead of erroring.
    """
    from minihost.audio_io import get_audio_info

    if reset:
        plugin.reset()

    # Pre-check the file's metadata for batch-mode invariants. This avoids
    # decoding the entire file just to discover the SR or channel count
    # disagrees with the batch baseline.
    if expected_sample_rate is not None or expected_channels is not None:
        try:
            info = get_audio_info(input_path)
        except Exception as e:
            print(f"Error reading '{input_path}': {e}", file=sys.stderr)
            return 1

        if (
            expected_sample_rate is not None
            and info["sample_rate"] != expected_sample_rate
            and not allow_resample
        ):
            print(
                f"Error: Sample rate mismatch in '{input_path}': "
                f"{info['sample_rate']} Hz (expected {expected_sample_rate} Hz)",
                file=sys.stderr,
            )
            return 1

        if expected_channels is not None and info["channels"] != expected_channels:
            print(
                f"Error: Channel count mismatch in '{input_path}': "
                f"{info['channels']} ch (expected {expected_channels} ch)",
                file=sys.stderr,
            )
            return 1

    bit_depth = args.bit_depth if args.bit_depth else 24
    progress = _ProgressBar(
        os.path.basename(input_path),
        enabled=getattr(args, "progress", False),
    )
    try:
        minihost.process_audio_to_file(
            plugin,
            input_path,
            output_path,
            tail_seconds=0.0,  # batch worker doesn't render tail
            block_size=args.block_size,
            bit_depth=bit_depth,
            resample_to_plugin_rate=allow_resample,
            duplicate_to_stereo=True,
            compensate_latency=True,
            normalize=getattr(args, "normalize", None),
            progress_callback=progress,
        )
        progress.finish()
    except (FileNotFoundError, RuntimeError, ValueError, OSError) as e:
        # process_audio_to_file raises on read errors, write errors, and
        # rate-mismatch when resample_to_plugin_rate=False. Map all to
        # the int return contract.
        print(
            f"Error processing '{input_path}' -> '{output_path}': {e}", file=sys.stderr
        )
        return 1

    return 0


def _cmd_process_batch(args, input_files):
    """Batch-process multiple audio files through a plugin."""
    from minihost.audio_io import get_audio_info

    # Detect sample rate and channel count from first file
    try:
        first_info = get_audio_info(input_files[0])
        sample_rate = int(first_info["sample_rate"])
        in_channels = int(first_info["channels"])
    except Exception as e:
        print(f"Error reading '{input_files[0]}': {e}", file=sys.stderr)
        return 1

    # Load plugin (or chain) once
    chain_spec = getattr(args, "chain", None)
    using_chain = chain_spec is not None
    if using_chain:
        try:
            plugin = minihost.load_chain(
                chain_spec,
                sample_rate=sample_rate,
                block_size=args.block_size,
            )
        except (FileNotFoundError, ValueError, ImportError, RuntimeError) as e:
            print(f"Error loading chain spec: {e}", file=sys.stderr)
            return 1
    else:
        try:
            plugin = minihost.Plugin(
                args.plugin,
                sample_rate=sample_rate,
                max_block_size=args.block_size,
                in_channels=in_channels,
            )
        except RuntimeError as e:
            print(f"Error loading plugin: {e}", file=sys.stderr)
            return 1

    # Load state / preset (skipped when using a chain spec)
    if not using_chain and args.state:
        try:
            with open(args.state, "rb") as f:
                plugin.set_state(f.read())
        except Exception as e:
            print(f"Warning: Could not load state: {e}", file=sys.stderr)

    if not using_chain and args.vstpreset:
        try:
            _load_vstpreset(plugin, args.vstpreset)
        except (FileNotFoundError, ValueError, RuntimeError) as e:
            print(f"Error loading .vstpreset: {e}", file=sys.stderr)
            return 1

    if not using_chain and args.preset is not None:
        if args.preset < 0 or args.preset >= plugin.num_programs:
            print(
                f"Error: Preset {args.preset} out of range (0-{plugin.num_programs - 1})",
                file=sys.stderr,
            )
            return 1
        plugin.program = args.preset

    # Apply --param static overrides
    if not using_chain and args.param:
        from minihost.automation import parse_param_arg

        for param_str in args.param:
            try:
                param_idx, value = parse_param_arg(param_str, plugin)
                plugin.set_param(param_idx, value)
            except ValueError as e:
                print(f"Error parsing --param: {e}", file=sys.stderr)
                return 1

    if not using_chain and args.non_realtime:
        plugin.non_realtime = True

    if not using_chain and args.bpm:
        plugin.set_transport(bpm=args.bpm, is_playing=True)

    # Save initial state for reset between files. Chains don't expose
    # get_state, so skip the snapshot -- the chain spec is the canonical
    # initial state and per-file reset is handled by the inner plugins.
    initial_state = plugin.get_state() if not using_chain else None

    label = chain_spec if using_chain else args.plugin
    print(f"Batch processing {len(input_files)} file(s) through {label}")
    print(f"  Output directory: {args.output}")

    failed = 0
    for i, inp_path in enumerate(input_files, 1):
        basename = os.path.basename(inp_path)
        out_path = os.path.join(args.output, basename)

        if os.path.exists(out_path) and not args.overwrite:
            print(
                f"  [{i}/{len(input_files)}] Skipping {basename} (exists, use -y to overwrite)"
            )
            continue

        # Reset plugin state between files. PluginChain doesn't expose
        # reset/set_state; each inner plugin is expected to recover via
        # its own DSP reset on the first audio block.
        if not using_chain:
            plugin.reset()
            plugin.set_state(initial_state)

        ret = _process_single_file(
            plugin,
            inp_path,
            out_path,
            args,
            reset=False,
            expected_sample_rate=sample_rate,
            expected_channels=in_channels,
            allow_resample=not getattr(args, "no_resample", False),
        )
        if ret != 0:
            print(f"  [{i}/{len(input_files)}] FAILED: {basename}", file=sys.stderr)
            failed += 1
        else:
            print(f"  [{i}/{len(input_files)}] {basename}")

    if failed:
        print(f"\n{failed} file(s) failed.", file=sys.stderr)
        return 1

    print(f"\nProcessed {len(input_files) - failed} file(s).")
    return 0


def _expand_globs(patterns):
    """Expand glob patterns in a list of file paths. Returns sorted unique paths."""
    import glob as globmod

    result = []
    seen = set()
    for pattern in patterns:
        # If pattern contains glob characters, expand it
        if any(c in pattern for c in "*?["):
            matches = sorted(globmod.glob(pattern, recursive=True))
            for m in matches:
                if m not in seen:
                    seen.add(m)
                    result.append(m)
        else:
            if pattern not in seen:
                seen.add(pattern)
                result.append(pattern)
    return result


def _is_batch_output(output_path):
    """Check if output path indicates batch mode (directory)."""
    return (
        output_path.endswith(os.sep)
        or output_path.endswith("/")
        or os.path.isdir(output_path)
    )


def cmd_process(args: argparse.Namespace) -> int:
    """Process audio file through plugin (offline).

    Thin shim over :func:`minihost.process_audio_to_file`. CLI-specific
    work (arg validation, glob expansion, batch routing, plugin
    construction, state/preset loading, summary printing) stays here;
    block iteration, MIDI/sidechain/automation routing, latency
    compensation, normalization, and write live in the library.
    """
    from minihost.audio_io import get_audio_info
    from minihost.automation import parse_automation_file, parse_param_arg

    # --- Validate --chain vs single-plugin flags ---
    chain_spec = getattr(args, "chain", None)
    using_chain = chain_spec is not None
    if using_chain:
        if args.plugin:
            print(
                "Error: positional 'plugin' is not permitted with --chain; "
                "the spec lists the plugins.",
                file=sys.stderr,
            )
            return 1
        conflicting = [
            ("--state", args.state),
            ("--vstpreset", args.vstpreset),
            ("--preset", args.preset),
            ("--param", args.param),
            ("--param-file", args.param_file),
            ("--out-channels", args.out_channels),
            ("--bpm", args.bpm),
            ("--non-realtime", args.non_realtime),
        ]
        rejected = [name for name, val in conflicting if val not in (None, [], False)]
        if rejected:
            print(
                f"Error: {', '.join(rejected)} cannot be combined with "
                f"--chain (the spec is the source of truth).",
                file=sys.stderr,
            )
            return 1
    else:
        if not args.plugin:
            print(
                "Error: provide a plugin path or use --chain.",
                file=sys.stderr,
            )
            return 1

    # --- Expand globs in input file list ---
    raw_inputs = args.input or []
    input_files = _expand_globs(raw_inputs) if raw_inputs else []
    midi_path = args.midi_input
    has_audio_input = len(input_files) > 0
    has_midi_input = midi_path is not None

    if not has_audio_input and not has_midi_input:
        print(
            "Error: At least one of --input or --midi-input is required.",
            file=sys.stderr,
        )
        return 1

    # --- Detect batch mode ---
    has_globs = any(any(c in p for c in "*?[") for p in raw_inputs)
    batch_mode = (
        has_audio_input
        and _is_batch_output(args.output)
        and not has_midi_input
        and (has_globs or len(raw_inputs) == 1)
    )

    if batch_mode:
        if len(input_files) == 0:
            print("Error: No files matched the input pattern(s).", file=sys.stderr)
            return 1
        os.makedirs(args.output, exist_ok=True)
        return _cmd_process_batch(args, input_files)

    # Check output doesn't exist (unless --overwrite)
    if (
        os.path.exists(args.output)
        and not os.path.isdir(args.output)
        and not args.overwrite
    ):
        print(
            f"Error: Output file '{args.output}' already exists. "
            f"Use -y/--overwrite to overwrite.",
            file=sys.stderr,
        )
        return 1

    # Detect sample rate + main-input channel count from first audio
    # file. The plugin needs both at construction time.
    sample_rate = int(args.sample_rate)
    main_in_ch = 2
    sidechain_path: str | None = None
    sidechain_ch = 0

    if has_audio_input:
        try:
            first_info = get_audio_info(input_files[0])
        except Exception as e:
            print(f"Error reading input file: {e}", file=sys.stderr)
            return 1
        sample_rate = int(first_info["sample_rate"])
        main_in_ch = int(first_info["channels"])

        if len(input_files) > 1:
            sidechain_path = input_files[1]
            try:
                sc_info = get_audio_info(sidechain_path)
            except Exception as e:
                print(
                    f"Error reading sidechain '{sidechain_path}': {e}", file=sys.stderr
                )
                return 1
            sidechain_ch = int(sc_info["channels"])

    # --- Load plugin (or chain) ---
    # `plugin` is a PluginChain when --chain is given, else a Plugin. The
    # Plugin-only config below (state / preset / non_realtime / automation)
    # narrows back to Plugin via isinstance, which is exactly `not using_chain`.
    plugin: minihost.Plugin | minihost.PluginChain
    if using_chain:
        assert chain_spec is not None  # using_chain == (chain_spec is not None)
        if sidechain_path is not None:
            print(
                "Error: sidechain input is not supported with --chain "
                "(PluginChain has no sidechain process method).",
                file=sys.stderr,
            )
            return 1
        try:
            plugin = minihost.load_chain(
                chain_spec,
                sample_rate=sample_rate,
                block_size=args.block_size,
            )
        except (FileNotFoundError, ValueError, ImportError, RuntimeError) as e:
            print(f"Error loading chain spec: {e}", file=sys.stderr)
            return 1
    else:
        try:
            plugin = minihost.Plugin(
                args.plugin,
                sample_rate=sample_rate,
                max_block_size=args.block_size,
                in_channels=main_in_ch,
                out_channels=args.out_channels if args.out_channels else 2,
                sidechain_channels=sidechain_ch,
            )
        except RuntimeError as e:
            print(f"Error loading plugin: {e}", file=sys.stderr)
            return 1

    out_ch = (
        args.out_channels if args.out_channels else max(plugin.num_output_channels, 2)
    )

    # --- Load state / preset ---
    if isinstance(plugin, minihost.Plugin) and args.state:
        try:
            with open(args.state, "rb") as f:
                plugin.set_state(f.read())
            print(f"Loaded state from {args.state}")
        except Exception as e:
            print(f"Warning: Could not load state: {e}", file=sys.stderr)

    if not using_chain and args.vstpreset:
        try:
            _load_vstpreset(plugin, args.vstpreset)
            print(f"Loaded .vstpreset from {args.vstpreset}")
        except (FileNotFoundError, ValueError, RuntimeError) as e:
            print(f"Error loading .vstpreset: {e}", file=sys.stderr)
            return 1

    if isinstance(plugin, minihost.Plugin) and args.preset is not None:
        if args.preset < 0 or args.preset >= plugin.num_programs:
            print(
                f"Error: Preset {args.preset} out of range (0-{plugin.num_programs - 1})",
                file=sys.stderr,
            )
            return 1
        plugin.program = args.preset
        print(f"Loaded preset [{args.preset}]: {plugin.get_program_name(args.preset)}")

    if isinstance(plugin, minihost.Plugin) and args.non_realtime:
        plugin.non_realtime = True

    # --- Resolve param_changes (CLI + automation file -> 3-tuples) ---
    # The automation parser needs an upper bound on total_samples; peek
    # the audio file's frame count or load MIDI events to get one.
    total_samples_hint: int | None = None
    if has_audio_input:
        total_samples_hint = int(first_info["frames"])
    elif has_midi_input:
        try:
            _events, _max = _load_midi_events(midi_path, sample_rate)
        except RuntimeError as e:
            print(f"Error loading MIDI: {e}", file=sys.stderr)
            return 1
        total_samples_hint = _max + int(args.tail * sample_rate)

    if not total_samples_hint:
        print("Error: No audio or MIDI input data to process.", file=sys.stderr)
        return 1

    param_changes: list[tuple[int, int, float]] = []
    if isinstance(plugin, minihost.Plugin) and args.param_file:
        try:
            param_changes = parse_automation_file(
                args.param_file,
                plugin,
                sample_rate,
                total_samples_hint,
                block_size=args.block_size,
            )
        except (FileNotFoundError, ValueError) as e:
            print(f"Error loading automation file: {e}", file=sys.stderr)
            return 1

    param_overrides: dict[int, float] = {}
    if isinstance(plugin, minihost.Plugin) and args.param:
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

    if param_overrides:
        overridden = set(param_overrides.keys())
        file_overridden = {idx for (_, idx, _) in param_changes if idx in overridden}
        if file_overridden:
            print(
                f"Warning: --param overrides automation file for "
                f"{len(file_overridden)} parameter(s)",
                file=sys.stderr,
            )
            param_changes = [
                (s, i, v) for (s, i, v) in param_changes if i not in overridden
            ]
        for param_idx, value in param_overrides.items():
            param_changes.append((0, param_idx, value))
        param_changes.sort(key=lambda x: x[0])

    # --- Print summary ---
    latency = plugin.latency_samples
    print(f"Plugin: {chain_spec if using_chain else args.plugin}")
    print(f"  Sample rate: {sample_rate} Hz")
    print(f"  Block size:  {args.block_size}")
    print(f"  Latency:     {latency} samples")
    if has_audio_input:
        print(f"  Input:       {main_in_ch} ch, {total_samples_hint} samples")
        if sidechain_path is not None:
            print(f"  Sidechain:   {sidechain_ch} ch ({sidechain_path})")
    if has_midi_input:
        print(f"  MIDI:        {midi_path}")
    if param_changes:
        print(f"  Automation:  {len(param_changes)} param change(s)")
    print(f"  Output:      {out_ch} ch -> {args.output}")

    # --- Delegate to library ---
    progress = _ProgressBar("processing", enabled=getattr(args, "progress", False))
    tail_seconds = args.tail if (has_midi_input and not has_audio_input) else 0.0
    bit_depth = args.bit_depth if args.bit_depth is not None else 24

    try:
        frames_written = minihost.process_audio_to_file(
            plugin,
            input_path=input_files[0] if has_audio_input else None,
            output_path=args.output,
            tail_seconds=tail_seconds,
            block_size=args.block_size,
            bit_depth=bit_depth,
            resample_to_plugin_rate=not getattr(args, "no_resample", False),
            compensate_latency=True,
            normalize=getattr(args, "normalize", None),
            progress_callback=progress,
            midi=midi_path if has_midi_input else None,
            sidechain=sidechain_path,
            param_changes=param_changes or None,
            bpm=(args.bpm if (not using_chain and args.bpm) else None),
        )
    except (FileNotFoundError, ValueError, RuntimeError, OSError) as e:
        progress.finish()
        print(f"Error: {e}", file=sys.stderr)
        return 1
    progress.finish()

    duration = frames_written / sample_rate
    print(f"Wrote {frames_written} samples ({duration:.2f}s) to {args.output}")
    return 0


def cmd_render(args: argparse.Namespace) -> int:
    """Render a project JSON file to its declared output sinks."""
    import minihost

    project_path = Path(args.project)
    if not project_path.exists():
        print(f"Error: project file not found: {project_path}", file=sys.stderr)
        return 1

    progress = None
    if args.progress:

        def progress(done: int, total: int) -> None:
            pct = 100.0 * done / max(1, total)
            print(
                f"\r  {done}/{total} frames ({pct:5.1f}%)",
                end="",
                file=sys.stderr,
                flush=True,
            )

    try:
        result = minihost.render_project(
            project_path,
            progress_callback=progress,
        )
    except minihost.ProjectError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    if args.progress:
        print(file=sys.stderr)

    for node in result.outputs:
        print(f"wrote {node.sink}", file=sys.stderr)
    return 0


def cmd_resample(args: argparse.Namespace) -> int:
    """Resample an audio file to a different sample rate."""
    from minihost.audio_io import read_audio, resample, write_audio

    if os.path.exists(args.output) and not args.overwrite:
        print(
            f"Error: Output file '{args.output}' already exists. "
            f"Use -y/--overwrite to overwrite.",
            file=sys.stderr,
        )
        return 1

    try:
        data, sr_in = read_audio(args.input)
    except (FileNotFoundError, RuntimeError) as e:
        print(f"Error: {e}", file=sys.stderr)
        return 1

    sr_out = args.target_rate
    if sr_in == sr_out:
        print(
            f"Input is already {sr_in} Hz, nothing to do.",
            file=sys.stderr,
        )
        return 1

    data = resample(data, sr_in, sr_out)
    bit_depth = args.bit_depth if args.bit_depth else 24

    try:
        write_audio(args.output, data, sr_out, bit_depth=bit_depth)
    except Exception as e:
        print(f"Error writing output: {e}", file=sys.stderr)
        return 1

    channels = data.shape[0]
    frames = data.shape[1]
    duration = frames / sr_out
    print(
        f"{sr_in} Hz -> {sr_out} Hz ({channels} ch, {duration:.2f}s) -> {args.output}"
    )
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
    scan_p.add_argument(
        "--refresh",
        action="store_true",
        help="Re-probe every plugin, ignoring cached entries",
    )
    scan_p.add_argument(
        "--no-cache",
        action="store_true",
        help="Bypass the scan cache entirely (full uncached scan)",
    )
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
    info_p.add_argument(
        "--refresh",
        action="store_true",
        help="Re-probe even if cached (with --probe)",
    )
    info_p.add_argument(
        "--no-cache",
        action="store_true",
        help="Bypass the scan cache (with --probe)",
    )
    info_p.set_defaults(func=cmd_info)

    # cache
    cache_p = subparsers.add_parser(
        "cache", help="Manage/query the persistent plugin-scan cache"
    )
    cache_sub = cache_p.add_subparsers(dest="cache_action", required=True)
    cache_sub.add_parser("path", help="Print the cache file path")
    cache_stats_p = cache_sub.add_parser("stats", help="Show cache statistics")
    cache_stats_p.add_argument("-j", "--json", action="store_true")
    cache_sub.add_parser("clear", help="Delete the cache file")
    cache_sub.add_parser("prune", help="Drop entries whose plugin is gone")
    cache_list_p = cache_sub.add_parser("list", help="Query cached plugins")
    cache_list_p.add_argument("-j", "--json", action="store_true")
    cache_list_p.add_argument("--format", help="Filter by format (VST3/AU/LV2)")
    cache_list_p.add_argument("--name", help="Filter by name substring")
    cache_list_p.add_argument("--vendor", help="Filter by vendor substring")
    cache_list_p.add_argument(
        "--midi-in", action="store_true", help="Only plugins that accept MIDI"
    )
    cache_list_p.add_argument(
        "--midi-out", action="store_true", help="Only plugins that produce MIDI"
    )
    cache_p.set_defaults(func=cmd_cache)

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

    # devices
    devices_p = subparsers.add_parser(
        "devices", help="List available audio playback/capture devices"
    )
    devices_p.add_argument("-j", "--json", action="store_true", help="Output as JSON")
    devices_p.set_defaults(func=cmd_devices)

    # presets
    presets_p = subparsers.add_parser(
        "presets",
        help="List plugin factory presets, or save current state as .vstpreset",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # List factory presets
  minihost presets /path/to/synth.vst3
  minihost presets /path/to/synth.vst3 --json

  # Export factory preset N as a .vstpreset
  minihost presets /path/to/synth.vst3 --program 5 --save preset5.vstpreset

  # Round-trip a .vstpreset through the plugin (loads, re-saves)
  minihost presets /path/to/synth.vst3 \\
    --load-vstpreset in.vstpreset --save out.vstpreset

  # Convert a raw state blob to .vstpreset
  minihost presets /path/to/synth.vst3 --state state.bin --save out.vstpreset
""",
    )
    presets_p.add_argument("plugin", help="Path to plugin")
    presets_p.add_argument("-j", "--json", action="store_true", help="Output as JSON")
    presets_p.add_argument(
        "--save",
        metavar="FILE",
        help="Save current plugin state as a .vstpreset file",
    )
    presets_p.add_argument(
        "--program",
        type=int,
        metavar="N",
        help="Select factory program N before saving",
    )
    presets_p.add_argument(
        "--state",
        metavar="FILE",
        help="Load raw state blob into the plugin before saving",
    )
    presets_p.add_argument(
        "--load-vstpreset",
        metavar="FILE",
        help="Load a .vstpreset file into the plugin before saving "
        "(its class_id is preserved when --save is used)",
    )
    presets_p.add_argument(
        "-y",
        "--overwrite",
        action="store_true",
        help="Overwrite --save output file if it exists",
    )
    presets_p.set_defaults(func=cmd_presets)

    # morph
    morph_p = subparsers.add_parser(
        "morph",
        help="Interpolate between two parameter snapshots (A/B morph)",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Morph 25% between factory programs 0 and 1 (default sources)
  minihost morph /path/to/synth.vst3 -t 0.25

  # Morph between two explicit programs, as JSON
  minihost morph /path/to/synth.vst3 --a-program 0 --b-program 5 -t 0.5 -j

  # Morph between two saved state files, apply, and save the result
  minihost morph /path/to/synth.vst3 \\
    --a-state a.state --b-state b.state -t 0.3 --save morphed.state
""",
    )
    morph_p.add_argument("plugin", help="Path to plugin")
    morph_p.add_argument("-j", "--json", action="store_true", help="Output as JSON")
    morph_p.add_argument(
        "--a-program", type=int, metavar="N", help="Snapshot A from factory program N"
    )
    morph_p.add_argument(
        "--b-program", type=int, metavar="N", help="Snapshot B from factory program N"
    )
    morph_p.add_argument(
        "--a-state", metavar="FILE", help="Snapshot A from a saved state file"
    )
    morph_p.add_argument(
        "--b-state", metavar="FILE", help="Snapshot B from a saved state file"
    )
    morph_p.add_argument(
        "-t",
        "--blend",
        type=float,
        default=0.5,
        metavar="T",
        help="Blend amount 0..1 (default: 0.5)",
    )
    morph_p.add_argument(
        "--apply",
        action="store_true",
        help="Apply the morphed snapshot to the plugin",
    )
    morph_p.add_argument(
        "--save", metavar="FILE", help="Apply and save the morphed state to FILE"
    )
    morph_p.set_defaults(func=cmd_morph)

    # play
    play_p = subparsers.add_parser("play", help="Play plugin with real-time audio/MIDI")
    play_p.add_argument("plugin", help="Path to plugin")
    play_p.add_argument(
        "-i",
        "--input",
        action="store_true",
        help="Enable audio input (duplex mode) for effect processing",
    )
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
    play_p.add_argument(
        "--midi-out", type=int, metavar="N", help="Connect to MIDI output port N"
    )
    play_p.add_argument(
        "--virtual-midi-out",
        type=str,
        metavar="NAME",
        help="Create virtual MIDI output with NAME",
    )
    play_p.add_argument(
        "--playback-device",
        metavar="INDEX_OR_NAME",
        help="Audio playback device (index from 'minihost devices' or case-insensitive "
        "substring of device name). Default: system default.",
    )
    play_p.add_argument(
        "--capture-device",
        metavar="INDEX_OR_NAME",
        help="Audio capture device for --input duplex mode (index or substring). "
        "Default: system default.",
    )
    play_p.add_argument(
        "--map",
        action="append",
        default=[],
        metavar="SPEC",
        help=(
            "Map an incoming MIDI CC to a plugin parameter. Repeatable. "
            "Format: 'channel:cc:param[:lo:hi[:curve]]'. "
            "channel and cc are integers (0-15 / 0-127); param is the plugin "
            "parameter name (case-insensitive). Optional lo:hi rescale the "
            "0..127 CC value (default 0:1). Optional curve is one of "
            "'linear' (default), 'exp' (more resolution at low end), or "
            "'log' (more resolution at high end). When --map is set, MIDI "
            "is routed through Python; unmapped events are forwarded to the "
            "plugin so notes still play. "
            "Example: --map 0:74:Cutoff:0:1:exp"
        ),
    )
    play_p.add_argument(
        "--map-file",
        type=str,
        metavar="PATH",
        help=(
            "Load CC->parameter mappings from a JSON file. See the docs "
            "for format. Combinable with --map (file first, then CLI args)."
        ),
    )
    play_p.add_argument(
        "--loop-midi",
        type=str,
        metavar="PATH",
        help=(
            "Play a MIDI file in a loop while playback runs. Events are "
            "scheduled in real time on a Python thread; All Notes Off is "
            "sent on every channel between iterations to silence sustained "
            "notes. Combinable with --midi (live + file MIDI are merged)."
        ),
    )
    play_p.add_argument(
        "--loop-audio",
        type=str,
        metavar="PATH",
        help=(
            "Loop an audio file as the plugin's input. Auto-enables the "
            "input ring buffer; the file is resampled to the device rate "
            "if needed. For effect plugins. Mutually exclusive with --input."
        ),
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
    process_p.add_argument(
        "plugin",
        nargs="?",
        default=None,
        help="Path to plugin (omit when using --chain)",
    )
    process_p.add_argument(
        "--chain",
        metavar="SPEC",
        default=None,
        help="Load a declarative plugin chain from a JSON or YAML spec "
        "file (see minihost.load_chain). When set, --state / "
        "--vstpreset / --preset / --param / --param-file are not "
        "permitted -- the spec is the source of truth.",
    )
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
    process_p.add_argument(
        "--no-resample",
        action="store_true",
        help="Error on sample rate mismatch instead of resampling",
    )
    process_p.add_argument(
        "--progress",
        action="store_true",
        help="Print a progress bar on stderr during processing",
    )
    process_p.add_argument(
        "--normalize",
        nargs="?",
        type=float,
        default=None,
        const=0.0,
        metavar="dBFS",
        help="Peak-normalize the output to the given dBFS target "
        "(0 dBFS = full scale; -1.0 = 1 dB headroom). With no "
        "argument, defaults to 0 dBFS.",
    )
    process_p.set_defaults(func=cmd_process)

    # resample
    resample_p = subparsers.add_parser(
        "resample",
        help="Resample audio file to a different sample rate",
    )
    resample_p.add_argument("input", help="Input audio file")
    resample_p.add_argument(
        "-o", "--output", required=True, help="Output audio file path"
    )
    resample_p.add_argument(
        "-r",
        "--target-rate",
        type=int,
        required=True,
        metavar="HZ",
        help="Target sample rate in Hz",
    )
    resample_p.add_argument(
        "--bit-depth",
        type=int,
        default=None,
        choices=[16, 24, 32],
        help="Output bit depth (default: 24)",
    )
    resample_p.add_argument(
        "-y", "--overwrite", action="store_true", help="Overwrite output if it exists"
    )
    resample_p.set_defaults(func=cmd_resample)

    # render (graph project)
    render_p = subparsers.add_parser(
        "render",
        help="Render a project file (graph executor v2) to its output sinks",
    )
    render_p.add_argument("project", help="Path to project.json")
    render_p.add_argument(
        "--progress",
        action="store_true",
        help="Print render progress to stderr.",
    )
    render_p.set_defaults(func=cmd_render)

    args = parser.parse_args()

    if not args.command:
        parser.print_help()
        return 1

    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
