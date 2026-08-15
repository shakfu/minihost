#!/usr/bin/env python3
"""Render a MIDI file through an instrument plugin (AudioUnit or VST3).

    bach.mid -> synth -> .wav

Four renders, each showing a different part of the offline MIDI path:

  1. **straight** -- ``render_midi`` into memory, checked for non-finite
     samples, then written with ``write_audio``. Rendering in memory
     first is what makes the check possible: a plugin that emits NaN or
     Inf is a real failure mode, and once the buffer has been quantized
     to a 16- or 24-bit file the evidence is gone.
  2. **programs** -- the same MIDI through the instrument's first few
     factory presets, one file each, via ``render_midi_to_file``. Skipped
     for plugins that expose a single program.
  3. **chain** -- instrument into an effect, rendered as one
     ``PluginChain``, with a progress callback and optional peak
     normalization. Skipped when no effect plugin is installed.
  4. **transposed** -- a variant MIDI built with ``MidiFile``'s write
     API (note events shifted, tempo and controllers carried over),
     saved next to the audio and rendered.

Tail handling is the part worth stealing. ``tail_seconds="auto"``
renders past the last note-off until the output falls below
``tail_threshold``, capped by ``max_tail_seconds``. A synth with a long
release, or an effect with a reverb tail, would otherwise be cut off
mid-decay -- and the right tail length is a property of the patch, not
something the caller can know in advance. Each render reports the tail
it actually needed.

Outputs land in ``build/output/midi/`` by default::

    bach_dexed.wav                 straight render
    bach_dexed_prog00_<name>.wav   one per factory preset
    bach_dexed_chain.wav           instrument into an effect
    bach_dexed_transposed.wav      the +12 variant
    bach_transposed.mid            the variant MIDI itself

Usage::

    uv run python examples/midi_render_instrument.py
    uv run python examples/midi_render_instrument.py --instrument "/path/to/Synth.vst3"
    uv run python examples/midi_render_instrument.py --programs 8 --transpose -12
    uv run python examples/midi_render_instrument.py --tail 4 --sample-rate 96000

The instrument is picked from ``INSTRUMENTS`` -- the first candidate
installed, AudioUnit or VST3 -- unless ``--instrument`` names one
explicitly. Candidates are checked by playing them a single test note
and listening for output, because metadata cannot answer the question:
Dexed reports zero audio inputs while Surge XT reports two, exactly like
an effect would, and FabFilter Pro-R 2 answers True to ``accepts_midi``
despite being a reverb. Anything that stays silent for the test note is
rejected and the search moves on.

Note that renders are as wide as the instrument's output, and nothing
here downmixes. An instrument with aux buses -- Surge XT emits its main
output plus a stereo pair per scene -- produces a six-channel file, and
the bus layout is printed so the extra channels are identifiable.

Requires numpy.
"""

from __future__ import annotations

import argparse
import contextlib
import re
import sys
import time
from pathlib import Path

import numpy as np

import minihost

REPO_ROOT = Path(__file__).resolve().parent.parent

SEARCH_DIRS = {
    "au": (
        Path("/Library/Audio/Plug-Ins/Components"),
        Path.home() / "Library/Audio/Plug-Ins/Components",
    ),
    "vst3": (
        Path("/Library/Audio/Plug-Ins/VST3"),
        Path.home() / "Library/Audio/Plug-Ins/VST3",
    ),
}
SUFFIX = {"au": ".component", "vst3": ".vst3"}

# Tried in order; the first one installed wins. Mixed formats on purpose
# -- the rendering code below never asks which format it got.
INSTRUMENTS: tuple[tuple[str, str], ...] = (
    ("Dexed", "vst3"),
    ("Surge XT", "au"),
    ("TyrellN6", "au"),
    ("TAL-NoiseMaker", "au"),
    ("Hive", "au"),
    ("Pendulate", "au"),
    ("syndt", "au"),
)

# Optional effect for the chain render, same search order.
EFFECTS: tuple[tuple[str, str], ...] = (
    ("FabFilter Pro-R 2", "au"),
    ("FabFilter Pro-R 2", "vst3"),
    ("ValhallaSupermassive", "au"),
    ("TAL-Reverb-4", "au"),
)


