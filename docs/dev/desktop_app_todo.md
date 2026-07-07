# Desktop App TODO

Tracks work for the `minihost-desktop` binary described in [desktop_app.md](desktop_app.md). Library-level work (CLI, Python bindings, C ABI improvements not specifically driven by the desktop app) lives in the root [TODO.md](../../TODO.md).

Ordered roughly by dependency: items lower in a tier generally depend on items above them.

## Phase 0 - Prove the stack

Throwaway prototype work. If any item here can't be done in ~a day, the stack choice or the scope is wrong and the design doc needs revisiting before further investment.

- [x] **Build a GUI-mode `libminihost`.** Sibling `minihost_gui` static library defined in `projects/libminihost/CMakeLists.txt` alongside the headless `minihost`. Built when `MINIHOST_BUILD_DESKTOP=ON` or `MINIHOST_BUILD_GUI_LIB=ON`. Both archives coexist in a single build tree.

- [x] **`mh_get_juce_processor` C ABI shim.** `void*` accessor in `minihost.{h,cpp}` returning the underlying `juce::AudioProcessor*`. Header stays valid C; only consumers that link JUCE cast.

- [x] **Editor-window smoke prototype.** `projects/minihost_desktop/ src/main.cpp`: loads a plugin via `mh_open`, opens its editor in a `DocumentWindow`, renders 5 s to a WAV via `mh_process_midi`. `--auto-render` flag runs the render-and-quit path non-interactively. Validated with Dexed (5.0 s of 48 kHz / 2 ch / 24-bit PCM, clean exit, editor visible throughout the render).

- [x] **Editor lifetime / locking probe.** `--probe [--iterations=N]` mode in `main.cpp`. Opens the editor, runs N consecutive 5 s renders on a worker, simultaneously drives parameter writes from (a) the message thread via a `juce::Timer` and (b) a dedicated worker thread at high rate. Watchdog-bounded. Validated:

  - Dexed (VST3, synth, 2238 params): 10 iters / 50 s of render, 1715 worker-thread param writes, exit 0.

  - gigaverb (VST3, effect, 8 params): 5 iters, 701 worker writes, exit 0.

  No deadlock, no crash, clean `mh_close` on shutdown with both writers active until just before the close. Editor-thread Timer writes are starved (~0.5 Hz effective) under render load -- not a correctness problem but documents that high-rate UI feedback during heavy DSP may lag.

  Follow-ups (not blocking v1, captured for honesty):
  - [ ] Re-run the probe under ThreadSanitizer. **Explicitly deferred:** needs a TSan build configuration + CI plumbing that's out of scope for in-session work. Schedule when CI machinery exists.

  - [x] **Editor open/close mid-render stress.** `--probe` now toggles the `AudioProcessorEditor` (full ctor/dtor cycle) every 250 ms across the render iterations. Validated on Dexed: 3 iters x ~5 s + ~60 editor cycles, 338 worker-thread param writes, exit 0. No crash, no deadlock against the render thread.

  - [ ] Broaden plugin coverage (AU, LV2; commercial plugins). **Deferred:** test-fixture procurement; the v1 probe runs cleanly on every VST3 we've tried (Dexed, gigaverb).

## Phase 1 - Offline graph renderer with editor windows

### Graph executor in C

- [x] **`mh_graph_v2_*` C API** in `projects/libminihost/` (`minihost_graph_v2.{h,cpp}`). Kahn topological sort, channel-count validation, per-node output buffer pool. Built-in node kinds: `input`, `output`, `mix` (gain skipped -- redundant with mix weights). C++ wrapper `minihost::GraphV2` in `minihost_graph_v2.hpp` (header-only, RAII).

- [x] **Render-block entry point** `mh_graph_v2_render_block` driving `mh_process` per plugin node. v1 uses block-level scheduling (sample-accurate automation handled inside each node by `mh_process_auto` if/when the graph plumbs automation through; topology and buffer pool prove out without it).

- [x] **Python binding `minihost.GraphV2`** in `src/minihost/_core.cpp`. Existing `PluginGraph` unchanged. Plugin refs kept alive via `nb::keep_alive` on `add_plugin`.

- [x] **Parity tests** in `tests/test_graph_v2.py` (15 tests): topology / validation (9), non-plugin numerical parity (4), plugin parity against `Plugin.process_audio` (2). Full suite 576 -> 591 passed, 71 skipped, no regressions.

- [x] **Automation passthrough.** New C ABI `mh_graph_v2_set_node_automation` stages `MH_ParamChange` lists for a plugin node on the next render_block. Plugin nodes with automation set dispatch via `mh_process_auto` (combined with MIDI if both are set; cleared after each render_block). Python binding `GraphV2.set_node_automation(node_id, [(sample_offset, param_index, value), ...])` with per-node scratch storage that outlives Python call boundaries. Parity test `test_graph_automation_matches_plugin_process_auto` asserts the graph path matches `Plugin.process_auto` to within 1e-5.

