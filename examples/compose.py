#!/usr/bin/env python3
"""Build callable audio pipelines with ``minihost.Compose``.

``Compose`` is an audiomentations-style composition layer over minihost's
native routing objects. A pipeline is an ordered list of *transforms*
applied to a whole buffer and returned as a new one. A transform is any
of:

  * a native processor -- ``Plugin`` / ``PluginChain`` / ``PluginBus``;
  * a pure-python transform -- ``Gain``, ``Normalize``, ``Trim``, ``Fade``;
  * a stochastic combinator -- ``Maybe``, ``OneOf``, ``SomeOf``,
    ``RandomParam``, ``AddGaussianNoise``;
  * a nested ``Compose``;
  * any callable ``fn(audio, sample_rate) -> audio``.

The pipeline is callable in the audiomentations idiom
(``fx(samples, sample_rate=...)``) and owns/closes the plugins it holds,
so an effects chain collapses to a single ``with`` block.

This script has three parts:

  1. Deterministic effect chain, file-to-file (delay -> reverb -> normalize).
  2. In-memory pure-python pipeline (no plugin, numpy only).
  3. Randomized augmentation pipeline for building ML training data.

Override the plugin/file paths via environment variables:
    MINIHOST_DELAY=/Library/Audio/Plug-Ins/Components/SomeDelay.component
    MINIHOST_REVERB=/Library/Audio/Plug-Ins/Components/SomeReverb.component
    MINIHOST_INPUT=path/to/input.wav
    MINIHOST_OUTPUT=path/to/output.wav

Requirements:
    - numpy (parts 2 and 3)
    - A delay and a reverb plugin for part 1 (skipped if missing).
"""

from __future__ import annotations

import os
import sys

import numpy as np

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
OUTPUT_PATH = os.environ.get("MINIHOST_OUTPUT", "piano_composed.wav")
SAMPLE_RATE = 48000


def deterministic_effect_chain() -> None:
    """Part 1: delay -> reverb -> normalize, file to file.

    ``Compose`` owns the two plugins and closes them on exit, so the
    whole chain lives in one ``with``. ``tail_seconds=4.0`` pads the
    input so the reverb tail rings out.
    """
    missing = [p for p in (DELAY_PATH, REVERB_PATH, INPUT_PATH) if not os.path.exists(p)]
    if missing:
        print("Part 1 (effect chain) skipped -- not found:")
        for p in missing:
            print(f"    {p}")
        return

    with minihost.Compose(
        [
            minihost.Plugin(DELAY_PATH, sample_rate=SAMPLE_RATE),
            minihost.Plugin(REVERB_PATH, sample_rate=SAMPLE_RATE),
            minihost.Normalize(-1.0),  # pure-python, brings peak to -1 dBFS
        ],
        tail_seconds=4.0,  # capture the reverb tail
    ) as fx:
        frames = fx.to_file(INPUT_PATH, OUTPUT_PATH)

    print(f"Part 1: {INPUT_PATH} -> {OUTPUT_PATH}  ({frames} frames @ {SAMPLE_RATE} Hz)")


def pure_python_pipeline() -> None:
    """Part 2: a plugin-free pipeline over an in-memory numpy array.

    With no native processor to infer the rate from, ``sample_rate`` is
    passed explicitly. numpy in -> numpy out; a 1-D array round-trips to
    1-D.
    """
    fx = minihost.Compose(
        [
            minihost.Gain(-6.0),
            minihost.Fade(fade_in=0.01, fade_out=0.01),
            minihost.Normalize(-3.0),
        ]
    )
    samples = np.random.uniform(-0.2, 0.2, size=(2, SAMPLE_RATE)).astype(np.float32)
    out = fx(samples, sample_rate=SAMPLE_RATE)
    peak_dbfs = 20.0 * np.log10(np.max(np.abs(out)) + 1e-12)
    print(f"Part 2: pure-python pipeline -> shape {out.shape}, peak {peak_dbfs:.1f} dBFS")


def augmentation_pipeline() -> None:
    """Part 3: a randomized pipeline for ML data augmentation.

    Every call re-rolls the stochastic transforms from the seeded RNG, so
    the pipeline is reproducible across runs but varied across calls.
    ``RandomParam`` is included only when a plugin is available.
    """
    transforms = [
        minihost.AddGaussianNoise(min_amplitude=0.001, max_amplitude=0.015),
        minihost.OneOf([minihost.Gain(-3.0), minihost.Gain(-6.0)]),
        minihost.Maybe(minihost.Normalize(-1.0), p=0.5),
    ]

    # Optionally fold a real plugin's parameter randomization into the mix.
    plugin = None
    if os.path.exists(REVERB_PATH):
        plugin = minihost.Plugin(REVERB_PATH, sample_rate=SAMPLE_RATE)
        # Randomize parameter index 0 across [0, 1] (normalized units).
        transforms.insert(0, minihost.RandomParam(plugin, 0, 0.0, 1.0))

    fx = minihost.Compose(transforms, seed=0)
    try:
        samples = np.zeros((2, SAMPLE_RATE // 2), dtype=np.float32)
        variants = [fx(samples, sample_rate=SAMPLE_RATE) for _ in range(3)]
    finally:
        if plugin is not None:
            plugin.close()

    peaks = [float(np.max(np.abs(v))) for v in variants]
    print(f"Part 3: 3 augmented variants, peaks {[round(p, 4) for p in peaks]}")


def main() -> int:
    deterministic_effect_chain()
    pure_python_pipeline()
    augmentation_pipeline()
    return 0


if __name__ == "__main__":
    sys.exit(main())
