"""Persistent plugin-scan cache.

Probing an audio plugin is slow: the OS plugin host loads each one to read
its metadata. This module keeps a JSON index of probe results keyed by
plugin path, with a filesystem fingerprint (mtime + size) so stale entries
are re-probed automatically. A repeat scan of an unchanged directory
returns instantly and never touches the plugins.

What is cached: the probe-level metadata (name, vendor, version, format,
unique_id, path, input/output channel counts, MIDI in/out) plus a
validation status (``ok`` / ``error`` / ``timeout`` / ``crash``). A plugin
that fails to probe is remembered so it is not re-probed on every scan.
Parameter lists require a full plugin load and are deliberately not cached.

Scanning probes each plugin in a child process by default, because probing
means loading, and a plugin that hangs or corrupts its heap on load would
otherwise take the scan -- and the interpreter running it -- with it. Five
of the ~350 plugins installed on the development machine do exactly that.
The last two statuses are the outcomes only that arrangement can report:
``timeout`` for a plugin that never finished, ``crash`` for a worker that
died before answering. Pass ``supervised=False`` to probe in-process.

Cache location (override with ``MINIHOST_CACHE_DIR``):
  - macOS:   ~/Library/Caches/minihost/plugins.json
  - Windows: %LOCALAPPDATA%/minihost/Cache/plugins.json
  - other:   $XDG_CACHE_HOME/minihost/plugins.json (or ~/.cache/...)

Plugin discovery is by known extension (.vst3, .component, .lv2, ...), so a
cached scan finds the same file/bundle plugins as the uncached
``minihost.scan_directory`` for the formats minihost supports. Pass
``no_cache`` at the CLI (or call ``minihost.scan_directory``) to bypass.
"""

from __future__ import annotations

import json
import os
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable

import minihost

from minihost._scan_worker import BEGIN as _WORKER_BEGIN
from minihost._scan_worker import END as _WORKER_END
from minihost._scan_worker import FLAG as _WORKER_FLAG

SCHEMA_VERSION = 1

# Seconds allowed for one probe before the child is killed and the plugin
# recorded as a timeout. Matches the C library's default; the same
# environment variable overrides both.
DEFAULT_PROBE_TIMEOUT = 60.0

# Path suffixes that denote a plugin bundle or binary. Discovery treats any
# matching entry as a leaf (it is not descended into).
PLUGIN_EXTS = {".vst3", ".component", ".lv2", ".vst", ".clap", ".dll", ".so"}


# -- cache location --------------------------------------------------- #


def _default_cache_dir() -> Path:
    env = os.environ.get("MINIHOST_CACHE_DIR")
    if env:
        return Path(env)
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Caches" / "minihost"
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA") or str(Path.home() / "AppData" / "Local")
        return Path(base) / "minihost" / "Cache"
    base = os.environ.get("XDG_CACHE_HOME") or str(Path.home() / ".cache")
    return Path(base) / "minihost"


def cache_file() -> Path:
    """Absolute path to the JSON cache file (honors MINIHOST_CACHE_DIR)."""
    return _default_cache_dir() / "plugins.json"


# -- probe + fingerprint (indirected for testing) --------------------- #


def _probe(path: str) -> dict:
    """Probe a single plugin. Indirected so tests can monkeypatch it."""
    return minihost.probe(path)


def _fingerprint(path: str) -> dict:
    """Cheap freshness key for a plugin path. Uses the path's own stat
    (mtime + size); for bundles this is the bundle directory's metadata,
    which changes when the plugin is (re)installed. Cheap by design -- it
    must never approach the cost of a probe."""
    st = os.stat(path)
    return {"mtime_ns": st.st_mtime_ns, "size": st.st_size}


# -- raw cache I/O ---------------------------------------------------- #


def _empty() -> dict:
    return {"schema": SCHEMA_VERSION, "entries": {}}


def _load_raw() -> dict:
    f = cache_file()
    if not f.exists():
        return _empty()
    try:
        doc = json.loads(f.read_text())
    except (json.JSONDecodeError, OSError):
        return _empty()
    if not isinstance(doc, dict) or doc.get("schema") != SCHEMA_VERSION:
        return _empty()
    if not isinstance(doc.get("entries"), dict):
        doc["entries"] = {}
    return doc


def _save_raw(doc: dict) -> None:
    f = cache_file()
    f.parent.mkdir(parents=True, exist_ok=True)
    tmp = f.with_suffix(f.suffix + ".tmp")
    tmp.write_text(json.dumps(doc, indent=2) + "\n")
    tmp.replace(f)


# -- discovery -------------------------------------------------------- #


