#!/usr/bin/env python3
"""Render one audio file through the same six-stage effects chain, once as
AudioUnits and once as VST3s, then null-test the two results.

Chain (identical in both formats):

    piano.wav -> EQ -> Saturation -> Compressor -> Delay -> Reverb -> Limiter -> .wav

The demo exercises the parts of minihost that matter for offline
rendering:

  * format-agnostic plugin loading (``.component`` vs ``.vst3``);
  * parameter addressing by *name* plus real-unit text
    (``param_from_text``), which is the only portable way to script a
    plugin across formats -- FabFilter's VST3 builds expose extra
    parameters, so raw indices do not line up between AU and VST3;
  * ``PluginChain`` for serial routing, with ``set_non_realtime(True)``
    so plugins take their offline code paths;
  * ``process_audio_to_file`` for the block loop, sample-rate
    conversion (the input is mono 22.05 kHz), mono-to-stereo
    duplication, plugin-delay compensation, and tail rendering;
  * cumulative per-stage stems, so each effect can be auditioned in
    isolation;
  * an AU-vs-VST3 null test on the rendered files, calibrated against a
    run-to-run repeatability check.

That last point is the interesting one. Rendering each format twice
first establishes a noise floor: some plugins are not bit-reproducible
across runs, because free-running modulation and analog-style drift
carry internal state that ``reset()`` does not reseed. In the default
chain the EQ, compressor, and limiter null to the last bit, while the
delay and the reverb do not -- so the AU-vs-VST3 residual is only
meaningful when read against the same format's run-to-run residual. If
the two numbers agree, the formats agree as closely as the plugins can
repeat themselves.

The chain runs at three settings, defined in ``PRESETS`` and rendered in
turn, so the same measurement can be watched under increasing load:

  * ``light`` -- corrective moves: 2 dB of EQ, 12 percent drive, 2:1
    compression, a trace of delay and reverb.
  * ``medium`` -- audible mix-bus processing: 6 dB EQ cuts, 45 percent
    drive, 4:1 compression from -24 dB, 6 dB into the limiter, with the
    wet stages deliberately held back.
  * ``aggressive`` -- destructive: a 12 dB low-mid scoop against a 9 dB
    presence boost, 90 percent drive, 8:1 compression from -36 dB with a
    0.5 ms attack, a 70 percent feedback ping-pong delay, a six-second
    reverb at 150 percent decay, and 12 dB slammed into the limiter.

Measured against the dry file, short-term dynamic range falls from about
19 dB to roughly 16, 12, and 6 dB across the three. Each stage trims its
own output so no intermediate stem clips, and every stem's peak is
printed so that stays checkable.

The presets vary along a second axis at the same time: how much of the
output comes from the delay and the reverb, the two plugins that are not
reproducible run to run. That is what decides whether the null test
still measures anything. Under ``light`` and ``medium`` the residual
lands 15 dB or more below the signal and the comparison is decisive;
under ``aggressive`` the feedback and decay amplify the plugins' internal
state until the residual rivals the signal, and the chain-level null
stops meaning anything at all -- which the script says out loud, and
which the closing table shows as a column. The per-stage stem table
keeps working at every setting. The lesson is transferable: dry stages
can be pushed as hard as you like, but wet, self-feeding stages are what
cost you measurability.

Outputs land in ``build/output/`` by default, one subdirectory per
preset so the file names stay identical across variants and can be A/B'd
or diffed directly::

    piano_dry_48000.wav              input resampled/duplicated, unprocessed
    light/piano_au_chain.wav         full AudioUnit chain
    light/piano_au_chain_take2.wav   second pass, for the repeatability check
    light/piano_vst3_chain.wav       full VST3 chain
    light/piano_au_stem_1_eq.wav     cumulative stem after stage 1 (etc.)
    light/piano_au_vst3_null.wav     difference of the two (32-bit float)
    medium/...                       same names again
    aggressive/...

Usage::

    uv run python examples/fx_chain_au_vst3.py
    uv run python examples/fx_chain_au_vst3.py --presets medium
    uv run python examples/fx_chain_au_vst3.py --formats au --no-stems
    uv run python examples/fx_chain_au_vst3.py --sample-rate 96000 --tail 6

The chain is built from FabFilter plugins because they ship in both
formats with matching parameter names. Substitute your own via the
``--au-dir`` / ``--vst3-dir`` options or by editing ``CHAIN``; any stage
whose plugin is missing is dropped with a warning, and a format with no
plugins at all is skipped entirely.

Requires numpy for the level metrics and the null test.
"""

