"""The MIDI loopback sender for this platform: CoreMIDI on macOS, ALSA on Linux.

Both back-ends expose `available`, `Sender`, `wait_for_destination`; the
destination value is opaque to callers. `assert_contains_in_order` is shared.
"""

from __future__ import annotations

import sys

if sys.platform == "darwin":
    from coremidi_loopback import Sender, available, wait_for_destination
else:
    from alsa_loopback import Sender, available, wait_for_destination

from coremidi_loopback import assert_contains_in_order

__all__ = ["Sender", "available", "wait_for_destination", "assert_contains_in_order"]