- [ ] **MIDI routing** -- second scheduling lane for MIDI events between nodes. Defer until a concrete use case appears.

### Application shell

- [x] **`projects/minihost_desktop/` sub-project.** Scaffolded with stub `main.cpp` (exits immediately) and CMakeLists linking `minihost_gui`. Top-level `CMakeLists.txt` adds it via `add_subdirectory` guarded by `MINIHOST_BUILD_DESKTOP` (default OFF).

- [x] **Split `libminihost_audio`** into sibling `minihost_audio` (PUBLIC-links headless `minihost`) and `minihost_audio_gui` (PUBLIC- links `minihost_gui`). Desktop links `minihost_audio_gui`. Python wheel and CLI tools continue to link `minihost_audio` unchanged (576 tests pass).

- [x] **MainWindow + menu bar.** `MainWindow` is a JUCE `DocumentWindow` + `MenuBarModel` with a system menu bar (macOS main menu on Mac, native menu bar elsewhere). File menu: Open Plugin... (async `juce::FileChooser` -> loads the plugin and opens an `EditorWindow`), Quit. Help menu: About. The shell launches with no command-line args and waits for the user. Edit / View / Render menus deferred until ProjectModel / GraphCanvas / RenderDialog exist (no useful commands without them).

- [x] **Multi-window plugin lifetime.** `DesktopApplication` owns an `OwnedArray<EditorWindow>`; each `EditorWindow` owns its `MH_Plugin*` and `mh_close`s in its destructor. `closeButtonPressed` invokes a callback that removes the window from the array. Single-plugin shortcut mode (positional plugin arg, no MainWindow) quits when the last EditorWindow closes. Phase 0 validation modes (`--auto-render`, `--probe`) still reachable behind explicit flags.

- [ ] **`ProjectModel` observable refactor.** **Explicitly deferred:** the current `ProjectDocument` + canvas direct-mutation pattern works. An observable refactor only buys value once undo/redo is on the roadmap; revisit then.

- [ ] **Plugin browser dialog.** **Explicitly deferred:** `File > Open Plugin...` and the canvas's `Add Plugin...` already do file- chooser-driven plugin opening. A persistent scan-paths dialog is quality-of-life polish, not a v1 blocker.

### Node-graph canvas

- [x] **Canvas widget (read-only view + drag).** Lives in `projects/minihost_desktop/src/canvas.{h,cpp}`. Renders a `project::ProjectDocument` as rounded-rectangle nodes with port circles and bezier-curve edges. Auto-layout by topological column (column = max predecessor column + 1) and row (sequence within column). Per-kind colour coding (input = blue, output = rust, mix = olive, plugin = slate). Click to select, drag to reposition. Wired into `MainWindow` via `File > Open Project...`.

- [x] **Connect / disconnect / delete** edit ops. Drag from a node's output port to an input port creates an edge; click an edge near its midpoint to select it; Delete / Backspace removes the selected node (cascading any edges that reference it) or the selected edge. In-progress connect drag draws a dashed bezier preview.

- [x] **Add nodes** (Mix presets, Plugin file chooser, Input via audio-file chooser, Output via save-file chooser). Right-click context menu wires all four kinds. Input takes channel count from the chosen file's actual channel count (via `mh_audio_get_file_info`); sample-rate mismatches surface as an alert. Output defaults to 2-channel / 24-bit; further property editing of existing nodes is deferred to a property-panel slice.

- [x] **Edit existing node properties.** Right-click a node -> Properties... dialog tailored to the kind: input (id, channels, source path), output (id, channels, sink path, bit depth), mix (id, num_inputs, channels, gains), plugin (id; path/state_b64 read-only). For mix nodes, shrinking `num_inputs` cascades: edges with `dst_port >= new_num_inputs` are dropped automatically. ID renames rewrite all referencing edges and the layout map.

- [x] **Channel-count validation at connect time.** Adding a plugin via the canvas now probes it eagerly (`mh_open` + `mh_get_info` + `mh_close`) and caches `probed_in_channels` / `probed_out_channels` on the `PluginNodeSpec`. `addEdgeToDoc` rejects mismatched edges with an alert before they hit the doc. Plugins loaded from disk (no fresh probe) skip canvas-side validation; render-time still catches mismatches via `mh_graph_v2_compile`.

- [ ] **Undo / redo.** **Explicitly deferred:** whole-subsystem scope (command stack, snapshots, coalescing of editor edits). Out of scope for polish; track separately if it becomes load- bearing.

