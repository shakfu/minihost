#!/usr/bin/env python3
"""Split one MIDI part across two instrument legs, wired two ways.

    bach.mid ---+--> instrument A ------------+--> mix -> .wav
                |                             |
                +--> MIDI effect -> instr. B -+

The same topology is built twice, once with ``PluginBus`` and once with
``PluginGraph``, and the two renders are nulled against each other. They
agree to the last bit: the bus is the convenience API for "branches in
parallel, summed", the graph is the general one, and a topology both can
express comes out the same either way.

Getting that null clean needs one precaution, which is the most useful
thing here. **The first substantial render in a process leaves plugin
state behind that changes every render after it.** Comparing two routes
means rendering twice, so without care the first route measured is the
odd one out and the null accuses whichever route happened to run first
-- by a margin large enough (a whole chord voicing, 1.3 dB under the
signal) to look like a routing bug. It is not one: the MIDI delivered to
the instruments is byte-identical either way, and two routes rendered
back to back agree exactly. The fix is a throwaway render before
anything is measured. A short warm-up is not enough; it has to be a full
render, which costs about half a second here.

Why two routes exist, and when to pick which:

  * ``PluginBus`` takes whole chains as branches and fans the same MIDI
    to all of them. Building this needs nothing but ``add_branch``. It
    cannot express a MIDI path that rejoins, or one where two branches
    want different MIDI.
  * ``PluginGraph`` wires MIDI as explicit edges, so a source can fan
    out, a MIDI effect can sit on one leg only, and audio rejoins
    through a mix node. More calls, no topology limits.

Both legs also render on their own, so the split can be auditioned.

Neither route can be driven by ``render_midi_to_file``: the high-level
renderers take a plugin or a chain, not a bus or a graph. The block loop
here is what fills that gap -- convert the file with
``midi_file_to_events``, bucket the events by block, and hand each
block's slice to the router. Reuse ``events_by_block`` for any
graph-driven render of a MIDI file.

Outputs land in ``build/output/routing/`` by default::

    bach_bus.wav              the whole split, through PluginBus
    bach_graph.wav            the same, through PluginGraph
    bach_leg_direct.wav       instrument A alone
    bach_leg_effect.wav       MIDI effect -> instrument B alone
    bach_bus_graph_null.wav   difference of the two routes (32-bit float)

Usage::

    uv run python examples/midi_split_routing.py
    uv run python examples/midi_split_routing.py --midi-fx "/path/to/Arp.component"
    uv run python examples/midi_split_routing.py --instrument "/path/to/Synth.vst3"

Without a MIDI effect the demo still runs: the second leg becomes a
plain second instrument, which is the layering case. Point ``--midi-fx``
at an arpeggiator or chorder (anything reporting MIDI output) to get the
full split.

Requires numpy.
"""

from __future__ import annotations

import argparse
import contextlib
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

INSTRUMENTS: tuple[tuple[str, str], ...] = (
    ("Dexed", "vst3"),
    ("Surge XT", "au"),
    ("TyrellN6", "au"),
    ("TAL-NoiseMaker", "au"),
)

# Anything that transforms MIDI and reports producing it.
MIDI_EFFECTS: tuple[tuple[str, str], ...] = (
    ("Chord Prism 2", "au"),
    ("Strokes", "au"),
    ("Scaper", "au"),
)


def resolve(name: str, fmt: str) -> Path | None:
    for directory in SEARCH_DIRS[fmt]:
        candidate = directory / f"{name}{SUFFIX[fmt]}"
        if candidate.exists():
            return candidate
    return None


# ---------------------------------------------------------------------------
# MIDI scheduling
# ---------------------------------------------------------------------------


def events_by_block(
    events: list[tuple[int, int, int, int]], block_size: int
) -> dict[int, list[tuple[int, int, int, int]]]:
    """Bucket absolute-sample events into per-block lists.

    Routers are driven a block at a time and expect offsets measured
    from the start of the block, so a file's absolute offsets have to be
    split and rebased. Returns {block_index: [(offset_in_block, ...)]}.
    """
    buckets: dict[int, list[tuple[int, int, int, int]]] = {}
    for offset, status, data1, data2 in events:
        index, within = divmod(offset, block_size)
        buckets.setdefault(index, []).append((within, status, data1, data2))
    return buckets


