"""Regression test: MIDI ports still open after MIDIServer has gone idle.

launchd stops MIDIServer a few seconds (2.5-8 s observed) after the last
client disposes. Before the fix, a process that had talked to that server,
even only to enumerate ports, could not create a client afterwards. In a full
suite, that made `test_midi_device_loopback.py` fail whenever earlier MIDI
work had ended a few seconds before it.

Each case runs in a fresh interpreter: an earlier test in this process may
already hold the client that the fix keeps alive. Passes without testing
anything if another application holds a CoreMIDI client, because the server
never stops.
"""

from __future__ import annotations

import subprocess
import sys

import pytest

pytestmark = pytest.mark.skipif(
    sys.platform != "darwin", reason="MIDIServer idle exit is CoreMIDI-specific"
)

# The first CoreMIDI contact. Enumeration creates no client of its own, so it
# needs a server another process started.
FIRST_CONTACT = {
    "open": 'minihost.MidiIn.open_virtual("mh-idle-a", lambda data: None).close()',
    "enumerate": """
subprocess.run([sys.executable, "-c",
    'import minihost; minihost.MidiIn.open_virtual("mh-idle-o", lambda d: None).close()'])
minihost.midi_get_input_ports()
""",
}

_SCRIPT = """
import subprocess, sys, time
import minihost
{first}
time.sleep(10)
minihost.MidiIn.open_virtual("mh-idle-b", lambda data: None).close()
"""


@pytest.mark.parametrize("first", FIRST_CONTACT)
def test_virtual_port_opens_after_the_midi_server_idles_out(first):
    script = _SCRIPT.format(first=FIRST_CONTACT[first])
    proc = subprocess.run(
        [sys.executable, "-c", script], capture_output=True, text=True, timeout=60
    )
    assert proc.returncode == 0, proc.stderr
