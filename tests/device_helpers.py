"""Audio-device availability, for the tests that need real hardware.

A machine with no sound card -- a CI runner, a headless container -- has
nothing for miniaudio to open, so `AudioDevice(...)` raises and every test
that builds one fails. That is the environment, not the code, so they skip
instead.

Two probes, because either alone is wrong. The device list is the portable
one and needs no plugin. ALSA is the exception: with no card behind it, its
configuration still enumerates, so on Linux `/proc/asound/cards` -- which
reads "no soundcards" or is absent -- is what actually answers the question.
Neither probe opens a device, so nothing here takes the hardware a test is
about to ask for.
"""

from __future__ import annotations

import functools
import sys
from pathlib import Path

import pytest


def _linux_has_a_soundcard() -> bool:
    cards = Path("/proc/asound/cards")
    try:
        listing = cards.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False  # snd not loaded at all
    return bool(listing.strip()) and "no soundcards" not in listing.lower()


@functools.lru_cache(maxsize=1)
def audio_device_available() -> bool:
    try:
        import minihost

        if not minihost.audio_get_playback_devices():
            return False
    except Exception:
        return False
    if sys.platform.startswith("linux"):
        return _linux_has_a_soundcard()
    return True


skip_if_no_audio_device = pytest.mark.skipif(
    not audio_device_available(),
    reason="no audio playback device on this machine",
)