def slugify(name: str) -> str:
    """A filename-safe stem for a plugin or program name."""
    slug = re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")
    return slug or "unnamed"


# ---------------------------------------------------------------------------
# MIDI inspection
# ---------------------------------------------------------------------------


def describe_midi(midi: minihost.MidiFile, path: Path) -> dict:
    """Print what is actually in the file, and return a small summary."""
    events: list[dict] = []
    for track in range(midi.num_tracks):
        events.extend(midi.get_events(track))

    kinds: dict[str, int] = {}
    for event in events:
        kinds[event["type"]] = kinds.get(event["type"], 0) + 1

    notes = [e for e in events if e["type"] == "note_on" and e.get("velocity", 0) > 0]
    pitches = [e["pitch"] for e in notes]
    velocities = [e["velocity"] for e in notes]
    channels = sorted({e["channel"] for e in events if "channel" in e})

    print(f"midi   {path}")
    print(
        f"       {midi.num_tracks} track(s), {midi.ticks_per_quarter} ticks/quarter, "
        f"{midi.duration_seconds:.2f} s"
    )
    print("       events: " + ", ".join(f"{k} {v}" for k, v in sorted(kinds.items())))
    if notes:
        print(
            f"       {len(notes)} notes, pitch {min(pitches)}-{max(pitches)}, "
            f"velocity {min(velocities)}-{max(velocities)}, "
            f"channel(s) {', '.join(str(c) for c in channels)}"
        )
    return {"events": events, "notes": len(notes), "duration": midi.duration_seconds}


def transpose(midi: minihost.MidiFile, semitones: int) -> minihost.MidiFile:
    """Copy a MIDI file with every note shifted by ``semitones``.

    Notes pushed outside 0..127 are dropped rather than wrapped, and the
    matching note-off goes with them. Tempo, controllers, program
    changes and pitch bend are carried over unchanged; other meta events
    (track names, time signatures) are not reproduced, since the write
    API has no entry point for them.
    """
    out = minihost.MidiFile()
    out.ticks_per_quarter = midi.ticks_per_quarter

    for track in range(midi.num_tracks):
        index = track if track < out.num_tracks else out.add_track()
        for event in midi.get_events(track):
            kind = event["type"]
            tick = event["tick"]
            if kind in ("note_on", "note_off"):
                pitch = event["pitch"] + semitones
                if not 0 <= pitch <= 127:
                    continue
                if kind == "note_on":
                    out.add_note_on(index, tick, event["channel"], pitch, event["velocity"])
                else:
                    out.add_note_off(index, tick, event["channel"], pitch, event["velocity"])
            elif kind == "control_change":
                out.add_control_change(
                    index, tick, event["channel"], event["controller"], event["value"]
                )
            elif kind == "program_change":
                out.add_program_change(index, tick, event["channel"], event["program"])
            elif kind == "pitch_bend":
                out.add_pitch_bend(index, tick, event["channel"], event["value"])
            elif kind == "meta" and "bpm" in event:
                out.add_tempo(index, tick, event["bpm"])
    return out


# ---------------------------------------------------------------------------
# plugin discovery
# ---------------------------------------------------------------------------


def resolve(name: str, fmt: str) -> Path | None:
    for directory in SEARCH_DIRS[fmt]:
        candidate = directory / f"{name}{SUFFIX[fmt]}"
        if candidate.exists():
            return candidate
    return None


def format_of(path: Path) -> str:
    return "au" if path.suffix == ".component" else "vst3"


SILENCE_DB = -80.0


def probe_midi() -> minihost.MidiFile:
    """A one-note snippet: middle C, two beats at 120 bpm."""
    probe = minihost.MidiFile()
    probe.ticks_per_quarter = 96
    probe.add_tempo(0, 0, 120.0)
    probe.add_note_on(0, 0, 0, 60, 100)
    probe.add_note_off(0, 192, 0, 60, 0)
    return probe


