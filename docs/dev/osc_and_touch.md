# OSC support and generated touch surfaces

Status: all six phases implemented. See CHANGELOG.md for what each
landed as, and the per-phase notes below for where the code and this plan
diverged. See the per-phase notes in section 4, and CHANGELOG.md for what each landed as. Companion to [desktop_app.md](desktop_app.md) in intent: a design that gets committed before the code, so the ordering decisions are arguable in review rather than discovered halfway through.

Goal: minihost speaks OSC natively, MIDI CC gains 14-bit resolution, and a script turns any plugin into a tablet control surface bound to one or both.


## 1. What already works

Worth establishing first, because the gap is smaller than it looks and two of the pieces are already shipping.

`minihost params <plugin> --json` (`src/minihost/cli.py:266`) emits a list of objects carrying `name`, `index`, `label`, `default_value`, `num_steps`, `is_boolean`, `is_automatable`, `category`.

The sibling project `py2tosc` has a `surface` module -- a `Parameter` dataclass, a `read()` that accepts exactly that JSON shape, and a `build()` that lays parameters out across paged TouchOSC controls bound to MIDI CC, to OSC, or to both. It is already exposed as `py2tosc build params.json`. `py2tosc/src/py2tosc/surface.py:111` even documents the seam: "A plugin host exports an `index` alongside each name."

So this pipes together today:

```console
$ minihost params ~/Library/Audio/Plug-Ins/VST3/Synth.vst3 --json > params.json
$ py2tosc build params.json --output synth.tosc
```

That is the flat path, and it stays useful as the zero-configuration one. py2tosc also has a second, richer entry point that Phase 6 targets instead: `py2tosc.ui_json`, a JSON dialect over the `ui` combinators, with `each` for walking a table of rows. Section 4, Phase 6 makes the case for generating that rather than the flat list.

What the flat path does not give you:

- Every control is a fader. The `is_boolean` / `num_steps` / `label` metadata minihost already exports is discarded, so a bypass toggle and a 16-position waveform selector both render as continuous faders.

- No matching `--map-file`, so the generated CC assignments and the host's mapping are two hand-maintained lists that silently drift.

- MIDI CC only. `surface.build(osc=True)` writes OSC addresses into the layout, but nothing in minihost listens on a UDP port, so those addresses go nowhere.

- 7-bit resolution on the path that does work.

- Nothing for transport.

The plan below closes those five gaps. Framing the work as "add OSC" alone would miss that most of the remaining cost is in the parameter delivery path and in transport, not in the OSC codec.


## 2. Decision: juce_osc, not liblo

The proposal was to embed liblo. Recommend against, and the reason is not a close call: OSC is already in the tree.

`thirdparty/JUCE/modules/juce_osc/` ships with the JUCE 8.0.12 that `scripts/download_juce.py` already downloads. Its module declaration reads:

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

The "no dependencies" rule is satisfied more literally by juce_osc than by any vendoring option, since it adds no dependency at all.

Two properties of `juce_osc` matter to the design and were verified by reading the source rather than assumed:

- `OSCReceiver::Pimpl` is its own `juce::Thread` (`juce_OSCReceiver.cpp:325`). Listeners registered with the `RealtimeCallback` template parameter are invoked directly on that socket thread; only `MessageLoopCallback` listeners go through `postMessage` (`juce_OSCReceiver.cpp:438-448`). The hot path therefore needs no JUCE message thread, which matters for the Python wheel.

- `OSCAddressPattern::matches` (`juce_OSCAddress.h:138`) gives wildcard address matching, so `/mh/param/*` dispatch is free.

And one limitation to state honestly: bundle time tags are parsed (`juce_OSCReceiver.cpp:141`) but not used for scheduled dispatch -- bundle contents are delivered immediately. Irrelevant for control surfaces, disqualifying if minihost ever wants OSC-scheduled events. Also UDP only: no TCP, no SLIP, so a serial OSC device is not served.

Rejected alternatives, for the record:

- **liblo**, per the table.

- **tinyosc** (MIT, single file). Fits the vendoring aesthetic but is codec only -- we would still write the UDP layer and receive thread, which is the part juce_osc already has.

- **Hand-rolled codec.** OSC 1.0 encoding is genuinely simple, maybe 400 lines with tests. Worth keeping as the escape hatch if a pure-C consumer ever needs OSC without linking JUCE, but it reinvents working code sitting in the tree.


## 3. The real cost centre: parameter delivery

This is the part that is not about OSC at all, and it should be built first.

Today `MidiMapper.__call__` (`src/minihost/control.py:259`) reaches a plugin parameter by calling `plugin.set_param`, which lands in `mh_set_param` (`projects/libminihost/minihost.cpp:717`):

```cpp
std::lock_guard<std::mutex> lock(p->stateMutex);
params.getUnchecked(index)->setValueNotifyingHost(normalized_0_1);
```