# ---------------------------------------------------------------------------
# the two routes
# ---------------------------------------------------------------------------


def render_bus(
    legs: list[list[minihost.Plugin]],
    schedule: dict[int, list[tuple[int, int, int, int]]],
    blocks: int,
    block_size: int,
    sample_rate: float,
    out_channels: int,
) -> np.ndarray:
    """Route with PluginBus: one chain per leg, MIDI fanned to all."""
    with contextlib.ExitStack() as stack:
        chains = []
        for plugins in legs:
            for plugin in plugins:
                plugin.reset()
            chain = minihost.PluginChain(plugins)
            stack.callback(chain.close)
            chains.append(chain)

        # A bus of instruments carries no audio in: plugins driven by
        # MIDI alone have no audio input bus to feed.
        bus = minihost.PluginBus(
            0, out_channels, max_block_size=block_size, sample_rate=sample_rate
        )
        stack.callback(bus.close)
        for chain in chains:
            bus.add_branch(chain)

        silence = np.zeros((0, block_size), dtype=np.float32)
        out = np.zeros((out_channels, block_size), dtype=np.float32)
        captured = []
        for index in range(blocks):
            bus.process_midi(silence, out, schedule.get(index, []))
            captured.append(out.copy())
    return np.concatenate(captured, axis=1)


def render_graph(
    legs: list[list[minihost.Plugin]],
    schedule: dict[int, list[tuple[int, int, int, int]]],
    blocks: int,
    block_size: int,
    sample_rate: float,
    out_channels: int,
) -> np.ndarray:
    """Route with PluginGraph: explicit MIDI edges into a mix node."""
    with contextlib.ExitStack() as stack:
        graph = minihost.PluginGraph(block_size, sample_rate)
        stack.callback(graph.close)
        source = graph.add_midi_input()
        mix = graph.add_mix(len(legs), out_channels)
        out_node = graph.add_output(out_channels)

        for slot, plugins in enumerate(legs):
            if not plugins:
                continue
            # MIDI enters the head of each leg from the shared source --
            # one source, many destinations -- then runs plugin to plugin
            # down the leg. Only the leg's last node feeds the mix.
            previous = graph.add_plugin(plugins[0])
            plugins[0].reset()
            graph.connect_midi(source, previous)
            for plugin in plugins[1:]:
                plugin.reset()
                node = graph.add_plugin(plugin)
                graph.connect_midi(previous, node)
                previous = node
            graph.connect(previous, mix, slot)

        graph.connect(mix, out_node)
        graph.compile()

        out = np.zeros((out_channels, block_size), dtype=np.float32)
        captured = []
        for index in range(blocks):
            graph.set_midi_input_events(source, schedule.get(index, []))
            graph.render_block([], [out], block_size)
            captured.append(out.copy())
    return np.concatenate(captured, axis=1)


# ---------------------------------------------------------------------------
# measurement
# ---------------------------------------------------------------------------


def db(value: float) -> float:
    return 20.0 * float(np.log10(max(value, 1e-12)))


def levels(audio: np.ndarray) -> tuple[float, float]:
    data = np.asarray(audio, dtype=np.float64)
    return db(float(np.max(np.abs(data)))), db(float(np.sqrt(np.mean(data**2))))


def null(a: np.ndarray, b: np.ndarray) -> dict:
    """Difference between the two routes, read against the signal."""
    x = np.asarray(a, dtype=np.float64)
    y = np.asarray(b, dtype=np.float64)
    frames = min(x.shape[-1], y.shape[-1])
    residual = x[:, :frames] - y[:, :frames]
    ref_rms = db(float(np.sqrt(np.mean(x[:, :frames] ** 2))))
    res_rms = db(float(np.sqrt(np.mean(residual**2))))
    return {
        "residual": residual,
        "res_rms": res_rms,
        "ref_rms": ref_rms,
        "below": ref_rms - res_rms,
        "corr": float(np.corrcoef(x[:, :frames].ravel(), y[:, :frames].ravel())[0, 1]),
    }


