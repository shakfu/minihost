#!/usr/bin/env python3
"""Process a .wav file through a chain of effect plugins.

Pipeline: piano.wav -> ValhallaDelay -> ValhallaSupermassive -> piano_processed.wav

This example demonstrates the typical offline-render flow for an effect chain:
  1. Read the input audio with minihost.read_audio.
  2. Resample / channel-adjust to match the plugins' configured layout.
  3. Open each effect plugin and combine them with PluginChain.
  4. Process block-by-block, including a tail of silent input so reverb /
     delay tails are captured.
  5. Write the result with minihost.write_audio.

Override the plugin paths or input file via environment variables:
    MINIHOST_DELAY=/Library/Audio/Plug-Ins/Components/SomeDelay.component
    MINIHOST_REVERB=/Library/Audio/Plug-Ins/Components/SomeReverb.component
    MINIHOST_INPUT=path/to/input.wav
    MINIHOST_OUTPUT=path/to/output.wav

Defaults assume Valhalla DSP's free Supermassive + paid Delay AU plugins are
installed at the standard macOS paths, and that the script is run from the
repository root (so tests/_wav/piano.wav exists).
"""

from __future__ import annotations

import os
import sys

import numpy as np

import minihost

SAMPLE_RATE = 48000
BLOCK_SIZE = 512
TAIL_SECONDS = 4.0  # extra silent input so the reverb tail rings out

DELAY_PATH = os.environ.get(
    "MINIHOST_DELAY",
    "/Library/Audio/Plug-Ins/Components/ValhallaDelay.component",
)
REVERB_PATH = os.environ.get(
    "MINIHOST_REVERB",
    "/Library/Audio/Plug-Ins/Components/ValhallaSupermassive.component",
)
INPUT_PATH = os.environ.get("MINIHOST_INPUT", "tests/_wav/piano.wav")
OUTPUT_PATH = os.environ.get("MINIHOST_OUTPUT", "piano_processed.wav")


def main() -> int:
    for path in (DELAY_PATH, REVERB_PATH):
        if not os.path.exists(path):
            print(f"Plugin not found: {path}", file=sys.stderr)
            print("Set MINIHOST_DELAY / MINIHOST_REVERB to override.",
                  file=sys.stderr)
            return 1

    if not os.path.exists(INPUT_PATH):
        print(f"Input file not found: {INPUT_PATH}", file=sys.stderr)
        return 1

    # 1. Read the input audio.
    audio, in_sr = minihost.read_audio(INPUT_PATH)
    print(f"input: {INPUT_PATH}  ({audio.shape[0]} ch, {in_sr} Hz, "
          f"{audio.shape[1] / in_sr:.2f}s)")

    # 2. Match the plugins' configured rate (48 kHz) and stereo layout.
    if in_sr != SAMPLE_RATE:
        audio = minihost.resample(audio, in_sr, SAMPLE_RATE)
    if audio.shape[0] == 1:
        audio = np.vstack([audio, audio])  # mono -> stereo by duplication

    # 3. Open both effects and chain them. minihost.PluginChain pins the
    #    Plugin objects' lifetime to the chain (via nb::keep_alive), so it's
    #    safe to construct from temporaries -- but holding the variables
    #    here makes per-plugin tweaks (e.g. set_param) easy.
    with minihost.Plugin(DELAY_PATH, sample_rate=SAMPLE_RATE,
                         max_block_size=BLOCK_SIZE) as delay, \
         minihost.Plugin(REVERB_PATH, sample_rate=SAMPLE_RATE,
                         max_block_size=BLOCK_SIZE) as reverb, \
         minihost.PluginChain([delay, reverb]) as chain:

        in_ch = chain.num_input_channels
        out_ch = chain.num_output_channels
        total_frames = audio.shape[1] + int(TAIL_SECONDS * SAMPLE_RATE)
        output = np.zeros((out_ch, total_frames), dtype=np.float32)

        # 4. Process block-by-block. The input is zero-padded past the
        #    source duration so the chain keeps running and we capture the
        #    decay of the delay + reverb tails.
        in_block = np.zeros((in_ch, BLOCK_SIZE), dtype=np.float32)
        out_block = np.zeros((out_ch, BLOCK_SIZE), dtype=np.float32)

        for start in range(0, total_frames, BLOCK_SIZE):
            end = min(start + BLOCK_SIZE, total_frames)
            n = end - start

            # Source audio if any remains, otherwise silence.
            in_block[:] = 0
            if start < audio.shape[1]:
                copy = min(n, audio.shape[1] - start)
                in_block[:in_ch, :copy] = audio[:in_ch, start:start + copy]

            chain.process(in_block, out_block)
            output[:, start:end] = out_block[:, :n]

    # 5. Write the result.
    minihost.write_audio(OUTPUT_PATH, output, SAMPLE_RATE, bit_depth=24)
    print(f"output: {OUTPUT_PATH}  ({out_ch} ch, {SAMPLE_RATE} Hz, "
          f"{output.shape[1] / SAMPLE_RATE:.2f}s, "
          f"peak {float(np.max(np.abs(output))):.3f})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