Meanwhile the live audio callback (`projects/libminihost_audio/minihost_audio.c:326`) calls `mh_process_midi_io` / `mh_chain_process_midi_io`, which per the header contract take no lock at all. So a control-thread parameter write runs concurrently with `processBlock`, ordered by nothing, applied at whatever moment the plugin next reads the value. Three consequences:

1. Not sample-accurate. Every write is effectively block-quantised, with jitter.

2. The mapper thread takes a mutex that offline callers also take, so `set_param` from a control surface can block behind a `morph` or a preset load.

3. It scales badly with rate, which is exactly what OSC and 14-bit CC introduce. A fader drag at 14-bit resolution emits far more messages than a 7-bit one.

`mh_process_auto` (`projects/libminihost/minihost.h:428`) and `mh_chain_process_auto` already exist and already split a block at each `MH_ParamChange` offset. The live path simply never uses them.

**Phase 0** is therefore: give `MH_AudioDevice` a parameter ring, drain it in the audio callback, and switch the callback to the `_auto` entry points.

- New `projects/libminihost_audio/param_ringbuffer.{h,cpp}`, an SPSC ring over `MH_ParamChange`, modelled directly on `midi_ringbuffer.{h,cpp}`.

- **Two** rings on the device, not one: one fed by the control-input thread (OSC socket thread or MIDI callback), one fed by programmatic `mh_audio_send_param`. This mirrors the `midi_in_buffer` / `midi_send_buffer` split, and the reason is recorded in the header comment at `minihost_audio.h:161`: sharing one SPSC ring between two producers corrupted its indices and lost events. Do not relearn that.

- **Coalesce on drain, last-wins per parameter index.** This is not an optimisation, it is a correctness requirement. `mh_process_auto` splits the block at every distinct offset; 200 messages for one fader inside one block would become 200 sub-blocks and blow the audio deadline. Collapse to one change per parameter per block, and cap the total distinct split points per block (start at 32) with the overflow folded into the last offset.

- Sample offset: v1 assigns 0, i.e. block start. Time-stamping arrival and placing the change inside the block is a later refinement -- it buys sub-block accuracy at the cost of jitter and a one-block safety delay, and should not gate the rest of the work.

- New C API `int mh_audio_send_param(MH_AudioDevice*, int param_index, float value)`, Python `AudioDevice.send_param(index, value)`.

- Redirect `MidiMapper` at the ring when an `AudioDevice` is attached, falling back to `set_param` when it is not (offline use).

Scope boundary: the device opens a plugin or a chain, never a graph (`mh_audio_open` / `mh_audio_open_chain`), so `graph_v2` needs nothing here. It already has `mh_graph_set_node_automation`.

Tests: extend `tests/tsan/ringbuffer_stress.cpp` and the `make tsan` target to cover the new ring; a Python test asserting coalescing (N writes to one param inside one block produce one change) and that the `_auto` path is reached.


## 4. Phases

Phase 0 is a prerequisite for 1, 3 and 5. The rest are independent enough to reorder.

### Phase 0 -- parameter ring and `_auto` in the live path -- DONE

As above. No user-visible feature; everything after it is cheaper and correct. Also fixes the existing 7-bit MIDI mapping path.

### Phase 1 -- OSC transport layer -- DONE

Landed as planned, with the device integration limited to numeric addressing (`/mh/param/<index>`, `/mh/<slot>/param/<index>`); name resolution moves to Phase 2, which can resolve at bind time and send numerically. Feedback (`mh_audio_set_osc_feedback`) stayed in Phase 4 where the plan puts it.

`projects/libminihost_audio/minihost_osc.{h,cpp}`, sitting beside `minihost_midi.{h,cpp}` because it is the same kind of thing: an I/O back-end for the audio layer.

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

Implementation is a thin `OSCReceiver` / `OSCSender` wrapper registering a `Listener<RealtimeCallback>`. CMake: add `juce::juce_osc` to both flavours in `_minihost_audio_configure_common`.

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

`py2tosc.surface.slug` (`surface.py:76`) already produces OSC-safe names with the same rules, so both ends agree on the spelling by construction -- which is the argument for using it rather than inventing a second slugger.

Python: `minihost.OscServer` / `minihost.OscClient` in `_core.cpp`, context-manager shaped like `MidiIn`.

### Phase 2 -- OSC mapping, unified with MIDI mapping -- DONE

Landed with the shared core keyed on a normalized float plus a source
identity, per section 7. `--osc-feedback` moved to Phase 4, where the
feedback machinery it would drive actually lives.

`src/minihost/control.py` grows an `OscMapper` alongside `MidiMapper`, and both should share a resolution/curve/range core rather than duplicating `_CCMapping.normalize`.

`OscMapper` differences from `MidiMapper`:

- Input is float32 already normalised 0..1, so no 7-bit quantisation. This is the whole point: `_CCMapping.normalize` divides by 127.0, giving 128 steps. A filter cutoff on 128 steps is audibly stepped; OSC float is not.

- Address-to-parameter resolution, with wildcard support delegated to `OSCAddressPattern`.

- Auto-binding: `OscMapper.bind_all(prefix="/mh/param")` maps every automatable parameter by slug in one call, which is what a generated surface wants.

`minihost play` grows `--osc-port N`, `--osc-feedback host:port`, and the `--map-file` schema grows an `"osc"` address key next to `"cc"`.

### Phase 3 -- 14-bit CC pairs -- DONE

**One rule below changed during implementation, and the plan was wrong.**
This section originally specified: "On MSB receipt: store it, reset the
cached LSB to 0, emit `msb << 7`." That produces a sawtooth. A controller
re-sending an unchanged `(MSB, LSB)` pair -- which is what a controller
sending full pairs does on every message -- would emit `msb << 7` and then
`(msb << 7) | lsb` forever, oscillating by up to 1/128 of the range at
message rate. The plan justified the transient by saying Phase 0 coalescing
hides it, which is true only when both halves land in the same audio block:
a pair takes about 2 ms on the wire against a 5.3 ms block at 48 kHz/256, so
they usually do, but not reliably.

The implemented rule keeps the last LSB instead. An unchanged pair therefore
emits an unchanged value, and the worst case is a stale fine position for the
microseconds until the LSB arrives -- an error of at most one coarse step,
never a periodic wobble. `tests/test_midi_14bit.py::test_a_repeated_pair_emits_a_stable_value`
is the test that decides it.

One case the plan did not cover: an LSB arriving before any MSB has been
seen. Emitting it alone reads as `msb = 0` and slams the parameter to the
bottom of its range, so it is held until an MSB gives it a coarse position to
refine.

The CLI spelling is `--map14` with the same grammar as `--map`, rather than
the `channel:cc14:param:...` form sketched below -- a separate flag keeps the
existing spec parser untouched and leaves no ambiguity about which field is
the controller number.


Per the MIDI spec, CC 0-31 carry the MSB and CC 32-63 carry the LSB for controller n, i.e. the pair is `(n, n + 32)` for n in 0..31.

API: `MidiMapper.map_cc14(channel, cc, param, value_range=..., curve=...)` where `cc` is the MSB number and must be 0..31.

Dispatch rule, chosen to work with both kinds of controller in the wild -- those that always send the full pair, and those that send LSB alone for a fine adjustment:

- On MSB receipt: store it, reset the cached LSB to 0, emit `msb << 7`.

- On LSB receipt: emit `(cached_msb << 7) | lsb`.

- Divide by 16383.0 before the curve, not 127.0.

That produces a brief transient when a full pair arrives -- the value lands at `msb << 7` and is corrected microseconds later by the LSB. Phase 0 makes this invisible: both writes land in the same audio block and coalesce to the second one. This is a concrete reason to keep the phase ordering.

Explicitly rejected: the ~50ms pairing window some implementations use. It needs a timer thread and buys nothing once coalescing exists.

Validation that must not be forgotten:

- `map_cc14(ch, n)` must reject if `map_cc(ch, n + 32)` is already mapped, and `map_cc(ch, m)` for m in 32..63 must reject if `map_cc14(ch, m - 32)` exists. Silent shadowing here would be near-undebuggable.

- `cc` outside 0..31 is an error with a message that says why.

Map file: add `"cc14": 7` as an alternative key to `"cc": 7` rather than a `"bits": 14` modifier -- one key, unambiguous parse, no invalid combination to validate. `--map` spec gains a parallel `channel:cc14:param:...` form.

Non-goal for this phase: RPN/NRPN (CC 98/99/6/38). It is a different mechanism, TouchOSC cannot emit it natively, and no touch surface needs it.

### Phase 4 -- feedback, host to surface -- DONE

Implemented by polling rather than by hooking the parameter-value callback
the plan proposed. The callback is a single slot the Python `Plugin` binding
already occupies, it fires on whatever thread changed the parameter
(including the audio thread), and a surface cannot use more than ~30 updates
a second anyway -- so its precision would have to be rate-limited back down
to what polling produces directly. The echo hazard the plan identified is
handled with the source identity Phase 2 put in the shared core.


Without this a generated surface is write-only: load a preset and every fader lies.

The mechanism already exists. `mh_set_param_value_callback` (`projects/libminihost/minihost.h:562`) fires `MH_ParamValueCallback` on plugin-initiated parameter changes, wired through `MH_Listener` at `minihost.cpp:328`. Hook it, push into a ring, have a sender thread emit OSC. No polling.

Two hazards to design around:

- **The callback thread is not ours.** `audioProcessorParameterChanged` fires on whatever thread changed the parameter, possibly the audio thread. The path from callback to socket must be lock-free and allocation-free: ring, then a sender thread drains it.