def _discover_plugins(directory: str) -> list[str]:
    """Return absolute paths of plugin bundles/files under `directory`,
    recursing into plain directories but treating any plugin-extension
    entry as a leaf (so we never descend into a .vst3 bundle)."""
    out: list[str] = []

    def walk(d: str) -> None:
        try:
            entries = list(os.scandir(d))
        except OSError:
            return
        for e in entries:
            ext = os.path.splitext(e.name)[1].lower()
            if ext in PLUGIN_EXTS:
                out.append(os.path.abspath(e.path))  # leaf
                continue
            try:
                is_dir = e.is_dir(follow_symlinks=False)
            except OSError:
                is_dir = False
            if is_dir:
                walk(e.path)

    walk(str(directory))
    return sorted(out)


# -- entry helpers ---------------------------------------------------- #


def _entry_fresh(entry: dict, path: str) -> bool:
    try:
        return entry.get("fp") == _fingerprint(path)
    except OSError:
        return False


def _probe_to_entry(path: str) -> dict:
    try:
        fp: Any = _fingerprint(path)
    except OSError:
        fp = None
    try:
        desc = _probe(path)
        # probe() reports no path (it is told one, so it does not repeat it
        # back); the cache carries it, as the C scanner's entries do.
        desc["path"] = path
        return {"status": "ok", "desc": desc, "fp": fp}
    except Exception as e:  # probe raises RuntimeError on failure
        return {
            "status": "error",
            "error": str(e),
            "desc": {"path": path},
            "fp": fp,
        }


# -- supervised probing ----------------------------------------------- #


def _worker_command() -> list[str]:
    """The command to spawn per plugin, without the plugin path.

    ``MINIHOST_SCAN_WORKER`` overrides it, which is how the C library's
    supervisor is pointed at another worker too -- and how the tests get a
    worker that misbehaves on demand without needing a plugin that does.
    """
    env = os.environ.get("MINIHOST_SCAN_WORKER")
    if env:
        return shlex.split(env)
    return [sys.executable, "-m", "minihost._scan_worker"]


def _probe_timeout(explicit: float | None) -> float:
    if explicit is not None:
        return explicit
    env = os.environ.get("MINIHOST_SCAN_TIMEOUT_MS")
    if env:
        try:
            ms = int(env)
            if ms > 0:
                return ms / 1000.0
        except ValueError:
            pass
    return DEFAULT_PROBE_TIMEOUT


def _payload(stdout: str) -> dict | None:
    """The worker's answer, dug out of whatever the plugin printed."""
    start = stdout.find(_WORKER_BEGIN)
    end = stdout.find(_WORKER_END)
    if start < 0 or end <= start:
        return None
    try:
        parsed = json.loads(stdout[start + len(_WORKER_BEGIN) : end])
    except ValueError:
        return None
    return parsed if isinstance(parsed, dict) else None


def _probe_to_entry_supervised(path: str, timeout: float) -> dict:
    """Probe `path` in a child process, and survive whatever it does."""
    try:
        fp: Any = _fingerprint(path)
    except OSError:
        fp = None

    def entry(status: str, error: str | None = None, desc: dict | None = None) -> dict:
        out = {"status": status, "desc": desc or {"path": path}, "fp": fp}
        if error is not None:
            out["error"] = error
        return out

    cmd = [*_worker_command(), _WORKER_FLAG, path]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return entry("timeout", f"plugin did not finish probing within {timeout:g}s")
    except OSError as e:
        raise RuntimeError(f"could not start scan worker {cmd[0]!r}: {e}") from e

    body = _payload(proc.stdout)
    if body is None:
        # No answer: the child died on this plugin. A signal death is the
        # usual cause, so the return code is reported only when it is one a
        # process chose to exit with.
        detail = f" (exit code {proc.returncode})" if proc.returncode > 0 else ""
        return entry("crash", f"scan worker died probing this plugin{detail}")

    if body.get("ok"):
        desc = {k: v for k, v in body.items() if k != "ok"}
        desc["path"] = path
        return entry("ok", desc=desc)
    return entry("error", str(body.get("error", "probe failed")))


# -- public API ------------------------------------------------------- #


