# Desktop App Audit

Status: open; G1-G5, R1-R3 and the parity gap fixed (see CHANGELOG); position A chosen. Date: 2026-11-04, against `cfc1769`. Scope: `projects/minihost_desktop/`, its tests, docs and example projects. Design: [desktop_app.md](desktop_app.md). Per-feature status: [desktop_app_todo.md](desktop_app_todo.md).

Method: read the design docs, README, CHANGELOG and examples. Ran the Python loader on example projects and the desktop test suite. Two code reviews read the C++ (about 11k lines): one on the realtime engine, one on the GUI, project I/O and scanner. Five findings were spot-checked against source (G1, G4, G5, R2, R5); all held. The rest are confirmed by reading or marked plausible. Nothing was reproduced at runtime.

## 1. Value

### The parity claim is false

**Fixed 2026-11-04:** Python accepts every desktop kind; `tests/test_project_desktop_kinds.py` checks bit-identical renders.

README: a graph built in the app "renders identically from the command line". This holds only for a subset of projects.

- The C++ registry has 20 node kinds (`node_registry.cpp`). `src/minihost/project.py:278-401` accepts 10. The rest raise `unknown node kind`.

- Python rejects: `device_output`, `device_input`, `meter`, `gain`, `bus`, `pick_channel`, `merge_channels`, `metronome`, `midi_clock`, `sequencer`.

- `minihost.load_project` fails on examples 01, 02, 04, 06 and 11. Every example has a `device_output`, so none of them load.

- C++ accepts a project whose only output is `device_output` (`project.cpp:396`). Python requires a file `output` (`project.py:404`).

- Python ignores `bpm`, `receives_midi`, `probe` and `name`.

- [desktop_app_todo.md](desktop_app_todo.md) states "same schema, same semantics".

The parity tests pass because they use only the shared kinds.

### Positioning

Two possible positions:

- **A. Visual editor for the Python/CLI pipeline.** Build a graph by ear, batch-render it in Python or CI. No existing tool does this. It requires schema parity.

- **B. Standalone live host.** Competes with Carla, Element and JUCE AudioPluginHost. These are more mature; some already host plugins out of process.

Recent work (sequencer, metronome, MIDI clock, live recording, status bar) moves toward B and toward the design doc's "DAW features" non-goal. Recommendation: A. Under B the app is a smaller copy of existing tools.

### Feature gaps

| Gap | Effect |
|-|-|
| No fan-in latency compensation (`minihost_graph_v2.h:24`) | Parallel FX (example 08) goes out of phase when a branch has lookahead. |
| `automation` is in the design doc schema; neither loader supports it | No time-varying parameters in renders. |
| Metronome is silent offline | Live and offline output differ by design. |
| No canvas zoom, pan, copy/paste, or recent files | Large graphs are hard to edit. |
| No packaging | Only users who build from source can run it. |

## 2. Defects: GUI, project I/O, scanner

### High

- **G1. Plugin library is saved only on clean exit.** `main.cpp:1866` is the only caller of `saveKnownPlugins` (`main.cpp:2089-2098`). A crash after a scan loses the scan. Its write result is ignored (`XmlElement::writeTo(File)` itself is atomic: temp file, then rename). Two instances both set `owns_library_` (`main.cpp:1809`); the last to exit overwrites the other. Confirmed. **Fixed 2026-11-04.**

- **G2. Undo reverts captured state.** These paths mutate the document without `recordUndo`: `captureStateForCanvasPlugin` (`main.cpp:3372`), `syncLiveStateIntoDocument` (`main.cpp:3298-3310`), `fillMissingProbes` (`main.cpp:3472`), the BPM dialog (`main.cpp:3631`). Undo restores a whole snapshot (`canvas.cpp:53-56`). Move a node, Capture State, Cmd+Z: the move is undone and `state_b64` reverts. Outside Live, that state is gone. Confirmed. **Fixed 2026-11-04.**

- **G3. Non-live editor edits are dropped.** Closing the window deletes it (`main.cpp:473-478`, `3254-3262`). State reaches the document only via Capture State. The unsaved-changes prompt (`main.cpp:1898-1907`) ignores open editors. Confirmed. **Fixed 2026-11-04.**

- **G4. Failed recovery deletes the autosave.** `main.cpp:2066-2072` calls `clearAutosave()` in the catch. An autosave written by a newer build with an unknown kind is the only copy of the work, and it is deleted. Confirmed. **Fixed 2026-11-04.**

- **G5. New and Open leave a stale autosave.** `newProject` (`main.cpp:3144-3157`) and `openProjectInCanvas` (`main.cpp:3384-3396`) reset `doc_dirty_` but do not call `clearAutosave`. Edit A, Don't Save, open B, crash before editing B: next launch offers A's discarded edits with A's path, and Save writes them over A. Confirmed. **Fixed 2026-11-04.**

### Medium