- [x] **Save canvas positions back to disk.** Schema extended with optional `layout: {node_id: {x, y}}`. Loader populates `ProjectDocument.layout`; canvas applies saved positions for known nodes (falls back to auto-layout for any node missing from the map). Drag end (`mouseUp`) writes the new position back to the document; `File > Save Project` serializes it via `project::saveProjectFile` (atomic tmp + rename). Verified: Python load->save round-trip preserves layout; C++ save -> Python load preserves layout byte-for-byte. Three new Python tests pass (`test_load_project_without_layout`, `test_layout_round_trip`, `test_layout_drops_unknown_ids`); full suite 604 -> 607 passed. `minihost_desktop --save-roundtrip=<path>` is the headless parity-test entry point.

### Editor windows

- [x] **`EditorWindow`** `DocumentWindow` subclass owning a transient `MH_Plugin*` + JUCE `AudioProcessorEditor*`. Configurable toolbar via `EditorOptions::toolbar_label` / `toolbar_action`. Used in three call sites: Phase 0 single-plugin shortcut ("Render 5s" button), MainWindow `File > Open Plugin...`, and canvas double-click (new this slice: "Capture State" button).

- [x] **Canvas double-click -> editor.** Double-clicking a plugin node opens a transient editor against a freshly-loaded plugin instance with state restored from `state_b64` if present. The "Capture State" button reads `mh_get_state`, base64-encodes it, and writes back to `doc.plugins[i].state_b64`. `File > Save Project` then persists the new state. Multiple editors can be open simultaneously (each owns its own `MH_Plugin*`).

- [ ] **Parameter mirror.** **Explicitly deferred:** JUCE's editor already shows live values; a host-side mirror only matters for automation-overlay UIs and MIDI-learn workflows, neither of which exist yet.

- [ ] **Editor session as a single undo entry.** **Explicitly deferred** together with the undo/redo subsystem above.

### Project file I/O

- [x] **JSON schema v1** (`minihost_project_version: 1`). Defined once in `src/minihost/project.py` (Python loader, canonical reference) and re-implemented in C++ at `projects/minihost_desktop/src/project.{h,cpp}` for the desktop binary. Same schema, same semantics.

- [x] **Load / save** (Python): `minihost.load_project`, `minihost.save_project` (atomic via tmp + rename). **Load** (C++): `minihost_desktop::project::loadProject` and `parseProjectFile`. Save in C++ deferred until the desktop UI needs to write back (no save-from-canvas yet).

- [x] **Plugin state blob** -- persisted via `mh_get_state` / `mh_set_state`, base64-encoded. Decode supported in both loaders.

- [x] **Round-trip + render parity tests.** Python: `tests/test_project.py` (13 tests). C++ vs Python parity: end-to-end demo renders the same project through both pipelines and observes 0.000e+00 max sample diff -- bit-identical output.

- [x] **`minihost render <project.json>` CLI** (Python entry point). **`minihost_desktop --render-project=<project.json>`** (C++ headless mode; same code path as the GUI menu).

- [x] **File > Render Project... menu** in the desktop shell. Async via `juce::ThreadWithProgressWindow` (modal progress bar with Cancel). Result alert on completion.

- [x] **File > New Project** -- creates an empty `ProjectDocument` (`sample_rate=48000`, `block_size=512`, no nodes/edges) and shows it on the canvas with the title bar reading "(untitled)". Drives the same edit surface (right-click to add nodes, drag ports to connect) as projects opened from disk. **File > Save Project As...** prompts for a path; **Save Project** on an untitled document falls through to Save As automatically and retitles the window on success.

### Render

- [x] **Render dialog.** Async `juce::AlertWindow` shown before `ProjectRenderJob::launch`: bit-depth selector (16/24/32) and normalize-to-dBFS field (0 = off, e.g. -1.0). Picks are threaded through `project::RenderOptions` to `renderProject`.

- [x] **Render thread.** `ProjectRenderJob` is a `juce::ThreadWithProgressWindow`. `run()` calls `loadProject` + `renderProject` block-by-block.

- [x] **Progress + cancel.** Progress wired via `setProgress(done/total)` per block; the progress window's Cancel button flips `threadShouldExit()` which the render loop's progress callback forwards to the `cancel_flag` atomic.

- [x] **Render parity test.** `tests/test_desktop_render_parity.py` (2 tests): C++ desktop `--render-project=...` vs Python `render_project` produces bit-identical WAVs (max sample diff == 0.0). C++ `--save-roundtrip=...` vs Python load preserves layout / edges / channels.

### Packaging

**Explicitly deferred** -- all three need CI machinery + dev certificates that aren't available in-session. The build already produces a runnable `.app` bundle locally (via the CMake APPLE block in `projects/minihost_desktop/CMakeLists.txt`); signing / notarization / installers are CI-time concerns.

