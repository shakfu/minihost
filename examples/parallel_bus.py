#!/usr/bin/env python3
"""Layer several instruments under one MIDI part with ``minihost.PluginBus``.

A :class:`~minihost.PluginBus` runs N ``PluginChain`` branches in parallel
against the same input and sums their outputs with a per-branch gain (a mix
bus). With :meth:`PluginBus.process_midi` the *same* MIDI is delivered to
every branch, so one chord drives every instrument at once -- the idiomatic
way to build a layered/stacked sound (e.g. saw + sub + pad).

This example loads one or more synth plugins, puts each in its own branch with
a different gain, renders a C-major chord through the bus, and writes the
summed result to a WAV file. It uses the ``AudioBuffer`` container throughout,
so it needs no numpy.

For parallel *effect* processing (parallel compression, dry-bus + reverb-send)
use :meth:`PluginBus.process` instead -- same fan-out-and-sum, no MIDI. For
arbitrary node-to-node routing, use :class:`~minihost.PluginGraph`.

When the branches themselves emit MIDI (e.g. parallel arpeggiators),
:meth:`PluginBus.process_midi` returns an ``(events, overflow)`` tuple: the
merged branch MIDI as ``(sample_offset, status, data1, data2)`` tuples,
time-ordered by offset (so it can drive a downstream instrument), plus a
bool flagging whether the ``midi_out_capacity`` cap was hit. Instrument
branches emit no MIDI, so this example ignores the return value.

Usage:
    # One instrument per path; each becomes a branch (pass 2-3 to layer):
    python parallel_bus.py /path/to/saw.vst3 /path/to/sub.vst3 /path/to/pad.vst3

    # Single plugin, or via env var:
    export MINIHOST_PLUGIN=/path/to/synth.vst3
    python parallel_bus.py

    # Choose the output file:
    python parallel_bus.py synth.vst3 -o layered.wav

Requirements:
    - One or more instrument plugins (VST3 or AudioUnit) that accept MIDI.
    - All branches must share I/O channel layout (this example opens each
      plugin as stereo-in / stereo-out, which suits most synths).
"""

from __future__ import annotations

import argparse
import contextlib
import math
import os
import sys

import minihost

SAMPLE_RATE = 48000
BLOCK = 512
CHANNELS = 2
CHORD = (60, 64, 67)  # C4, E4, G4
VELOCITY = 100
NOTE_SECONDS = 1.0  # how long the chord is held
TAIL_SECONDS = 1.5  # extra time rendered after note-off for release/tails
# Per-branch summing gains, applied in order; extra branches default to 1.0.
GAINS = (1.0, 0.7, 0.5, 0.4)


def events_in_block(events, start, count):
    """Translate absolute-frame MIDI events into block-relative offsets.

    ``events`` is a list of ``(abs_frame, status, data1, data2)`` tuples;
    returns the events landing in ``[start, start + count)`` with their
    ``sample_offset`` rebased to the block.
    """
    return [
        (frame - start, status, d1, d2)
        for (frame, status, d1, d2) in events
        if start <= frame < start + count
    ]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Layer instruments under one MIDI part with PluginBus.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "plugins", nargs="*",
        help="Instrument plugin path(s); each becomes a bus branch. "
             "Falls back to the MINIHOST_PLUGIN env var.",
    )
    parser.add_argument(
        "--output", "-o", default="layered.wav", metavar="FILE",
        help="Output WAV path (default: layered.wav).",
    )
    args = parser.parse_args()

    paths = args.plugins or (
        [os.environ["MINIHOST_PLUGIN"]] if os.environ.get("MINIHOST_PLUGIN") else []
    )
    if not paths:
        print("Error: no plugin specified. Pass a path or set MINIHOST_PLUGIN.",
              file=sys.stderr)
        return 1
    for path in paths:
        if not os.path.exists(path):
            print(f"Not found: {path}", file=sys.stderr)
            return 1

    note_frames = int(NOTE_SECONDS * SAMPLE_RATE)
    total_frames = note_frames + int(TAIL_SECONDS * SAMPLE_RATE)
    num_blocks = math.ceil(total_frames / BLOCK)

    # Note-on for the chord at frame 0; note-off after the hold time.
    midi = [(0, 0x90, p, VELOCITY) for p in CHORD]
    midi += [(note_frames, 0x80, p, 0) for p in CHORD]

    with contextlib.ExitStack() as stack:
        # Open every plugin and wrap each in its own single-plugin chain.
        chains = []
        for path in paths:
            plugin = stack.enter_context(
                minihost.Plugin(path, sample_rate=SAMPLE_RATE, max_block_size=BLOCK,
                                in_channels=CHANNELS, out_channels=CHANNELS)
            )
            chains.append(stack.enter_context(minihost.PluginChain([plugin])))

        # A bus requires every branch to share the I/O layout, so derive it
        # from the first branch; add_branch raises if a later one differs.
        in_ch = chains[0].num_input_channels
        out_ch = chains[0].num_output_channels
        bus = stack.enter_context(
            minihost.PluginBus(in_ch, out_ch,
                               max_block_size=BLOCK, sample_rate=float(SAMPLE_RATE))
        )
        for i, (path, chain) in enumerate(zip(paths, chains)):
            gain = GAINS[i] if i < len(GAINS) else 1.0
            bus.add_branch(chain, gain=gain)
            print(f"  branch {i}: {os.path.basename(path)}  gain={gain}")

        print(f"Layering {bus.num_branches} instrument(s) "
              f"({in_ch}-in/{out_ch}-out) -> {args.output}")

        # Synths ignore audio input, but minihost still hands them a buffer.
        silence = minihost.AudioBuffer(in_ch, BLOCK)
        silence.clear()

        out = minihost.AudioBuffer(out_ch, total_frames)
        out.clear()

        block_out = minihost.AudioBuffer(out_ch, BLOCK)
        for b in range(num_blocks):
            start = b * BLOCK
            count = min(BLOCK, total_frames - start)
            # Fan this block's MIDI to every branch and sum the audio.
            bus.process_midi(silence, block_out, events_in_block(midi, start, count))
            out[:, start:start + count] = block_out[:, :count]

        minihost.write_audio(args.output, out, SAMPLE_RATE, bit_depth=24)

    peak = out.magnitude()
    peak_db = 20.0 * math.log10(peak) if peak > 0 else -120.0
    print(f"Wrote {args.output}: {total_frames} frames @ {SAMPLE_RATE} Hz, "
          f"peak {peak_db:.1f} dBFS")
    if peak <= 0:
        print("  (output is silent -- is the plugin an instrument that accepts MIDI?)",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
