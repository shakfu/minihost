"""Closing an object another one still uses, and closing mid-call.

Chains, buses, graphs and devices held a raw pointer to the native plugin or
chain they were built on, and ``close()`` freed it regardless. Afterwards the
container processed freed memory: in one run a chain whose plugin was closed
processed through an unrelated plugin that reused the allocation. ``close()``
from another thread during a GIL-released ``process`` freed the object
mid-call the same way.

Now the native object is shared: ``close()`` drops only the caller's
reference, and a per-object lock makes it wait for a native call in flight.
Each scenario runs in its own interpreter -- under Guard Malloc on macOS,
which turns a use-after-free into a deterministic crash -- so a regression
fails one test instead of taking down the suite.
"""

from __future__ import annotations

import os
import subprocess
import sys
import textwrap

import pytest

from cli_helpers import find_test_plugin

FX = find_test_plugin("MinihostTestFx", "MINIHOST_TEST_PLUGIN_FX")

requires_fx = pytest.mark.skipif(
    not FX or not os.path.exists(FX),
    reason="MinihostTestFx not built; set MINIHOST_TEST_PLUGIN_FX or build "
    "with -DMINIHOST_BUILD_TEST_PLUGIN=ON",
)

GUARD_MALLOC = "/usr/lib/libgmalloc.dylib"

PRELUDE = f"""
import threading, random, time
import numpy as np
import minihost
FX = {FX!r}
def fx(gain=1.0):
    p = minihost.Plugin(FX, sample_rate=48000.0, max_block_size=512)
    p.set_param(0, gain)
    return p
x = np.ones((2, 512), np.float32)
y = np.zeros_like(x)
"""


def run_isolated(body: str) -> str:
    env = dict(os.environ)
    if sys.platform == "darwin" and os.path.exists(GUARD_MALLOC):
        env["DYLD_INSERT_LIBRARIES"] = GUARD_MALLOC
    proc = subprocess.run(
        [sys.executable, "-c", PRELUDE + textwrap.dedent(body)],
        capture_output=True,
        text=True,
        timeout=300,
        env=env,
    )
    assert proc.returncode == 0, (
        f"exit {proc.returncode}\nstdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
    )
    return proc.stdout


@requires_fx
def test_chain_keeps_its_plugin_after_plugin_close():
    # The freed allocation used to be reused by q, and the chain processed
    # through q (output 0.25) -- a plugin it was never given.
    out = run_isolated("""
        p = fx(1.0)
        c = minihost.PluginChain([p])
        p.close()
        q = fx(0.25)
        c.process(x, y)
        print(y[0, 0], c.latency_samples)
    """)
    assert out.split()[0] == "1.0"


@requires_fx
def test_graph_keeps_its_plugin_after_plugin_close():
    out = run_isolated("""
        p = fx(1.0)
        g = minihost.PluginGraph(512, 48000.0)
        a = g.add_input(2); n = g.add_plugin(p); o = g.add_output(2)
        g.connect(a, n); g.connect(n, o); g.compile()
        p.close()
        g.set_node_automation(n, [(0, 0, 0.5)])
        g.render_block([x], [y], 512)
        print(y[0, 0])
    """)
    assert out.strip() == "0.5"


@requires_fx
def test_bus_keeps_its_chain_after_chain_and_plugin_close():
    out = run_isolated("""
        p = fx(1.0)
        c = minihost.PluginChain([p])
        b = minihost.PluginBus(2, 2, 512, 48000.0)
        b.add_branch(c, 1.0)
        c.close(); p.close()
        b.process(x, y)
        print(y[0, 0], b.latency_samples)
    """)
    assert out.split()[0] == "1.0"


@requires_fx
def test_closed_plugin_cannot_join_a_container():
    out = run_isolated("""
        p = fx(); p.close()
        for make in (lambda: minihost.PluginChain([p]),
                     lambda: minihost.PluginGraph(512, 48000.0).add_plugin(p)):
            try:
                make(); print("accepted")
            except RuntimeError as e:
                print("closed" if "closed" in str(e) else e)
    """)
    assert out.split() == ["closed", "closed"]


