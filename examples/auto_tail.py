#!/usr/bin/env python3
"""
Example: Offline MIDI rendering with automatic tail detection.

Demonstrates tail_seconds="auto", which keeps rendering after the last MIDI
event until the plugin's output decays below a threshold (-80 dB by default).
This is useful for reverbs, delays, and any plugin with a decaying tail --
you get exactly as much tail as needed without guessing a fixed duration.

Three modes are compared:
  1. Fixed tail (default)  -- renders a fixed 2.0s of tail (may cut off or waste time)
  2. Auto tail (default threshold)  -- stops when output drops below -80 dB
  3. Auto tail (custom threshold)   -- stops at a higher threshold (-40 dB)

Usage:
    python auto_tail.py /path/to/synth.vst3
    python auto_tail.py /path/to/synth.vst3 --midi song.mid
    python auto_tail.py /path/to/synth.vst3 --output-dir ./renders/

    # Or use environment variable:
    export MINIHOST_PLUGIN=/path/to/synth.vst3
    python auto_tail.py

Requirements:
    - A synth plugin (VST3 or AudioUnit)
    - numpy
"""

import argparse
import os
import sys
import time

import numpy as np

import minihost


def create_test_midi():
    """Create a simple MIDI file with a chord for testing tail behavior."""
    mf = minihost.MidiFile()
    mf.ticks_per_quarter = 480
    track = mf.add_track()

    # Set tempo: 120 BPM
    mf.add_tempo(track, 0, 120.0)

    # Play a C major chord for 2 beats (1 second at 120 BPM)
    for pitch in [60, 64, 67]:  # C4, E4, G4
        mf.add_note_on(track, 0, 0, pitch, 100)
        mf.add_note_off(track, 960, 0, pitch, 0)  # 2 quarter notes

    return mf


def render_with_mode(plugin_path, midi_file, label, tail_seconds,
                     tail_threshold=1e-4, max_tail_seconds=30.0):
    """Render a MIDI file and report duration and peak level."""
    plugin = minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)

    start = time.monotonic()
    audio = minihost.render_midi(
        plugin, midi_file,
        block_size=512,
        tail_seconds=tail_seconds,
        tail_threshold=tail_threshold,
        max_tail_seconds=max_tail_seconds,
    )
    elapsed = time.monotonic() - start

    duration = audio.shape[1] / 48000.0
    peak = float(np.max(np.abs(audio)))
    peak_db = 20.0 * np.log10(peak) if peak > 0 else -120.0

    # Find the actual tail: last sample above threshold
    abs_audio = np.max(np.abs(audio), axis=0)
    above = np.where(abs_audio > tail_threshold)[0]
    if len(above) > 0:
        effective_tail_end = (above[-1] + 1) / 48000.0
    else:
        effective_tail_end = 0.0

    print(f"  [{label}]")
    print(f"    Total duration:  {duration:.3f}s ({audio.shape[1]} samples)")
    print(f"    Peak level:      {peak_db:.1f} dB")
    print(f"    Signal ends at:  {effective_tail_end:.3f}s")
    print(f"    Render time:     {elapsed:.3f}s")

    return audio, duration


def main():
    parser = argparse.ArgumentParser(
        description="Compare fixed vs auto tail detection in offline MIDI rendering.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "plugin", nargs="?",
        help="Path to synth plugin. Can also use MINIHOST_PLUGIN env var.",
    )
    parser.add_argument(
        "--midi", "-m", metavar="FILE",
        help="MIDI file to render. Omit for built-in test chord.",
    )
    parser.add_argument(
        "--output-dir", "-o", metavar="DIR",
        help="Write rendered WAV files to this directory.",
    )

    args = parser.parse_args()

    plugin_path = args.plugin or os.environ.get("MINIHOST_PLUGIN")
    if not plugin_path:
        print("Error: No plugin specified.", file=sys.stderr)
        print("Provide plugin path as argument or set MINIHOST_PLUGIN.", file=sys.stderr)
        sys.exit(1)

    if not os.path.exists(plugin_path):
        print(f"Error: Plugin not found: {plugin_path}", file=sys.stderr)
        sys.exit(1)

    # Verify plugin loads
    try:
        probe = minihost.probe(plugin_path)
        print(f"Plugin: {probe['name']} ({probe['format']})")
    except RuntimeError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    # Get or create MIDI
    if args.midi:
        midi_file = minihost.MidiFile()
        if not midi_file.load(args.midi):
            print(f"Error: Failed to load MIDI file: {args.midi}", file=sys.stderr)
            sys.exit(1)
        print(f"MIDI: {args.midi}")
    else:
        midi_file = create_test_midi()
        print("MIDI: built-in C major chord (1 second at 120 BPM)")

    print(f"Sample rate: 48000 Hz")
    print()

    # --- Mode 1: Fixed tail (2 seconds) ---
    audio_fixed, dur_fixed = render_with_mode(
        plugin_path, midi_file,
        label="Fixed tail (2.0s)",
        tail_seconds=2.0,
    )

    # --- Mode 2: Auto tail (default -80 dB threshold) ---
    audio_auto, dur_auto = render_with_mode(
        plugin_path, midi_file,
        label="Auto tail (-80 dB)",
        tail_seconds="auto",
        tail_threshold=1e-4,
    )

    # --- Mode 3: Auto tail (higher -40 dB threshold, cuts sooner) ---
    audio_loud, dur_loud = render_with_mode(
        plugin_path, midi_file,
        label="Auto tail (-40 dB)",
        tail_seconds="auto",
        tail_threshold=1e-2,
    )

    # Summary
    print()
    print("Summary:")
    print(f"  Fixed 2.0s tail: {dur_fixed:.3f}s rendered")
    print(f"  Auto -80 dB:     {dur_auto:.3f}s rendered  "
          f"({dur_auto - dur_fixed:+.3f}s vs fixed)")
    print(f"  Auto -40 dB:     {dur_loud:.3f}s rendered  "
          f"({dur_loud - dur_fixed:+.3f}s vs fixed)")

    # Optionally write output files
    if args.output_dir:
        os.makedirs(args.output_dir, exist_ok=True)
        for name, audio_data in [
            ("fixed_tail", audio_fixed),
            ("auto_tail_80dB", audio_auto),
            ("auto_tail_40dB", audio_loud),
        ]:
            path = os.path.join(args.output_dir, f"{name}.wav")
            minihost.write_audio(path, audio_data, 48000, bit_depth=24)
            print(f"  Wrote: {path}")


    # --- Bonus: MidiRenderer with auto tail ---
    print()
    print("--- MidiRenderer with auto tail ---")
    plugin = minihost.Plugin(plugin_path, sample_rate=48000, max_block_size=512)
    renderer = minihost.MidiRenderer(plugin, midi_file, tail_seconds="auto")
    print(f"  Max duration: {renderer.duration_seconds:.3f}s")

    block_count = 0
    while not renderer.is_finished:
        block = renderer.render_block()
        block_count += 1

    actual_duration = renderer.current_sample / renderer.sample_rate
    print(f"  Rendered {block_count} blocks, {actual_duration:.3f}s")
    print(f"  Progress at finish: {renderer.progress:.1%}")


if __name__ == "__main__":
    main()
