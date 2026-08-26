# OSC support and generated touch surfaces

Status: plan, nothing implemented. Companion to
[desktop_app.md](desktop_app.md) in intent: a design that gets committed
before the code, so the ordering decisions are arguable in review rather
than discovered halfway through.

Goal: minihost speaks OSC natively, MIDI CC gains 14-bit resolution, and a
script turns any plugin into a tablet control surface bound to one or both.


## 1. What already works

Worth establishing first, because the gap is smaller than it looks and two
of the pieces are already shipping.

`minihost params <plugin> --json` (`src/minihost/cli.py:266`) emits a list
of objects carrying `name`, `index`, `label`, `default_value`, `num_steps`,
`is_boolean`, `is_automatable`, `category`.

The sibling project `py2tosc` has a `surface` module -- a `Parameter`
dataclass, a `read()` that accepts exactly that JSON shape, and a `build()`
that lays parameters out across paged TouchOSC controls bound to MIDI CC,
to OSC, or to both. It is already exposed as `py2tosc build params.json`.
`py2tosc/src/py2tosc/surface.py:111` even documents the seam: "A plugin host
exports an `index` alongside each name."

So this pipes together today:

```console
$ minihost params ~/Library/Audio/Plug-Ins/VST3/Synth.vst3 --json > params.json
$ py2tosc build params.json --output synth.tosc
```

That is the flat path, and it stays useful as the zero-configuration one.
py2tosc also has a second, richer entry point that Phase 6 targets instead:
`py2tosc.ui_json`, a JSON dialect over the `ui` combinators, with `each` for
walking a table of rows. Section 4, Phase 6 makes the case for generating
that rather than the flat list.

What the flat path does not give you:

- Every control is a fader. The `is_boolean` / `num_steps` / `label`
  metadata minihost already exports is discarded, so a bypass toggle and a
  16-position waveform selector both render as continuous faders.
- No matching `--map-file`, so the generated CC assignments and the host's
  mapping are two hand-maintained lists that silently drift.
- MIDI CC only. `surface.build(osc=True)` writes OSC addresses into the
  layout, but nothing in minihost listens on a UDP port, so those addresses
  go nowhere.
- 7-bit resolution on the path that does work.
- Nothing for transport.

The plan below closes those five gaps. Framing the work as "add OSC" alone
would miss that most of the remaining cost is in the parameter delivery path
and in transport, not in the OSC codec.


## 2. Decision: juce_osc, not liblo

The proposal was to embed liblo. Recommend against, and the reason is not
a close call: OSC is already in the tree.

`thirdparty/JUCE/modules/juce_osc/` ships with the JUCE 8.0.12 that
`scripts/download_juce.py` already downloads. Its module declaration reads:

```
license:            AGPLv3/Commercial
minimumCppStandard: 17
dependencies:       juce_events
```

Every point of comparison favours it:

| | juce_osc | liblo |
|---|---|---|
| New vendored code | none | ~10k lines into `projects/` |
| `docs/vendored.md` | untouched | new row |
| License posture | identical to `juce_core`, `juce_events`, `juce_audio_processors_headless`, all already linked | LGPL-2.1+, the first non-permissive entry among MIT-0 / BSD-0 / BSD-2 |
| Build | one `target_link_libraries` line | autotools-generated `config.h`, or its less-exercised CMake path |
| Windows | JUCE's own tested socket layer | winsock path, historically the weak platform; minihost ships Windows wheels |
| C++ standard | 17, matches `libminihost` | C89 + pthreads |
| New transitive deps | none -- `juce_audio_processors_headless` already depends on `juce_events` | pthreads shim on Windows |

The "no dependencies" rule is satisfied more literally by juce_osc than by
any vendoring option, since it adds no dependency at all.

Two properties of `juce_osc` matter to the design and were verified by
reading the source rather than assumed:

- `OSCReceiver::Pimpl` is its own `juce::Thread`
  (`juce_OSCReceiver.cpp:325`). Listeners registered with the
  `RealtimeCallback` template parameter are invoked directly on that socket
  thread; only `MessageLoopCallback` listeners go through `postMessage`
  (`juce_OSCReceiver.cpp:438-448`). The hot path therefore needs no JUCE
  message thread, which matters for the Python wheel.
- `OSCAddressPattern::matches` (`juce_OSCAddress.h:138`) gives wildcard
  address matching, so `/mh/param/*` dispatch is free.