from __future__ import annotations

import argparse
import contextlib
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

import minihost

REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_AU_DIRS = (
    Path("/Library/Audio/Plug-Ins/Components"),
    Path.home() / "Library/Audio/Plug-Ins/Components",
)
DEFAULT_VST3_DIRS = (
    Path("/Library/Audio/Plug-Ins/VST3"),
    Path.home() / "Library/Audio/Plug-Ins/VST3",
)

FORMATS = ("au", "vst3")
SUFFIX = {"au": ".component", "vst3": ".vst3"}


@dataclass(frozen=True)
class Stage:
    """One link in the chain, described independently of plugin format.

    ``params`` maps parameter *names* to the human-readable text a user
    would type into the plugin UI. The text is converted with
    ``Plugin.param_from_text`` at load time, so the demo never hardcodes
    normalized 0..1 values -- those differ per plugin and are unreadable.
    """

    slug: str
    role: str
    plugin: str
    params: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class Preset:
    """One setting of the whole chain, from restrained to destructive.

    ``params`` is keyed by stage slug. ``tail_seconds`` travels with the
    preset because the settings decide it: a two-second reverb needs far
    less rendered tail than a six-second one at 150 percent decay.
    """

    name: str
    summary: str
    tail_seconds: float
    params: dict[str, dict[str, str]]


# The chain itself: slug, printed role, plugin name. Formats and presets
# vary around this; the order and the plugins do not.
CHAIN: tuple[tuple[str, str, str], ...] = (
    ("eq", "EQ", "FabFilter Pro-Q 4"),
    ("saturation", "Saturation", "FabFilter Saturn 2"),
    ("compressor", "Compressor", "FabFilter Pro-C 3"),
    ("delay", "Delay", "FabFilter Timeless 3"),
    ("reverb", "Reverb", "FabFilter Pro-R 2"),
    # Pro-L 2 reports 3115 samples of latency; process_audio_to_file
    # removes it, which is what keeps every render sample-aligned.
    ("limiter", "Limiter", "FabFilter Pro-L 2"),
)