- **G6. C++ loader validates less than Python** (`project.py:244-258`, `_require_int_at_least`). Missing checks confirmed; effects plausible.

  - `sample_rate`, `block_size` unbounded (`project.cpp:346-347`). `block_size <= 0` makes `n <= 0`, so `frame` never advances (`project.cpp:~934-946`). Possible hang unless plugin open or prepare rejects it first.

  - `channels` has no minimum or maximum (`node_registry.cpp:165, 407`). Large values allocate huge buffers.

  - `dst_port` cast without a type check (`project.cpp:408`).

  - `duration_seconds` may be negative; `(int) lround` overflows above about 12 h at 48 kHz (`project.cpp:629`).

- **G7. One save makes a project non-relocatable.** Paths are parsed relative to the project file but serialized with `getFullPathName()` (`node_registry.cpp:186, 306, 541, 620`). Confirmed. `dialog_apply` builds `juce::File(text)` from user text (`node_registry.cpp:233`); a relative entry hits JUCE's absolute-path assertion. Plausible.

- **G8. Scanner spawn failure blacklists every plugin.** The `launchWorkerProcess` result is ignored (`plugin_scanner.cpp:113-115`). On failure `sendMessageToWorker` returns false, so `findPluginTypesFor` returns false (`plugin_scanner.cpp:180-185, 205-206`). Every scanned plugin is blacklisted. A malformed XML reply yields `gotResult` with null xml (`plugin_scanner.cpp:133, 225-236`); that plugin vanishes without being blacklisted. Confirmed.

- **G9. Program and Bypass skip the audio guard on live editors.** They call `mh_set_program` / `mh_set_bypass` directly (`main.cpp:396-399, 407-409`). Preset load and Capture use `runStateCall`. `mh_set_program` is documented as safe alongside `mh_process`; bypass is unverified. Plausible.

### Low

- **G10. Capture can silently do nothing.** Lookup is by node id (`main.cpp:3361-3364`). After a rename (`canvas.cpp:1190-1212`), delete, or undo that removes the node, it returns with no message. Confirmed.

- **G11. Duplicate non-live editors.** `openCanvasPluginEditor` (`main.cpp:3216-3262`) does not deduplicate. Two windows on one node overwrite each other's capture. Confirmed.

## 3. Defects: realtime engine

- **R1. Mutex on the audio thread while recording.** `live.cpp:1022` calls `ThreadedWriter::write`, which calls `timeSliceThread.notify()` (`juce_AudioFormatWriter.cpp:264`), which takes `std::lock_guard<std::mutex>` (`juce_WaitableEvent.cpp:71`). One lock per chunk per file output. The comment at `live.h:21-22` ("does no mutex'd work") is wrong. Confirmed. **Fixed 2026-11-04.**

- **R2. FIFO overflow corrupts takes.** `live.cpp:1020-1024`: `all &= write(...)` per recorder. If one of two recorders drops a block, the files are offset by n frames for good. Dropped blocks are not replaced with silence. `recordingBacklog()` (`live.h:170`) uses the first recorder's count and goes wrong after a partial drop, possibly negative. The only report is one stderr line at close (`live.cpp:538`). Confirmed. **Fixed 2026-11-04.**

- **R3. Audio-thread failure is permanent and invisible.** **Fixed 2026-11-04.** `renderBlock` throws `std::runtime_error` on failure (`minihost_graph_v2.hpp:226`), allocating on the audio thread. The catch (`live.cpp:1002-1010`) sets `running_=false` but keeps `compiled_`. Confirmed effects:

  - The menu still shows Live as running (`setLiveRunning` is never cleared). Correction: the status bar reads `isRunning()` and did show Live off.

  - `syncLiveStateIntoDocument` (`main.cpp:3300`) returns early, so uncaptured plugin edits are lost on Restart or Stop.

  - `newProject` / `openProjectInCanvas` (`main.cpp:3148, 3388`) skip `stopLive`, so live editors stay attached.

  - The status-bar toggle (`main.cpp:1753`) calls `startLive` instead of stop.

- **R4. Beat position and tempo disagree.** The plugin playhead sums beats per block (`live.cpp:771`). The sequencer (`sequencer.h:125, 131`), metronome (`project.cpp:185, 242`) and MIDI clock use `pos_samples * bpm_now`. After a mid-play BPM change they drift apart. At a loop wrap (`live.cpp:782`) plugin beats jump to the absolute formula. BPM is read 3 times per chunk (lines 743, 755, 966) and play state twice (742, 967), though the comment at 740 says once. Confirmed.

- **R5. Stop then Play within one callback.** `stopTransport` and `play` (`live.cpp:515, 443`) both run before the audio thread sees `play=false`. `stopped_now` (line 813) stays false: no note release for file MIDI or the sequencer, no MIDI clock Stop/Start. `rewind_pending_` is checked only at callback top (line 726); mid-callback, the next chunk plays from the old position into the new take. Confirmed by reading; needs a fast double action.

- **R6. `held[]` is a bool, not a count** (`live.cpp:35-36`, `live.h:349`). Overlapping same-pitch notes in a `.mid` (on, on, off) clear `held` at the first off. The second voice gets no note-off at wrap or Stop and hangs. Confirmed.

