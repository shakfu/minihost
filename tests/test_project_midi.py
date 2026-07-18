"""MIDI routing in the project schema (offline render).

These tests exercise the project loader/renderer's MIDI path without a
plugin: MIDI flows midi_input -> [processor/merge] -> midi_output, where
the input reads a .mid `source` and the output writes a .mid `sink`. A
single audio input->output passthrough is included only to give the graph
a valid audio subgraph and to set the render duration.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import numpy as np
import pytest

import minihost
from minihost import audio_io, midi_file_to_events

SR = 48000
TPQ = 480  # ticks/quarter at 120 BPM -> 480 ticks == 0.5 s == 24000 samples


def _write_source_mid(path: Path, notes: list[tuple[int, int, int, int]]) -> None:
    """notes: (tick, status_hi_nibble_channel, data1, data2). Only note
    on/off are needed here."""
    mf = minihost.MidiFile()
    mf.ticks_per_quarter = TPQ
    mf.add_tempo(0, 0, 120.0)
    for tick, status, d1, d2 in notes:
        hi = status & 0xF0
        ch = status & 0x0F
        if hi == 0x90:
            mf.add_note_on(0, tick, ch, d1, d2)
        elif hi == 0x80:
            mf.add_note_off(0, tick, ch, d1, d2)
        else:  # pragma: no cover - tests only use notes
            raise ValueError(f"unsupported status {status:#x}")
    mf.save(str(path))


def _audio_in(path: Path, frames: int = SR) -> None:
    rng = np.random.default_rng(0)
    sig = (rng.standard_normal((2, frames)) * 0.1).astype(np.float32)
    audio_io.write_audio(str(path), sig, SR, bit_depth=24)


def _render(
    tmp_path: Path,
    midi_nodes: list[dict],
    midi_edges: list[dict],
    *,
    duration_seconds: float = 1.0,
) -> None:
    in_wav = tmp_path / "in.wav"
    out_wav = tmp_path / "out.wav"
    _audio_in(in_wav, int(duration_seconds * SR))
    doc = {
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 256,
        "duration_seconds": duration_seconds,
        "nodes": [
            {"id": "in", "kind": "input", "channels": 2, "source": str(in_wav)},
            {
                "id": "out",
                "kind": "output",
                "channels": 2,
                "sink": str(out_wav),
                "bit_depth": 24,
            },
            *midi_nodes,
        ],
        "edges": [
            {"src": "in", "dst": "out"},
            *midi_edges,
        ],
    }
    proj = tmp_path / "p.json"
    proj.write_text(json.dumps(doc))
    minihost.render_project(proj)


def test_project_midi_passthrough_roundtrip(tmp_path):
    src = tmp_path / "src.mid"
    _write_source_mid(src, [(0, 0x90, 60, 100), (TPQ, 0x80, 60, 0)])
    out_mid = tmp_path / "out.mid"

    _render(
        tmp_path,
        midi_nodes=[
            {"id": "mi", "kind": "midi_input", "source": str(src)},
            {"id": "mo", "kind": "midi_output", "sink": str(out_mid)},
        ],
        midi_edges=[{"src": "mi", "dst": "mo", "kind": "midi"}],
    )

    src_ev = midi_file_to_events(str(src), SR)
    out_ev = midi_file_to_events(str(out_mid), SR)
    # Status + data identical; sample offsets preserved (allow +-2 for the
    # two tick<->sample rounding trips).
    assert [e[1:] for e in out_ev] == [e[1:] for e in src_ev]
    assert all(abs(a[0] - b[0]) <= 2 for a, b in zip(out_ev, src_ev))


def test_project_midi_transpose(tmp_path):
    src = tmp_path / "src.mid"
    _write_source_mid(src, [(0, 0x90, 60, 100), (TPQ, 0x80, 60, 0)])
    out_mid = tmp_path / "out.mid"

    _render(
        tmp_path,
        midi_nodes=[
            {"id": "mi", "kind": "midi_input", "source": str(src)},
            {"id": "tr", "kind": "midi_transpose", "semitones": 12},
            {"id": "mo", "kind": "midi_output", "sink": str(out_mid)},
        ],
        midi_edges=[
            {"src": "mi", "dst": "tr", "kind": "midi"},
            {"src": "tr", "dst": "mo", "kind": "midi"},
        ],
    )

    pitches = [e[2] for e in midi_file_to_events(str(out_mid), SR)]
    assert pitches == [72, 72]  # 60 + 12, both note-on and note-off


def test_project_midi_filter_drops_out_of_range(tmp_path):
    src = tmp_path / "src.mid"
    # Two notes: C3 (48, in range) and C5 (72, out of range for 36..60).
    _write_source_mid(
        src,
        [
            (0, 0x90, 48, 100),
            (0, 0x90, 72, 100),
            (TPQ, 0x80, 48, 0),
            (TPQ, 0x80, 72, 0),
        ],
    )
    out_mid = tmp_path / "out.mid"

    _render(
        tmp_path,
        midi_nodes=[
            {"id": "mi", "kind": "midi_input", "source": str(src)},
            {"id": "f", "kind": "midi_filter", "min_note": 36, "max_note": 60},
            {"id": "mo", "kind": "midi_output", "sink": str(out_mid)},
        ],
        midi_edges=[
            {"src": "mi", "dst": "f", "kind": "midi"},
            {"src": "f", "dst": "mo", "kind": "midi"},
        ],
    )

    pitches = {e[2] for e in midi_file_to_events(str(out_mid), SR)}
    assert pitches == {48}  # 72 filtered out


def test_project_midi_merge_two_sources(tmp_path):
    src_a = tmp_path / "a.mid"
    src_b = tmp_path / "b.mid"
    _write_source_mid(src_a, [(0, 0x90, 60, 100), (TPQ, 0x80, 60, 0)])
    _write_source_mid(src_b, [(0, 0x90, 67, 100), (TPQ, 0x80, 67, 0)])
    out_mid = tmp_path / "out.mid"

    _render(
        tmp_path,
        midi_nodes=[
            {"id": "ma", "kind": "midi_input", "source": str(src_a)},
            {"id": "mb", "kind": "midi_input", "source": str(src_b)},
            {"id": "mrg", "kind": "midi_merge", "num_inputs": 2},
            {"id": "mo", "kind": "midi_output", "sink": str(out_mid)},
        ],
        midi_edges=[
            {"src": "ma", "dst": "mrg", "kind": "midi", "dst_port": 0},
            {"src": "mb", "dst": "mrg", "kind": "midi", "dst_port": 1},
            {"src": "mrg", "dst": "mo", "kind": "midi"},
        ],
    )

    pitches = {e[2] for e in midi_file_to_events(str(out_mid), SR)}
    assert pitches == {60, 67}  # both branches present


def test_project_midi_output_without_sink_renders(tmp_path):
    # A midi_output with no sink must drain-and-discard, not error, and
    # not write any file.
    src = tmp_path / "src.mid"
    _write_source_mid(src, [(0, 0x90, 60, 100), (TPQ, 0x80, 60, 0)])

    _render(
        tmp_path,
        midi_nodes=[
            {"id": "mi", "kind": "midi_input", "source": str(src)},
            {"id": "mo", "kind": "midi_output"},  # no sink
        ],
        midi_edges=[{"src": "mi", "dst": "mo", "kind": "midi"}],
    )
    # The audio sink still rendered.
    assert (tmp_path / "out.wav").exists()


def test_project_midi_input_missing_source_raises(tmp_path):
    from minihost.project import ProjectError

    with pytest.raises(ProjectError, match="source not found"):
        _render(
            tmp_path,
            midi_nodes=[
                {
                    "id": "mi",
                    "kind": "midi_input",
                    "source": str(tmp_path / "nope.mid"),
                },
                {"id": "mo", "kind": "midi_output"},
            ],
            midi_edges=[{"src": "mi", "dst": "mo", "kind": "midi"}],
        )


def test_project_unknown_edge_kind_raises(tmp_path):
    from minihost.project import ProjectError

    src = tmp_path / "src.mid"
    _write_source_mid(src, [(0, 0x90, 60, 100)])
    with pytest.raises(ProjectError, match="edge kind"):
        _render(
            tmp_path,
            midi_nodes=[
                {"id": "mi", "kind": "midi_input", "source": str(src)},
                {"id": "mo", "kind": "midi_output"},
            ],
            midi_edges=[{"src": "mi", "dst": "mo", "kind": "control"}],
        )


# Instrument round-trip: drive a synth from a project MIDI source and
# confirm it produces audio. Requires a real instrument plugin.
SYNTH_PLUGIN = (
    os.environ.get("MINIHOST_TEST_PLUGIN") or "/Library/Audio/Plug-Ins/VST3/Dexed.vst3"
)
skip_if_no_synth = pytest.mark.skipif(
    not os.path.exists(SYNTH_PLUGIN),
    reason=f"instrument plugin not found at {SYNTH_PLUGIN}",
)


@skip_if_no_synth
def test_project_midi_drives_instrument(tmp_path):
    probe = minihost.Plugin(SYNTH_PLUGIN, sample_rate=SR, max_block_size=256)
    out_ch = probe.num_output_channels
    probe.close()
    if out_ch < 1:
        pytest.skip("synth reports zero output channels")

    src = tmp_path / "src.mid"
    _write_source_mid(src, [(0, 0x90, 60, 100), (TPQ, 0x80, 60, 0)])
    out_wav = tmp_path / "synth_out.wav"

    doc = {
        "minihost_project_version": 1,
        "sample_rate": SR,
        "block_size": 256,
        "duration_seconds": 1.0,
        "nodes": [
            {"id": "mi", "kind": "midi_input", "source": str(src)},
            {"id": "synth", "kind": "plugin", "path": SYNTH_PLUGIN},
            {
                "id": "out",
                "kind": "output",
                "channels": out_ch,
                "sink": str(out_wav),
                "bit_depth": 24,
            },
        ],
        "edges": [
            {"src": "mi", "dst": "synth", "kind": "midi"},
            {"src": "synth", "dst": "out"},
        ],
    }
    proj = tmp_path / "p.json"
    proj.write_text(json.dumps(doc))
    minihost.render_project(proj)

    audio, _ = audio_io.read_audio(str(out_wav), as_=np.ndarray)
    assert float(np.max(np.abs(audio))) > 1e-6, "synth produced silence"