# The three presets differ along two axes at once, deliberately. The
# audible one is how hard the signal is worked. The measurable one is
# how much of the output comes from the two plugins that are not
# reproducible run to run -- the delay's feedback and the reverb's
# decay. Raising the first while holding the second down is what keeps
# the null test meaningful; the aggressive preset gives that up.
PRESETS: dict[str, Preset] = {
    "light": Preset(
        name="light",
        summary="gentle corrective moves; the null test is cleanest here",
        tail_seconds=3.0,
        params={
            "eq": {
                "Band 1 Used": "1",
                "Band 1 Enabled": "1",
                "Band 1 Frequency": "220 Hz",
                "Band 1 Gain": "-2 dB",
                "Band 1 Q": "1.0",
                "Band 2 Used": "1",
                "Band 2 Enabled": "1",
                "Band 2 Frequency": "3200 Hz",
                "Band 2 Gain": "+1.5 dB",
                "Band 2 Q": "0.8",
                "Output Level": "0 dB",
            },
            "saturation": {
                "Band 1 Drive": "12%",
                "Band 1 Mix": "100%",
                "Output Gain": "0 dB",
            },
            "compressor": {
                "Threshold": "-14 dB",
                "Ratio": "2:1",
                "Attack": "20 ms",
                "Release": "200 ms",
                "Mix": "100%",
            },
            "delay": {
                "Delay Time": "375 ms",
                "Feedback": "12%",
                "Mix": "5%",
            },
            "reverb": {
                "Space": "1.5 sec",
                "Decay Rate": "100%",
                "Mix": "7%",
            },
            "limiter": {
                "Gain": "+1 dB",
                "Output Level": "-1 dBTP",
            },
        },
    ),
    "medium": Preset(
        name="medium",
        summary="audible mix-bus processing that still nulls well",
        tail_seconds=5.0,
        params={
            "eq": {
                "Band 1 Used": "1",
                "Band 1 Enabled": "1",
                "Band 1 Frequency": "220 Hz",
                "Band 1 Gain": "-6 dB",
                "Band 1 Q": "1.6",
                "Band 2 Used": "1",
                "Band 2 Enabled": "1",
                "Band 2 Frequency": "3200 Hz",
                "Band 2 Gain": "+4.5 dB",
                "Band 2 Q": "1.0",
                "Band 3 Used": "1",
                "Band 3 Enabled": "1",
                "Band 3 Frequency": "90 Hz",
                "Band 3 Gain": "+3 dB",
                "Band 3 Q": "1.0",
                "Output Level": "-3 dB",
            },
            "saturation": {
                "Band 1 Drive": "45%",
                "Band 1 Mix": "100%",
                "Output Gain": "-4 dB",
            },
            "compressor": {
                "Threshold": "-24 dB",
                "Ratio": "4:1",
                "Attack": "3 ms",
                "Release": "80 ms",
                "Knee": "3 dB",
                "Mix": "100%",
            },
            "delay": {
                "Delay Time": "375 ms",
                # Held back on purpose. The dry stages above can be
                # pushed as hard as you like without hurting the null
                # test; this wet path is what carries the delay's
                # nondeterminism into the output, so its mix is the knob
                # that decides whether the measurement stays usable.
                "Feedback": "20%",
                "Feedback Cross Mix": "50%",
                "Mix": "7%",
            },
            "reverb": {
                "Space": "2 sec",
                "Decay Rate": "100%",
                "Brightness": "20%",
                "Character": "40%",
                "Mix": "9%",
            },
            "limiter": {
                "Gain": "+6 dB",
                "Output Level": "-0.5 dBTP",
            },
        },
    ),
    "aggressive": Preset(
        name="aggressive",
        summary="destructive settings; the chain-level null stops meaning anything",
        tail_seconds=8.0,
        params={
            "eq": {
                # Band 1: gut the low mids rather than merely tame them.
                "Band 1 Used": "1",
                "Band 1 Enabled": "1",
                "Band 1 Frequency": "220 Hz",
                "Band 1 Gain": "-12 dB",
                "Band 1 Q": "2.5",
                # Band 2: hard presence bite.
                "Band 2 Used": "1",
                "Band 2 Enabled": "1",
                "Band 2 Frequency": "3200 Hz",
                "Band 2 Gain": "+9 dB",
                "Band 2 Q": "1.5",
                # Band 3: low thump to replace what band 1 removed.
                "Band 3 Used": "1",
                "Band 3 Enabled": "1",
                "Band 3 Frequency": "90 Hz",
                "Band 3 Gain": "+6 dB",
                "Band 3 Q": "1.0",
                # Trim so the boosts do not hand the next stage a clipped signal.
                "Output Level": "-6 dB",
            },
            "saturation": {
                "Band 1 Drive": "90%",
                "Band 1 Mix": "100%",
                # Heavy drive adds a lot of level; take it back at the output.
                "Output Gain": "-9 dB",
            },
            "compressor": {
                # Deep threshold, high ratio, fast attack, short release:
                # the piano is pinned flat and pumps audibly. Auto Gain
                # (on by default) makes up the loss, so the stage gets
                # much louder.
                "Threshold": "-36 dB",
                "Ratio": "8:1",
                "Attack": "0.5 ms",
                "Release": "40 ms",
                "Knee": "0 dB",
                "Mix": "100%",
            },
            "delay": {
                "Delay Time": "375 ms",
                # High feedback plus full cross mix: long ping-pong
                # repeats that need the extra tail seconds to ring out.
                "Feedback": "70%",
                "Feedback Cross Mix": "100%",
                "Mix": "45%",
            },
            "reverb": {
                "Space": "6 sec",
                "Decay Rate": "150%",
                "Brightness": "40%",
                "Character": "70%",
                "Stereo Width": "110%",
                "Mix": "50%",
            },
            "limiter": {
                # Slam it: 12 dB into the ceiling, output just under 0 dBTP.
                "Gain": "+12 dB",
                "Output Level": "-0.3 dBTP",
            },
        },
    ),
}


def stages_for(preset: Preset) -> tuple[Stage, ...]:
    """Bind a preset's parameter sets to the chain skeleton."""
    return tuple(
        Stage(slug=slug, role=role, plugin=plugin, params=preset.params.get(slug, {}))
        for slug, role, plugin in CHAIN
    )


# ---------------------------------------------------------------------------
# plugin discovery
# ---------------------------------------------------------------------------


