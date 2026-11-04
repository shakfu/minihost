"""Live recording keeps every take aligned when the disk falls behind.

``--recorder-selftest=<dir>`` drives TakeRecorder, the live engine's
recorder, through a full FIFO with the writer held back. It checks that a
block that does not fit is dropped for every output at once and written as
silence in its place, so both takes keep the transport's length and order.
"""

from __future__ import annotations

import subprocess

import numpy as np

from minihost import audio_io

from desktop_helpers import DESKTOP_BIN, skip_if_no_desktop

BLOCK = 40


@skip_if_no_desktop
def test_dropped_blocks_become_aligned_silence(tmp_path):
    res = subprocess.run(
        [str(DESKTOP_BIN), f"--recorder-selftest={tmp_path}"],
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert res.returncode == 0, f"stdout:{res.stdout}\nstderr:{res.stderr}"
    assert "recorder-selftest OK" in res.stderr

    # Blocks 0, 1 and 4 were written; 2 and 3 were dropped.
    expect = np.repeat([1.0, 2.0, 0.0, 0.0, 5.0], BLOCK).astype(np.float32)
    for i, sign in ((0, 1.0), (1, -1.0)):
        data, sr = audio_io.read_audio(str(tmp_path / f"take{i}.wav"), as_=np.ndarray)
        assert sr == 48000
        assert data.shape[-1] == 5 * BLOCK
        for ch in np.atleast_2d(data):
            np.testing.assert_array_equal(ch, sign * expect)