And one limitation to state honestly: bundle time tags are parsed
(`juce_OSCReceiver.cpp:141`) but not used for scheduled dispatch -- bundle
contents are delivered immediately. Irrelevant for control surfaces,
disqualifying if minihost ever wants OSC-scheduled events. Also UDP only:
no TCP, no SLIP, so a serial OSC device is not served.

Rejected alternatives, for the record:

- **liblo**, per the table.
- **tinyosc** (MIT, single file). Fits the vendoring aesthetic but is codec
  only -- we would still write the UDP layer and receive thread, which is
  the part juce_osc already has.
- **Hand-rolled codec.** OSC 1.0 encoding is genuinely simple, maybe 400
  lines with tests. Worth keeping as the escape hatch if a pure-C consumer
  ever needs OSC without linking JUCE, but it reinvents working code sitting
  in the tree.


## 3. The real cost centre: parameter delivery

This is the part that is not about OSC at all, and it should be built first.

Today `MidiMapper.__call__` (`src/minihost/control.py:259`) reaches a plugin
parameter by calling `plugin.set_param`, which lands in `mh_set_param`
(`projects/libminihost/minihost.cpp:717`):

```cpp
std::lock_guard<std::mutex> lock(p->stateMutex);
params.getUnchecked(index)->setValueNotifyingHost(normalized_0_1);
```

Meanwhile the live audio callback (`projects/libminihost_audio/minihost_audio.c:326`)
calls `mh_process_midi_io` / `mh_chain_process_midi_io`, which per the
header contract take no lock at all. So a control-thread parameter write
runs concurrently with `processBlock`, ordered by nothing, applied at
whatever moment the plugin next reads the value. Three consequences:

1. Not sample-accurate. Every write is effectively block-quantised, with
   jitter.
2. The mapper thread takes a mutex that offline callers also take, so
   `set_param` from a control surface can block behind a `morph` or a
   preset load.
3. It scales badly with rate, which is exactly what OSC and 14-bit CC
   introduce. A fader drag at 14-bit resolution emits far more messages
   than a 7-bit one.

`mh_process_auto` (`projects/libminihost/minihost.h:428`) and
`mh_chain_process_auto` already exist and already split a block at each
`MH_ParamChange` offset. The live path simply never uses them.

**Phase 0** is therefore: give `MH_AudioDevice` a parameter ring, drain it in
the audio callback, and switch the callback to the `_auto` entry points.

- New `projects/libminihost_audio/param_ringbuffer.{h,cpp}`, an SPSC ring
  over `MH_ParamChange`, modelled directly on `midi_ringbuffer.{h,cpp}`.
- **Two** rings on the device, not one: one fed by the control-input thread
  (OSC socket thread or MIDI callback), one fed by programmatic
  `mh_audio_send_param`. This mirrors the `midi_in_buffer` /
  `midi_send_buffer` split, and the reason is recorded in the header comment
  at `minihost_audio.h:161`: sharing one SPSC ring between two producers
  corrupted its indices and lost events. Do not relearn that.
- **Coalesce on drain, last-wins per parameter index.** This is not an
  optimisation, it is a correctness requirement. `mh_process_auto` splits
  the block at every distinct offset; 200 messages for one fader inside one
  block would become 200 sub-blocks and blow the audio deadline. Collapse to
  one change per parameter per block, and cap the total distinct split
  points per block (start at 32) with the overflow folded into the last
  offset.
- Sample offset: v1 assigns 0, i.e. block start. Time-stamping arrival and
  placing the change inside the block is a later refinement -- it buys
  sub-block accuracy at the cost of jitter and a one-block safety delay, and
  should not gate the rest of the work.
- New C API `int mh_audio_send_param(MH_AudioDevice*, int param_index, float value)`,
  Python `AudioDevice.send_param(index, value)`.
- Redirect `MidiMapper` at the ring when an `AudioDevice` is attached,
  falling back to `set_param` when it is not (offline use).

Scope boundary: the device opens a plugin or a chain, never a graph
(`mh_audio_open` / `mh_audio_open_chain`), so `graph_v2` needs nothing here.
It already has `mh_graph_set_node_automation`.

Tests: extend `tests/tsan/ringbuffer_stress.cpp` and the `make tsan` target
to cover the new ring; a Python test asserting coalescing (N writes to one
param inside one block produce one change) and that the `_auto` path is
reached.


## 4. Phases

Phase 0 is a prerequisite for 1, 3 and 5. The rest are independent enough to
reorder.