def resolve_plugin(name: str, fmt: str, search_dirs: tuple[Path, ...]) -> Path | None:
    """Return the on-disk bundle for ``name`` in ``fmt``, or None."""
    for directory in search_dirs:
        candidate = directory / f"{name}{SUFFIX[fmt]}"
        if candidate.exists():
            return candidate
    return None


# ---------------------------------------------------------------------------
# loading and configuration
# ---------------------------------------------------------------------------


def apply_params(plugin: minihost.Plugin, stage: Stage) -> list[str]:
    """Apply a stage's named parameters; return one report line per param.

    Names that a given format does not expose are reported rather than
    raised, so a chain still renders when the AU and VST3 builds of a
    plugin disagree about their parameter list.
    """
    lines = []
    for name, text in stage.params.items():
        try:
            index = plugin.find_param(name)
        except RuntimeError:
            lines.append(f"      {name:24s} -- not exposed by this format, skipped")
            continue
        value = plugin.param_from_text(index, text)
        plugin.set_param(index, value)
        readback = plugin.param_to_text(index, plugin.get_param(index))
        lines.append(f"      {name:24s} [{index:4d}] = {readback}")
    return lines


def load_stages(
    stages: tuple[Stage, ...],
    fmt: str,
    search_dirs: tuple[Path, ...],
    sample_rate: float,
    block_size: int,
    stack: contextlib.ExitStack,
    verbose: bool,
) -> list[tuple[Stage, minihost.Plugin]]:
    """Open every available stage plugin in ``fmt`` and configure it."""
    loaded: list[tuple[Stage, minihost.Plugin]] = []
    for stage in stages:
        path = resolve_plugin(stage.plugin, fmt, search_dirs)
        if path is None:
            print(f"  [skip] {stage.role:11s} {stage.plugin} ({fmt}) not installed")
            continue

        opened = time.perf_counter()
        plugin = minihost.Plugin(
            str(path),
            sample_rate=sample_rate,
            max_block_size=block_size,
        )
        stack.callback(plugin.close)
        load_ms = (time.perf_counter() - opened) * 1e3

        report = apply_params(plugin, stage)
        print(
            f"  [ok]   {stage.role:11s} {path.name:26s} "
            f"{plugin.num_input_channels}in/{plugin.num_output_channels}out  "
            f"{plugin.num_params:4d} params  "
            f"latency {plugin.latency_samples:5d}  "
            f"loaded in {load_ms:6.1f} ms"
        )
        if verbose:
            for line in report:
                print(line)
        loaded.append((stage, plugin))
    return loaded


# ---------------------------------------------------------------------------
# metrics
# ---------------------------------------------------------------------------


def db(value: float) -> float:
    return 20.0 * float(np.log10(max(value, 1e-12)))


def measure(path: Path) -> dict:
    """Peak/RMS levels of a rendered file."""
    audio, rate = minihost.read_audio(path, as_=np.ndarray)
    audio = np.asarray(audio, dtype=np.float64)
    return {
        "peak_db": db(float(np.max(np.abs(audio)))),
        "rms_db": db(float(np.sqrt(np.mean(audio**2)))),
        "frames": audio.shape[-1],
        "channels": audio.shape[0],
        "sample_rate": rate,
    }


def compare(path_a: Path, path_b: Path, residual_path: Path | None = None) -> dict | None:
    """Null two renders against each other.

    Returns the residual level relative to the first render, or None if
    the files are not comparable. With ``residual_path`` the difference
    signal is written out so it can be auditioned.
    """
    a, rate_a = minihost.read_audio(path_a, as_=np.ndarray)
    b, rate_b = minihost.read_audio(path_b, as_=np.ndarray)
    a = np.asarray(a, dtype=np.float64)
    b = np.asarray(b, dtype=np.float64)
    if rate_a != rate_b or a.shape[0] != b.shape[0]:
        return None

    frames = min(a.shape[-1], b.shape[-1])
    trimmed = abs(a.shape[-1] - b.shape[-1])
    a, b = a[:, :frames], b[:, :frames]
    residual = a - b

    if residual_path is not None:
        # 32-bit float: a difference signal can exceed 0 dBFS when the two
        # renders are out of phase, and integer formats would clip it.
        minihost.write_audio(residual_path, residual.astype(np.float32), rate_a, bit_depth=32)

    ref_rms = db(float(np.sqrt(np.mean(a**2))))
    res_rms = db(float(np.sqrt(np.mean(residual**2))))
    return {
        "res_peak_db": db(float(np.max(np.abs(residual)))),
        "res_rms_db": res_rms,
        "ref_rms_db": ref_rms,
        "below_db": ref_rms - res_rms,
        "corr": float(np.corrcoef(a.ravel(), b.ravel())[0, 1]),
        "frames": frames,
        "trimmed": trimmed,
    }