- **Echo.** `mh_set_param` calls `setValueNotifyingHost`, which fires the same callback -- so our own writes echo back to the surface that made them. Harmless when idle, but during a drag the echo fights the finger. Mitigation: tag the origin endpoint and suppress echo back to it for a short window, or drop echoes whose delta is below an epsilon. Pick one and write down which.

Rate-limit the feedback stream (30 Hz per parameter is plenty for a moving fader) so a modulating parameter cannot saturate the link.

### Phase 5 -- transport -- DONE

Both halves landed together: the live playhead the plan called out as
missing, and the OSC addresses on top of it. A command ring rather than
atomics, because this project avoids C11 `<stdatomic.h>` on purpose.


Larger than it looks, and worth calling out before anyone commits to it.

`mh_set_transport` (`projects/libminihost/minihost.h:407`) exists and offline renders now use it -- that was the fix documented in `tests/test_transport_advance.py`. But `grep -n transport projects/libminihost_audio/minihost_audio.c` returns nothing: **the live audio device has no playhead at all.** It never calls `mh_set_transport`, so a tempo-synced delay running under `minihost play` sees no host tempo and a playhead pinned at zero.

So "a transport touch surface" decomposes into two jobs, and only the second is OSC:

1. Give `MH_AudioDevice` a host playhead: a sample counter advanced per callback, a settable BPM and time signature, play/stop state, optional loop points, pushed via `mh_set_transport` before each process call. This is worth doing on its own merits regardless of OSC -- it is a real gap in the live path.

2. Expose it over OSC:

```
/mh/transport/play      -> start
/mh/transport/stop      -> stop
/mh/transport/bpm       f
/mh/transport/position  f   (beats)
/mh/transport/loop      i
/mh/panic               -> all-notes-off on every channel
```

with feedback on `/mh/transport/position` so the surface can show a playhead.

Recommend splitting job 1 into its own TODO entry under Tier 1 or 2 independent of this plan, and letting Phase 5 depend on it.

### Phase 6 -- surface generation -- DONE

Landed as designed, on `ui_json` schema 2 with a `case`/`when` branch table.
One detail the plan did not anticipate: two branches per widget kind rather
than three total, because a parameter past the CC limit has no controller
number and `each` cannot conditionally include a message.


The user-facing deliverable: `minihost touch <plugin>` writes a layout and the matching map file, and they agree by construction.

#### Target the `py2tosc.ui` JSON dialect, not the flat `surface` list

`py2tosc.surface.read` accepts a flat list of names -- one control per entry, laid out four across and three down, every control a fader. It is the zero-configuration path and it stays useful as one, but it is the wrong generation target for anything richer, because the layout it produces is not described anywhere the generator or the user can reach.

`py2tosc.ui_json` (`src/py2tosc/ui_json.py`) is the better target. It is a JSON dialect over the `py2tosc.ui` combinators -- `row`, `column`, `tiles`, `stack`, `grid`, `pager`, `labelled`, `inset` -- with `sizes`, `gap`, `pad` and `frame` as arguments, so nesting and space division are described rather than baked in. From `py2tosc/tests/data/mixer.ui.json`:

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

- **`each` is a parameter table.** `repeat` counts; `each` walks a list of records, binding every field of a row as `$field` the way `repeat` binds `$i`. Its docstring names the case exactly -- a layout "whose names and numbers follow no sequence" -- and permits an empty list, "since a list of nothing is what a generator with nothing to emit produces". A plugin's parameter list is precisely that.

- **A whole-string placeholder keeps its type** (`ui_json.py:321`): `"$cc"` substitutes the *number* 74, while `"cutoff $unit"` substitutes a string. So `{"midi_cc": "$cc"}` binds a real controller number from a row, with no string-to-int coercion anywhere in the format.

- **`//` keys are comments**, ignored wherever a key may appear. A generated layout can explain itself: which parameter index a control came from, why a CC number was skipped, what the plugin called it before slugging.

- **It is read but never written** -- there is no `to_ui_json`, because a resolved layout has frames and no memory of the `row` that placed them. That is exactly the right shape here: minihost is a producer, py2tosc is the compiler, and the direction never needs to reverse.

`py2tosc.load()` dispatches on the envelope's `format` field (`document.py:294`), so a `.ui.json` is loadable and convertible with what already exists:

```console
$ py2tosc convert synth.ui.json -o synth.tosc
```

#### What this buys, concretely

**The generation step loses its dependency.** minihost writes JSON text; it imports nothing from py2tosc to do so. py2tosc is needed only to compile the result into a `.tosc`. If it is not installed, `minihost touch` still produces a complete, valid `.ui.json` and says how to compile it -- rather than failing with an ImportError and producing nothing.