### Phase 0 -- parameter ring and `_auto` in the live path

As above. No user-visible feature; everything after it is cheaper and
correct. Also fixes the existing 7-bit MIDI mapping path.

### Phase 1 -- OSC transport layer

`projects/libminihost_audio/minihost_osc.{h,cpp}`, sitting beside
`minihost_midi.{h,cpp}` because it is the same kind of thing: an I/O
back-end for the audio layer.

C API, deliberately shaped like the MIDI one:

```c
typedef struct MH_OscServer MH_OscServer;
typedef struct MH_OscClient MH_OscClient;

// Called on the OSC socket thread. Must not block.
typedef void (*MH_OscCallback)(const char* address,
                               const float* args, int num_args,
                               void* user_data);

MH_OscServer* mh_osc_server_open(int port, MH_OscCallback cb, void* user_data,
                                 char* err_buf, size_t err_buf_size);
void          mh_osc_server_close(MH_OscServer* s);

MH_OscClient* mh_osc_client_open(const char* host, int port,
                                 char* err_buf, size_t err_buf_size);
void          mh_osc_client_close(MH_OscClient* c);
int           mh_osc_send_float(MH_OscClient* c, const char* address, float value);
int           mh_osc_send_int(MH_OscClient* c, const char* address, int value);
int           mh_osc_send_string(MH_OscClient* c, const char* address, const char* value);
```

Implementation is a thin `OSCReceiver` / `OSCSender` wrapper registering a
`Listener<RealtimeCallback>`. CMake: add `juce::juce_osc` to both flavours
in `_minihost_audio_configure_common`.

Device integration, mirroring `mh_audio_connect_midi_input`:

```c
int mh_audio_connect_osc(MH_AudioDevice* dev, int port);
int mh_audio_disconnect_osc(MH_AudioDevice* dev);
int mh_audio_set_osc_feedback(MH_AudioDevice* dev, const char* host, int port);
```

Address scheme. Needs to answer "which plugin" for chains:

```
/mh/param/<slug>            f    single plugin, by name slug
/mh/param/<index>           f    single plugin, by index
/mh/<slot>/param/<slug>     f    chain slot
/mh/transport/...                see Phase 5
```

`py2tosc.surface.slug` (`surface.py:76`) already produces OSC-safe names
with the same rules, so both ends agree on the spelling by construction --
which is the argument for using it rather than inventing a second slugger.

Python: `minihost.OscServer` / `minihost.OscClient` in `_core.cpp`,
context-manager shaped like `MidiIn`.

### Phase 2 -- OSC mapping, unified with MIDI mapping

`src/minihost/control.py` grows an `OscMapper` alongside `MidiMapper`, and
both should share a resolution/curve/range core rather than duplicating
`_CCMapping.normalize`.

`OscMapper` differences from `MidiMapper`:

- Input is float32 already normalised 0..1, so no 7-bit quantisation. This
  is the whole point: `_CCMapping.normalize` divides by 127.0, giving 128
  steps. A filter cutoff on 128 steps is audibly stepped; OSC float is not.
- Address-to-parameter resolution, with wildcard support delegated to
  `OSCAddressPattern`.
- Auto-binding: `OscMapper.bind_all(prefix="/mh/param")` maps every
  automatable parameter by slug in one call, which is what a generated
  surface wants.

`minihost play` grows `--osc-port N`, `--osc-feedback host:port`, and the
`--map-file` schema grows an `"osc"` address key next to `"cc"`.

### Phase 3 -- 14-bit CC pairs

Per the MIDI spec, CC 0-31 carry the MSB and CC 32-63 carry the LSB for
controller n, i.e. the pair is `(n, n + 32)` for n in 0..31.

API: `MidiMapper.map_cc14(channel, cc, param, value_range=..., curve=...)`
where `cc` is the MSB number and must be 0..31.

Dispatch rule, chosen to work with both kinds of controller in the wild --
those that always send the full pair, and those that send LSB alone for a
fine adjustment:

- On MSB receipt: store it, reset the cached LSB to 0, emit `msb << 7`.
- On LSB receipt: emit `(cached_msb << 7) | lsb`.
- Divide by 16383.0 before the curve, not 127.0.

That produces a brief transient when a full pair arrives -- the value lands
at `msb << 7` and is corrected microseconds later by the LSB. Phase 0 makes
this invisible: both writes land in the same audio block and coalesce to the
second one. This is a concrete reason to keep the phase ordering.