# ---------------------------------------------------------------------------
# rendering
# ---------------------------------------------------------------------------


def render_dry_reference(
    input_path: Path, out_path: Path, sample_rate: int, bit_depth: int
) -> None:
    """Write the unprocessed input at the render rate, in stereo, for A/B."""
    audio, rate = minihost.read_audio(input_path, as_=np.ndarray)
    audio = np.asarray(audio, dtype=np.float32)
    if rate != sample_rate:
        audio = minihost.resample(audio, rate, sample_rate)
    if audio.shape[0] == 1:
        audio = np.repeat(audio, 2, axis=0)
    minihost.write_audio(out_path, audio, sample_rate, bit_depth=bit_depth)


def render_chain(
    plugins: list[minihost.Plugin],
    out_path: Path,
    args: argparse.Namespace,
    tail: float,
) -> tuple[int, float, int, float]:
    """Render the whole input through ``plugins`` into ``out_path``.

    Every plugin is reset first so delay lines and reverb tails from a
    previous render cannot bleed into this one.
    """
    for plugin in plugins:
        plugin.reset()
    with minihost.PluginChain(plugins) as chain:
        # Offline rendering: plugins may take higher-quality or
        # non-causal code paths and must not drop blocks.
        chain.set_non_realtime(True)
        started = time.perf_counter()
        frames = minihost.process_audio_to_file(
            chain,
            args.input,
            out_path,
            tail_seconds=tail,
            block_size=args.block_size,
            bit_depth=args.bit_depth,
            # The limiter reports 3115 samples of delay; strip it so the
            # AU and VST3 renders stay sample-aligned with the dry
            # reference, and with each other.
            compensate_latency=True,
        )
        elapsed = time.perf_counter() - started
        return frames, elapsed, chain.latency_samples, chain.tail_seconds


def render_format(
    fmt: str,
    preset: Preset,
    tail: float,
    search_dirs: tuple[Path, ...],
    args: argparse.Namespace,
    out_dir: Path,
) -> dict | None:
    """Render the full chain (and optional stems) for one plugin format."""
    label = fmt.upper()
    print(f"\n--- {preset.name} / {label} ---")

    with contextlib.ExitStack() as stack:
        loaded = load_stages(
            stages_for(preset),
            fmt,
            search_dirs,
            float(args.sample_rate),
            args.block_size,
            stack,
            args.verbose,
        )
        if not loaded:
            print(f"  no {label} plugins available, format skipped")
            return None

        plugins = [plugin for _, plugin in loaded]

        # Cumulative stems: stage 1, stages 1-2, ... Each sub-chain is a
        # separate PluginChain over the same Plugin objects; closing a
        # chain does not close its plugins, and reset() clears delay and
        # reverb state so one stem never bleeds into the next.
        if args.stems:
            for count in range(1, len(plugins) + 1):
                stage = loaded[count - 1][0]
                stem_path = out_dir / f"piano_{fmt}_stem_{count}_{stage.slug}.wav"
                for plugin in plugins[:count]:
                    plugin.reset()
                with minihost.PluginChain(plugins[:count]) as sub:
                    sub.set_non_realtime(True)
                    minihost.process_audio_to_file(
                        sub,
                        args.input,
                        stem_path,
                        tail_seconds=tail,
                        block_size=args.block_size,
                        bit_depth=args.bit_depth,
                    )
                peak = measure(stem_path)["peak_db"]
                flag = "  CLIPPED" if peak >= -0.05 else ""
                print(
                    f"  stem  {count}/{len(plugins)}  {stem_path.name:34s} "
                    f"peak {peak:+7.2f} dBFS{flag}"
                )

        out_path = out_dir / f"piano_{fmt}_chain.wav"
        frames, elapsed, chain_latency, chain_tail = render_chain(
            plugins, out_path, args, tail
        )

        take2_path = None
        if args.repeat_check:
            take2_path = out_dir / f"piano_{fmt}_chain_take2.wav"
            render_chain(plugins, take2_path, args, tail)

    metrics = measure(out_path)
    duration = frames / float(args.sample_rate)
    print(
        f"  render {out_path.name}: {frames} frames "
        f"({duration:.2f} s) in {elapsed:.2f} s "
        f"= {duration / elapsed:.1f}x realtime"
    )
    print(
        f"  chain latency {chain_latency} samples, "
        f"reported tail {chain_tail:.2f} s, "
        f"peak {metrics['peak_db']:+.2f} dBFS, rms {metrics['rms_db']:+.2f} dBFS"
    )

    repeat = None
    if take2_path is not None:
        repeat = compare(out_path, take2_path)
        if repeat is not None:
            verdict = (
                "bit-reproducible"
                if repeat["res_rms_db"] < -140.0
                else f"varies between runs, {repeat['below_db']:.1f} dB below signal"
            )
            print(f"  repeatability: residual rms {repeat['res_rms_db']:+.2f} dBFS -- {verdict}")

    return {
        "format": fmt,
        "path": out_path,
        "take2_path": take2_path,
        "repeat": repeat,
        "stages": [stage.role for stage, _ in loaded],
        "slugs": [stage.slug for stage, _ in loaded],
        "frames": frames,
        "elapsed": elapsed,
        "latency": chain_latency,
        **metrics,
    }


