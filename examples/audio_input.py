#!/usr/bin/env python3
"""
Example: Real-time effect processing with audio input.

Demonstrates using AudioDevice.enable_input() / write_input() to feed audio
through an effect plugin in real time via a lock-free ring buffer.

Three modes are shown:
  1. Sine wave generator  -- synthesize a test tone and process it through an effect
  2. File playback        -- stream a WAV/FLAC file through an effect in real time
  3. Loopback             -- read from an audio file in chunks, simulating a live source

Usage:
    # Process a sine wave through an effect (no input file needed)
    python audio_input.py /path/to/reverb.vst3

    # Stream a file through an effect
    python audio_input.py /path/to/reverb.vst3 --input vocals.wav

    # Adjust ring buffer size (in frames, default ~0.5s)
    python audio_input.py /path/to/reverb.vst3 --buffer-capacity 48000

    # Or use environment variable:
    export MINIHOST_PLUGIN=/path/to/effect.vst3
    python audio_input.py --input guitar.wav

Requirements:
    - An effect plugin (VST3 or AudioUnit) that processes audio input
    - numpy
"""

import argparse
import math
import os
import signal
import sys
import time

import numpy as np

import minihost


def generate_sine_block(frequency, sample_rate, channels, block_size, phase):
    """Generate a block of sine wave audio. Returns (audio, new_phase)."""
    t = np.arange(block_size, dtype=np.float32) / sample_rate
    samples = 0.3 * np.sin(2.0 * np.pi * frequency * t + phase)
    new_phase = phase + 2.0 * np.pi * frequency * block_size / sample_rate
    # Keep phase in range to avoid float precision issues
    new_phase = new_phase % (2.0 * np.pi)
    # Broadcast to all channels: shape (channels, block_size)
    audio = np.tile(samples, (channels, 1)).astype(np.float32)
    return audio, new_phase


def example_sine_through_effect(plugin_path, sample_rate, buffer_size, capacity, duration):
    """Feed a synthesized sine wave through an effect plugin."""
    print("--- Sine wave through effect ---")

    plugin = minihost.Plugin(plugin_path, sample_rate=sample_rate, max_block_size=buffer_size)
    print(f"Loaded: {plugin_path}")
    print(f"  In: {plugin.num_input_channels} ch, Out: {plugin.num_output_channels} ch")

    audio = minihost.AudioDevice(plugin, sample_rate=sample_rate, buffer_frames=buffer_size)
    actual_sr = audio.sample_rate
    channels = audio.channels
    print(f"  Device: {actual_sr:.0f} Hz, {channels} ch, {audio.buffer_frames} frames")

    # Enable ring-buffer-based input
    audio.enable_input(capacity_frames=capacity)
    print(f"  Input ring buffer enabled ({capacity} frames)")

    audio.start()
    print(f"  Playing {duration:.1f}s of 440 Hz sine through effect...")

    phase = 0.0
    feed_block = 512
    elapsed = 0.0

    running = True

    def on_signal(sig, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, on_signal)

    try:
        while elapsed < duration and running:
            block, phase = generate_sine_block(440.0, actual_sr, channels, feed_block, phase)
            written = audio.write_input(block)
            elapsed += feed_block / actual_sr

            # Pace the producer to roughly real time
            # (in production you'd use a proper timer or callback)
            time.sleep(feed_block / actual_sr * 0.9)

            if int(elapsed) != int(elapsed - feed_block / actual_sr):
                avail = audio.input_available
                print(f"  {elapsed:.0f}s ... ring buffer: {avail} frames queued")
    except KeyboardInterrupt:
        pass

    audio.stop()
    audio.disable_input()
    print("  Done.\n")


def example_file_through_effect(plugin_path, input_path, sample_rate, buffer_size, capacity):
    """Stream an audio file through an effect plugin in real time."""
    print("--- File through effect ---")

    # Read the audio file
    data, file_sr = minihost.audio_read(input_path)
    file_channels, file_frames = data.shape
    duration = file_frames / file_sr
    print(f"Input: {input_path} ({file_channels} ch, {file_sr} Hz, {duration:.2f}s)")

    if file_sr != sample_rate:
        print(f"  Warning: file is {file_sr} Hz, device will run at {sample_rate} Hz")
        print(f"  (sample rate conversion is not yet supported)")

    plugin = minihost.Plugin(
        plugin_path, sample_rate=file_sr, max_block_size=buffer_size,
        in_channels=file_channels, out_channels=file_channels,
    )
    print(f"Loaded: {plugin_path}")

    audio = minihost.AudioDevice(plugin, sample_rate=file_sr, buffer_frames=buffer_size)
    channels = audio.channels
    actual_sr = audio.sample_rate
    print(f"  Device: {actual_sr:.0f} Hz, {channels} ch")

    audio.enable_input(capacity_frames=capacity)
    audio.start()
    print(f"  Streaming {duration:.2f}s of audio through effect...")

    feed_block = 512
    pos = 0

    running = True

    def on_signal(sig, frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGINT, on_signal)

    try:
        while pos < file_frames and running:
            end = min(pos + feed_block, file_frames)
            chunk = data[:, pos:end]

            # Pad/truncate channels to match device
            if chunk.shape[0] < channels:
                padded = np.zeros((channels, chunk.shape[1]), dtype=np.float32)
                padded[:chunk.shape[0], :] = chunk
                chunk = padded
            elif chunk.shape[0] > channels:
                chunk = chunk[:channels, :]

            written = audio.write_input(chunk)
            pos = end

            # Pace to real time
            time.sleep((end - (pos - feed_block)) / actual_sr * 0.9)

            elapsed = pos / actual_sr
            if int(elapsed) != int((pos - feed_block) / actual_sr):
                print(f"  {elapsed:.0f}s / {duration:.0f}s")
    except KeyboardInterrupt:
        pass

    # Let the tail play out
    time.sleep(0.5)
    audio.stop()
    audio.disable_input()
    print("  Done.\n")


def main():
    parser = argparse.ArgumentParser(
        description="Real-time effect processing with audio input via ring buffer.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "plugin", nargs="?",
        help="Path to effect plugin. Can also use MINIHOST_PLUGIN env var.",
    )
    parser.add_argument(
        "--input", "-i", metavar="FILE",
        help="Audio file to stream through the effect. Omit for sine wave demo.",
    )
    parser.add_argument(
        "--sample-rate", "-r", type=int, default=48000, metavar="N",
        help="Sample rate in Hz (default: 48000)",
    )
    parser.add_argument(
        "--buffer", "-b", type=int, default=512, metavar="N",
        help="Audio buffer size in frames (default: 512)",
    )
    parser.add_argument(
        "--buffer-capacity", type=int, default=0, metavar="N",
        help="Ring buffer capacity in frames (default: ~0.5s at sample rate)",
    )
    parser.add_argument(
        "--duration", "-d", type=float, default=5.0, metavar="SECS",
        help="Duration for sine wave demo (default: 5.0)",
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

    capacity = args.buffer_capacity if args.buffer_capacity > 0 else int(args.sample_rate * 0.5)

    if args.input:
        if not os.path.exists(args.input):
            print(f"Error: Input file not found: {args.input}", file=sys.stderr)
            sys.exit(1)
        example_file_through_effect(
            plugin_path, args.input, args.sample_rate, args.buffer, capacity,
        )
    else:
        example_sine_through_effect(
            plugin_path, args.sample_rate, args.buffer, capacity, args.duration,
        )


if __name__ == "__main__":
    main()
