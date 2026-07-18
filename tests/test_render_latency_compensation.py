"""Test: MidiRenderer compensates for plugin latency.

Uses a fake plugin that reports a fixed latency_samples and writes a
recognizable signal so we can verify the head of the rendered output is
what comes from the plugin AFTER skipping `latency` samples.
"""

from __future__ import annotations

import numpy as np
import pytest

from minihost.render import MidiRenderer


class _FakePlugin:
    """Minimal plugin shim: reports a configurable latency, writes a ramp
    from the input frame index so test code can verify which samples were
    emitted versus skipped.
    """

    def __init__(
        self,
        latency_samples: int,
        num_input_channels: int = 2,
        num_output_channels: int = 2,
        sample_rate: float = 48000.0,
        tail_seconds: float = 0.0,
    ):
        self.latency_samples = latency_samples
        self.num_input_channels = num_input_channels
        self.num_output_channels = num_output_channels
        self.sample_rate = sample_rate
        self.tail_seconds = tail_seconds
        self._frame_counter = 0  # tracks total frames the plugin has "seen"

    def reset(self):
        self._frame_counter = 0

    def process_midi(self, input_buffer, output_buffer, midi_events):
        # Write a recognisable monotonic signal: each output sample equals
        # the absolute frame number the plugin processed.
        # output_buffer may be an AudioBuffer or a numpy ndarray (the
        # renderer passes AudioBuffer; older tests pass numpy). Coerce
        # via np.asarray for uniform handling -- AudioBuffer satisfies
        # this via its __array__ hook (zero-copy view).
        view = np.asarray(output_buffer)
        n = view.shape[1]
        idx = np.arange(self._frame_counter, self._frame_counter + n, dtype=np.float32)
        # Broadcast across all channels.
        view[:, :] = idx
        self._frame_counter += n


def _make_midi():
    from minihost._core import MidiFile

    mf = MidiFile()
    t = mf.add_track()
    mf.add_note_on(t, 0, 0, 60, 100)
    mf.add_note_off(t, 480, 0, 60)  # quarter note at 120 BPM = 0.5s
    return mf


def test_latency_zero_is_no_op():
    plug = _FakePlugin(latency_samples=0)
    r = MidiRenderer(plug, _make_midi(), block_size=128, tail_seconds=0.1)
    assert r.latency_samples == 0
    assert r._render_samples == r.total_samples


def test_latency_extends_internal_render_bound():
    plug = _FakePlugin(latency_samples=64)
    r = MidiRenderer(plug, _make_midi(), block_size=128, tail_seconds=0.1)
    assert r.latency_samples == 64
    assert r._render_samples == r.total_samples + 64


def test_latency_skips_first_n_samples_of_output():
    # Render with latency=200; the first 200 samples of plugin output
    # should be discarded. The first sample of returned audio should
    # therefore correspond to plugin frame index 200.
    LATENCY = 200
    plug = _FakePlugin(latency_samples=LATENCY)
    r = MidiRenderer(plug, _make_midi(), block_size=128, tail_seconds=0.1)

    blocks = []
    while not r.is_finished:
        b = r.render_block()
        if b is not None:
            blocks.append(b)

    audio = np.concatenate(blocks, axis=1)
    # First user-visible sample == plugin frame LATENCY (the ramp values
    # are the plugin's frame counter).
    assert audio[0, 0] == float(LATENCY), (
        f"expected first sample to equal plugin frame {LATENCY}, got {audio[0, 0]}"
    )
    # Total emitted output equals user-visible total_samples.
    assert audio.shape[1] == r.total_samples


def test_latency_does_not_change_user_visible_total_samples():
    # User-visible total_samples is identical with or without latency.
    base = MidiRenderer(
        _FakePlugin(latency_samples=0), _make_midi(), block_size=128, tail_seconds=0.5
    )
    with_lat = MidiRenderer(
        _FakePlugin(latency_samples=512), _make_midi(), block_size=128, tail_seconds=0.5
    )
    assert base.total_samples == with_lat.total_samples


def test_latency_handles_skip_larger_than_block():
    # With block_size=64 and latency=200, the first ~3 blocks are entirely
    # consumed by skip and render_block returns None for them. The 4th
    # block returns a partial block (200 - 3*64 = 8 samples skipped from
    # head of that block).
    LATENCY = 200
    BLOCK = 64
    plug = _FakePlugin(latency_samples=LATENCY)
    r = MidiRenderer(plug, _make_midi(), block_size=BLOCK, tail_seconds=0.1)

    none_count = 0
    first_real_block = None
    while not r.is_finished:
        b = r.render_block()
        if b is None:
            none_count += 1
        else:
            first_real_block = b
            break
    assert none_count == LATENCY // BLOCK  # 200 // 64 = 3
    assert first_real_block is not None
    # First emitted sample should be plugin frame LATENCY.
    assert first_real_block[0, 0] == float(LATENCY)