# ---------------------------------------------------------------------------
# comparison
# ---------------------------------------------------------------------------


def stage_divergence(au: dict, vst3: dict, out_dir: Path) -> None:
    """Null the cumulative stems to locate where the formats part ways."""
    rows = []
    for index, slug in enumerate(au["slugs"], start=1):
        a = out_dir / f"piano_au_stem_{index}_{slug}.wav"
        b = out_dir / f"piano_vst3_stem_{index}_{slug}.wav"
        if not (a.exists() and b.exists()):
            return
        result = compare(a, b)
        if result is None:
            return
        rows.append((index, slug, result))

    if not rows:
        return

    print("\n    cumulative stems, AU vs VST3:")
    print(f"      {'stage':22s} {'residual rms':>13s} {'below signal':>13s} {'corr':>9s}")
    for index, slug, r in rows:
        print(
            f"      {index}. {slug:19s} {r['res_rms_db']:13.2f} "
            f"{r['below_db']:13.1f} {r['corr']:9.6f}"
        )


# How far under the signal a residual must sit for the null test to be
# decisive on its own. Closer than this and the measurement is usually
# dominated by the delay feedback and reverb decay amplifying plugin
# state rather than by anything about the formats.
#
# "Usually", not "always": a large residual can also mean the two
# formats really do render differently, which is a finding, not noise.
# The run-to-run floor is what separates the two cases, so a residual
# standing clear of that floor counts as usable however loud it is. See
# the meaningful/verdict logic in null_test().
MEANINGFUL_MARGIN_DB = 12.0