def probe_level(plugin: minihost.Plugin, block_size: int) -> float:
    """Peak dBFS produced by one test note. Silence means not an instrument."""
    plugin.reset()
    audio = minihost.render_midi(
        plugin, probe_midi(), block_size=block_size, tail_seconds=0.5, as_=np.ndarray
    )
    data = np.asarray(audio, dtype=np.float64)
    data = np.where(np.isfinite(data), data, 0.0)
    plugin.reset()
    return db(float(np.max(np.abs(data))))


def reject_reason(plugin: minihost.Plugin, block_size: int) -> str | None:
    """Why this plugin cannot serve as the instrument, or None if it can.

    The metadata gets a first pass, but it cannot decide the question on
    its own. An instrument is not identifiable by its buses: Dexed and
    TyrellN6 report zero audio inputs, while Surge XT and TAL-NoiseMaker
    report two, exactly like an effect. Nor by MIDI support -- plenty of
    effects declare a MIDI input for parameter control, so FabFilter
    Pro-R 2 answers True to ``accepts_midi`` and would sail past any
    metadata test, then render 50 seconds of digital silence.

    So the deciding test is behavioural: play one note and see whether
    anything comes out. That also rejects instruments which load but
    cannot sound -- an unlicensed demo build, say.
    """
    if not plugin.accepts_midi:
        return "takes no MIDI input"
    if plugin.is_midi_effect:
        return "is a MIDI effect, not an instrument"
    if plugin.num_output_channels < 1:
        return "has no audio output"

    level = probe_level(plugin, block_size)
    if level <= SILENCE_DB:
        return (
            f"renders silence for a test note ({level:.0f} dBFS) -- "
            "an effect, or an instrument that cannot sound"
        )
    return None


def open_instrument(
    explicit: Path | None,
    sample_rate: float,
    block_size: int,
    stack: contextlib.ExitStack,
) -> tuple[minihost.Plugin, Path] | None:
    """Open the first candidate that is really an instrument."""
    if explicit is not None:
        if not explicit.exists():
            print(f"Instrument not found: {explicit}", file=sys.stderr)
            return None
        candidates = [explicit]
    else:
        candidates = [p for p in (resolve(n, f) for n, f in INSTRUMENTS) if p is not None]
        if not candidates:
            print("None of the candidate instruments are installed:", file=sys.stderr)
            for name, fmt in INSTRUMENTS:
                print(f"  {name} ({fmt})", file=sys.stderr)
            return None

    for path in candidates:
        started = time.perf_counter()
        plugin = minihost.Plugin(str(path), sample_rate=sample_rate, max_block_size=block_size)
        load_ms = (time.perf_counter() - started) * 1e3

        reason = reject_reason(plugin, block_size)
        if reason is not None:
            print(f"  [skip] {path.name}: {reason}")
            plugin.close()
            if explicit is not None:
                return None
            continue

        stack.callback(plugin.close)
        print(f"instrument {path}")
        print(
            f"       {format_of(path).upper()}, "
            f"{plugin.num_output_channels} out, {plugin.num_params} params, "
            f"{plugin.num_programs} program(s), latency {plugin.latency_samples}, "
            f"loaded in {load_ms:.0f} ms"
        )
        if plugin.num_output_buses > 1:
            # Worth spelling out: an instrument with aux buses renders a
            # file wider than stereo, and nothing here downmixes it.
            # Surge XT, for one, emits its main output plus a pair per
            # scene, so its renders are six channels.
            print(f"       {plugin.num_output_buses} output buses, rendered side by side:")
            channel = 0
            for index in range(plugin.num_output_buses):
                bus = plugin.get_bus_info(False, index)
                span = f"ch {channel}-{channel + bus['num_channels'] - 1}"
                main = " (main)" if bus["is_main"] else ""
                print(f"         {span:9s} {bus['name']}{main}")
                channel += bus["num_channels"]
        return plugin, path
    return None


def open_effect(
    sample_rate: float, block_size: int, stack: contextlib.ExitStack
) -> tuple[minihost.Plugin, Path] | None:
    for name, fmt in EFFECTS:
        path = resolve(name, fmt)
        if path is None:
            continue
        plugin = minihost.Plugin(str(path), sample_rate=sample_rate, max_block_size=block_size)
        stack.callback(plugin.close)
        return plugin, path
    return None


# ---------------------------------------------------------------------------
# metrics
# ---------------------------------------------------------------------------