# ---------------------------------------------------------------------------
# entry point
# ---------------------------------------------------------------------------


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Split one MIDI part across two legs, routed by bus and by graph.",
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
        default=REPO_ROOT / "build/output/routing",
        help="directory for rendered files (default: build/output/routing)",
    )
    parser.add_argument("--instrument", type=Path, help="instrument plugin to use")
    parser.add_argument(
        "--midi-fx",
        type=Path,
        help="MIDI effect for the second leg (omit to layer two plain instruments)",
    )
    parser.add_argument("--sample-rate", type=int, default=48000)
    parser.add_argument("--block-size", type=int, default=512)
    parser.add_argument("--bit-depth", type=int, default=24, choices=(16, 24, 32))
    parser.add_argument(
        "--tail",
        type=float,
        default=3.0,
        help="seconds rendered past the last event (default: 3)",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if not args.midi.exists():
        print(f"MIDI file not found: {args.midi}", file=sys.stderr)
        return 1

    instrument_path = args.instrument
    if instrument_path is None:
        found = [p for p in (resolve(n, f) for n, f in INSTRUMENTS) if p is not None]
        if not found:
            print("No candidate instrument installed; pass --instrument", file=sys.stderr)
            return 1
        instrument_path = found[0]
    if not instrument_path.exists():
        print(f"Instrument not found: {instrument_path}", file=sys.stderr)
        return 1

    fx_path = args.midi_fx
    if fx_path is None:
        for name, fmt in MIDI_EFFECTS:
            candidate = resolve(name, fmt)
            if candidate is not None:
                fx_path = candidate
                break

    out_dir = args.output_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    midi = minihost.MidiFile()
    if not midi.load(str(args.midi)):
        print(f"Could not parse MIDI file: {args.midi}", file=sys.stderr)
        return 1
    events = minihost.midi_file_to_events(str(args.midi), float(args.sample_rate))
    schedule = events_by_block(events, args.block_size)
    total_frames = int((midi.duration_seconds + args.tail) * args.sample_rate)
    blocks = total_frames // args.block_size

    print(f"minihost {minihost.__version__}  (C API {minihost.api_version_string()})")
    print(f"midi       {args.midi}: {midi.duration_seconds:.2f} s, {len(events)} events")
    print(f"instrument {instrument_path}")

    def open_plugin(path: Path) -> minihost.Plugin:
        return minihost.Plugin(
            str(path),
            sample_rate=float(args.sample_rate),
            max_block_size=args.block_size,
        )

    with contextlib.ExitStack() as stack:
        probe = open_plugin(instrument_path)
        stack.callback(probe.close)
        if not probe.accepts_midi:
            print(f"{instrument_path.name} accepts no MIDI", file=sys.stderr)
            return 1
        out_channels = probe.num_output_channels

        # Leg two gets a MIDI effect in front when one is available. Every
        # plugin instance is distinct: the same object cannot sit in two
        # places at once, and each leg needs its own voice state.
        fx_leg_label = "second instrument (no MIDI effect found)"
        if fx_path is not None:
            fx_probe = open_plugin(fx_path)
            stack.callback(fx_probe.close)
            if fx_probe.produces_midi:
                fx_leg_label = f"{fx_path.name} -> instrument"
                print(f"midi fx    {fx_path}")
            else:
                print(f"midi fx    {fx_path.name} reports no MIDI output, ignoring")
                fx_path = None

        def build_legs() -> list[list[minihost.Plugin]]:
            direct = [stack.enter_context(open_plugin(instrument_path))]
            if fx_path is None:
                second = [stack.enter_context(open_plugin(instrument_path))]
            else:
                second = [
                    stack.enter_context(open_plugin(fx_path)),
                    stack.enter_context(open_plugin(instrument_path)),
                ]
            return [direct, second]

        print(
            f"render     {args.sample_rate} Hz, block {args.block_size}, "
            f"{blocks} blocks (+{args.tail:.1f} s tail) -> {out_dir}"
        )
        print(f"           leg 1: instrument   leg 2: {fx_leg_label}")

        # One throwaway render before anything is measured, on instances
        # of its own. The first substantial render in a process leaves
        # state behind that changes every render after it -- with these
        # plugins the difference is a whole chord voicing, and it is the
        # plugins' doing, not the routing: the MIDI delivered to the
        # instruments is byte-identical either way, and two routes
        # rendered back to back agree to the last bit. Without this, the
        # first route measured is the odd one out and the comparison
        # below accuses whichever route happened to run first. A short
        # warm-up does not do it; the throwaway has to be a full render.
        # It costs about half a second at these speeds.
        print("           (rendering one throwaway pass first -- see the comment in main)")
        render_bus(
            build_legs(), schedule, blocks, args.block_size,
            float(args.sample_rate), out_channels,
        )

        renders: dict[str, tuple[np.ndarray, float]] = {}
        for label, builder, route in (
            ("bus", build_legs, render_bus),
            ("graph", build_legs, render_graph),
        ):
            started = time.perf_counter()
            audio = route(
                builder(),
                schedule,
                blocks,
                args.block_size,
                float(args.sample_rate),
                out_channels,
            )
            renders[label] = (audio, time.perf_counter() - started)

        # Each leg on its own, for auditioning and to prove the split is
        # really carrying two different parts.
        legs = build_legs()
        solo = {
            "leg_direct": render_graph(
                [legs[0]], schedule, blocks, args.block_size,
                float(args.sample_rate), out_channels
            ),
            "leg_effect": render_graph(
                [legs[1]], schedule, blocks, args.block_size,
                float(args.sample_rate), out_channels
            ),
        }

    print("\n=== renders ===")
    rows = []
    for label, (audio, elapsed) in renders.items():
        peak, rms = levels(audio)
        path = out_dir / f"{args.midi.stem}_{label}.wav"
        minihost.write_audio(path, audio.astype(np.float32), args.sample_rate,
                             bit_depth=args.bit_depth)
        seconds = audio.shape[-1] / float(args.sample_rate)
        rows.append((label, peak, rms, seconds / elapsed))
        print(
            f"  {path.name:28s} peak {peak:+7.2f} dBFS  rms {rms:+7.2f} dBFS  "
            f"{seconds / elapsed:5.1f}x realtime"
        )
    for label, audio in solo.items():
        peak, rms = levels(audio)
        path = out_dir / f"{args.midi.stem}_{label}.wav"
        minihost.write_audio(path, audio.astype(np.float32), args.sample_rate,
                             bit_depth=args.bit_depth)
        print(f"  {path.name:28s} peak {peak:+7.2f} dBFS  rms {rms:+7.2f} dBFS")

    # Self-check before the comparison. Summing is what both routers do,
    # so each route's two-leg render must equal its own legs added
    # together. Whichever route fails this is the one at fault, which is
    # more useful than knowing only that the two disagree.
    print("\n=== self-check: each route against the sum of its legs ===")
    leg_sum = solo["leg_direct"] + solo["leg_effect"]
    faults = []
    for label in ("bus", "graph"):
        check = null(renders[label][0], leg_sum)
        verdict = "matches" if check["res_rms"] < -100.0 else "DOES NOT MATCH"
        if check["res_rms"] >= -100.0:
            faults.append(label)
        print(f"  {label:6s} vs leg1+leg2: residual {check['res_rms']:+8.2f} dBFS  {verdict}")

    print("\n=== bus vs graph ===")
    result = null(renders["bus"][0], renders["graph"][0])
    null_path = out_dir / f"{args.midi.stem}_bus_graph_null.wav"
    minihost.write_audio(
        null_path, result["residual"].astype(np.float32), args.sample_rate, bit_depth=32
    )
    print(f"  residual rms {result['res_rms']:+.2f} dBFS, {result['below']:.1f} dB below signal")
    print(f"  correlation {result['corr']:.6f}")
    print(f"  difference written to {null_path.name} (32-bit float)")
    if result["res_rms"] < -100.0:
        print("  the two routes render the same audio, sample for sample")
    else:
        print(
            "  the routes disagree. Both are deterministic on their own -- rendering "
            "either one twice gives identical output -- so a difference here is not "
            "noise, and the self-check above names the route that broke the sum."
        )
        if faults:
            print(
                f"  route(s) failing the self-check: {', '.join(faults)}. The usual "
                "cause is not the routing but plugin state carried across renders "
                "within one process, which is why a throwaway render runs first; if "
                "this persists, a longer throwaway may be needed for these plugins."
            )

    # The legs must not be identical, or the split is not doing anything.
    leg_gap = null(solo["leg_direct"], solo["leg_effect"])
    print(
        f"\n  the two legs differ by {leg_gap['res_rms'] - leg_gap['ref_rms']:+.1f} dB "
        f"relative to leg 1 (correlation {leg_gap['corr']:.3f})"
    )

    print(f"\nWrote {len(list(out_dir.glob('*.wav')))} file(s) to {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