def scan(
    directory: str | Path,
    *,
    refresh: bool = False,
    include_errors: bool = False,
    supervised: bool = True,
    timeout: float | None = None,
    on_progress: Callable[[int, int, str], None] | None = None,
) -> list[dict]:
    """Scan `directory` for plugins, using and updating the cache.

    Only new or changed plugins (by fingerprint) are probed; everything
    else is served from cache. Returns the list of probe-metadata dicts for
    successfully-probed plugins (plus error stubs if `include_errors`).
    `refresh=True` re-probes every discovered plugin. `on_progress(done,
    total, path)` is called per plugin if provided.

    Each plugin is probed in a child process, so one that hangs or crashes
    on load is recorded as ``timeout`` / ``crash`` and the scan carries on.
    `timeout` bounds one probe in seconds (default 60, or
    ``MINIHOST_SCAN_TIMEOUT_MS``). `supervised=False` probes in this
    process instead: faster by a process launch per plugin, and the way
    this worked before -- at the price that a bad plugin ends the scan and
    takes the interpreter with it.

    The cache is written as the scan proceeds when supervised, so a scan
    interrupted part way keeps what it had and a re-run resumes.
    """
    doc = _load_raw()
    entries = doc["entries"]
    paths = _discover_plugins(str(directory))
    results: list[dict] = []
    changed = 0
    probe_timeout = _probe_timeout(timeout)

    for i, path in enumerate(paths):
        entry = entries.get(path)
        if refresh or entry is None or not _entry_fresh(entry, path):
            entry = (
                _probe_to_entry_supervised(path, probe_timeout)
                if supervised
                else _probe_to_entry(path)
            )
            entries[path] = entry
            changed += 1
            # Flush periodically. In-process this guards against a plugin
            # killing the interpreter; supervised it costs little and keeps
            # a cancelled scan (Ctrl-C) from throwing away its work.
            if changed % 16 == 0:
                _save_raw(doc)
        if on_progress is not None:
            on_progress(i + 1, len(paths), path)
        if entry["status"] == "ok":
            results.append(entry["desc"])
        elif include_errors:
            results.append(
                {
                    "path": path,
                    "status": entry["status"],
                    "error": entry.get("error"),
                }
            )

    if changed:
        _save_raw(doc)
    return results


def info(path: str | Path, *, refresh: bool = False) -> dict:
    """Return cached probe metadata for one plugin, probing (and caching)
    on a cache miss or stale fingerprint. Raises RuntimeError if the plugin
    cannot be probed (the failure is also cached).

    Probing happens in this process: one named plugin is the caller's own
    choice, so a hang is visible and interruptible, unlike the same hang
    buried in a scan of several hundred. Use ``scan`` on its directory to
    get the supervised path.
    """
    abspath = os.path.abspath(str(path))
    doc = _load_raw()
    entries = doc["entries"]
    entry = entries.get(abspath)
    if refresh or entry is None or not _entry_fresh(entry, abspath):
        entry = _probe_to_entry(abspath)
        entries[abspath] = entry
        _save_raw(doc)
    if entry["status"] != "ok":
        raise RuntimeError(entry.get("error", "probe failed"))
    return entry["desc"]


def query(
    *,
    format: str | None = None,
    name_contains: str | None = None,
    vendor_contains: str | None = None,
    accepts_midi: bool | None = None,
    produces_midi: bool | None = None,
    min_inputs: int | None = None,
    min_outputs: int | None = None,
) -> list[dict]:
    """Query cached plugins (the whole index, across all scanned
    directories). All filters are ANDed; omitted filters match everything.
    Returns matching probe-metadata dicts sorted by name then path. Only
    successfully-probed (status ok) plugins are considered."""
    out: list[dict] = []
    for entry in _load_raw()["entries"].values():
        if entry.get("status") != "ok":
            continue
        d = entry.get("desc", {})
        if format is not None and d.get("format", "").lower() != format.lower():
            continue
        if (
            name_contains is not None
            and name_contains.lower() not in d.get("name", "").lower()
        ):
            continue
        if (
            vendor_contains is not None
            and vendor_contains.lower() not in d.get("vendor", "").lower()
        ):
            continue
        if accepts_midi is not None and bool(d.get("accepts_midi")) != accepts_midi:
            continue
        if produces_midi is not None and bool(d.get("produces_midi")) != produces_midi:
            continue
        if min_inputs is not None and int(d.get("num_inputs", 0)) < min_inputs:
            continue
        if min_outputs is not None and int(d.get("num_outputs", 0)) < min_outputs:
            continue
        out.append(d)
    out.sort(key=lambda x: (x.get("name", "").lower(), x.get("path", "")))
    return out


def all_entries(*, include_errors: bool = True) -> list[dict]:
    """Return every cached entry as ``{path, status, desc, error?}``."""
    out = []
    for path, entry in _load_raw()["entries"].items():
        if not include_errors and entry.get("status") != "ok":
            continue
        out.append(
            {
                "path": path,
                "status": entry.get("status"),
                "desc": entry.get("desc", {}),
                "error": entry.get("error"),
            }
        )
    out.sort(key=lambda x: x["path"])
    return out


def prune() -> int:
    """Drop cache entries whose plugin path no longer exists on disk.
    Returns the number removed."""
    doc = _load_raw()
    entries = doc["entries"]
    gone = [p for p in entries if not os.path.exists(p)]
    for p in gone:
        del entries[p]
    if gone:
        _save_raw(doc)
    return len(gone)


def clear() -> None:
    """Delete the cache file entirely."""
    f = cache_file()
    try:
        f.unlink()
    except FileNotFoundError:
        pass


def stats() -> dict:
    """Summary counts plus the resolved cache-file path."""
    entries = _load_raw()["entries"]
    ok = sum(1 for e in entries.values() if e.get("status") == "ok")
    err = sum(1 for e in entries.values() if e.get("status") == "error")
    return {
        "path": str(cache_file()),
        "exists": cache_file().exists(),
        "total": len(entries),
        "ok": ok,
        "error": err,
    }