def db(value: float) -> float:
    return 20.0 * float(np.log10(max(value, 1e-12)))


def levels(audio: np.ndarray) -> dict:
    """Peak/RMS plus a non-finite count, which is the interesting one.

    A synth that emits NaN or Inf -- from an uninitialized filter state,
    a denormal blowup, or a patch the headless build never exercises --
    still "renders" and still writes a file. The count is checked before
    anything is written, because integer output formats cannot represent
    a NaN and will bury it.
    """
    finite = np.isfinite(audio)
    bad = int(finite.size - np.count_nonzero(finite))
    clean = np.where(finite, audio, 0.0)
    return {
        "peak_db": db(float(np.max(np.abs(clean)))),
        "rms_db": db(float(np.sqrt(np.mean(np.square(clean, dtype=np.float64))))),
        "frames": int(audio.shape[-1]),
        "channels": int(audio.shape[0]),
        "nonfinite": bad,
    }


def measure_file(path: Path) -> dict:
    audio, _ = minihost.read_audio(path, as_=np.ndarray)
    return levels(np.asarray(audio, dtype=np.float64))


def report(
    label: str,
    path: Path,
    stats: dict,
    elapsed: float,
    sample_rate: int,
    midi_duration: float,
) -> dict:
    """Print one render's outcome and return its summary row."""
    duration = stats["frames"] / float(sample_rate)
    tail = duration - midi_duration
    print(
        f"  {path.name}: {stats['frames']} frames ({duration:.2f} s, "
        f"tail {tail:+.2f} s) in {elapsed:.2f} s = {duration / elapsed:.1f}x realtime"
    )
    print(
        f"    {stats['channels']} ch, peak {stats['peak_db']:+.2f} dBFS, "
        f"rms {stats['rms_db']:+.2f} dBFS"
    )
    if stats["peak_db"] <= SILENCE_DB:
        print("    WARNING: this render is silent")
    if stats["nonfinite"]:
        print(
            f"    WARNING: {stats['nonfinite']} non-finite sample(s) -- "
            "the plugin emitted NaN or Inf; they were zeroed before writing"
        )
    return {"label": label, "path": path, "elapsed": elapsed, "tail": tail, **stats}


# ---------------------------------------------------------------------------
# renders
# ---------------------------------------------------------------------------


def render_straight(
    plugin: minihost.Plugin,
    midi: minihost.MidiFile,
    out_path: Path,
    args: argparse.Namespace,
    midi_duration: float,
) -> dict:
    """In-memory render, inspected, then written."""
    print("\n=== straight render ===")
    plugin.reset()
    started = time.perf_counter()
    audio = minihost.render_midi(
        plugin,
        midi,
        block_size=args.block_size,
        tail_seconds=args.tail,
        tail_threshold=args.tail_threshold,
        max_tail_seconds=args.max_tail,
        as_=np.ndarray,
    )
    elapsed = time.perf_counter() - started

    audio = np.asarray(audio, dtype=np.float32)
    stats = levels(audio.astype(np.float64))
    # Zero any NaN/Inf rather than handing them to the encoder.
    if stats["nonfinite"]:
        audio = np.where(np.isfinite(audio), audio, np.float32(0.0))
    minihost.write_audio(out_path, audio, args.sample_rate, bit_depth=args.bit_depth)
    return report("straight", out_path, stats, elapsed, args.sample_rate, midi_duration)


def render_programs(
    plugin: minihost.Plugin,
    midi: minihost.MidiFile,
    out_dir: Path,
    stem: str,
    args: argparse.Namespace,
    midi_duration: float,
) -> list[dict]:
    """Render the same MIDI through the first few factory presets."""
    count = min(args.programs, plugin.num_programs)
    if count < 2:
        print("\n=== programs === (skipped: the instrument exposes fewer than two)")
        return []

    print(f"\n=== programs === (first {count} of {plugin.num_programs})")
    rows = []
    original = plugin.program
    try:
        for index in range(count):
            plugin.program = index
            plugin.reset()
            name = plugin.get_program_name(index).strip()
            path = out_dir / f"{stem}_prog{index:02d}_{slugify(name)}.wav"
            started = time.perf_counter()
            minihost.render_midi_to_file(
                plugin,
                midi,
                str(path),
                block_size=args.block_size,
                tail_seconds=args.tail,
                bit_depth=args.bit_depth,
                tail_threshold=args.tail_threshold,
                max_tail_seconds=args.max_tail,
            )
            elapsed = time.perf_counter() - started
            print(f"  [{index:2d}] {name!r}")
            rows.append(
                report(
                    f"program {index}",
                    path,
                    measure_file(path),
                    elapsed,
                    args.sample_rate,
                    midi_duration,
                )
            )
    finally:
        plugin.program = original
        plugin.reset()
    return rows


