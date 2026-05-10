#!/usr/bin/env python3
"""Process a .wav file through a chain of effect plugins.

Pipeline: piano.wav -> ValhallaDelay -> ValhallaSupermassive -> piano_processed.wav

Demonstrates the high-level :func:`minihost.process_audio_to_file` helper,
which handles block iteration, latency compensation, sample-rate
conversion, channel matching, and tail rendering. For lower-level control
see :func:`minihost.process_audio` (in-memory) or open-code the block loop
against :meth:`PluginChain.process`.

Override the plugin paths or input/output files via environment variables:
    MINIHOST_DELAY=/Library/Audio/Plug-Ins/Components/SomeDelay.component
    MINIHOST_REVERB=/Library/Audio/Plug-Ins/Components/SomeReverb.component
    MINIHOST_INPUT=path/to/input.wav
    MINIHOST_OUTPUT=path/to/output.wav
"""

from __future__ import annotations

import os
import sys

import minihost

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
    for path in (DELAY_PATH, REVERB_PATH, INPUT_PATH):
        if not os.path.exists(path):
            print(f"Not found: {path}", file=sys.stderr)
            return 1

    with (
        minihost.Plugin(DELAY_PATH, sample_rate=48000) as delay,
        minihost.Plugin(REVERB_PATH, sample_rate=48000) as reverb,
        minihost.PluginChain([delay, reverb]) as chain,
    ):
        frames = minihost.process_audio_to_file(
            chain,
            INPUT_PATH,
            OUTPUT_PATH,
            tail_seconds=4.0,  # capture reverb tail
        )

    print(f"{INPUT_PATH} -> {OUTPUT_PATH}  ({frames} frames @ 48000 Hz)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