Explicitly rejected: the ~50ms pairing window some implementations use. It
needs a timer thread and buys nothing once coalescing exists.

Validation that must not be forgotten:

- `map_cc14(ch, n)` must reject if `map_cc(ch, n + 32)` is already mapped,
  and `map_cc(ch, m)` for m in 32..63 must reject if `map_cc14(ch, m - 32)`
  exists. Silent shadowing here would be near-undebuggable.
- `cc` outside 0..31 is an error with a message that says why.

Map file: add `"cc14": 7` as an alternative key to `"cc": 7` rather than a
`"bits": 14` modifier -- one key, unambiguous parse, no invalid combination
to validate. `--map` spec gains a parallel `channel:cc14:param:...` form.

Non-goal for this phase: RPN/NRPN (CC 98/99/6/38). It is a different
mechanism, TouchOSC cannot emit it natively, and no touch surface needs it.

### Phase 4 -- feedback, host to surface

Without this a generated surface is write-only: load a preset and every
fader lies.

The mechanism already exists. `mh_set_param_value_callback`
(`projects/libminihost/minihost.h:562`) fires `MH_ParamValueCallback` on
plugin-initiated parameter changes, wired through `MH_Listener` at
`minihost.cpp:328`. Hook it, push into a ring, have a sender thread emit OSC.
No polling.

Two hazards to design around:

- **The callback thread is not ours.** `audioProcessorParameterChanged` fires
  on whatever thread changed the parameter, possibly the audio thread. The
  path from callback to socket must be lock-free and allocation-free: ring,
  then a sender thread drains it.
- **Echo.** `mh_set_param` calls `setValueNotifyingHost`, which fires the
  same callback -- so our own writes echo back to the surface that made
  them. Harmless when idle, but during a drag the echo fights the finger.
  Mitigation: tag the origin endpoint and suppress echo back to it for a
  short window, or drop echoes whose delta is below an epsilon. Pick one and
  write down which.

Rate-limit the feedback stream (30 Hz per parameter is plenty for a moving
fader) so a modulating parameter cannot saturate the link.

### Phase 5 -- transport

Larger than it looks, and worth calling out before anyone commits to it.

`mh_set_transport` (`projects/libminihost/minihost.h:407`) exists and offline
renders now use it -- that was the fix documented in
`tests/test_transport_advance.py`. But `grep -n transport
projects/libminihost_audio/minihost_audio.c` returns nothing: **the live
audio device has no playhead at all.** It never calls `mh_set_transport`, so
a tempo-synced delay running under `minihost play` sees no host tempo and a
playhead pinned at zero.

So "a transport touch surface" decomposes into two jobs, and only the second
is OSC:

1. Give `MH_AudioDevice` a host playhead: a sample counter advanced per
   callback, a settable BPM and time signature, play/stop state, optional
   loop points, pushed via `mh_set_transport` before each process call. This
   is worth doing on its own merits regardless of OSC -- it is a real gap in
   the live path.
2. Expose it over OSC:

```
/mh/transport/play      -> start
/mh/transport/stop      -> stop
/mh/transport/bpm       f
/mh/transport/position  f   (beats)
/mh/transport/loop      i
/mh/panic               -> all-notes-off on every channel
```

with feedback on `/mh/transport/position` so the surface can show a
playhead.

Recommend splitting job 1 into its own TODO entry under Tier 1 or 2
independent of this plan, and letting Phase 5 depend on it.

### Phase 6 -- surface generation

The user-facing deliverable: `minihost touch <plugin>` writes a layout and
the matching map file, and they agree by construction.

#### Target the `py2tosc.ui` JSON dialect, not the flat `surface` list

`py2tosc.surface.read` accepts a flat list of names -- one control per
entry, laid out four across and three down, every control a fader. It is the
zero-configuration path and it stays useful as one, but it is the wrong
generation target for anything richer, because the layout it produces is not
described anywhere the generator or the user can reach.

`py2tosc.ui_json` (`src/py2tosc/ui_json.py`) is the better target. It is a
JSON dialect over the `py2tosc.ui` combinators -- `row`, `column`, `tiles`,
`stack`, `grid`, `pager`, `labelled`, `inset` -- with `sizes`, `gap`, `pad`
and `frame` as arguments, so nesting and space division are described rather
than baked in. From `py2tosc/tests/data/mixer.ui.json`:

```json
{
  "format": "py2tosc.ui",
  "//": "eight channels over a bank of mutes, in a canvas divided three to one",
  "root": {
    "column": [
      {"row": [
        {"fader": "ch$i", "messages": [
          {"osc": "/mixer/{name}"},
          {"midi_cc": "$i0"}
        ], "repeat": 8}
      ], "gap": 4},
      {"grid": "BUTTON", "columns": 8, "rows": 2, "name": "mutes"}
    ],
    "sizes": [3, 1], "gap": 8, "pad": 8, "name": "mixer",
    "frame": [0, 0, 1024, 768]
  }
}
```

Four properties of the dialect earn it the job:

- **`each` is a parameter table.** `repeat` counts; `each` walks a list of
  records, binding every field of a row as `$field` the way `repeat` binds
  `$i`. Its docstring names the case exactly -- a layout "whose names and
  numbers follow no sequence" -- and permits an empty list, "since a list of
  nothing is what a generator with nothing to emit produces". A plugin's
  parameter list is precisely that.
- **A whole-string placeholder keeps its type** (`ui_json.py:321`): `"$cc"`
  substitutes the *number* 74, while `"cutoff $unit"` substitutes a string.
  So `{"midi_cc": "$cc"}` binds a real controller number from a row, with no
  string-to-int coercion anywhere in the format.
- **`//` keys are comments**, ignored wherever a key may appear. A generated
  layout can explain itself: which parameter index a control came from, why
  a CC number was skipped, what the plugin called it before slugging.
- **It is read but never written** -- there is no `to_ui_json`, because a
  resolved layout has frames and no memory of the `row` that placed them.
  That is exactly the right shape here: minihost is a producer, py2tosc is
  the compiler, and the direction never needs to reverse.

`py2tosc.load()` dispatches on the envelope's `format` field
(`document.py:294`), so a `.ui.json` is loadable and convertible with what
already exists:

```console
$ py2tosc convert synth.ui.json -o synth.tosc
```

#### What this buys, concretely

**The generation step loses its dependency.** minihost writes JSON text; it
imports nothing from py2tosc to do so. py2tosc is needed only to compile the
result into a `.tosc`. If it is not installed, `minihost touch` still
produces a complete, valid `.ui.json` and says how to compile it -- rather
than failing with an ImportError and producing nothing.

**The output is a source file, not an artefact.** A `.tosc` is a zipped XML
blob; nobody hand-edits one, so a generator that emits one directly owns
every layout decision forever. A `.ui.json` is reviewable, diffable, and
editable -- move a control, change a gap, add a page, then recompile. The
generator stops having to anticipate what anyone might want.

**Templates become user-supplied.** `minihost touch --template mine.ui.json`
takes a layout containing an `each` node with a marker, and injects the
parameter rows into it. The user owns the visual design; minihost owns only
the parameter table and the bindings. This is the feature that makes the
command worth having, and it falls out of the dialect rather than needing to
be built.

**The widget-metadata extension to `surface.Parameter` is no longer
needed.** The earlier draft of this plan proposed pushing `kind`, `steps`,
`unit` and `group` into py2tosc's `Parameter` dataclass so `surface.build`
could choose widgets. With ui-json, minihost chooses the widget itself by
emitting `{"button": ...}` or `{"radio": ...}` instead of `{"fader": ...}`.
No py2tosc change is required at all. Drop that work.

#### One consequence: `each` cannot vary the tag

Substitution reaches values, never keys. So a row can change what a control
is *called*, *bound to* and *numbered*, but not what it *is*: the tag stays
fixed in the template, and one `each` yields one widget kind. Plugin
parameters are mixed -- a bypass wants `button`, a waveform selector wants
`radio`, a cutoff wants `fader`.

Not a flaw. Keys carry the meaning (which tag, which property) and values
carry the data; letting a row rewrite keys would mean a node's type and
property set are unknown until expansion, so nothing could be validated
against the tag table beforehand. That is the checkability the dialect is
built on, and `{"$kind": "$name"}` would be worse to read than what it
replaced.

It does mean the generator picks one of:

1. **One `each` per widget kind.** Compact, but regroups by widget and
   discards the plugin's own parameter order.
2. **Explicit per-parameter nodes.** Verbose, exact, order-preserving.
3. **Hybrid**: `each` for homogeneous runs, explicit nodes for mixed pages.

