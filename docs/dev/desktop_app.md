# Desktop Application — Design Document

Status: implemented, pre-release. Single binary, phased: Phase 1 (offline graph renderer with plugin editor windows) and Phase 2 (realtime audio device callback driver and live MIDI input) are both built and exercised; see [desktop_app_todo.md](desktop_app_todo.md) for per-feature status. What remains before a public release is packaging (code signing, notarization, installers), broader automated test coverage, and the deferred UX polish (undo/redo, plugin browser). The sections below are the original design and are kept as the reference for intended behaviour; where the prose still reads as future tense ("v1 will..."), that work now exists unless the TODO marks it deferred.

Working name: `minihost-desktop` (binary), `minihost.app` on macOS. Source lives in `projects/minihost_desktop/` alongside the other sub-projects (`projects/libminihost/`, `projects/libminihost_audio/`, `projects/libminihost_graph/`). The top-level `CMakeLists.txt` adds it behind `MINIHOST_BUILD_DESKTOP` (default OFF).

## Goal

A developer-facing host app that loads VST3/AU/LV2 plugins, wires them into a node graph, exposes plugin editor windows, and renders to disk offline. v2 extends the same graph executor to drive a realtime audio device callback.

Non-goals (both phases):

- DAW features: arrangement timeline, clip editing, mixer automation curves drawn in-app, time-stretching, take comping.

- Preset browser UI beyond `read_vstpreset` / `set_state`.

- MIDI learn / parameter mapping UI.

- Plugin shell / multi-instrument disambiguation.

- Cross-graph routing (every project is one graph).

The app is a thin GUI on top of `libminihost`, `libminihost_graph`, and JUCE's plugin / device modules. Where v1 needs functionality the C library doesn't have, the design favours extending the C ABI over duplicating logic in the GUI process.

## Stack

JUCE C++, single binary, statically linked.

- UI: `juce_gui_basics`, `juce_gui_extra`. Native widgets, no web view.

- Audio engine v1: existing `libminihost` + `libminihost_graph`, driven from a non-realtime render thread.

- Audio engine v2: `juce_audio_devices` `AudioIODeviceCallback` driving the same graph executor.

- Plugin editor windows: JUCE's `AudioProcessorEditor`, hosted in a `DocumentWindow` per plugin instance. Requires `MINIHOST_HEADLESS=OFF` for the GUI build; the headless build of `libminihost` is unaffected (separate CMake target).