- **R7. Metronome and MIDI clock ignore a wrap inside a chunk.** `live.cpp:970-973` pass only `chunk_pos`. Clicks and ticks after `loop_e` use the pre-wrap grid. No Song Position Pointer at the wrap. Confirmed.

- **R8. Edit restart resets the loop region.** `restartLiveForEdit` -> `startLive` -> `start()` -> `setLoop(0, duration)` (`live.cpp:329`). A narrowed region is lost and the transport rewinds to 0. With `duration_frames == 0` (no file inputs) the loop is disabled while `isLooping()` reports true. Confirmed.

- **R9. `param_queue_` is not drained in `start()`.** `seq_queue_` is (`live.cpp:346-347`). Entries are positional, so stale writes would hit whichever plugin now has that index. Latent: only the selftest produces into it (`main.cpp:2591`). No multi-producer misuse found. Confirmed.

- **R10. Minor races.** `setLoop` does 4 separate stores (`live.h:148-152`); the audio thread can see a mixed region for one chunk (`loop_e > loop_s` guards division by zero). `preview_source_->setLooping` (`live.cpp:432`) writes a plain bool read by the device thread; undefined behaviour, harmless in practice.

Checked and not defects: sample-rate mismatch is flagged as documented; device callbacks larger than the block are chunked correctly; editors close before `stop()` frees plugins (`main.cpp:3442, 3516, 1876`); staged MIDI is cleared after each render.

## 4. Tests

`pytest tests/test_desktop_*.py`: 60 passed, 7 skipped. All 7 skips need a real plugin (macOS AU, Dexed, or `MINIHOST_TEST_PLUGIN`). On Linux no test drives a real plugin through the app.

Untested realtime behaviour:

- Recorder FIFO overflow, including partial drops across outputs. The selftest waits for the backlog to drain (`main.cpp:2516`), so drops never occur.

- `withAudioHeld` overlapping a running callback.

- Device MIDI ring: arrival offsets, the 256-per-callback cap, overflow, the flush in `start()`.

- BPM change mid-play; plugin vs metronome beat consistency.

- Stop then Play within one callback; `rewind_pending_` ordering.

- `setLoop` while playing, and loop spans shorter than one callback.

- Metronome and MIDI clock across a loop wrap.

- The render-exception path and its GUI effect.

- `param_queue_` draining under load and across restart.

- `hasSampleRateMismatch` (needs a real `juce::AudioIODevice`).

- Summing several `device_output` nodes, the output-0 fallback, preview mixing.

- Note-off release when `collectTransportMidi` reaches capacity.

## 5. Documentation drift

- ~~[desktop_app_todo.md](desktop_app_todo.md): "same schema, same semantics" (see section 1).~~ True since the parity fix.

- [desktop_app.md](desktop_app.md) status line lists undo/redo and the plugin browser as deferred. Both exist.

- `examples/projects/README.md`: "Graph edits during live mode take effect only after Stop Live and Start Live". Live now restarts on edit.

- `live.h:21-22`: recorder "does no mutex'd work" (see R1).

## 6. Structure

`main.cpp` is 3,704 lines. `doc_dirty_` and `unsaved_changes_` are set by hand at 9 sites, which causes G2 and G5. Proposed split:

| Lines | Content | Target |
|-|-|-|
| 196-314 | `renderToFile`, `ParamTweakTimer`, watchdog | `editor_probe.cpp` |
| 322-823 | `EditorWindow`, `loadPlugin` | `editor_window.{h,cpp}` |
| 830-1325 | `MainWindow` | `main_window.{h,cpp}` |
| 1336-1445, 2283-2641 | `ProjectRenderJob`, render options and artifacts | `render_controller.cpp` |
| 1461-1660 | CLI dispatch | `cli.cpp`, with a mode table in place of the if-chain |
| 1946-2080, 3074-3160, 3384-3404 | Settings, autosave, recovery, save/new/open | `document_controller.{h,cpp}`, owning both dirty flags |
| 2081-2282, 2981-3072 | Plugin library, browser, picker, scan | `plugin_library.cpp` |
| 2642-2975 | Self-tests | `selftests.cpp` |
| 3164-3640 | Canvas editors, live control, transport dialogs | `live_controller.cpp` |

## 7. Recommended order

1. ~~Choose position A or B.~~ A chosen and done 2026-11-04.

2. ~~Fix G1-G5.~~ Done 2026-11-04.

3. ~~Fix R2 and R3 so failures reach the GUI. Then R1.~~ Done 2026-11-04.

4. Add fan-in latency compensation.

5. Add one plugin-backed test on Linux using a free VST3. Then split `main.cpp`, starting with the document controller.

## 8. Open questions

- Who is the target user: the author, plugin developers, or sound designers?

- Should projects with device nodes render offline with those nodes skipped, or stay a separate project class?

- Out-of-process hosting waits on crash evidence, but opt-in crash reporting does not exist. What evidence would trigger it?
