"""Persistent plugin-scan cache.

These tests use synthetic plugin files and a monkeypatched probe, so they
need no real plugins. The cache file is redirected to a tmp dir via
MINIHOST_CACHE_DIR. The central guarantee under test: a plugin is probed
once, then served from cache until its fingerprint changes.
"""

from __future__ import annotations

import os

import pytest

from minihost import plugincache


@pytest.fixture
def cache_env(tmp_path, monkeypatch):
    """Isolate the cache file and replace the real probe with a synthetic,
    call-counting one. Returns (plugins_dir, probe_calls)."""
    monkeypatch.setenv("MINIHOST_CACHE_DIR", str(tmp_path / "cache"))
    probe_calls: list[str] = []

    def fake_probe(path: str) -> dict:
        probe_calls.append(path)
        name = os.path.splitext(os.path.basename(path))[0]
        ext = os.path.splitext(path)[1].lower()
        fmt = {".vst3": "VST3", ".component": "AudioUnit", ".lv2": "LV2"}.get(
            ext, "VST3"
        )
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

    monkeypatch.setattr(plugincache, "_probe", fake_probe)

    plugins = tmp_path / "plugins"
    plugins.mkdir()
    return plugins, probe_calls


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
    _touch_plugin(plugins, "synthA.vst3")     # accepts_midi, vendor Acme, VST3
    _touch_plugin(plugins, "fxB.component")   # AU, vendor Other
    _touch_plugin(plugins, "arpC.vst3")       # produces_midi
    plugincache.scan(plugins)

    assert {d["name"] for d in plugincache.query(format="VST3")} == {"synthA", "arpC"}
    assert {d["name"] for d in plugincache.query(format="AudioUnit")} == {"fxB"}
    assert {d["name"] for d in plugincache.query(accepts_midi=True)} == {"synthA"}
    assert {d["name"] for d in plugincache.query(produces_midi=True)} == {"arpC"}
    assert {d["name"] for d in plugincache.query(vendor_contains="acme")} == {"synthA"}
    assert {d["name"] for d in plugincache.query(name_contains="arp")} == {"arpC"}
    # Instruments (no audio inputs).
    assert {d["name"] for d in plugincache.query(min_outputs=2)} == {
        "synthA", "fxB", "arpC"
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
