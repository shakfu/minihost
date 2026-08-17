"""Probe one plugin, print the answer, exit.

Run as ``python -m minihost._scan_worker --mh-probe-one <path>``. Nothing
imports this module for its functions: it exists to be a disposable
process, so that a plugin which hangs or crashes while being probed takes
this process down instead of the scan that spawned it.

The output convention is the one in ``minihost.h``
(``MH_SCAN_WORKER_BEGIN`` / ``MH_SCAN_WORKER_END``), so this worker is
interchangeable with the C library's own -- either supervisor can drive
either worker. The markers matter because plugins write to stdout while
loading, several of them copiously, so the answer has to be findable
inside that noise rather than assumed to be all of it.
"""

from __future__ import annotations

import json
import sys

BEGIN = "<<<MH_PROBE>>>"
END = "<<<MH_PROBE_END>>>"

FLAG = "--mh-probe-one"


def _plugin_path(argv: list[str]) -> str | None:
    if FLAG in argv:
        i = argv.index(FLAG)
        return argv[i + 1] if i + 1 < len(argv) else None
    return argv[0] if argv else None


def main(argv: list[str]) -> int:
    path = _plugin_path(argv)
    if path is None:
        body: dict = {"ok": False, "error": "no plugin path given"}
    else:
        # Imported here rather than at module scope: loading the extension
        # is the expensive part of this process, and there is no reason to
        # pay for it before knowing there is something to probe.
        import minihost

        try:
            body = {"ok": True}
            body.update(minihost.probe(path))
        except Exception as e:  # probe raises RuntimeError; a plugin may do worse
            body = {"ok": False, "error": str(e)}

    sys.stdout.write(f"\n{BEGIN}{json.dumps(body)}{END}\n")
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