Recommend (2). Verbosity costs nothing in a machine-written file, and
parameter order is information the plugin author chose. `each` stays the
right tool for hand-written templates, which is what `--template` injects
into and where its compactness actually pays.

Filed as a feature request against py2tosc rather than worked around here:
letting an `each` row select among fully-written branches would make
heterogeneous tables expressible without touching the key invariant. See
section 6, request 1. Phase 6 does not wait on it.

#### Work in minihost

`src/minihost/touch.py` plus a `cmd_touch` in `cli.py`:

- Build one parameter table from `MH_ParamInfo`: `is_boolean` to a
  `button`, `num_steps > 0` to a `radio` with that many positions, otherwise
  a `fader`; `label` to the caption unit; `category` to the page grouping;
  skip `is_automatable == 0` unless `--all`.
- Slug names with the same rules `py2tosc.surface.slug` uses
  (`surface.py:76`) so both ends spell an address identically. Reuse it when
  py2tosc is importable, mirror it when not, and test that the two agree --
  a divergence here is a silently dead address.
- Assign CC numbers and OSC addresses **once**, then render both the
  `.ui.json` and the map file from that single table. This is the whole
  reason to do it in minihost rather than piping two CLIs: the layout and
  the host mapping cannot disagree because they come from the same rows.
- Emit `"schema": 1` in the envelope and pin the extra accordingly (below).
- Annotate with `//` comments: parameter index per control, and a header
  noting the plugin, its version, and the minihost version that generated
  the file.
- Warn on CC exhaustion. There are 128 controller numbers and plugins
  routinely have more parameters; those spill to OSC-only. `surface.py:53`
  already does this silently, which for a 300-parameter plugin is a
  surprise rather than a decision.
- Optionally append a transport page (Phase 5) behind `--transport`.

```console
$ minihost touch Synth.vst3 --out synth --osc-port 9000 --feedback-port 9001
wrote synth.ui.json, synth.map.json
  128 parameters bound to MIDI CC and OSC
   47 parameters bound to OSC only (CC numbers exhausted)
   12 non-automatable parameters skipped (--all to include)
compiled synth.tosc

$ minihost play Synth.vst3 --map-file synth.map.json --osc-port 9000 \
      --osc-feedback 192.168.1.40:9001
```

Flags: `--template FILE`, `--midi-only` / `--osc-only`, `--size WxH`,
`--columns` / `--rows`, `--params` to select a subset by name or index
range, `--no-compile` to stop at the `.ui.json`.

#### Dependency handling and schema tracking

`py2tosc` has `dependencies = []` itself, so it enters as an optional extra
and `pyproject.toml`'s core `dependencies = []` is untouched:

```toml
[project.optional-dependencies]
touch = ["py2tosc >= 0.5.0"]
```

A floor, not a minor pin. `ui_json` versions itself: the envelope carries a
`schema` number, `SCHEMA = 1` today, documented as "a change that would stop
an already written file from reading gets a new one"
(`ui_json.py:105`). `build()` rejects only `schema > SCHEMA`
(`ui_json.py:901`), so a newer py2tosc still reads an older description --
files are durable and readers advance. `docs/stability.md:73` points at the
same mechanism: the dialect is provisional, "it carries a `schema` number of
its own for the case where a change would stop an already written
description from building."

So the thing to track is the schema, not the package version. minihost
emits `"schema": 1` explicitly rather than letting it default, and keeps the
mapping it targets:

| minihost | emits ui_json schema | py2tosc known to build it |
|---|---|---|
| 0.8.x (planned) | 1 | 0.5.0 -- 0.5.1 |

`ui_json` first appears in py2tosc 0.5.0, which sets the floor. No published
mapping exists upstream yet -- the changelog has no `ui_json` or `schema`
entries -- so this table is the record for now. The next py2tosc release
versions the schemas properly; see section 6 for what that changes here, and
for why minihost must stamp `"schema"` rather than let it default.

What the schema does not cover is a provisional change that alters *output*
without stopping a file from building -- different default sizing, say. The
generated-file golden tests in section 5 are what catch that, and they catch
it on a deliberate extra bump rather than at a user's machine. That is the
right place for the cost, and it is why the floor is a floor.

Import lazily inside `cmd_touch`, the same shape as the lazy `import json`
already used through `cli.py`, and fail with an install hint rather than a
traceback -- while still having written the `.ui.json`.

## 5. Testing

Nothing here justifies weakening the existing standard. Per phase:

- **Phase 0**: extend `tests/tsan/ringbuffer_stress.cpp` and `make tsan`.
  Python tests for coalescing and for `_auto` reachability. Add to
  `tests/test_rt_allocations.py` -- the drain path must not allocate.
- **Phase 1**: loopback test, server and client in one process, no network
  dependency and no fixed port (bind 0, read back the assigned port).
  Malformed-packet handling: a truncated datagram must not crash the socket
  thread.
- **Phase 2**: extend `tests/test_midi_mapper.py`, or add
  `tests/test_osc_mapper.py` alongside it.
- **Phase 3**: table-driven over the MSB/LSB orderings -- pair in order,
  LSB alone, MSB alone, LSB before MSB, plus both conflict-rejection cases.
  Assert full 0..16383 range coverage, which is the thing that would
  regress silently.
- **Phase 4**: assert the echo suppression actually suppresses; a test that
  a `set_param` originating from endpoint A does not send back to A.
- **Phase 5**: assert the live device advances `position_samples` at the
  expected rate, recording `set_transport` calls the way
  `test_transport_advance.py` already does for the offline path.
- **Phase 6**: golden-file tests on the generated `.ui.json` and map JSON.
  Most of this suite needs no py2tosc at all, since generation is now pure
  JSON emission -- assert the envelope, the widget choice per parameter
  kind, the CC assignment, and that the map file and the layout name the
  same addresses. Behind the optional extra, one round-trip test that
  `py2tosc.load` accepts the generated file and that every address in the
  resolved document resolves back to a real parameter index. Plus a test
  that minihost's slugger and `py2tosc.surface.slug` agree on a corpus of
  awkward parameter names, since a divergence there is a silently dead
  address.

`make test` after each phase, per the project rule. `make qa` before
declaring a phase done.


## 6. Upstream: py2tosc schema versioning

Recorded here because Phase 6 depends on it and the plan should not have to
be re-derived when it lands.

**Coming in the next py2tosc release:** versioned schemas for `ui_json`,
and possibly for `json_codec` as well -- the verbose dialect that mirrors
the `.tosc` XML node for node.

The `schema` field is not new in either; what is coming is treating it as a
tracked, documented thing rather than a constant nobody has yet had cause to
bump. Both dialects sit at `SCHEMA = 1` today (`ui_json.py:105`,
`json_codec.py:100`), and both reject a schema newer than they read.

One asymmetry between them matters to minihost, and it is the reason this
section exists:

- `json_codec` **writes** its own envelope, stamping `"schema": SCHEMA` at
  `json_codec.py:324`. A file it produces is self-describing for free.
- `ui_json` is **read and never written** -- there is no `to_ui_json`,
  because a resolved layout has frames and no memory of the `row` that
  placed them. It accepts `schema` and defaults it to `SCHEMA` when absent.

So for the dialect Phase 6 targets, **the producer stamps the schema, and
minihost is the producer**. Emitting `"schema": 1` explicitly rather than
letting it default is therefore not a formality: a generated file with no
`schema` key silently means "whatever the reader is", which is precisely the
ambiguity versioning exists to remove. Any minihost-generated `.ui.json`
must carry it.

What changes once upstream versioning lands:

- The compatibility table in Phase 6 stops being a unilateral record and
  becomes checkable against a published mapping -- better still if the range
  is readable at runtime, which is request 2 below.
- A schema bump becomes minihost's signal to regenerate goldens and widen
  the table, on a deliberate extra bump.
- If `json_codec` is versioned too, a second, lower-level generation target
  opens up: emitting the faithful node tree directly, bypassing the `ui`
  combinators and their provisional status entirely. Not worth taking --
  it means owning all sizing and layout arithmetic that `ui.resolve` does
  for free -- but worth knowing the escape hatch exists if the `ui`
  carve-out ever becomes intolerable.

One observation from the design discussion, kept for whoever picks this up:
**value-only substitution is a design invariant, not a limitation to work
around.** The reasoning is in Phase 6; the short version is that keys carry
meaning and values carry data, and rewritable keys would cost the dialect
its static checkability. Do not file it as a bug. The request below works
with that invariant rather than against it.

#### Feature requests for py2tosc

Two, both arising from minihost being a *generator* rather than a
hand-author. Neither blocks Phase 6 -- the plan is written to work without
them -- but both would remove real friction, and the second is close to
free.

**1. Heterogeneous `each` tables.**