def render_chain(
    plugin: minihost.Plugin,
    effect: tuple[minihost.Plugin, Path] | None,
    midi: minihost.MidiFile,
    out_path: Path,
    args: argparse.Namespace,
    midi_duration: float,
) -> dict | None:
    """Instrument into an effect, rendered as a single chain."""
    if effect is None:
        reason = "--no-chain" if not args.chain else "no effect plugin installed"
        print(f"\n=== chain === (skipped: {reason})")
        return None

    fx, fx_path = effect
    print(f"\n=== chain === (instrument -> {fx_path.name})")

    ticks = 0

    def progress(done: int, total: int) -> None:
        # Called once per block, so thousands of times per render. Rewrite
        # one line every 1000 blocks rather than scrolling. `total` is the
        # estimate made before rendering starts, which an auto-detected
        # tail can overshoot -- hence the clamp.
        nonlocal ticks
        ticks += 1
        if ticks % 1000 == 0 or done >= total:
            pct = min(100.0, 100.0 * done / max(total, 1))
            print(f"\r    rendering {pct:5.1f}%", end="", flush=True)

    plugin.reset()
    fx.reset()
    with minihost.PluginChain([plugin, fx]) as chain:
        chain.set_non_realtime(True)
        started = time.perf_counter()
        minihost.render_midi_to_file(
            chain,
            midi,
            str(out_path),
            block_size=args.block_size,
            tail_seconds=args.tail,
            bit_depth=args.bit_depth,
            tail_threshold=args.tail_threshold,
            max_tail_seconds=args.max_tail,
            normalize=args.normalize,
            progress_callback=progress,
        )
        elapsed = time.perf_counter() - started
    print()
    return report("chain", out_path, measure_file(out_path), elapsed, args.sample_rate, midi_duration)


def render_transposed(
    plugin: minihost.Plugin,
    midi: minihost.MidiFile,
    out_dir: Path,
    stem: str,
    args: argparse.Namespace,
) -> dict | None:
    """Build a transposed variant with the MidiFile write API, then render it."""
    if not args.transpose:
        print("\n=== transposed === (skipped: --transpose 0)")
        return None

    print(f"\n=== transposed === ({args.transpose:+d} semitones)")
    variant = transpose(midi, args.transpose)
    midi_path = out_dir / f"{Path(args.midi).stem}_transposed.mid"
    if not variant.save(str(midi_path)):
        print(f"  could not write {midi_path}", file=sys.stderr)
        return None
    print(f"  wrote {midi_path.name}: {variant.duration_seconds:.2f} s")

    plugin.reset()
    path = out_dir / f"{stem}_transposed.wav"
    started = time.perf_counter()
    minihost.render_midi_to_file(
        plugin,
        variant,
        str(path),
        block_size=args.block_size,
        tail_seconds=args.tail,
        bit_depth=args.bit_depth,
        tail_threshold=args.tail_threshold,
        max_tail_seconds=args.max_tail,
    )
    elapsed = time.perf_counter() - started
    return report(
        "transposed", path, measure_file(path), elapsed, args.sample_rate, variant.duration_seconds
    )


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------