def null_test(au: dict, vst3: dict, out_dir: Path, bit_depth: int = 24) -> dict | None:
    """Subtract the AU and VST3 renders and interpret the residual.

    A perfect null is not the expectation. The AU and VST3 builds of a
    plugin can differ in default state, parameter smoothing, or
    oversampling -- and, more importantly here, some plugins are not
    even reproducible against themselves. The repeatability figure
    measured earlier is the floor this residual should be read against.

    Returns the measurement plus a one-word verdict, or None when the
    two renders cannot be compared.
    """
    print("\n  AU vs VST3 null test:")
    result = compare(au["path"], vst3["path"], out_dir / "piano_au_vst3_null.wav")
    if result is None:
        print("    not comparable: differing sample rate or channel count")
        return None

    if result["trimmed"]:
        print(f"    lengths differ by {result['trimmed']} frames, comparing {result['frames']}")
    print(
        f"    residual peak {result['res_peak_db']:+.2f} dBFS, "
        f"rms {result['res_rms_db']:+.2f} dBFS"
    )
    print(f"    that is {result['below_db']:.1f} dB below the AU render's rms")
    print(f"    correlation {result['corr']:.6f}")
    print("    difference written to piano_au_vst3_null.wav (32-bit float)")

    # Everything here is read against two floors: how well the plugins
    # repeat themselves, and the resolution of the files being compared.
    floors = [r["repeat"]["res_rms_db"] for r in (au, vst3) if r.get("repeat")]
    nondeterminism = max(floors) if floors else None
    quantization = -6.02 * bit_depth + 12.0

    if nondeterminism is not None:
        print(f"    run-to-run floor for these plugins: {nondeterminism:+.2f} dBFS")

    if nondeterminism is not None and result["res_rms_db"] <= nondeterminism + 3.0:
        verdict = "nondeterminism"
        print(
            "    verdict: the formats differ by no more than the chain differs "
            "from itself -- AU and VST3 agree to within plugin nondeterminism"
        )
    elif result["res_rms_db"] <= quantization:
        verdict = "identical"
        print(
            f"    verdict: the residual is at the {bit_depth}-bit file's own "
            "quantization floor -- the renders are identical for practical purposes"
        )
    elif result["below_db"] >= 60.0:
        verdict = "inaudible"
        print(
            "    verdict: the formats differ, but the difference sits "
            f"{result['below_db']:.0f} dB under the signal and will not be audible"
        )
    else:
        verdict = "different"
        basis = "both floors" if nondeterminism is not None else "the file's quantization floor"
        print(
            f"    verdict: the residual is well above {basis}, so the two "
            "formats genuinely render differently"
        )

    # A residual close to the signal is uninformative only when something
    # else is saturating the measurement. A verdict of "different" means
    # the residual stands clear of the run-to-run floor, which is a real
    # finding however close to the signal it lands -- but only when that
    # floor was actually measured. Without the repeatability pass there
    # is nothing to stand clear of, so the residual stays ambiguous.
    meaningful = result["below_db"] >= MEANINGFUL_MARGIN_DB or (
        verdict == "different" and nondeterminism is not None
    )

    if not meaningful:
        if nondeterminism is None:
            print(
                "    note: the residual is not meaningfully below the signal, and "
                "without --repeat-check there is no run-to-run floor to compare it "
                "against, so this null cannot separate a format difference from "
                "plugin nondeterminism."
            )
        else:
            print(
                "    note: the residual is not meaningfully below the signal, so this "
                "preset's chain-level null says little either way. High delay feedback "
                "and a long reverb decay amplify the plugins' internal state "
                "differences -- read the per-stage table instead."
            )

    stage_divergence(au, vst3, out_dir)
    return {**result, "verdict": verdict, "meaningful": meaningful}


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render an audio file through matching AU and VST3 effect chains.",
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=REPO_ROOT / "tests/_wav/piano.wav",
        help="input audio file (default: tests/_wav/piano.wav)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=REPO_ROOT / "build/output",
        help="directory for rendered .wav files (default: build/output)",
    )
    parser.add_argument(
        "--formats",
        default="au,vst3",
        help="comma-separated subset of: au, vst3 (default: both)",
    )
    parser.add_argument(
        "--presets",
        default=",".join(PRESETS),
        help=f"comma-separated subset of: {', '.join(PRESETS)} (default: all three)",
    )
    parser.add_argument("--sample-rate", type=int, default=48000, help="render rate in Hz")
    parser.add_argument("--block-size", type=int, default=512, help="processing block size")
    parser.add_argument(
        "--tail",
        type=float,
        default=None,
        help=(
            "extra seconds rendered past the input so delay/reverb tails ring out "
            "(default: per preset, "
            + ", ".join(f"{p.name} {p.tail_seconds:.0f}s" for p in PRESETS.values())
            + ")"
        ),
    )
    parser.add_argument("--bit-depth", type=int, default=24, choices=(16, 24, 32))
    parser.add_argument(
        "--no-stems",
        dest="stems",
        action="store_false",
        help="skip the cumulative per-stage renders",
    )
    parser.add_argument(
        "--no-repeat-check",
        dest="repeat_check",
        action="store_false",
        help="skip the second pass that measures run-to-run reproducibility",
    )
    parser.add_argument(
        "--au-dir",
        type=Path,
        action="append",
        help="extra directory to search for .component bundles (repeatable)",
    )
    parser.add_argument(
        "--vst3-dir",
        type=Path,
        action="append",
        help="extra directory to search for .vst3 bundles (repeatable)",
    )
    parser.add_argument(
        "-v",
        "--verbose",
        action="store_true",
        help="print every parameter as it is applied",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    formats = [f.strip().lower() for f in args.formats.split(",") if f.strip()]
    unknown = [f for f in formats if f not in FORMATS]
    if unknown:
        print(f"Unknown format(s): {', '.join(unknown)}", file=sys.stderr)
        return 2

    preset_names = [p.strip().lower() for p in args.presets.split(",") if p.strip()]
    unknown = [p for p in preset_names if p not in PRESETS]
    if unknown:
        print(
            f"Unknown preset(s): {', '.join(unknown)}. Choose from {', '.join(PRESETS)}.",
            file=sys.stderr,
        )
        return 2

    if not args.input.exists():
        print(f"Input not found: {args.input}", file=sys.stderr)
        return 1

    search_dirs = {
        "au": tuple(args.au_dir or ()) + DEFAULT_AU_DIRS,
        "vst3": tuple(args.vst3_dir or ()) + DEFAULT_VST3_DIRS,
    }

    out_dir = args.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    info = minihost.get_audio_info(args.input)
    print(f"minihost {minihost.__version__}  (C API {minihost.api_version_string()})")
    print(
        f"input  {args.input}: {info['channels']} ch, {info['sample_rate']} Hz, "
        f"{info['frames']} frames, {info['duration']:.2f} s"
    )
    print(
        f"render {args.sample_rate} Hz stereo, block {args.block_size}, "
        f"{args.bit_depth}-bit -> {out_dir}"
    )
    print(f"presets: {', '.join(preset_names)}   formats: {', '.join(formats)}")

    dry_path = out_dir / f"piano_dry_{args.sample_rate}.wav"
    render_dry_reference(args.input, dry_path, args.sample_rate, args.bit_depth)
    dry = measure(dry_path)
    print(
        f"dry reference: {dry_path.name}  "
        f"peak {dry['peak_db']:+.2f} dBFS, rms {dry['rms_db']:+.2f} dBFS"
    )

    # Each preset renders into its own subdirectory, so the file names
    # stay identical across variants and can be diffed or A/B'd directly.
    runs: dict[str, dict] = {}
    for name in preset_names:
        preset = PRESETS[name]
        tail = preset.tail_seconds if args.tail is None else args.tail
        preset_dir = out_dir / preset.name
        preset_dir.mkdir(parents=True, exist_ok=True)

        print(f"\n=== preset: {preset.name} ===")
        print(f"  {preset.summary}")
        print(f"  +{tail:.1f} s tail -> {preset_dir}")

        results = {}
        for fmt in formats:
            result = render_format(fmt, preset, tail, search_dirs[fmt], args, preset_dir)
            if result is not None:
                results[fmt] = result

        if not results:
            print("  nothing rendered for this preset")
            continue

        null = None
        if "au" in results and "vst3" in results:
            if results["au"]["stages"] == results["vst3"]["stages"]:
                null = null_test(results["au"], results["vst3"], preset_dir, args.bit_depth)
            else:
                print("\n  skipping null test: the two chains hold different stages")

        runs[name] = {"preset": preset, "results": results, "null": null, "dir": preset_dir}

    if not runs:
        print("\nNothing rendered: no plugins from CHAIN are installed.", file=sys.stderr)
        return 1

    print("\n=== summary ===")
    header = (
        f"{'preset':11s} {'fmt':5s} {'frames':>8s} {'peak dB':>8s} "
        f"{'rms dB':>8s} {'x rt':>6s} {'repeat dB':>10s}"
    )
    print(header)
    print("-" * len(header))
    for name, run in runs.items():
        for fmt, r in run["results"].items():
            rt = (r["frames"] / float(args.sample_rate)) / r["elapsed"]
            repeat = f"{r['repeat']['res_rms_db']:10.2f}" if r.get("repeat") else f"{'--':>10s}"
            print(
                f"{name:11s} {fmt.upper():5s} {r['frames']:8d} "
                f"{r['peak_db']:8.2f} {r['rms_db']:8.2f} {rt:6.1f} {repeat}"
            )

    nulls = {name: run["null"] for name, run in runs.items() if run["null"]}
    if nulls:
        # The point of running three presets: watch the same measurement
        # go from decisive to useless as the wet, nondeterministic stages
        # take over the output.
        print("\n=== null test across presets ===")
        header = (
            f"{'preset':11s} {'residual dB':>12s} {'below signal':>13s} "
            f"{'corr':>9s} {'usable':>7s}  verdict"
        )
        print(header)
        print("-" * len(header))
        for name, n in nulls.items():
            print(
                f"{name:11s} {n['res_rms_db']:12.2f} {n['below_db']:13.1f} "
                f"{n['corr']:9.6f} {('yes' if n['meaningful'] else 'no'):>7s}  {n['verdict']}"
            )

    written = sum(1 for _ in out_dir.rglob("*.wav"))
    print(f"\nWrote {written} file(s) under {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
