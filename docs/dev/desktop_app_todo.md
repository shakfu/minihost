# Desktop App TODO

Tracks work for the `minihost-desktop` binary described in
[desktop_app.md](desktop_app.md). Library-level work (CLI, Python
bindings, C ABI improvements not specifically driven by the desktop
app) lives in the root [TODO.md](../../TODO.md).

Ordered roughly by dependency: items lower in a tier generally depend
on items above them.

## Phase 0 - Prove the stack

Throwaway prototype work. If any item here can't be done in ~a day,
the stack choice or the scope is wrong and the design doc needs
revisiting before further investment.

- [x] **Build a GUI-mode `libminihost`.** Sibling `minihost_gui`
  static library defined in `projects/libminihost/CMakeLists.txt`
  alongside the headless `minihost`. Built when
  `MINIHOST_BUILD_DESKTOP=ON` or `MINIHOST_BUILD_GUI_LIB=ON`. Both
  archives coexist in a single build tree.
- [ ] **`mh_get_juce_processor` C ABI shim.** Opaque-pointer accessor
  returning `juce::AudioProcessor*` from an `MH_Plugin*`. Header
  guards so headless consumers don't see the JUCE type.
- [ ] **Editor-window smoke prototype.** Standalone JUCE app: load
  one plugin via `mh_open`, open its editor in a `DocumentWindow`,
  tweak a parameter, render 5 s to a WAV via `mh_process_*`. Throwaway.
- [ ] **Editor lifetime / locking probe.** Open an editor while a
  render thread is calling `mh_process_*` on the same plugin instance.
  Confirm no deadlock or race against `libminihost`'s internal mutex.
  This is the validation-plan item 2 in the design doc.

## Phase 1 - Offline graph renderer with editor windows

### Graph executor in C

- [ ] **`mh_graph_v2_*` C API** in
  `projects/libminihost_graph/`. Extends the existing parallel-
  branches `PluginGraph` to a general DAG: Kahn topological sort,
  channel-count validation, edge-buffer pool with liveness reuse.
  Built-in node kinds: `input`, `output`, `gain`, `mix`.
- [ ] **Render-block entry point** `mh_graph_v2_render_block` driving
  `mh_process_auto` per node. Allocation-free in the steady state
  (v2 will tighten this with an RT-allocation test; v1 only requires
  steady-state, not strictly RT).
- [ ] **Python binding `minihost.GraphV2`** wrapping the new ABI.
  Existing `PluginGraph` stays as-is. Used by the desktop app's
  project loader and exposed for scripting.
- [ ] **Parity tests** against `PluginChain.process_auto` for linear
  graphs and against `PluginGraph` for the fan-out + mix case.
  Mirrors the validation plan in `docs/dev/graph.md`.

### Application shell

- [x] **`projects/minihost_desktop/` sub-project.** Scaffolded with
  stub `main.cpp` (exits immediately) and CMakeLists linking
  `minihost_gui`. Top-level `CMakeLists.txt` adds it via
  `add_subdirectory` guarded by `MINIHOST_BUILD_DESKTOP` (default OFF).
- [x] **Split `libminihost_audio`** into sibling `minihost_audio`
  (PUBLIC-links headless `minihost`) and `minihost_audio_gui` (PUBLIC-
  links `minihost_gui`). Desktop links `minihost_audio_gui`. Python
  wheel and CLI tools continue to link `minihost_audio` unchanged
  (576 tests pass).
- [ ] **MainWindow + menu bar.** File / Edit / View / Render menus,
  no-op handlers wired but unimplemented.
- [ ] **`ProjectModel`** observable type owning the graph state. All
  GUI mutations go through commands for undo.
- [ ] **Plugin browser dialog.** Wraps `mh_scan_directory`; remembers
  scanned paths across launches in a config file.

### Node-graph canvas

- [ ] **Canvas widget.** Nodes as draggable rectangles, ports as
  circles, edges as bezier curves. No fancy layout - manual placement
  is fine for v1.
- [ ] **Add / remove / connect / disconnect** edit ops, each as an
  undoable command.
- [ ] **Channel-count validation** at connect time, with a visible
  error state on invalid edges.
- [ ] **Selection + delete** for nodes and edges.

### Editor windows

