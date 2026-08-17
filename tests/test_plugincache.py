"""Persistent plugin-scan cache.

These tests use synthetic plugin files and a synthetic probe, so they need
no real plugins. The cache file is redirected to a tmp dir via
MINIHOST_CACHE_DIR. The central guarantee under test: a plugin is probed
once, then served from cache until its fingerprint changes.

Scanning probes in a child process by default, which a monkeypatched probe
in this process would never reach -- so the fake answers come from
``tests/fake_probe.py``, used both as the in-process probe and, via
MINIHOST_SCAN_WORKER, as the scan worker. The cache semantics below are
therefore exercised through the path that actually ships, and probes are
counted through a log file rather than a list, since some of them happen
in another process.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

import fake_probe
from minihost import plugincache


class ProbeLog:
    """The probes recorded so far, wherever they happened.

    Reads the log file on every access, because the supervised path writes
    it from child processes -- a list in this process would stay empty.
    """

    def __init__(self, path: Path):
        self._path = path

    def _entries(self) -> list[str]:
        if not self._path.exists():
            return []
        return self._path.read_text().split()

    def __len__(self) -> int:
        return len(self._entries())

    def __getitem__(self, i):
        return self._entries()[i]

    def __iter__(self):
        return iter(self._entries())

    def __contains__(self, item) -> bool:
        return item in self._entries()


@pytest.fixture
def cache_env(tmp_path, monkeypatch):
    """Isolate the cache file and answer probes synthetically, whether they
    happen here or in a scan worker. Returns (plugins_dir, probe_log)."""
    monkeypatch.setenv("MINIHOST_CACHE_DIR", str(tmp_path / "cache"))

    log = tmp_path / "probes.log"
    monkeypatch.setenv("MINIHOST_PROBE_LOG", str(log))

    # In-process probing (info(), and scan(supervised=False)).
    monkeypatch.setattr(plugincache, "_probe", fake_probe.probe)
    # Supervised probing: the same answers, from a process of its own.
    worker = Path(fake_probe.__file__).resolve()
    # Quoted: both parsers are quote-aware, and a Windows temp path or
    # interpreter can sit under "C:\\Program Files".
    monkeypatch.setenv("MINIHOST_SCAN_WORKER", f'"{sys.executable}" "{worker}"')
    monkeypatch.setenv("MINIHOST_SCAN_TIMEOUT_MS", "5000")

    plugins = tmp_path / "plugins"
    plugins.mkdir()
    return plugins, ProbeLog(log)


def _touch_plugin(directory, name: str) -> str:
    p = directory / name
    p.write_text("x")  # a plain file is a valid "leaf" plugin for discovery
    return str(p)


# -- discovery -------------------------------------------------------- #


def test_discovery_finds_extensions_and_treats_bundles_as_leaves(cache_env):
    plugins, _ = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "fxB.component")
    (plugins / "notes.txt").write_text("ignore me")
    # A bundle directory must be a leaf: nested plugin-like files inside it
    # are NOT discovered separately.
    bundle = plugins / "C.vst3"
    (bundle / "Contents").mkdir(parents=True)
    (bundle / "Contents" / "inner.so").write_text("x")
    # A nested plain subdirectory IS descended into.
    sub = plugins / "more"
    sub.mkdir()
    _touch_plugin(sub, "deepD.vst3")

    found = plugincache._discover_plugins(str(plugins))
    names = sorted(os.path.basename(p) for p in found)
    assert names == ["C.vst3", "deepD.vst3", "fxB.component", "synthA.vst3"]


# -- caching behaviour ------------------------------------------------ #


def test_scan_probes_once_then_serves_from_cache(cache_env):
    plugins, calls = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "fxB.vst3")

    first = plugincache.scan(plugins)
    assert {d["name"] for d in first} == {"synthA", "fxB"}
    assert len(calls) == 2

    # Second scan: nothing changed -> zero probes.
    second = plugincache.scan(plugins)
    assert {d["name"] for d in second} == {"synthA", "fxB"}
    assert len(calls) == 2  # unchanged


def test_changed_fingerprint_reprobes_only_that_plugin(cache_env):
    plugins, calls = cache_env
    a = _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "fxB.vst3")
    plugincache.scan(plugins)
    assert len(calls) == 2

    # Modify one plugin's size/mtime -> only it is re-probed.
    with open(a, "w") as f:
        f.write("changed content (different size)")
    plugincache.scan(plugins)
    assert len(calls) == 3
    assert calls[-1] == os.path.abspath(a)


def test_refresh_reprobes_everything(cache_env):
    plugins, calls = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "fxB.vst3")
    plugincache.scan(plugins)
    assert len(calls) == 2
    plugincache.scan(plugins, refresh=True)
    assert len(calls) == 4


def test_new_plugin_added_is_probed_on_next_scan(cache_env):
    plugins, calls = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    plugincache.scan(plugins)
    assert len(calls) == 1
    _touch_plugin(plugins, "fxB.vst3")
    res = plugincache.scan(plugins)
    assert len(calls) == 2
    assert {d["name"] for d in res} == {"synthA", "fxB"}


# -- error caching ---------------------------------------------------- #


def test_probe_failure_is_cached_and_not_retried(cache_env):
    plugins, calls = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "brokenX.vst3")

    res = plugincache.scan(plugins)
    # Broken plugin is excluded from default results.
    assert {d["name"] for d in res} == {"synthA"}
    assert len(calls) == 2

    # Re-scan: the failure is cached -> not retried.
    res2 = plugincache.scan(plugins, include_errors=True)
    assert len(calls) == 2
    statuses = {r.get("status") for r in res2}
    assert "error" in statuses


# -- single info() ---------------------------------------------------- #


def test_info_caches_single_plugin(cache_env):
    plugins, calls = cache_env
    a = _touch_plugin(plugins, "synthA.vst3")
    d1 = plugincache.info(a)
    d2 = plugincache.info(a)
    assert d1["name"] == "synthA"
    assert d2 == d1
    assert len(calls) == 1  # second call served from cache


def test_info_raises_and_caches_error(cache_env):
    plugins, calls = cache_env
    b = _touch_plugin(plugins, "brokenX.vst3")
    with pytest.raises(RuntimeError, match="cannot probe"):
        plugincache.info(b)
    with pytest.raises(RuntimeError):
        plugincache.info(b)
    assert len(calls) == 1  # error cached, not retried


# -- query ------------------------------------------------------------ #


def test_query_filters(cache_env):
    plugins, _ = cache_env
    _touch_plugin(plugins, "synthA.vst3")  # accepts_midi, vendor Acme, VST3
    _touch_plugin(plugins, "fxB.component")  # AU, vendor Other
    _touch_plugin(plugins, "arpC.vst3")  # produces_midi
    plugincache.scan(plugins)

    assert {d["name"] for d in plugincache.query(format="VST3")} == {"synthA", "arpC"}
    assert {d["name"] for d in plugincache.query(format="AudioUnit")} == {"fxB"}
    assert {d["name"] for d in plugincache.query(accepts_midi=True)} == {"synthA"}
    assert {d["name"] for d in plugincache.query(produces_midi=True)} == {"arpC"}
    assert {d["name"] for d in plugincache.query(vendor_contains="acme")} == {"synthA"}
    assert {d["name"] for d in plugincache.query(name_contains="arp")} == {"arpC"}
    # Instruments (no audio inputs).
    assert {d["name"] for d in plugincache.query(min_outputs=2)} == {
        "synthA",
        "fxB",
        "arpC",
    }


# -- management ------------------------------------------------------- #


def test_prune_removes_missing(cache_env):
    plugins, _ = cache_env
    a = _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "fxB.vst3")
    plugincache.scan(plugins)
    assert plugincache.stats()["total"] == 2

    os.remove(a)
    removed = plugincache.prune()
    assert removed == 1
    assert plugincache.stats()["total"] == 1
    assert {d["name"] for d in plugincache.query()} == {"fxB"}


def test_clear_and_stats(cache_env):
    plugins, _ = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "brokenX.vst3")
    plugincache.scan(plugins)

    s = plugincache.stats()
    assert s["total"] == 2 and s["ok"] == 1 and s["error"] == 1 and s["exists"]

    plugincache.clear()
    assert plugincache.stats()["total"] == 0
    assert not plugincache.cache_file().exists()


def test_corrupt_cache_file_is_ignored(cache_env):
    plugins, calls = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    plugincache.scan(plugins)
    # Corrupt the JSON; the cache must degrade to empty, not crash.
    plugincache.cache_file().write_text("{ not valid json ]")
    res = plugincache.scan(plugins)  # re-probes since cache unreadable
    assert {d["name"] for d in res} == {"synthA"}


# -- supervised probing ------------------------------------------------ #
#
# The reason scanning spawns a process per plugin: probing means loading,
# and an installed collection can be relied on to contain a plugin that
# hangs or crashes on load. These two outcomes exist only on that path --
# in process they are not outcomes, they are the end of the scan (and of
# the interpreter) -- so they also pin that the default really is
# supervised, which the other tests cannot distinguish.


def test_a_crashing_plugin_costs_one_entry(cache_env):
    plugins, _ = cache_env
    _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "crashX.vst3")  # the worker aborts on this one
    _touch_plugin(plugins, "synthB.vst3")

    results = plugincache.scan(plugins)

    assert {d["name"] for d in results} == {"synthA", "synthB"}
    entries = plugincache.all_entries(include_errors=True)
    crashed = next(e for e in entries if "crashX" in e["path"])
    assert crashed["status"] == "crash"


def test_a_hanging_plugin_costs_one_entry(cache_env, monkeypatch):
    plugins, _ = cache_env
    monkeypatch.setenv("MINIHOST_SCAN_TIMEOUT_MS", "700")
    _touch_plugin(plugins, "synthA.vst3")
    _touch_plugin(plugins, "hangX.vst3")  # the worker sleeps for ten minutes

    results = plugincache.scan(plugins)

    assert {d["name"] for d in results} == {"synthA"}
    entries = plugincache.all_entries(include_errors=True)
    hung = next(e for e in entries if "hangX" in e["path"])
    assert hung["status"] == "timeout"


def test_bad_plugins_are_not_re_probed(cache_env, monkeypatch):
    plugins, calls = cache_env
    monkeypatch.setenv("MINIHOST_SCAN_TIMEOUT_MS", "700")
    _touch_plugin(plugins, "hangX.vst3")
    _touch_plugin(plugins, "crashX.vst3")

    plugincache.scan(plugins)
    after_first = len(calls)

    plugincache.scan(plugins)

    assert len(calls) == after_first  # both remembered, neither retried


def test_in_process_scan_bypasses_the_worker(cache_env):
    plugins, _ = cache_env
    _touch_plugin(plugins, "crashX.vst3")

    # In this process the name means nothing: the synthetic probe answers
    # normally, which is how we know the worker was not consulted.
    results = plugincache.scan(plugins, supervised=False)

    assert {d["name"] for d in results} == {"crashX"}


# -- worker command parsing -------------------------------------------- #
#
# MINIHOST_SCAN_WORKER is a command line, and the first version of this
# split it with POSIX rules everywhere. On Windows that reads a backslash
# as an escape, so C:\Users\me\python.exe arrived as C:Usersmepython.exe
# and every scan failed with "cannot find the file specified". Both modes
# are checked here, on whatever platform is running, because the one that
# broke is the one this machine does not use.


@pytest.mark.parametrize(
    "command, expected",
    [
        (
            r"C:\Users\me\python.exe C:\tmp\worker.py",
            [r"C:\Users\me\python.exe", r"C:\tmp\worker.py"],
        ),
        (
            r'"C:\Program Files\Py\python.exe" C:\tmp\worker.py',
            [r"C:\Program Files\Py\python.exe", r"C:\tmp\worker.py"],
        ),
        (
            r"python.exe -m minihost._scan_worker",
            ["python.exe", "-m", "minihost._scan_worker"],
        ),
    ],
)
def test_windows_command_split_keeps_backslashes(command, expected):
    assert plugincache._split_command(command, windows=True) == expected


@pytest.mark.parametrize(
    "command, expected",
    [
        (
            "/usr/bin/python3 -m minihost._scan_worker",
            ["/usr/bin/python3", "-m", "minihost._scan_worker"],
        ),
        (
            "'/opt/py 3.13/bin/python3' /tmp/worker.py",
            ["/opt/py 3.13/bin/python3", "/tmp/worker.py"],
        ),
    ],
)
def test_posix_command_split(command, expected):
    assert plugincache._split_command(command, windows=False) == expected