- Project file: JSON, schema-versioned. See [Project file](#project-file).

Rationale for JUCE C++ over Python+Qt or Tauri+web:

1. Plugin editor windows require a native window handle that composes with JUCE's `AudioProcessorEditor`. Other shells force either window embedding or an out-of-process editor daemon — both add a permanent IPC seam between UI and audio that JUCE-native avoids.

2. `libminihost` is already JUCE. The GUI build flips `MINIHOST_HEADLESS=OFF` and adds `juce_gui_basics` to the same CMake project; nothing is reimplemented.

3. The Python bindings remain the supported scripting surface. The desktop app is not a Python application.

Cost accepted: JUCE's UI toolkit is older than web/Qt. Acceptable for a developer-facing host where fidelity and packaging beat visual polish.

## Phase 1 — Offline graph renderer with editor windows

### Scope

- Load / save project files.

- Add, remove, connect plugin nodes via a node-graph canvas.

- Open plugin editor windows; parameter changes propagate to the underlying `MH_Plugin` immediately and are persisted on save.

- Render the graph to a WAV/FLAC file via the offline executor.

- Progress reporting on long renders; cancellation.

Out of scope for v1:

- Realtime playback through an audio device.

- Live MIDI input.

- Latency compensation across fan-in paths (matches `graph.md`'s v1 scope).

- Feedback loops.

### Architecture

```text
+--------------------------------------------------+
|                    GUI thread                    |
|  - MainWindow, GraphCanvas, EditorHost windows   |
|  - ProjectModel (observable, owns Graph state)   |
+--------------------+-----------------------------+
                     | command queue (lock-free SPSC)
                     v
+--------------------------------------------------+
|              Render / engine thread              |
|  - GraphExecutor (mh_graph_*)                    |
|  - mh_process_auto per node, per block           |
|  - Writes to AudioFileWriter (libminihost_audio) |
+--------------------------------------------------+
```

In v1 there is no audio device. The render thread is started by the "Render" action, runs to completion (or cancellation), and exits. Editor window parameter writes call `mh_set_param` directly on the GUI thread; `libminihost` already serializes these against the audio thread via its internal mutex, and no audio thread exists during editor-only use.

### Graph executor

The C-level executor lives in `libminihost` (the parallel bus is already present as `PluginBus`, parallel-branches-summed). v1 extends it from "parallel branches summed" to a general DAG:

1. New C API in `projects/libminihost_graph/`:

   - `mh_graph_create`, `mh_graph_add_node`, `mh_graph_connect`, `mh_graph_compile`, `mh_graph_render_block`.

   - Built-in non-plugin node kinds: `input` (file or buffer source), `output` (file or buffer sink), `gain`, `mix`.

2. Compilation does Kahn topological sort, channel-count validation, and edge-buffer pool allocation with liveness reuse.

3. Per-block step matches `docs/dev/graph.md` §Execution.

The parallel bus is preserved as `PluginBus` (`mh_bus_*`), the parallel-branches-summed shortcut; `mh_graph_*` is the general DAG case. The Python `PluginBus` binding is the shortcut; the `minihost.PluginGraph` binding wraps the DAG ABI and is what the desktop app's project loader emits.

Justification for keeping the executor in C rather than the Python sketch from `graph.md`: the desktop app's render thread runs in-process with no GIL, and we want the same executor to drive the v2 realtime callback. Python isn't on the realtime path in either phase.

### Editor window hosting

One `DocumentWindow` per opened plugin editor. The window owns:

- A `juce::AudioProcessorEditor*` obtained from the plugin instance underlying the `MH_Plugin` handle. This requires a new C ABI call: `mh_get_juce_processor(MH_Plugin*) -> juce::AudioProcessor*` (opaque pointer; only the desktop binary, which links JUCE, dereferences it).

- A resize listener that calls the editor's `setSize`.

- A close handler that destroys the editor but leaves the plugin instance alive.

Parameter changes from the editor reach `MH_Plugin` through JUCE's existing `AudioProcessorParameter::Listener` mechanism, which `libminihost` already wires to `mh_register_param_listener`. The GUI mirrors parameter state from listener callbacks, not by polling.

### Project file

```json
{
  "minihost_project_version": 1,
  "sample_rate": 48000,
  "block_size": 512,
  "nodes": [
    {"id": "in",   "kind": "input",  "channels": 2, "source": "kick.wav"},
    {"id": "comp", "kind": "plugin", "path": "/Library/.../OTT.vst3",
     "state_b64": "..."},
    {"id": "out",  "kind": "output", "channels": 2, "sink": "bounce.wav"}
  ],
  "edges": [
    {"src": ["in", 0],   "dst": ["comp", 0], "channels": 2},
    {"src": ["comp", 0], "dst": ["out", 0],  "channels": 2}
  ],
  "automation": [
    {"node": "comp", "param": "Threshold",
     "points": [[0.0, -20.0], [5.0, -8.0]]}
  ]
}
```

Plugin state is the opaque blob returned by `mh_get_state`, base64-encoded. Project files are not portable across plugin versions that change state format; that's the plugin's contract, not ours.

### Render path

"Render" action:

1. Snapshot the `ProjectModel` to a `CompiledGraph` on the GUI thread.

2. Spawn the render thread. The thread loops `mh_graph_render_block` advancing `t0` by `block_size`, writing each output node's buffer to its sink via `mh_audio_write` (streaming, not all-at-once).

3. Progress is reported by frames written; the GUI polls at 30 Hz.

4. Cancellation flips an atomic flag checked at the top of each block.

The render path reuses `process_audio_to_file` semantics where possible — peak normalization, latency-comp trim — but operates over the graph executor's output node(s) rather than a single plugin.

## Phase 2 — Realtime mode

### Scope additions

- Audio device selection (input / output device, sample rate, block size) via `juce::AudioDeviceManager`.

- Transport bar: play / stop / loop region / BPM. Drives `MH_TransportInfo` for each block.

- Live MIDI input routed to designated plugin nodes.

- The same graph executor compiled in "realtime" mode (allocation-free block step; pre-allocated buffer pool sized at `prepare`).

### Architecture delta

```text
+---------------------------+   +------------------------------+
|       GUI thread          |   |   AudioIODevice callback     |
|  - transport controls     |   |  - GraphExecutor.process()   |
|  - MIDI device pick       |   |  - reads SPSC command queue  |
+-------------+-------------+   +------------------------------+
              |                          ^
              | commands (SPSC)          |
              +--------------------------+
              | MIDI input thread --> MIDI ring buffer
```

Constraints the executor must satisfy in v2:

- Allocation-free `render_block` (already true for `mh_chain_process_*`; must extend to `mh_graph_render_block`). Tracked by an RT-allocation test analogous to the existing `tests/test_rt_allocations.py`.

- Parameter writes from the GUI thread reach the audio thread via a bounded SPSC queue with drop-newest-on-overflow. The existing `mh_set_param` mutex is fine for offline; v2 introduces an explicit lock-free path for live editing.

- Editor window parameter writes go through the same lock-free path.

Recompilation of the graph (topology change) requires stopping the device callback, swapping the `CompiledGraph` under a brief mute, and restarting. Parameter and automation changes do not recompile.

### Items deferred to v2.5+

- Latency compensation across fan-in paths. Plugin latency is known (`mh_get_latency`); inserting per-path delay at fan-in points is a scheduler change, not a UI change.

- Feedback loops with a one-block delay node.

- Sidechain input buses on plugins (blocked on a C ABI gap noted in `graph.md`).

- Plugin process isolation (out-of-process plugin hosting for crash containment). Significant scope; deferred unless crashes become a routine support burden. See [Crash resilience](#crash-resilience-decided) for the shipping decision and the cheaper mitigations adopted instead.

## Cross-cutting concerns

### Build configuration

- New sub-project at `projects/minihost_desktop/` with its own `CMakeLists.txt` defining the `minihost_desktop` executable target. Depends on `minihost_gui` (sibling static library produced by `projects/libminihost/CMakeLists.txt` alongside the headless `minihost`), and the JUCE GUI modules. The top-level `CMakeLists.txt` includes the sub-project via `add_subdirectory` guarded by `MINIHOST_BUILD_DESKTOP`. Both archives coexist in the same build tree; enabling the desktop does not invalidate the headless library or the Python wheel.

- `libminihost_audio` will need an equivalent split (`minihost_audio` + `minihost_audio_gui`) once the desktop needs audio file or device I/O, since its current PUBLIC link on the headless `minihost` would conflict with `minihost_gui` symbols. Tracked in the TODO.

- The headless `libminihost` static / shared library and the Python wheel build are unaffected; they continue to use the headless JUCE module set.

- `MINIHOST_BUILD_DESKTOP=OFF` by default; opt-in to keep CI matrix cheap.

### Testing

- Project file round-trip: load → save → byte-identical (after canonicalization).

- Render parity: a project containing one input → one plugin → one output renders bit-identically to a CLI invocation of `process_audio_to_file` over the same plugin and parameters.

- Graph render parity vs manual `PluginChain` for linear graphs; vs the parallel-branches `PluginBus` for the fan-out + mix case. These mirror the validation plan in `docs/dev/graph.md`.

- Editor window smoke test: open / close 50× on a known plugin under ASan; assert no leaks.

- v2: RT-allocation test for `mh_graph_render_block` analogous to `test_rt_allocations.py`.

### Distribution

- macOS: `.app` bundle, signed and notarized; `.dmg` for download.

- Windows: portable `.exe` first; installer once configuration paths stabilize.

- Linux: AppImage. Distro packages out of scope until demand exists.

### Telemetry / crash reporting

None in v1. v2 may add opt-in crash reporting if hosted plugins prove to be a routine source of crashes. No usage telemetry either way.

## Crash resilience (decided)

**Decision (for the first public release): ship in-process, document the limitation, add cheap recovery, and gate out-of-process hosting on evidence.**

Hosting third-party plugins in the app's own process means a plugin crash takes down the app. Out-of-process hosting (one helper per plugin or per format) is the standard mitigation, but it adds an IPC seam to every audio block and roughly doubles the engine's complexity. Paying that cost before we know crashes are a real problem for our users is premature. The stance is therefore:

1. **Ship in-process.** The realtime and offline engines both run plugins in-process, as built today. This is the same trust model every plugin loaded into a DAW already lives under.

2. **Document the limitation.** A user-facing note (README + in-app About/first-run) states plainly that a misbehaving plugin can crash the app, and that unsaved graph edits may be lost. This is honesty, not a disclaimer to hide behind — it sets the expectation that matches the architecture.

3. **Make a crash cheap to recover from.** Cheap mitigations that do not require an IPC redesign:
   - *Autosave / crash recovery*: periodically snapshot the working `ProjectDocument` to a sidecar file; on the next launch, offer to reopen the last session. This bounds the blast radius of a crash to "a few seconds of unsaved canvas edits," which is the actual user-visible harm.
   - *Out-of-process plugin scanning*: enumerating an unknown plugin folder is the one place we deliberately run untrusted code before the user asked to host it. JUCE supports scanning in a child process; adopt it when the plugin browser lands (see the deferred TODO) so a plugin that crashes on instantiation-during-scan cannot take down the whole app.
   - *Opt-in crash reporting*: a minimal, opt-in crash dump (no usage telemetry) so we can see whether in-process crashes are actually routine. This is the evidence that would justify — or not — the out-of-process investment.

4. **Revisit out-of-process hosting only on evidence.** If crash reports (or user reports) show that in-process plugin crashes are a routine support burden, escalate to out-of-process hosting for the audio path. Until then it stays deferred (see Phase 2.5+ in the TODO). The decision is reversible: the engine already sits behind the `mh_graph_*` C ABI, so an out-of-process backend can be introduced without rewriting the UI.

## Open questions

- **Project file versioning.** `minihost_project_version: 1` is the obvious lever, but plugin state blobs embedded in projects are versioned by the plugin author, not by us. A project that loads cleanly on one machine may produce different audio on another if a plugin's state format silently changed. Document this; don't try to solve it.

- **Undo/redo granularity.** Per-parameter undo is cheap; per-editor- session undo (grouping a burst of edits from a plugin's own GUI) is harder. v1 ships per-command undo (add node, connect, set parameter from the app's own controls); editor-window edits go into a single coalesced undo entry per editor open/close cycle.

- **macOS sandbox.** Hosting arbitrary VST3/AU code is hostile to App Store distribution; direct download only.

## Validation plan

Before committing to the design, confirm:

1. The C-level graph executor (`mh_graph_*`) can be built from the existing `PluginBus` source with bounded effort. If extending "parallel branches" to a general DAG requires a rewrite rather than an extension, reconsider whether to host the executor in Python (per the original `graph.md` sketch) for v1 and port to C only for v2's realtime path.

2. JUCE's `AudioProcessorEditor` lifetime composes with `MH_Plugin`'s internal locking. Specifically: opening an editor while `mh_process_*` runs on another thread must not deadlock or race. Smoke-test before committing to in-process editor hosting.

3. A throwaway prototype loads one plugin, opens its editor window, tweaks a parameter, and renders 5 seconds of output to a WAV. If that's not working end-to-end in a week, the stack choice (or the scope) is wrong.