- [ ] **`EditorHostWindow`** `DocumentWindow` subclass owning an
  `AudioProcessorEditor*`. One per opened plugin instance.
- [ ] **Parameter mirror.** Listen on `mh_register_param_listener`
  for the plugin instance; reflect changes in the app's parameter
  view without polling.
- [ ] **Editor session as a single undo entry.** Coalesce all
  parameter writes between open and close into one undo command, per
  the design doc's open question resolution.

### Project file I/O

- [ ] **JSON schema v1.** Matches the design doc's example.
  `minihost_project_version: 1`.
- [ ] **Save / load** with atomic write (write to `*.tmp`, rename).
- [ ] **Plugin state blob** persisted via `mh_get_state` /
  `mh_set_state`, base64-encoded.
- [ ] **Round-trip test.** Load -> save -> byte-identical after
  canonicalization.

### Render

- [ ] **Render dialog.** Output path, format (WAV / FLAC), bit depth,
  normalize-to-dBFS toggle. Reuses the CLI's render options.
- [ ] **Render thread.** Spawns on Render action, runs
  `mh_graph_v2_render_block` in a loop, writes via streaming
  `mh_audio_write`.
- [ ] **Progress + cancel.** Atomic frame counter polled at 30 Hz by
  the GUI; atomic cancel flag checked at the top of each block.
- [ ] **Render parity test.** A one-plugin project renders bit-
  identically to a CLI `process_audio_to_file` over the same plugin
  and parameters.

### Packaging

- [ ] **macOS `.app` bundle** with code signing + notarization. CI
  job behind a manual trigger.
- [ ] **Windows portable `.exe`.** Installer deferred until
  configuration paths stabilize.
- [ ] **Linux AppImage.** Distro packages explicitly out of scope.

## Phase 2 - Realtime mode

Adds the audio device callback driver and live MIDI input. Same
graph, stricter realtime constraints.

- [ ] **Audio device picker.** Wraps `juce::AudioDeviceManager`;
  persists choice across launches.
- [ ] **`AudioIODeviceCallback` driving the graph.** The callback
  calls `mh_graph_v2_render_block` and copies the output node(s) into
  the device output buffer.
- [ ] **Allocation-free `mh_graph_v2_render_block`.** Tighten v1's
  steady-state-only guarantee to strict RT-safety. RT-allocation test
  analogous to `tests/test_rt_allocations.py`.
- [ ] **SPSC command queue.** Lock-free, bounded, drop-newest-on-
  overflow. Used for parameter writes from the GUI to the audio
  thread and for topology-swap commands.
- [ ] **Topology swap under mute.** Stop device callback, swap
  `CompiledGraph`, restart. Audible click during swap is acceptable;
  pop-suppression is a nice-to-have.
- [ ] **Transport bar.** Play / stop / loop region / BPM. Drives
  `MH_TransportInfo` per block.
- [ ] **Live MIDI input.** `juce::MidiInput` per selected device,
  routed via a ring buffer to designated plugin nodes. Designation
  UI = right-click a plugin node, "MIDI input from {device}".

## Phase 2.5+ - Deferred

Items the design doc lists as deferred. Surfaced here so they're not
forgotten, but not scheduled.

- [ ] **Latency compensation across fan-in paths.** Insert per-path
  delay at fan-in points using `mh_get_latency`.
- [ ] **Feedback loops** with a one-block delay node on the back-edge.
- [ ] **Sidechain input buses.** Blocked on a C ABI gap (see
  `docs/dev/graph.md` open questions).
- [ ] **Out-of-process plugin hosting** for crash containment. Decide
  only if v1/v2 testing shows in-process crashes are routine.
- [ ] **Opt-in crash reporting.** No usage telemetry either way.

## Non-goals

Mirrors the design doc; restated here so PRs that drift toward these
get a one-link rejection:

- DAW features (timeline, clip editing, mixer automation curves drawn
  in-app, time-stretching, take comping).
- Preset browser UI beyond what `read_vstpreset` / `set_state` give us.
- MIDI learn / parameter mapping UI.
- Plugin shell disambiguation.
- Cross-graph routing (every project is exactly one graph).
- App Store distribution (incompatible with hosting arbitrary plugin
  code).