- [ ] **macOS `.app` bundle** with code signing + notarization.

- [ ] **Windows portable `.exe`.**

- [ ] **Linux AppImage.**

## Phase 2 - Realtime mode

Adds the audio device callback driver and live MIDI input. Same graph, stricter realtime constraints.

- [x] **Audio device picker.** `LiveEngine` owns a `juce::AudioDeviceManager`. `Audio > Audio Device Settings...` opens a `juce::AudioDeviceSelectorComponent` in a dialog. **Persisted across launches:** settings file at `~/Library/Application Support/minihost/desktop_settings.xml` (or the platform equivalent). Saved on `shutdown()` and applied on `initialise()`. Audio device + MIDI input identifier both round-trip.

- [x] **`AudioIODeviceCallback` driving the graph.** `LiveEngine::audioDeviceIOCallbackWithContext` drains pending GUI commands + transport + MIDI, then calls `compiled_->graph->renderBlock` and copies output node 0 to the device output channels. `Audio > Start Live` / `Stop Live` / `Restart Live` menu items.

- [x] **Allocation-free `mh_graph_v2_render_block`.** The render path uses pre-allocated pool storage from `compile()`; allocation- free for the steady-state caller pattern (planar buffer pointers passed each call don't grow the pool). `tests/test_graph_v2_rt.py` (5 tests): repeated-calls stability, varying nframes, input immutability, output-reuse safety, gain-change-without-recompile.

- [x] **SPSC command queue.** `RtParamQueue<1024>` in `projects/minihost_desktop/src/rt_param_queue.h` (header-only, power-of-two capacity, drop-newest-on-overflow). Producer = GUI thread, consumer = audio callback. Audio thread drains up to 64 commands per block and applies them via `juce::AudioProcessorParameter::setValue` (RT-safe; does not acquire `mh_set_param`'s mutex).

- [x] **Topology swap under mute.** `LiveEngine::start` always calls `stop()` first, which detaches the audio callback before mutating `compiled_`. Switching project files in the canvas (`File > Open Project`) also calls `stop()` on the engine before parsing. Audible click is acceptable per the design.

- [x] **Transport bar (BPM + loop region).** `LiveEngine::setTransportPlaying`, `setBpm`, `setLoop(start, end, is_looping)`. Audio thread builds an `MH_TransportInfo` from atomic bpm/playing/loop state + audio-thread-owned sample/beat counters and pushes it to every plugin via `mh_set_transport` before each block. Position wraps inside `[loop_start, loop_end)` when looping is enabled. Menu items: `Audio > Transport: Play`, `Stop`, `Set BPM...`, `Set Loop Region...`.

- [x] **Live MIDI input.** `juce::MidiInputCallback` on `LiveEngine`; lock-free ring buffer (`midi_ring_`, capacity 1024) bridges OS MIDI thread -> audio thread. Audio callback drains up to 256 events per block and fans them out to plugin nodes via `mh_graph_v2_set_node_midi`. `Audio > MIDI Input...` shows a PopupMenu listing `juce::MidiInput::getAvailableDevices()`. **Per-plugin destination routing:** new `receives_midi` field on `PluginNodeSpec` (default true). LiveEngine skips plugins with `receives_midi = false` during fan-out. Toggle exposed in the per-node Properties... dialog.

- [x] **`mh_graph_v2_set_node_midi` C ABI extension.** Stages MIDI events to deliver to a plugin node on the next render_block. Plugin nodes with pending MIDI dispatch via `mh_process_midi`; nodes without dispatch via `mh_process` as before. Pending events are cleared after every render_block (caller must re-stage every block). Caller-owned pointer; serializes with render_block via the audio thread's single-callback contract.

## Phase 2.5+ - Deferred

Items the design doc lists as deferred. Surfaced here so they're not forgotten, but not scheduled.

- [ ] **Latency compensation across fan-in paths.** Insert per-path delay at fan-in points using `mh_get_latency`.

- [ ] **Feedback loops** with a one-block delay node on the back-edge.

- [ ] **Sidechain input buses.** Blocked on a C ABI gap (see `docs/dev/graph.md` open questions).

- [ ] **Out-of-process plugin hosting** for crash containment. Decide only if v1/v2 testing shows in-process crashes are routine.

- [ ] **Opt-in crash reporting.** No usage telemetry either way.

## Non-goals

Mirrors the design doc; restated here so PRs that drift toward these get a one-link rejection:

- DAW features (timeline, clip editing, mixer automation curves drawn in-app, time-stretching, take comping).

- Preset browser UI beyond what `read_vstpreset` / `set_state` give us.

- MIDI learn / parameter mapping UI.

- Plugin shell disambiguation.

- Cross-graph routing (every project is exactly one graph).

- App Store distribution (incompatible with hosting arbitrary plugin code).