@requires_fx
@pytest.mark.parametrize("kind", ["plugin", "chain", "graph"])
def test_close_during_process_on_another_thread(kind):
    # process/render release the GIL; close() used to free the object while
    # the native call was still running in it.
    out = run_isolated(f"""
        kind = {kind!r}
        for _ in range(60):
            p = fx()
            if kind == "plugin":
                o, call = p, (lambda: p.process(x, y))
            elif kind == "chain":
                o = minihost.PluginChain([p]); call = lambda: o.process(x, y)
            else:
                o = minihost.PluginGraph(512, 48000.0)
                a = o.add_input(2); n = o.add_plugin(p); out_ = o.add_output(2)
                o.connect(a, n); o.connect(n, out_); o.compile()
                call = lambda: o.render_block([x], [y], 512)
            stop = [False]
            def loop():
                while not stop[0]:
                    try:
                        call()
                    except Exception:
                        break
            t = threading.Thread(target=loop); t.start()
            time.sleep(random.random() * 0.005)
            o.close(); stop[0] = True; t.join()
            p.close()
        print("survived")
    """)
    assert out.strip() == "survived"


@requires_fx
def test_closed_session_refuses_to_open():
    out = run_isolated("""
        s = minihost.Session(); s.close()
        try:
            s.open(FX, 48000.0, 512); print("opened")
        except RuntimeError as e:
            print(e)
    """)
    assert "Session is closed" in out


@requires_fx
def test_closed_plugin_raises_instead_of_answering_defaults():
    # The C calls return defaults on a null plugin, so a closed Plugin used to
    # answer get_state() -> b'' (a save that silently lost the state),
    # get_param -> 0.0, and a process() error naming a garbage channel count
    # read from an uninitialised MH_Info.
    import numpy as np

    import minihost

    p = minihost.Plugin(FX, sample_rate=48000.0, max_block_size=512)
    p.close()
    x = np.zeros((2, 64), np.float32)
    calls = {
        "get_state": lambda: p.get_state(),
        "get_param": lambda: p.get_param(0),
        "sample_rate": lambda: p.sample_rate,
        "num_params": lambda: p.num_params,
        "latency_samples": lambda: p.latency_samples,
        "non_realtime": lambda: p.non_realtime,
        "process": lambda: p.process(x, np.zeros_like(x)),
        "process_midi": lambda: p.process_midi(x, np.zeros_like(x), []),
        "set_transport": lambda: p.set_transport(bpm=120.0),
        "bypass": lambda: setattr(p, "bypass", True),
        "set_param_value_callback": lambda: p.set_param_value_callback(None),
    }
    for name, call in calls.items():
        with pytest.raises(RuntimeError, match="Plugin is closed"):
            call()
    # The wrapper's own queue and close() stay usable.
    assert p.poll_callbacks() == 0
    p.close()


@requires_fx
def test_state_size_and_data_come_from_one_snapshot():
    # mh_get_state_size and mh_get_state each serialised the state: twice the
    # cost, and a change between the calls could make the second dump outgrow
    # the buffer sized by the first. The data now is the snapshot the size
    # call took. Driven through the C API, since Python makes both calls at once.
    out = run_isolated("""
        import ctypes
        import minihost._core as core
        L = ctypes.CDLL(core.__file__)
        L.mh_open.restype = ctypes.c_void_p
        L.mh_open.argtypes = [ctypes.c_char_p, ctypes.c_double, ctypes.c_int,
                              ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
        for name, args in [("mh_get_state_size", [ctypes.c_void_p]),
                           ("mh_get_state", [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]),
                           ("mh_set_param", [ctypes.c_void_p, ctypes.c_int, ctypes.c_float]),
                           ("mh_close", [ctypes.c_void_p])]:
            getattr(L, name).argtypes = args
        err = ctypes.create_string_buffer(512)
        h = L.mh_open(FX.encode(), 48000.0, 512, 2, 2, err, 512)
        L.mh_set_param(h, 0, 0.3)
        size = L.mh_get_state_size(h)
        L.mh_set_param(h, 0, 0.8)  # after the size call
        buf = ctypes.create_string_buffer(size)
        assert L.mh_get_state(h, buf, size)
        snapshot = buf.raw
        fresh = ctypes.create_string_buffer(size * 2)
        assert L.mh_get_state(h, fresh, size * 2)  # no snapshot pending: now
        L.mh_close(h)
        p = fx(); p.set_state(snapshot); print(round(p.get_param(0), 3))
        p.set_state(fresh.raw[:len(p.get_state())]); print(round(p.get_param(0), 3))
    """)
    assert out.split() == ["0.3", "0.8"]
