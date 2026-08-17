"""Synthetic probe results, shared by the cache tests and their worker.

``plugincache`` probes in one of two ways -- in this process, or in a child
process it is willing to lose -- and the cache tests need the same
synthetic answers either way, so both come from here rather than from two
copies that could drift apart.

Run as a script, this is a scan worker: ``python fake_probe.py
--mh-probe-one <path>`` prints the same metadata using the marker protocol
from ``minihost.h``. Every probe, by either route, is appended to the file
named by ``MINIHOST_PROBE_LOG``, which is how a test counts them.
"""

from __future__ import annotations

import json
import os
import sys

MARKER_BEGIN = "<<<MH_PROBE>>>"
MARKER_END = "<<<MH_PROBE_END>>>"


def log_call(path: str) -> None:
    """Record one probe, for tests that count them."""
    log = os.environ.get("MINIHOST_PROBE_LOG")
    if log:
        with open(log, "a") as f:
            f.write(path + "\n")


def synthetic(path: str) -> dict:
    """Plausible probe metadata derived from the file name.

    A name starting with ``broken`` fails to probe, ``hang`` never returns
    and ``crash`` kills the process -- the three ways a real plugin ruins a
    scan, on demand.
    """
    name = os.path.splitext(os.path.basename(path))[0]
    ext = os.path.splitext(path)[1].lower()
    fmt = {".vst3": "VST3", ".component": "AudioUnit", ".lv2": "LV2"}.get(ext, "VST3")

    if name.startswith("broken"):
        raise RuntimeError(f"cannot probe {name}")

    return {
        "name": name,
        "vendor": "Acme" if "synth" in name else "Other",
        "version": "1.0",
        "format": fmt,
        "unique_id": f"id-{name}",
        "path": path,
        "accepts_midi": name.startswith("synth"),
        "produces_midi": name.startswith("arp"),
        "num_inputs": 0 if name.startswith("synth") else 2,
        "num_outputs": 2,
    }


def probe(path: str) -> dict:
    """In-process probe: log the call, then answer."""
    log_call(path)
    return synthetic(path)


def _worker_main(argv: list[str]) -> int:
    path = (
        argv[argv.index("--mh-probe-one") + 1] if "--mh-probe-one" in argv else argv[0]
    )
    log_call(path)

    name = os.path.basename(path)
    if name.startswith("hang"):
        import time

        time.sleep(600)
    if name.startswith("crash"):
        os.abort()

    try:
        body: dict = {"ok": True}
        body.update(synthetic(path))
    except RuntimeError as e:
        body = {"ok": False, "error": str(e)}

    # Real plugins print while loading; the markers exist to be findable in
    # the middle of that, so print some noise around the answer.
    print("fake plugin says hello")
    print(f"{MARKER_BEGIN}{json.dumps(body)}{MARKER_END}")
    return 0


if __name__ == "__main__":
    raise SystemExit(_worker_main(sys.argv[1:]))
