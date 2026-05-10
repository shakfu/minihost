"""Verify that minihost can import and exercise the AudioBuffer-only code
paths in environments where numpy is not installed.

This test runs a sub-Python process with numpy hidden via a sys.path
prepend trick, then asserts the package imports and the AudioBuffer-only
surface works. Skipped when the test environment has numpy strongly bound
into the C extension (which it does in dev), so we run the check via a
subprocess that's instructed to fail on numpy import.
"""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap


# Script run in a child process. Hides numpy by injecting an import-blocker
# meta-finder, then exercises the AudioBuffer-only path.
_SCRIPT = textwrap.dedent(
    """
    import sys

    # Block numpy at import time.
    class _BlockNumpy:
        def find_spec(self, fullname, path=None, target=None):
            if fullname == "numpy" or fullname.startswith("numpy."):
                raise ImportError(
                    "numpy is hidden by the numpy-optional regression test"
                )
            return None
    sys.meta_path.insert(0, _BlockNumpy())

    # Confirm numpy is unreachable.
    try:
        import numpy
        print("FAIL: numpy was importable")
        sys.exit(1)
    except ImportError:
        pass

    # Now exercise minihost.
    import minihost

    buf = minihost.AudioBuffer(2, 256)
    assert buf.shape == (2, 256), buf.shape
    assert buf.magnitude() == 0.0

    buf[0, 100] = 0.5
    buf[1, 50] = -0.25
    assert buf[0, 100] == 0.5
    assert buf[1, 50] == -0.25
    assert abs(buf.magnitude() - 0.5) < 1e-6

    sub = buf[:, 0:10]
    assert isinstance(sub, minihost.AudioBuffer)
    assert sub.shape == (2, 10)

    # File I/O round-trip without numpy.
    import os as _os, tempfile
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as f:
        path = f.name
    try:
        minihost.write_audio(path, buf, 48000)
        info = minihost.get_audio_info(path)
        assert info["channels"] == 2 and info["sample_rate"] == 48000
        data, sr = minihost.read_audio(path)
        assert isinstance(data, minihost.AudioBuffer)
        assert data.shape == (2, 256)
        assert sr == 48000
    finally:
        _os.unlink(path)

    # Calling .as_ndarray() must raise a clear ImportError.
    raised = False
    try:
        buf.as_ndarray()
    except ImportError as e:
        msg = str(e)
        if "numpy" in msg.lower() and "minihost[numpy]" in msg:
            raised = True
    assert raised, "AudioBuffer.as_ndarray() should raise ImportError without numpy"

    print("OK")
    """
)


def test_minihost_imports_and_works_without_numpy():
    proc = subprocess.run(
        [sys.executable, "-c", _SCRIPT],
        capture_output=True,
        text=True,
        env={**os.environ},
        timeout=30,
    )
    assert proc.returncode == 0, (
        f"subprocess failed:\nSTDOUT:\n{proc.stdout}\nSTDERR:\n{proc.stderr}"
    )
    assert proc.stdout.strip().endswith("OK"), proc.stdout
