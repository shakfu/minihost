#!/usr/bin/env python3
"""Generate example graphs for the minihost desktop app.

Writes source audio, a MIDI file and one project JSON per example into
this directory (or the directory given as the first argument). Paths in
the projects are relative, so the set works from any checkout. Renders
and live recordings land in ``build/out/projects/`` at the repo root.

    uv run python examples/projects/make_examples.py

See README.md for what each graph exercises. Examples whose plugin is
not installed are skipped with a message.
"""

from __future__ import annotations

import base64
import json
import platform
import sys
from pathlib import Path

import numpy as np

import minihost
from minihost import audio_io

SR = 48000
BPM = 120.0
BEAT = 60.0 / BPM
DEXED = Path("/Library/Audio/Plug-Ins/VST3/Dexed.vst3")


# ---------------------------------------------------------------- audio --


def _t(seconds: float, sr: int = SR) -> np.ndarray:
    return np.arange(int(seconds * sr)) / sr


def drums() -> np.ndarray:
    """One bar at 120 BPM: kick on 1 and 3, snare on 2 and 4, eighth hats."""
    out = np.zeros((2, int(4 * BEAT * SR)), np.float32)
    rng = np.random.default_rng(1)

    def place(sig, beat, pan=0.0):
        i = int(beat * BEAT * SR)
        n = min(len(sig), out.shape[1] - i)
        out[0, i : i + n] += sig[:n] * (1.0 - pan)
        out[1, i : i + n] += sig[:n] * (1.0 + pan)

    t = _t(0.35)
    kick = np.sin(2 * np.pi * (50 * t + 60 * (1 - np.exp(-t * 30)) / 30)) * np.exp(-t * 9)
    t = _t(0.2)
    snare = (rng.standard_normal(len(t)) * 0.5 + np.sin(2 * np.pi * 190 * t)) * np.exp(-t * 22)
    t = _t(0.05)
    hat = rng.standard_normal(len(t)) * np.exp(-t * 90) * 0.25
    for b in (0, 2):
        place(kick * 0.8, b)
    for b in (1, 3):
        place(snare * 0.4, b)
    for k in range(8):
        place(hat, k * 0.5, pan=0.4 if k % 2 else -0.4)
    return out * (0.7 / np.abs(out).max())


def pad() -> np.ndarray:
    """Five seconds of an A minor chord with a slow L/R pan."""
    t = _t(5.0)
    sig = sum(np.sin(2 * np.pi * f * t) for f in (220.0, 261.63, 329.63, 440.0)) / 4
    env = np.minimum(1.0, t / 0.5) * np.minimum(1.0, (5.0 - t) / 1.0)
    pan = 0.5 + 0.5 * np.sin(2 * np.pi * 0.25 * t)
    sig = sig * env * 0.4
    return np.stack([sig * (1 - pan), sig * pan]).astype(np.float32)


def stereo_id() -> np.ndarray:
    """Left: 440 Hz beeps on the beat. Right: 880 Hz beeps on the off-beat."""
    t = _t(4.0)

    def beep(f: float, offset: float) -> np.ndarray:
        return np.sin(2 * np.pi * f * t) * (((t + offset) % 0.5) < 0.12)

    return (np.stack([beep(440.0, 0.0), beep(880.0, 0.25)]) * 0.3).astype(np.float32)


def tone_44k() -> np.ndarray:
    """A 3 s rising sweep at 44.1 kHz, to exercise input resampling."""
    sr = 44100
    t = _t(3.0, sr)
    f = 200.0 * (8.0 ** (t / 3.0))
    sig = np.sin(2 * np.pi * np.cumsum(f) / sr) * 0.3
    return np.stack([sig, sig]).astype(np.float32)