A generated parameter table is mixed by nature: a bypass wants `button`, a
waveform selector wants `radio`, a cutoff wants `fader`. Because the tag is
a key and keys are not substituted, one `each` yields one widget kind, so a
generator either emits one `each` per kind (regrouping by widget, discarding
the plugin's own parameter order) or abandons `each` and writes explicit
nodes. Phase 6 takes the second, and it is fine -- but it means the dialect's
best feature is unavailable to exactly the caller its docstring describes,
"a generator with nothing to emit".

The fix that keeps the invariant is selection among fully-written branches,
chosen by a row's value at expansion time:

```json
{
  "each": [
    {"kind": "cont", "name": "cutoff", "caption": "Cutoff", "cc": 74},
    {"kind": "sw",   "name": "bypass", "caption": "Bypass", "cc": 75}
  ],
  "of": {"case": "$kind", "when": {
    "cont": {"fader":  "$name", "messages": [{"midi_cc": "$cc"}]},
    "sw":   {"button": "$name", "messages": [{"midi_cc": "$cc"}]}
  }}
}
```

Why this shape rather than a substitutable tag:

- Every key stays literal. No key anywhere is built from a row.
- Every branch is a complete node, so the whole `when` table is checkable
  against the tag table *before* expansion -- the property-set validation
  that motivates the invariant is untouched.
- The failure modes are nameable: a row selecting a branch that does not
  exist, or a `when` branch no row selects.
- It reads. `{"$kind": "$name"}` would not.

Naming is py2tosc's call -- `case`/`when` matches the existing plain-word
vocabulary (`row`, `each`, `of`, `from`, `as`), but `choose`/`by` or making
`of` accept a mapping alongside a node would do as well. The shape is the
request; the spelling is not.

This is a schema-bumping change (a file using it will not build on an older
reader), which is the mechanism working as intended.

**2. A readable schema range: `SCHEMAS` or `supports(schema)`.**

Today a producer cannot ask what the installed py2tosc reads. The only way
to discover it is to build and catch `FormatError` -- which conflates "this
schema is newer than I read" with "node 14 has a property that is not a
property", two errors with completely different remedies. `SCHEMA` is a
single number naming the newest, and says nothing about the floor.

Requested, on both dialects:

```python
py2tosc.ui_json.SCHEMAS          # e.g. range(1, 3) -- what this release builds
py2tosc.ui_json.supports(2)      # -> bool
py2tosc.json_codec.SCHEMAS       # same, for the faithful dialect
```

That lets `minihost touch` check before writing and fail with "this py2tosc
reads ui_json schemas 1-2, minihost emits 3; upgrade py2tosc" -- an error
naming its own remedy, rather than one about a node.

A distinguishable error type for the too-new case (a `SchemaError` subclass
of `FormatError`, say) would serve the same end for callers that would
rather catch than check, and is worth having either way since the two
failures want different messages.


## 7. Open questions

- **Chain and graph addressing.** `/mh/<slot>/param/<slug>` is proposed
  above, but the device can also be opened on a chain whose slots change at
  runtime. Does the address bind to slot position (breaks on reorder) or to
  a stable slot id? Leaning stable id, which may need one to exist first.
- **Sample-offset placement.** Phase 0 ships offset 0. Is sub-block
  placement ever worth the jitter and the one-block delay for a human
  finger on a tablet? Probably not; worth deciding rather than leaving open.
- **`MidiMapper` reuse.** `OscMapper` and `MidiMapper` should share a core,
  but `MidiMapper` is public API. Refactoring behind it is fine; changing
  its signatures is not.
- **Discovery.** Should minihost advertise itself over Bonjour/mDNS so
  TouchOSC finds it without typing an IP? Real usability gain, but it is a
  platform-specific dependency and JUCE does not provide it. Leaning no.
- **"MIDI learn" is a listed non-goal** (`TODO.md:202`). Generated
  surfaces are a different thing -- the mapping is computed and written to a
  file, not learned interactively -- but the boundary should be stated in
  the docs so the non-goal is not read as excluding this work.


## 8. Non-goals

- Timetag-scheduled OSC bundles. `juce_osc` delivers bundle contents
  immediately; matching that is fine for control surfaces.
- OSC over TCP or SLIP. UDP only, which is what TouchOSC uses.
- RPN/NRPN.
- Hosting a web UI, or any surface format other than `.tosc`. The generator
  should keep the parameter table separable so a second back-end is possible
  later, but only one gets written.
- OSC query protocol.