def parse_tail(value: str) -> float | str:
    if value.lower() == "auto":
        return "auto"
    return float(value)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Render a MIDI file through an instrument plugin (AU or VST3).",
    )
    parser.add_argument(
        "--midi",
        type=Path,
        default=REPO_ROOT / "tests/_midi/bach.mid",
        help="input MIDI file (default: tests/_midi/bach.mid)",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=REPO_ROOT / "build/output/midi",
        help="directory for rendered files (default: build/output/midi)",
    )
    parser.add_argument(
        "--instrument",
        type=Path,
        help="path to a specific instrument bundle instead of searching",
    )
    parser.add_argument("--sample-rate", type=int, default=48000, help="render rate in Hz")
    parser.add_argument("--block-size", type=int, default=512, help="processing block size")
    parser.add_argument("--bit-depth", type=int, default=24, choices=(16, 24, 32))
    parser.add_argument(
        "--tail",
        type=parse_tail,
        default="auto",
        help='seconds rendered past the last note, or "auto" to detect (default: auto)',
    )
    parser.add_argument(
        "--tail-threshold",
        type=float,
        default=1e-4,
        help="peak amplitude below which auto-tail stops (default: 1e-4, about -80 dBFS)",
    )
    parser.add_argument(
        "--max-tail",
        type=float,
        default=30.0,
        help="safety cap on auto-tail length in seconds (default: 30)",
    )
    parser.add_argument(
        "--programs",
        type=int,
        default=4,
        help="how many factory presets to render (default: 4, 0 disables)",
    )
    parser.add_argument(
        "--transpose",
        type=int,
        default=12,
        help="semitones for the transposed variant (default: +12, 0 disables)",
    )
    parser.add_argument(
        "--normalize",
        type=float,
        default=None,
        help="peak-normalize the chain render to this dBFS target",
    )
    parser.add_argument(
        "--no-chain",
        dest="chain",
        action="store_false",
        help="skip the instrument-into-effect render",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if not args.midi.exists():
        print(f"MIDI file not found: {args.midi}", file=sys.stderr)
        return 1

    midi = minihost.MidiFile()
    if not midi.load(str(args.midi)):
        print(f"Could not parse MIDI file: {args.midi}", file=sys.stderr)
        return 1

    out_dir = args.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"minihost {minihost.__version__}  (C API {minihost.api_version_string()})")
    summary = describe_midi(midi, args.midi)
    midi_duration = summary["duration"]
    tail_text = args.tail if isinstance(args.tail, str) else f"{args.tail:.1f} s"
    print(
        f"render {args.sample_rate} Hz, block {args.block_size}, {args.bit_depth}-bit, "
        f"tail {tail_text} -> {out_dir}"
    )

    rows: list[dict] = []
    with contextlib.ExitStack() as stack:
        found = open_instrument(
            args.instrument, float(args.sample_rate), args.block_size, stack
        )
        if found is None:
            return 1
        plugin, path = found
        stem = f"{args.midi.stem}_{slugify(path.stem)}"

        effect = None
        if args.chain:
            effect = open_effect(float(args.sample_rate), args.block_size, stack)

        rows.append(
            render_straight(plugin, midi, out_dir / f"{stem}.wav", args, midi_duration)
        )
        if args.programs:
            rows.extend(
                render_programs(plugin, midi, out_dir, stem, args, midi_duration)
            )
        chain_row = render_chain(
            plugin, effect, midi, out_dir / f"{stem}_chain.wav", args, midi_duration
        )
        if chain_row is not None:
            rows.append(chain_row)
        variant_row = render_transposed(plugin, midi, out_dir, stem, args)
        if variant_row is not None:
            rows.append(variant_row)

    print("\n=== summary ===")
    header = (
        f"{'render':14s} {'frames':>9s} {'seconds':>8s} {'tail':>7s} "
        f"{'peak dB':>8s} {'rms dB':>8s} {'x rt':>6s}"
    )
    print(header)
    print("-" * len(header))
    for row in rows:
        seconds = row["frames"] / float(args.sample_rate)
        print(
            f"{row['label']:14s} {row['frames']:9d} {seconds:8.2f} {row['tail']:7.2f} "
            f"{row['peak_db']:8.2f} {row['rms_db']:8.2f} {seconds / row['elapsed']:6.1f}"
        )

    bad = sum(row["nonfinite"] for row in rows)
    if bad:
        print(f"\n{bad} non-finite sample(s) across all renders -- see warnings above")

    print(f"\nWrote {len(list(out_dir.glob('*.wav')))} audio file(s) to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