def melody(path: Path) -> None:
    """Two bars of an arpeggio at 120 BPM."""
    mf = minihost.MidiFile()
    tpq = mf.ticks_per_quarter
    mf.add_tempo(0, 0, BPM)
    notes = [57, 60, 64, 69, 64, 60, 57, 60, 62, 65, 69, 74, 69, 65, 62, 65]
    for i, n in enumerate(notes):
        tick = i * tpq // 2
        mf.add_note_on(0, tick, 0, n, 96)
        mf.add_note_off(0, tick + tpq // 2 - 10, 0, n, 0)
    mf.save(str(path))


# -------------------------------------------------------------- graphs --


def au(name: str, ident: str, category: str = "Effects") -> dict:
    """A plugin node for a stock Apple AudioUnit, identified by descriptor."""
    xml = f'<PLUGIN name="{name}" format="AudioUnit" file="AudioUnit:{category}/{ident}"/>'
    return {"kind": "plugin", "name": name, "descriptor": base64.b64encode(xml.encode()).decode()}


def project(nodes: dict, edges: list, cols: dict, **top) -> dict:
    """nodes: id -> node fields; edges: (src, dst[, port]) or (src, dst, 'midi');
    cols: id -> (column, row) for the canvas layout."""
    out_edges = []
    for e in edges:
        if len(e) == 3 and e[2] == "midi":
            out_edges.append({"src": e[0], "dst": e[1], "kind": "midi"})
        else:
            out_edges.append({"src": e[0], "dst": e[1], "dst_port": e[2] if len(e) == 3 else 0})
    return {
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 512,
        **top,
        "nodes": [{"id": k, **v} for k, v in nodes.items()],
        "edges": out_edges,
        "layout": {k: {"x": 60 + 220 * c, "y": 60 + 110 * r} for k, (c, r) in cols.items()},
    }


def inp(src: str, ch: int = 2, **kw) -> dict:
    return {"kind": "input", "channels": ch, "source": src, **kw}


# Relative to this directory: <repo>/build/out/projects/.
OUT_DIR = "../../build/out/projects"


def out(name: str, ch: int = 2) -> dict:
    return {"kind": "output", "channels": ch, "sink": f"{OUT_DIR}/{name}.wav", "bit_depth": 24}


def seq(**params) -> dict:
    """A generative sequencer node; see the desktop app's sequencer.h."""
    return {"kind": "sequencer", **params}


SPK = {"kind": "device_output", "channels": 2}


def examples() -> dict[str, dict]:
    ex = {}

    ex["01_passthrough"] = project(
        {"drums": inp("audio/drums.wav"), "out": out("01_passthrough"), "speakers": SPK},
        [("drums", "out"), ("drums", "speakers")],
        {"drums": (0, 0), "out": (1, 0), "speakers": (1, 1)},
    )

    ex["02_gain_meter"] = project(
        {
            "pad": inp("audio/pad.wav"),
            "gain": {"kind": "gain", "channels": 2, "gain": 0.5},
            "meter": {"kind": "meter", "channels": 2},
            "out": out("02_gain_meter"),
            "speakers": SPK,
        },
        [("pad", "gain"), ("gain", "meter"), ("gain", "out"), ("gain", "speakers")],
        {"pad": (0, 0), "gain": (1, 0), "out": (2, 0), "meter": (2, 1), "speakers": (2, 2)},
    )

    ex["03_mix_two_files"] = project(
        {
            "drums": inp("audio/drums.wav"),
            "pad": inp("audio/pad.wav"),
            "mix": {"kind": "mix", "num_inputs": 2, "channels": 2, "gains": [1.0, 0.6]},
            "meter": {"kind": "meter", "channels": 2},
            "out": out("03_mix_two_files"),
            "speakers": SPK,
        },
        [("drums", "mix", 0), ("pad", "mix", 1), ("mix", "out"), ("mix", "meter"), ("mix", "speakers")],
        {"drums": (0, 0), "pad": (0, 1), "mix": (1, 0), "out": (2, 0), "meter": (2, 1), "speakers": (2, 2)},
    )

    ex["04_swap_channels"] = project(
        {
            "beeps": inp("audio/stereo_id.wav"),
            "left": {"kind": "pick_channel", "in_channels": 2, "channel_index": 0},
            "right": {"kind": "pick_channel", "in_channels": 2, "channel_index": 1},
            "merge": {"kind": "merge_channels", "out_channels": 2},
            "out": out("04_swap_channels"),
            "speakers": SPK,
        },
        # Right goes to merge port 0 (left), left to port 1 (right).
        [("beeps", "left"), ("beeps", "right"), ("right", "merge", 0), ("left", "merge", 1),
         ("merge", "out"), ("merge", "speakers")],
        {"beeps": (0, 0), "left": (1, 0), "right": (1, 1), "merge": (2, 0), "out": (3, 0), "speakers": (3, 1)},
    )

    ex["05_resample_input"] = project(
        {"sweep": inp("audio/sweep_44k.wav", resample=True), "out": out("05_resample_input"), "speakers": SPK},
        [("sweep", "out"), ("sweep", "speakers")],
        {"sweep": (0, 0), "out": (1, 0), "speakers": (1, 1)},
    )

    ex["06_metronome"] = project(
        {
            "click": {"kind": "metronome", "channels": 2, "gain": 0.5, "freq_hz": 1000.0, "decay_ms": 30.0},
            "drums": inp("audio/drums.wav"),
            "mix": {"kind": "mix", "num_inputs": 2, "channels": 2},
            "out": out("06_metronome"),
            "speakers": SPK,
        },
        [("click", "mix", 0), ("drums", "mix", 1), ("mix", "out"), ("mix", "speakers")],
        {"click": (0, 0), "drums": (0, 1), "mix": (1, 0), "out": (2, 0), "speakers": (2, 1)},
    )

    if platform.system() == "Darwin":
        ex["07_au_chain"] = project(
            {
                "drums": inp("audio/drums.wav"),
                "lowpass": au("AULowpass", "aufx,lpas,appl"),
                "delay": au("AUDelay", "aufx,dely,appl"),
                "out": out("07_au_chain"),
                "speakers": SPK,
            },
            [("drums", "lowpass"), ("lowpass", "delay"), ("delay", "out"), ("delay", "speakers")],
            {"drums": (0, 0), "lowpass": (1, 0), "delay": (2, 0), "out": (3, 0), "speakers": (3, 1)},
        )
        ex["08_parallel_fx"] = project(
            {
                "pad": inp("audio/pad.wav"),
                "delay": au("AUDelay", "aufx,dely,appl"),
                "reverb": au("AUReverb2", "aufx,rvb2,appl"),
                "mix": {"kind": "mix", "num_inputs": 3, "channels": 2, "gains": [0.7, 0.5, 0.5]},
                "meter": {"kind": "meter", "channels": 2},
                "out": out("08_parallel_fx"),
                "speakers": SPK,
            },
            [("pad", "mix", 0), ("pad", "delay"), ("pad", "reverb"), ("delay", "mix", 1),
             ("reverb", "mix", 2), ("mix", "out"), ("mix", "meter"), ("mix", "speakers")],
            {"pad": (0, 1), "delay": (1, 0), "reverb": (1, 2), "mix": (2, 1), "out": (3, 0),
             "meter": (3, 1), "speakers": (3, 2)},
        )
        ex["09_mic_delay"] = project(
            {
                "mic": {"kind": "device_input", "channels": 2},
                "delay": au("AUDelay", "aufx,dely,appl"),
                "meter": {"kind": "meter", "channels": 2},
                "out": out("09_mic_delay"),
                "speakers": SPK,
            },
            [("mic", "delay"), ("delay", "meter"), ("delay", "out"), ("delay", "speakers")],
            {"mic": (0, 0), "delay": (1, 0), "out": (2, 0), "meter": (2, 1), "speakers": (2, 2)},
            duration_seconds=8.0,
        )

    if platform.system() == "Darwin":
        # Stereo; DLSMusicDevice has 4 outputs.
        def synth() -> dict:
            return au("AUMIDISynth", "aumu,msyn,appl", "Synths")

        ex["11_sequencer"] = project(
            {
                "seq": seq(steps=16, rate=0.25, root=60, scale="minor_pentatonic",
                           octaves=2, density=0.65, gate=0.4, mutate=0.1, seed=7),
                "synth": synth(),
                "reverb": au("AUReverb2", "aufx,rvb2,appl"),
                "out": out("11_sequencer"),
                "speakers": SPK,
            },
            [("seq", "synth", "midi"), ("synth", "reverb"), ("reverb", "out"),
             ("reverb", "speakers")],
            {"seq": (0, 0), "synth": (1, 0), "reverb": (2, 0), "out": (3, 0), "speakers": (3, 1)},
            bpm=110.0, duration_seconds=8.0,
        )
        ex["12_two_sequencers"] = project(
            {
                "bass": seq(steps=8, rate=0.5, root=36, scale="dorian", octaves=1,
                            density=0.8, gate=0.8, velocity_min=90, velocity_max=110,
                            seed=3),
                "lead": seq(steps=12, rate=0.25, root=64, scale="dorian", octaves=2,
                            density=0.5, gate=0.3, mutate=0.25, seed=11),
                "bass_synth": synth(),
                "lead_synth": synth(),
                "mix": {"kind": "mix", "num_inputs": 2, "channels": 2, "gains": [0.9, 0.6]},
                "delay": au("AUDelay", "aufx,dely,appl"),
                "meter": {"kind": "meter", "channels": 2},
                "out": out("12_two_sequencers"),
                "speakers": SPK,
            },
            [("bass", "bass_synth", "midi"), ("lead", "lead_synth", "midi"),
             ("bass_synth", "mix", 0), ("lead_synth", "mix", 1), ("mix", "delay"),
             ("delay", "out"), ("delay", "meter"), ("delay", "speakers")],
            {"bass": (0, 0), "lead": (0, 1), "bass_synth": (1, 0), "lead_synth": (1, 1),
             "mix": (2, 0), "delay": (3, 0), "out": (4, 0), "meter": (4, 1), "speakers": (4, 2)},
            bpm=96.0, duration_seconds=10.0,
        )

    if DEXED.exists():
        ex["13_sequencer_dexed"] = project(
            {
                "seq": seq(steps=16, rate=0.25, root=48, scale="minor", octaves=2,
                           density=0.7, gate=0.5, mutate=0.15, seed=21),
                "dexed": {"kind": "plugin", "path": str(DEXED)},
                "out": out("13_sequencer_dexed"),
                "speakers": SPK,
            },
            [("seq", "dexed", "midi"), ("dexed", "out"), ("dexed", "speakers")],
            {"seq": (0, 0), "dexed": (1, 0), "out": (2, 0), "speakers": (2, 1)},
            bpm=124.0, duration_seconds=8.0,
        )

        ex["10_midi_synth"] = project(
            {
                "notes": {"kind": "midi_input", "source": "audio/melody.mid"},
                "transpose": {"kind": "midi_transpose", "semitones": 12},
                "dexed": {"kind": "plugin", "path": str(DEXED)},
                "out": out("10_midi_synth"),
                "speakers": SPK,
            },
            [("notes", "transpose", "midi"), ("transpose", "dexed", "midi"),
             ("dexed", "out"), ("dexed", "speakers")],
            {"notes": (0, 0), "transpose": (1, 0), "dexed": (2, 0), "out": (3, 0), "speakers": (3, 1)},
            duration_seconds=5.0,
        )
    else:
        print(f"skip 10_midi_synth: {DEXED} not installed")
    return ex


def main(dest: Path) -> None:
    (dest / "audio").mkdir(parents=True, exist_ok=True)
    audio_io.write_audio(str(dest / "audio/drums.wav"), drums(), SR, bit_depth=24)
    audio_io.write_audio(str(dest / "audio/pad.wav"), pad(), SR, bit_depth=24)
    audio_io.write_audio(str(dest / "audio/stereo_id.wav"), stereo_id(), SR, bit_depth=24)
    audio_io.write_audio(str(dest / "audio/sweep_44k.wav"), tone_44k(), 44100, bit_depth=24)
    melody(dest / "audio/melody.mid")
    for name, doc in examples().items():
        (dest / f"{name}.json").write_text(json.dumps(doc, indent=2) + "\n")
        print(f"wrote {name}.json")


if __name__ == "__main__":
    main(Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent)