**The output is a source file, not an artefact.** A `.tosc` is a zipped XML blob; nobody hand-edits one, so a generator that emits one directly owns every layout decision forever. A `.ui.json` is reviewable, diffable, and editable -- move a control, change a gap, add a page, then recompile. The generator stops having to anticipate what anyone might want.

**Templates become user-supplied.** `minihost touch --template mine.ui.json` takes a layout containing an `each` node with a marker, and injects the parameter rows into it. The user owns the visual design; minihost owns only the parameter table and the bindings. This is the feature that makes the command worth having, and it falls out of the dialect rather than needing to be built.

**The widget-metadata extension to `surface.Parameter` is no longer needed.** The earlier draft of this plan proposed pushing `kind`, `steps`, `unit` and `group` into py2tosc's `Parameter` dataclass so `surface.build` could choose widgets. With ui-json, minihost chooses the widget itself by emitting `{"button": ...}` or `{"radio": ...}` instead of `{"fader": ...}`. No py2tosc change is required at all. Drop that work.

#### Heterogeneous tables: `each` with a branch table

Substitution reaches values, never keys, so the tag stays fixed in the template and one `each` builds one kind of control. That is a design invariant, not a limitation: keys carry the meaning (which tag, which property) and values carry the data, and a node whose type came from a row could not be checked against the tag table before expansion.

Plugin parameters are mixed, though -- a bypass wants `button`, a waveform selector `radio`, a cutoff `fader` -- so an earlier draft of this plan routed around it by emitting explicit per-parameter nodes and filed a feature request upstream.

**py2tosc 0.5.2 shipped it.** An `of` may now hold a branch table instead of a node, and a row says which branch:

```json
{
  "each": [
    {"kind": "cont", "name": "cutoff", "cc": 74},
    {"kind": "sw",   "name": "bypass", "cc": 75}
  ],
  "of": {"case": "$kind", "when": {
    "cont": {"fader":  "$name", "messages": [{"midi_cc": "$cc"}]},
    "sw":   {"button": "$name", "messages": [{"midi_cc": "$cc"}]}
  }}
}
```

Every key stays literal, so every branch is validated against the tag table before any row is read -- a branch naming two tags or none is refused whether or not a row selects it. This is `ui_json` **schema 2**.

Two behaviours worth knowing before writing the generator, both from the 0.5.2 notes:

- **Only the selected branch is substituted into.** A branch reads the fields its own rows carry and no others, so a `sw` row needs no `steps` for a `radio` branch that mentions one. Rows can therefore be exactly as wide as their kind requires; no padding the table to a union of fields.

- **A branch nothing selects is not an error.** A template carrying a `radio` branch for a plugin with no stepped parameters is fine. A row selecting a branch nobody wrote *is* an error, and names the value it read. So minihost can emit one full-width template covering every widget kind and let the parameter table decide what appears.

**So the recommendation flips.** Use `each` with a `case`/`when` branch table as the default, not explicit per-parameter nodes. It keeps parameter order (rows expand in list order, each selecting its own branch), which was the objection that ruled out grouping by widget kind, and it keeps the compactness that made `each` worth targeting in the first place. The parameter table lands in the file as a table -- which is what it is, and what makes the output legible and hand-editable.

Explicit nodes remain the fallback for anything the branch table cannot express, and the generator should be structured so that choice is one function, not a shape assumption spread through the emitter.

#### Work in minihost

`src/minihost/touch.py` plus a `cmd_touch` in `cli.py`:

- Build one parameter table from `MH_ParamInfo`: `is_boolean` to a `button`, `num_steps > 0` to a `radio` with that many positions, otherwise a `fader`; `label` to the caption unit; `category` to the page grouping; skip `is_automatable == 0` unless `--all`.

- Slug names with the same rules `py2tosc.surface.slug` uses (`surface.py:76`) so both ends spell an address identically. Reuse it when py2tosc is importable, mirror it when not, and test that the two agree -- a divergence here is a silently dead address.

- Assign CC numbers and OSC addresses **once**, then render both the `.ui.json` and the map file from that single table. This is the whole reason to do it in minihost rather than piping two CLIs: the layout and the host mapping cannot disagree because they come from the same rows.

- Emit the rows as one `each` over a `case`/`when` branch table -- one branch per widget kind, rows in plugin parameter order. A branch no row selects is legal, so the template can carry all three unconditionally.

- Stamp the envelope with `required_schema(document)` rather than a constant, since `--template` means the layout is not wholly minihost's. See the schema notes below.

- Annotate with `//` comments: parameter index per control, and a header noting the plugin, its version, and the minihost version that generated the file.

- Warn on CC exhaustion. There are 128 controller numbers and plugins routinely have more parameters; those spill to OSC-only. `surface.py:53` already does this silently, which for a 300-parameter plugin is a surprise rather than a decision.

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

Flags: `--template FILE`, `--midi-only` / `--osc-only`, `--size WxH`, `--columns` / `--rows`, `--params` to select a subset by name or index range, `--no-compile` to stop at the `.ui.json`.

#### Dependency handling and schema tracking

`py2tosc` has `dependencies = []` itself, so it enters as an optional extra and `pyproject.toml`'s core `dependencies = []` is untouched:

```toml
[project.optional-dependencies]
touch = ["py2tosc >= 0.5.2"]
```

A floor, not a minor pin. Both JSON dialects version themselves: the envelope carries a `schema` number, and a reader rejects only what is above its range, so a newer py2tosc still builds an older description -- files are durable, readers advance. As of 0.5.2:

| dialect | `SCHEMA` | `SCHEMAS` |
|---|---|---|
| `ui_json` | 2 | `range(1, 3)` |
| `json_codec` | 1 | `range(1, 2)` |

0.5.2 is the floor because `case`/`when` is schema 2 and arrived in it.

**Stamp the schema, and compute it rather than remember it.** `ui_json` is read and never written, so the producer stamps -- and minihost is the producer. A description carrying no `schema` key means "whatever the reader is", which is the ambiguity a version number exists to remove.

Hardcoding `2` is the wrong fix, because with `--template` the layout is partly the user's and may use less (or later, more) than minihost's own emitter does. Ask instead:

```python
schema = py2tosc.ui_json.required_schema(document)   # lowest schema that builds it
```

Understating the stamp is the mistake nothing catches by building -- the reader that would catch it is by definition new enough not to care. It surfaces on someone else's older release as a `FormatError` about a node that is perfectly fine. `py2tosc validate` warns on it, and so does the vendored checker described in section 5.

Check compatibility before writing, not after:

```python
if not py2tosc.ui_json.supports(schema):
    # "this py2tosc reads ui_json schemas 1-2, this layout needs 3; upgrade py2tosc"
```

`SchemaError` (a `FormatError` subclass, new in 0.5.2) is the catch-first equivalent for the too-new case.

The table minihost keeps:

| minihost | emits ui_json schema | py2tosc known to build it |
|---|---|---|
| 0.8.x (planned) | 2 (1 for layouts using no schema-2 spelling) | 0.5.2+ |

What the schema does not cover is a provisional change that alters *output* without stopping a file from building -- different default sizing, say -- nor, per the `required_schema` docstring, a future schema that changed what an existing spelling *does*, since the description would be textually identical. Golden files are the guard for both, and they catch it on a deliberate extra bump rather than at a user's machine. That is the right place for the cost, and it is why the floor is a floor.

Import lazily inside `cmd_touch`, the same shape as the lazy `import json` already used through `cli.py`, and fail with an install hint rather than a traceback -- while still having written the `.ui.json`.

## 5. Testing

Nothing here justifies weakening the existing standard. Per phase:

- **Phase 0**: extend `tests/tsan/ringbuffer_stress.cpp` and `make tsan`. Python tests for coalescing and for `_auto` reachability. Add to `tests/test_rt_allocations.py` -- the drain path must not allocate.

- **Phase 1**: loopback test, server and client in one process, no network dependency and no fixed port (bind 0, read back the assigned port). Malformed-packet handling: a truncated datagram must not crash the socket thread.

- **Phase 2**: extend `tests/test_midi_mapper.py`, or add `tests/test_osc_mapper.py` alongside it.

- **Phase 3**: table-driven over the MSB/LSB orderings -- pair in order, LSB alone, MSB alone, LSB before MSB, plus both conflict-rejection cases. Assert full 0..16383 range coverage, which is the thing that would regress silently.

- **Phase 4**: assert the echo suppression actually suppresses; a test that a `set_param` originating from endpoint A does not send back to A.

- **Phase 5**: assert the live device advances `position_samples` at the expected rate, recording `set_transport` calls the way `test_transport_advance.py` already does for the offline path.

- **Phase 6**: golden-file tests on the generated `.ui.json` and map JSON. Nearly all of this needs no py2tosc, since generation is pure JSON emission -- assert the envelope, the stamped schema, the branch a parameter kind selects, the CC assignment, and that the map file and the layout name the same addresses.

  Vendor `scripts/check_json.py` from py2tosc into the test tree and run every generated file through its `check(data)`. It is one stdlib-only file written to be copied by projects that produce these descriptions, so it costs no dependency and catches the class of fault a golden file cannot: a key nothing reads, silently dropping a subtree while the output still looks correct. Record its py2tosc version in `docs/vendored.md` alongside the C libraries, and re-copy on a schema bump.

  Assert the stamp specifically: that minihost stamps what `required_schema` computes, not a constant. Since `required_schema` detects spellings rather than meanings, pair it with a golden file -- that is the documented guard for the case it cannot see.

  Behind the optional extra, the tests that need the real compiler: that `py2tosc.load` builds the generated file, that it resolves (a `sizes` that does not divide and a row too narrow for its children are invisible to the standalone checker), and that every address in the resolved document maps back to a real parameter index.

  Plus a test that minihost's slugger and `py2tosc.surface.slug` agree on a corpus of awkward parameter names, since a divergence there is a silently dead address.

`make test` after each phase, per the project rule. `make qa` before declaring a phase done.


## 6. Upstream: what py2tosc 0.5.2 landed

Both feature requests this plan raised shipped in py2tosc 0.5.2, in the shape proposed, plus two things nobody asked for that change how Phase 6 is tested. The changelog credits minihost by name as the caller that found the gaps -- a generator rather than a hand-author.

Recorded here so the plan does not have to be re-derived, and so the reasoning survives if any of it needs revisiting.

**1. `case`/`when` branch tables -- `ui_json` schema 2.** Covered in Phase
6. The consequence for this plan is that the generator's default output shape flipped from explicit per-parameter nodes back to `each`.

**2. `SCHEMAS`, `supports()`, `SchemaError`, on both dialects.** `SCHEMA` names only the newest and says nothing about the floor; `SCHEMAS` is the range a release builds and `supports(n)` asks about one, so a generator checks before writing rather than catching after. A schema above the range is now `SchemaError` (subclass of `FormatError`); a `schema` key that is not a number stays a plain `FormatError`, since that is an envelope fault rather than a version fault. 0.5.2 also started refusing a schema *below* the range -- there has never been a schema 0.

**3. `required_schema()` and a `py2tosc validate` warning.** Not requested, and the more useful of the two additions for minihost. `SCHEMAS` answers what a release reads; `required_schema(data)` answers what a description needs -- the lowest schema that builds it, which is the number to stamp.

This closes a failure that is otherwise structurally uncatchable by the producer: understating the stamp cannot fail on the machine that wrote it, because the reader that would catch it is new enough to build the file anyway. It fails later, on someone else's older release, as an error about a node that is fine. `py2tosc validate` now warns:

```console
$ py2tosc validate synth.ui.json
warning: <envelope>: the description declares schema 1 and uses schema 2; a
release reading only schema 1 will refuse it with a message about a node
```

A warning, not a refusal -- a refusal would only ever fire on files the refusing reader can build. Exit codes unchanged. Two stated limits carry over into how minihost should test: `required_schema` detects spellings, not meanings, and the table behind it is a hand-written historical record that a future schema bump could under-extend.

**4. `scripts/check_json.py` -- a standalone, stdlib-only checker.** One file, no py2tosc import, explicitly meant to be copied into a project that *writes* these files. That is minihost's case exactly, and it is why section 5's Phase 6 tests mostly do not need the optional extra.

It reads both dialects (told apart by `format`, as `py2tosc.load` does), exposes `check(data)` returning findings, and catches the failure class this format has to close: a key nothing reads, silently ignored, so a `childs` typo drops a subtree and the output looks like a file that read correctly. Also `$name` no repeat binds, a binding a control cannot carry, and a schema stamped below the spellings used.

Conservative by construction -- everything it calls an error, py2tosc refuses too -- and the reverse is explicitly not promised: it resolves nothing, so a `sizes` that does not divide, a row too narrow for its children, and a property that will not coerce are invisible to it. Those need the real compiler, which is the one place Phase 6's tests want the extra installed.

Its tables are generated off the live modules by `scripts/make_check_json.py` (with a `--check` mode for CI) rather than retyped, so the copy cannot drift silently.

**Still outstanding upstream:** nothing this plan needs. `json_codec` remains at schema 1, which is fine -- Phase 6 does not target it, and the lower-level escape hatch noted earlier stays hypothetical.

One observation from the design discussion, kept because it explains the shape of item 1 and is worth not relitigating: **value-only substitution is a design invariant, not a limitation worked around.** The 0.5.2 notes make the same argument -- every key stays literal, so every branch is checked before any row is looked at, which is a check a node whose type came from a row could not have.

## 7. Surface transports: why not a web page over Web MIDI

Asked before Phase 2, because if a browser UI were the better target then the
mapping layer should be built facing it. It is not, but the reasoning is worth
keeping so it does not get re-litigated.

### The constraint everything follows from

**A browser cannot send UDP.** No page will ever speak OSC to minihost
directly. That leaves exactly three transports from a surface to this process,
and they reach different devices:

| transport | reaches | new minihost code | resolution |
|---|---|---|---|
| `.tosc` + OSC | iPad, iPhone, Android, desktop | none (Phase 1) | float32 |
| web page + Web MIDI | desktop Chrome / Firefox / Edge, Android | none | 7-bit, 14-bit after Phase 3 |
| web page + WebSocket | everything, iPad included | HTTP/WS server | float32 |

### Web MIDI does not reach an iPad

Safari on iOS/iPadOS does not support the Web MIDI API -- not in any version
through 26.6 -- and Safari on macOS does not either, through 27 and Technology
Preview. WebKit has declined to ship it for years on fingerprinting grounds,
since MIDI devices report unique identifying IDs, and there is no published
roadmap. Support is Chrome 43+, Edge 79+, Firefox 108+ and Chrome on Android.

Reaching for Chrome on the iPad does not help: every iPadOS browser is WebKit
underneath, outside the EU alternative-engine carve-out.

So for the case that motivates a touch surface at all -- a tablet on a stand
driving a plugin -- Web MIDI is unavailable on the dominant tablet. That is
the whole answer to "should this replace `.tosc`": it cannot, because it does
not run where `.tosc` runs.

Sources: <https://caniuse.com/midi>,
<https://developer.mozilla.org/en-US/docs/Web/API/Navigator/requestMIDIAccess>

### Where it does win, and it is free to try

`mh_audio_create_virtual_midi_input` already exists, so a page calling
`requestMIDIAccess()` and sending CC reaches `MidiMapper` today with **no new
minihost code at all**. Against `.tosc` it also wins on:

- Generation. Emitting HTML is trivial next to generating a zip of XML.
- Iteration. Reload the page; no transfer to a device, no paid app.
- Display. Parameter names with units, value curves, meters -- things a
  TouchOSC layout does poorly or not at all.

Two costs to know before starting. `requestMIDIAccess()` requires a secure
context, so `https://` or `http://localhost` rather than `file://` in
practice, which means serving the page from somewhere. And virtual MIDI ports
do not exist natively on Windows -- libremidi's WinMM backend cannot create
one -- so Windows users need loopMIDI or an equivalent driver.

Worth a short prototype on those terms: a virtual MIDI input, eight
`<input type="range">`, `output.send([0xB0, cc, value])`. If it is pleasant on
a laptop second screen it is a free win. It is simply not the iPad answer and
should not be planned as one.

### If a browser UI is what is actually wanted, WebSocket is the one

It beats Web MIDI on every axis except the dependency: it runs on iPad,
carries float32 so the 7-bit problem never arises, needs no virtual MIDI
driver on Windows, satisfies the secure-context rule automatically when served
from localhost, and makes Phase 4's feedback direction fall out of the same
connection instead of needing a second channel.

The cost is an HTTP/WebSocket server, which is a real new dependency and the
reason this is not being adopted now. One licensing trap if it ever is:
**Mongoose is dual GPLv2/commercial, and GPLv2-only is incompatible with this
project's GPL-3.0-or-later.** civetweb (MIT) is the clean choice.

The honest counterweight is touch UX. A web page handles continuous controls
worse than a native touch app: multitouch fader ergonomics have to be built by
hand, browser chrome eats screen, pull-to-refresh fights vertical drags, and
the screen sleeps. TouchOSC solved all of that years ago, and for performance
use the gap is not small.

### What this means for Phase 2

Nothing gets decided now, and nothing needs to be. All three transports are
the same parameter table with different back ends, which section 4's Phase 6
already separates.

The one constraint worth adopting immediately, because it is free today and
expensive later: **the shared resolution/curve/range core should key on a
normalized float plus a source identity, not on CC numbers or OSC addresses.**
Then a Web MIDI or WebSocket back end is a new adapter rather than a refactor.
Phase 1's numeric `/mh/param/<index>` addressing already has that shape.


## 8. Open questions

- **Chain and graph addressing.** `/mh/<slot>/param/<slug>` is proposed above, but the device can also be opened on a chain whose slots change at runtime. Does the address bind to slot position (breaks on reorder) or to a stable slot id? Leaning stable id, which may need one to exist first.

- **Sample-offset placement.** Phase 0 ships offset 0. Is sub-block placement ever worth the jitter and the one-block delay for a human finger on a tablet? Probably not; worth deciding rather than leaving open.

- **`MidiMapper` reuse.** `OscMapper` and `MidiMapper` should share a core, but `MidiMapper` is public API. Refactoring behind it is fine; changing its signatures is not.

- **Discovery.** Should minihost advertise itself over Bonjour/mDNS so TouchOSC finds it without typing an IP? Real usability gain, but it is a platform-specific dependency and JUCE does not provide it. Leaning no.

- **"MIDI learn" is a listed non-goal** (`TODO.md:202`). Generated surfaces are a different thing -- the mapping is computed and written to a file, not learned interactively -- but the boundary should be stated in the docs so the non-goal is not read as excluding this work.


## 9. Non-goals

- Timetag-scheduled OSC bundles. `juce_osc` delivers bundle contents immediately; matching that is fine for control surfaces.

- OSC over TCP or SLIP. UDP only, which is what TouchOSC uses.

- RPN/NRPN.

- Hosting a web UI, or any surface format other than `.tosc`. The generator should keep the parameter table separable so a second back-end is possible later, but only one gets written.

- OSC query protocol.
