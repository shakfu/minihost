# Changelog

## [Unreleased]

### Added

- **`minihost touch`: a control surface generated from a plugin's parameters.** Writes `<name>.ui.json` (a layout in the `py2tosc.ui` dialect) and `<name>.map.json` (a `--map-file` mapping) from one parameter table, and compiles a `.tosc` when the optional `minihost[touch]` extra is installed. Widget choice comes from metadata minihost already exported and the flat `py2tosc.surface` path discards: `is_boolean` becomes a button, `num_steps` a radio, everything else a fader.

  Rendering both artefacts from the same rows is the reason to do this here rather than pipe two command-line tools: the layout and the host's mapping cannot disagree. A test asserts the controller numbers in the two files match, and another loads the generated map back through `--map-file`.

  Targeting `ui_json` rather than emitting a `.tosc` directly means **generation imports nothing**. `touch.py` writes JSON text; py2tosc is needed only to compile. Absent, the command still produces a complete, valid description and prints how to compile it, rather than failing and producing nothing. It also makes the output a source file rather than an artefact -- a `.tosc` is a zipped XML blob nobody hand-edits, so a generator that emits one owns every layout decision forever.

  Widget variety needs a branch table, because substitution in that dialect reaches values and never keys, so one `each` would otherwise build one kind of control. The `of: {case, when}` form py2tosc 0.5.2 added at minihost's request is exactly this, and it is why the envelope stamps `"schema": 2`. There are two arms per widget kind rather than three total: a parameter past the 128th has no controller number left and `each` cannot conditionally include a message, so the row selects a branch that has no `midi_cc` binding at all. A branch nothing selects is not an error, so a surface under 128 parameters never reaches those arms.

  Parameter order is preserved, which is what ruled out the simpler shape of one `each` per widget kind -- grouping by widget regroups the surface and throws away an ordering the plugin author chose. CC exhaustion is printed rather than silent; py2tosc's own flat path lets the remainder go OSC-only without saying so, which for a 300-parameter plugin is a discovery rather than a decision.

  `tests/check_json.py` is vendored from py2tosc 0.5.2 and every generated description goes through it. It is one stdlib-only file published for projects that *write* these descriptions, so it costs no dependency and catches what a golden file cannot: a key nothing reads, silently ignored, so a typo drops a subtree while the output still looks correct. Recorded in `docs/vendored.md` as a test-only vendoring.

- **A host playhead for the live device.** `grep -n transport projects/libminihost_audio/minihost_audio.c` used to return nothing: the realtime device never called `mh_set_transport`, so a tempo-synced delay, an arpeggiator or an LFO running under `minihost play` saw no host tempo and a playhead pinned at sample 0. Offline renders had one; realtime did not. New `mh_audio_set_transport_enabled`, `mh_audio_transport_play` / `_stop` / `_set_bpm` / `_set_time_sig` / `_set_position` / `_set_loop` / `_set_recording` and `mh_audio_get_transport`, exposed on `AudioDevice`. Off by default, so a device that does not ask behaves exactly as before.

  The audio thread owns the transport and is its only writer; control threads post to a lock-free command ring it drains at the top of each block. A ring rather than a set of atomics for two reasons: this project deliberately avoids C11 `<stdatomic.h>` because MSVC gates it behind a flag the Visual Studio generator does not reliably pass, and a transport is a state machine whose updates want to be ordered rather than independently torn -- setting tempo and position together should not be observable half-applied. `position_beats` is derived from position and tempo rather than commanded, because two sources of truth for one instant is how a playhead ends up disagreeing with itself. The loop wrap is a modulo, not a subtract, so a loop shorter than one block still lands back in range.

  A chain device hands the playhead to every plugin in the chain, resolved once at open: `mh_chain_get_plugin` is a thread-safe (locking) accessor, and calling it per block would put a mutex on the audio path for a membership that is fixed when the chain is created.

  OSC drives it too, parsed in C alongside the parameter addresses: `/mh/transport/play`, `/stop`, `/bpm`, `/position` (in beats, converted to samples), `/loop`, `/record`. A surface button sends 1.0 on press and 0.0 on release, so a zero argument is ignored rather than treated as a command -- otherwise every press would be a press-and-undo.

- **OSC feedback: parameter values back out to a surface.** `OscFeedback` polls the parameters a mapper has bound and sends the ones that changed, so a surface tracks preset loads and anything else that moves a parameter instead of showing where the finger last left it. `minihost play --osc-feedback HOST:PORT`.

  Polling rather than hooking `mh_set_param_value_callback`, which looks like the obvious answer: that callback is a single slot the Python `Plugin` binding already occupies, so taking it would silently break any caller who wanted their own; it fires on whatever thread changed the parameter, including the audio thread, so anything hung off it must be lock-free all the way to the socket; and a surface cannot use more than about 30 updates a second anyway, so its precision would have to be rate-limited back down to what polling produces directly.

  Echo suppression uses the source identity Phase 2 put in the shared core: a parameter the mapper wrote within the last 150 ms is skipped, because during a drag the echo arrives a frame behind the finger and fights it. Suppression delays rather than drops, so the surface still converges once the window closes.

- **14-bit MIDI CC pairs.** `MidiMapper.map_cc14(channel, cc, param, ...)` pairs controller `cc` (0-31, high 7 bits) with `cc + 32` (low 7 bits) for 16384 steps instead of 128. A plain CC gives 128 steps across a parameter's whole range, which is audibly stepped on a filter cutoff. `minihost play` gained `--map14`, with the same grammar as `--map`; the map file accepts a `"cc14"` key as an alternative to `"cc"`, chosen over a `"bits": 14` modifier so there is one key to read and no invalid combination to validate.

  **The dispatch rule departs from what the plan specified, and the plan was wrong.** It called for resetting the cached LSB to zero on each MSB. That produces a sawtooth: a controller re-sending an unchanged `(MSB, LSB)` pair -- which is what a controller sending full pairs does on every message -- would emit `msb << 7` and then `(msb << 7) | lsb` forever, oscillating by up to 1/128 of the range at message rate. The plan argued Phase 0's coalescing hides the transient, which holds only when both halves land in the same audio block; a pair takes roughly 2 ms on the wire against a 5.3 ms block at 48 kHz/256, so usually but not reliably. Keeping the last LSB instead means an unchanged pair emits an unchanged value, with a worst case of a stale fine position for the microseconds until the LSB arrives -- at most one coarse step of error, never a periodic wobble. There is a test whose only job is to fail if that oscillation returns.

  An LSB arriving before any MSB is held rather than emitted, a case the plan did not cover: alone it reads as `msb = 0` and would slam the parameter to the bottom of its range.

  Overlap between the two kinds is rejected at map time in both directions -- `map_cc14` refuses a pair whose MSB or LSB is already a plain CC, and `map_cc` refuses either half of an existing pair. Without that check a stray `map_cc` on `cc + 32` silently shadows the LSB, and the symptom is a fader that moves in coarse steps with nothing anywhere to say why.

- **`OscMapper`, and a shared resolution core under both mappers.** `control.py` gained `OscMapper` alongside `MidiMapper`, callable with the `(address, args)` signature `OscServer` delivers. `map_address` binds one address to one parameter with a range and curve; `bind_all` binds every automatable parameter at once under a prefix, addressed by slugged name -- which is what a generated surface wants, and what lets the layout and the host agree without a hand-written table in between. `minihost play` gained `--osc-port` and `--osc-prefix`.

  The visible difference from `MidiMapper` is resolution: a 7-bit CC gives 128 steps, audibly stepped on a filter cutoff, where OSC carries float32 and is not quantized at all.

  The structural change matters more. Both mappers now share `_Binding`, and it is deliberately keyed on **a normalized float in 0..1 plus a source identity**, not on CC numbers or OSC addresses -- each transport converts at its own edge, MIDI dividing by 127 and OSC passing its float32 through. That keeps curve and range logic in one place (a test asserts both mappers produce identical values for every curve), makes Phase 3's 14-bit CC a change to one divisor, and makes a Web MIDI or WebSocket back end an adapter rather than a rewrite. The `source` argument is carried but unused today, because the feedback direction needs it to avoid echoing a value back to the surface that just sent it, and threading it through later would mean touching every call site.

  Wildcard addressing is supported and **delegated to JUCE** via a new `mh_osc_address_matches` rather than reimplemented in Python, so both ends of a connection agree on the OSC pattern rules by construction instead of by two implementations happening to concur. An exact address is a dict lookup; only a miss containing `*?[]{}` pays for matching, and a pattern writes every parameter it matches, since addressing a page at once is what a pattern is for.

  `bind_all` binds the numeric form (`/mh/param/3`) alongside the name by default, so one port accepts what `AudioDevice.connect_osc` parses natively -- the alternative being a surprise, where a sender written against `connect_osc` silently does nothing through the mapper. Duplicate parameter names are numbered exactly as `py2tosc.surface.unique` does, because plugins really do expose three parameters called "Bypass" and two sharing one address makes the second unreachable. `minihost.slug` mirrors `py2tosc.surface.slug`, with a test asserting the two agree on a corpus of awkward names whenever py2tosc is installed -- a divergence there is a silently dead control.

- **`minihost play` now routes MIDI parameter writes through the device ring.** Phase 0 gave `MidiMapper` somewhere better to write than `Plugin.set_param`, and nothing in the CLI was using it: the mapper is now bound to the running `AudioDevice`, so a CC no longer takes the plugin's state mutex on the MIDI thread while the audio thread is inside `processBlock`.

- **OSC input and output, on juce_osc rather than a vendored library.** `projects/libminihost_audio/minihost_osc.{h,cpp}` sits beside `minihost_midi` because it is the same kind of thing: a transport for control data arriving from outside the process. `MH_OscServer` / `MH_OscClient` in C, `minihost.OscServer` / `minihost.OscClient` in Python, both shaped like the existing `MidiIn` -- factory, context manager, non-movable because the C layer stores the object's address as callback user data.

  The library choice was between embedding liblo and using what is already in the tree. `thirdparty/JUCE/modules/juce_osc` ships with the JUCE the build already downloads, depends only on `juce_events` (which `juce_audio_processors_headless` already pulls in), and declares the same licence as every other JUCE module minihost links -- so it adds no dependency at all, and `docs/vendored.md` is untouched. liblo would have been the first non-permissive entry beside MIT-0 / BSD-0 / BSD-2, wants autotools or its less-exercised CMake path, and is weakest on Windows, where minihost ships wheels. The full comparison is in `docs/dev/osc_and_touch.md`.

  Listeners are registered with `RealtimeCallback`, so messages are delivered straight from the socket thread and never touch a JUCE message loop -- which matters because the Python wheel does not run one. Two limits are stated in the header rather than discovered later: UDP only (no TCP, no SLIP), and bundle time tags are parsed but not scheduled, since JUCE delivers bundle contents immediately.

  Three details came out of building against the real API rather than the documented shape. `OSCReceiver::connect(port)` reports only success and keeps its socket private, so a server opened on port 0 could never say which port it got; the socket is now bound directly and handed over with `connectToSocket`, which is what makes `port=0` usable and the test suite independent of fixed ports. `OSCAddress` has no static validator -- it throws `OSCFormatError` from its constructor -- so validation means attempting, and every entry point taking an address funnels through one guard that stops a C++ exception crossing back into C. Non-numeric OSC arguments are reported as `0.0` rather than skipped, because skipping would shift every later index and deliver a surface's value at a different position to different receivers.

- **`AudioDevice.connect_osc`: OSC to plugin parameters with no Python in the path.** `/mh/param/<index>` and `/mh/<slot>/param/<index>` are parsed in C on the socket thread and pushed straight onto the control parameter ring, so the path takes neither a lock nor the GIL. The alternative -- receiving through `OscServer` and calling `send_param_control` -- pays a GIL acquisition per message, which is the wrong shape for a fader being dragged.

  Numeric addressing only, deliberately: resolving a parameter *name* means a name table and a lock, and the socket thread must have neither. Name handling belongs in the mapping layer above, which resolves once at bind time and sends numerically. Unrecognised addresses are ignored, and the tests cover the near-misses that matter -- the failure worth guarding is not a crash but a silent misroute, an address that parses as some other index and moves the wrong control.

- **Live parameter writes go through a lock-free ring instead of racing `processBlock`.** `MidiMapper` reached a parameter by calling `Plugin.set_param`, which takes the plugin's state mutex and calls `setValueNotifyingHost` -- while the audio callback was in `mh_process_midi_io`, which by contract takes no lock at all. Nothing ordered the two. A control write therefore landed at an undefined point inside the block, and the MIDI thread could block behind an offline caller holding that mutex. The sample-accurate entry points that fix this, `mh_process_auto` and `mh_chain_process_auto`, already existed; the live path simply never used them.

  `MH_AudioDevice` now carries two parameter rings and drains both at the top of the callback into one coalesced array, which it hands to the `_auto` entry points. Two rings rather than one for the reason the MIDI pair already documents: `param_ctl_buffer` belongs to whichever thread drives a control surface, `param_send_buffer` to application code, and a single-producer ring with two producers corrupts its indices. New C entry points `mh_audio_send_param` and `mh_audio_send_param_control`, exposed as `AudioDevice.send_param` / `send_param_control`; `MidiMapper` takes a `device=` argument (and a `bind_device()` setter) and routes through the control ring when bound, keeping the direct `set_param` path for offline use where there is no audio thread to race.

  The drain coalesces per `(plugin_index, param_index)`, which is a correctness requirement and not an optimisation: `mh_process_auto` splits the block at every distinct change offset, so handing it every intermediate value of a fader drag would turn one block into hundreds of sub-blocks. Measured with the plugin's own parameter-value listener, which fires once per applied change: **200 writes to one parameter reach the processor as 1**. Delivery is at block start (`sample_offset` 0) for now, which costs nothing over the previous path -- both `_auto` implementations apply every change at or before the current sample *before* computing the chunk boundary, so N changes at offset 0 produce exactly one chunk, not N.

  Overflow defers rather than drops. The first implementation discarded a change that did not fit the drain array, which the TSan harness caught immediately: nothing upstream resends, so a discarded value is lost for good and the parameter sits at a stale setting. The drain now stops at the first change it cannot place and leaves it queued for the next block. `tests/tsan/ringbuffer_stress.cpp` gained a parameter section asserting the properties that actually hold under coalescing -- no torn triples, no parameter twice in one drain, and every parameter converged on the producer's last value -- deliberately driven with more parameters than the drain array holds so the overflow branch is exercised.

### Fixed

- **Closing a busy `OscServer` or `MidiIn` deadlocked.** `OscServer.close()` joins the OSC socket thread while holding the GIL, and that thread needs the GIL to run the Python callback -- so close waited for a thread that was waiting for close. A hard deadlock, not a slow path, and `MidiIn.close()` had the same shape against the libremidi input thread.

  It hid because the original test for close sent a single message, so the socket thread was reliably idle by the time close ran. It surfaced the moment a feedback test pushed 158 values through in one burst. Both now release the GIL across the join, and there is a regression test that closes with 300 messages still in flight.

- **Setting any Python callback leaked the plugin, permanently.** `plugin.set_param_value_callback(lambda i, v: ...)` at module scope leaked the `Plugin`, its native instance and the loaded plugin bundle for the life of the process. The cycle is ordinary rather than exotic: a callable defined at module scope reaches its module's `__dict__` through `__globals__`, that dict normally holds the plugin, and the plugin holds the callable in a C++ member. nanobind types carry no `tp_traverse` by default, so Python's collector cannot see an edge held in C++ -- the cycle was invisible and therefore uncollectable. Confirmed by contrast: a builtin callback (`print`, which has no `__globals__`) never leaked, and clearing the callback never leaked.

  Six types needed the fix, not one. Fixing `Plugin` alone left the original reproduction still leaking, because `AudioDevice` pinned its plugin with `nb::keep_alive`, which records the edge in nanobind's internal `keep_alive` table -- a side map the collector cannot walk either. The same applied to `PluginChain` (its plugin list), `PluginBus.add_branch` and `PluginGraph.add_plugin`. Each `keep_alive` was *replaced* by a traversable `nb::object` member rather than supplemented, since keeping both leaves the invisible edge in place; `MidiIn` holds its callback the same way and got the same treatment. All six now supply `Py_tp_traverse` / `Py_tp_clear`, which is what makes nanobind set `Py_TPFLAGS_HAVE_GC` on the type and put its instances in front of the collector at all.

  Two details the implementation turns on. Instances are GC-tracked from allocation, which is *before* the C++ constructor runs, so every traverse guards on `nb::inst_ready` rather than reading uninitialised storage. And `tp_clear` closes the device, chain or graph before dropping the reference, because the audio thread reaches the plugin through a raw pointer -- using the C entry point directly rather than the raising wrapper, since an exception must not escape a GC pass.

  `tests/test_callback_gc.py` covers it, including that the lifetime guarantee did not weaken: dropping the caller's reference, forcing collection, then actually starting the device and processing. The first version of those tests was worthless and passed against a deliberately unfixed build -- `del holder` on a closed-over variable empties the closure cell, dismantling the cycle by hand before the collector ever ran. Rewritten to build each cycle inside a helper and leave it intact, they fail correctly on the unfixed build. A direct `Py_TPFLAGS_HAVE_GC` assertion sits alongside them so a regression that drops the slots fails loudly rather than silently voiding every other test in the file.


## [0.7.2]

### Testing

- **A Native CLI workflow, because CI only ever built the native binaries in Release.** `build-cli` compiles `minihost_c` and `minihost_cpp` on three platforms and runs the CLI suite against them, but Release compiles out every `assert()` and licenses the optimizer to delete observable undefined behaviour, so a whole class of defect in the CLIs and `libminihost` was invisible to it. `.github/workflows/native.yml` adds the two configurations that can see those defects: a Debug matrix across macOS, Linux and Windows gating pushes and PRs that touch the native sources, and an ASan+UBSan job on Linux running weekly and on demand.

  The sanitizer job is kept off the PR path deliberately -- it is a fully instrumented rebuild of JUCE and `libminihost`, far too slow to sit in front of every change, and its value is finding latent bugs rather than guarding a specific diff. Both jobs live outside the Build workflow for the reasons `tsan.yml` already documents: the binaries they produce must never ship, and neither should be able to block a release by failing on something unrelated to the published artifacts.

  Build flags, sanitizer options and test selection live in `make cli-debug` / `make cli-asan` rather than in the YAML, so a developer runs exactly what CI runs and the two cannot drift. Leak detection is off by default: JUCE holds deliberately-immortal singletons to process exit, so LeakSanitizer reports them every run and would bury the real findings; turning it on (`make cli-asan ASAN_DETECT_LEAKS=1`) needs a suppression file first. clang is pinned in the sanitizer job for the same reason `tsan.yml` pins `clang++` -- it is the toolchain the flags were verified against, and a gcc failure would be ambiguous between a real bug and a different sanitizer.

  Both configurations were built and run before the workflow was written, cold from an empty build directory. Neither reports anything today, so the jobs start green rather than being born red; the ASan runtime is confirmed genuinely linked in (46 `__asan` symbols, 14 `__ubsan` handlers) rather than silently dropped by the build.

### Fixed

- **`tests/test_release_version.py` could not see the native binaries on Windows, and broke any CI job without the wheel.** Two defects in the guard added in 0.7.1. It hardcoded the single-config binary layout (`build/projects/<name>/<name>`), which the Visual Studio generator does not produce -- so on the one platform where the layout differs it *skipped* rather than failed, quietly losing exactly the coverage it exists to provide. It also did `import minihost` at module scope, which turns any job that installs pytest without building the wheel into a collection error, taking the binary checks down with it.

  The locator that `test_cli_conformance.py` already had -- it knows the `Release/<name>.exe` layout and honours `MINIHOST_C_BIN` / `MINIHOST_CPP_BIN` -- moved to `tests/cli_helpers.py` and is now shared by both, following the `desktop_helpers.py` convention rather than being copied and left to drift. The package import became lazy, so the pyproject/CHANGELOG/template/binary checks all run with only pytest installed and the two package-dependent cases skip. Verified against a bare virtualenv holding nothing but pytest: 5 pass, 2 skip.

## [0.7.1]

### Changed

- **The version was only consistent at the packaging layer; the shipped binaries could not report it.** CI already derived every artifact name from the `version` field in `pyproject.toml`, so a release *archive* was correctly stamped -- but nothing inside it was. `minihost_c` and `minihost_cpp` had no `--version` flag at all, and the desktop app's `getApplicationVersion()` returned a hardcoded `"0.0.0"`, which is also what landed in the macOS bundle's `CFBundleShortVersionString`. Given a binary on disk there was no way to tell which release it came from, and the desktop app actively claimed the wrong answer.

  `pyproject.toml` is now the single source of truth for everything, not just the archive names. The top-level `CMakeLists.txt` resolves the release version once -- from `SKBUILD_PROJECT_VERSION` for wheel builds, by parsing the same `version = "..."` line for standalone builds -- and generates `minihost_version.h` from `cmake/minihost_version.h.in`. An INTERFACE target carries the include directory, and the three executables link it. Both CLI binaries and the Python CLI gained a `--version` flag reporting the release version and the linked library's C ABI version; the desktop app returns the real string and sets both macOS bundle version keys from it. The generated header is listed in `CMAKE_CONFIGURE_DEPENDS`, so a version bump re-runs configure with no other edit.

  The long `--version` is deliberate in all three: `-V` is already `--verbose` in `minihost_c`, and the three CLIs are kept spelled the same way rather than one of them differing.

  `MH_API_VERSION_*` in `minihost.h` is untouched and stays a separate axis -- it versions the C ABI, is bumped when struct layouts or entry points change, and is currently 2.8.0 against a 0.7.1 release. Merging the two would force an ABI bump on every release and vice versa.

  The one remaining hand-maintained copy is `__version__` in `src/minihost/__init__.py`, kept a literal so importing the package costs no metadata lookup.

### Fixed

- **Reopening the desktop Plugin Browser segfaulted.** Scanning worked, but closing the browser and opening it again took the app down. The window was launched with `DialogWindow::LaunchOptions::launchAsync()`, which enters the modal state with `deleteWhenDismissed = true` -- so the window frees *itself* on dismissal, and the `std::unique_ptr` the app held it in was left dangling the moment you closed it. The reopen path checked that pointer for null, found the stale address, and called `toFront()` on freed memory; `shutdown()` then double-freed the same block. The chain is entirely in JUCE and runs on both the close button and Escape: `closeButtonPressed` calls `setVisible(false)`, `ModalItem::componentVisibilityChanged` sees the window is no longer showing and cancels the modal item, and `~ModalItem` deletes the window because `autoDelete` is set.

  The window is now tracked by a non-owning `juce::Component::SafePointer`, which nulls itself when JUCE performs that delete, so the reopen guard sees an empty slot and launches a fresh browser. `shutdown()` deletes the window only if it is still on screen, which cannot double-free because `Component::~Component` makes the modal manager drop its auto-delete claim first. The scan itself was never implicated -- it is simply what makes you close and reopen the window.

- **Instruments were named `fx_N` on the canvas.** Adding Surge XT produced `fx_1` while its effects build produced `fx_2`, with nothing to tell them apart. The node id prefix came from a probe-time guess -- no audio input, or a pure MIDI effect, meant "synth" -- and the assumption that an instrument has no audio input is simply false: Surge XT's only input bus is a stereo audio in, so it probed as `num_input_ch = 2` and fell through to `fx`. Classic synths with no audio input (Dexed) were classified correctly, which is why the gap went unnoticed.

  The answer was already on disk and being discarded. A scanned `juce::PluginDescription` carries `isInstrument`, taken from the plugin's own declared category (VST3 `kInstrument`, AU `aumu`, LV2 `InstrumentPlugin`) -- `known_plugins.xml` records `isInstrument="1"` for Surge XT and `"0"` for Surge XT Effects -- but the add path dropped the whole description for anything with a real file path and let the probe guess again. The declared category is now threaded through to the naming step, which is exact rather than heuristic.

  The probe heuristic stays as the fallback for the "Browse to file..." path, which has no scanned description. It was left alone deliberately: swept over the 36 installed plugins against the scanner's `isInstrument` as ground truth, the existing rule errs on 6, every one of them an instrument that also takes audio in (Surge XT, Cardinal, Quanta 2, plugdata, FilterscapeVA, Element). Every `accepts_midi`-based alternative tried was **twice as bad** (12 wrong), because plenty of effects accept MIDI -- Presswerk, Satin, BYOD, Glitch2, Replicant 3 among them. No probe-only rule separates the two cases, since an instantiated plugin does not expose its category; only the declared one can. The browse fallback therefore still guesses, always in the "instrument called fx" direction.

  Note that ids are minted once when a node is added and then persist in the project file, so nodes in existing projects keep the names they were given.

### Changed

- **`-DMINIHOST_HEADLESS=OFF` did nothing and has been removed from the build.** It was a real `option()` when headless mode was introduced, toggling a single `minihost` target between the headless and full JUCE format classes. The desktop-app work replaced that with two sibling targets built from the same sources -- `minihost` (headless) and `minihost_gui` (non-headless) -- and dropped the option, but the flag was left behind in `make desktop`, in the CI desktop job, and across the docs. Nothing had read it since: CMake recorded it as `MINIHOST_HEADLESS:UNINITIALIZED=OFF`, and the generated compile line for `minihost` is byte-identical with and without it. It was worse than merely inert, because it reads as though it disables headless mode and does not -- `minihost` still compiles with `MINIHOST_HEADLESS=1` when you pass `OFF`.

  The compile definition itself is untouched and still load-bearing: `target_compile_definitions(minihost PRIVATE MINIHOST_HEADLESS=1)` is what selects `VST3PluginFormatHeadless` and friends over the full formats, and `minihost_gui` deliberately never receives it. Only the command-line flag is gone.

### Documentation

- **The docs told you to disable headless mode with a flag that does nothing.** `README.md`, `docs/getting_started.md` and `docs/hosting_guide.md` all instructed `cmake -DMINIHOST_HEADLESS=OFF` to obtain GUI support, which silently had no effect for anyone who followed it. They now point at `-DMINIHOST_BUILD_GUI_LIB=ON`, which builds `libminihost_gui.a` -- the same sources against the full `juce_audio_processors` -- alongside the headless `libminihost.a`, both in one build tree. Verified from a clean configure, including that `minihost_gui` is a member of the default `all` target so the documented `cmake --build build` really does produce it. The comments in the top-level `CMakeLists.txt` and `docs/dev/desktop_app.md`, which described the same non-existent switch, were corrected too.

### Testing

- **`tests/test_release_version.py`, which makes version drift un-mergeable.** The single-source-of-truth arrangement above is only worth anything if a violation fails the build rather than being noticed at release time. The tests assert that `minihost.__version__`, the `CHANGELOG.md` section heading, the Python CLI's `--version` and both native CLIs' `--version` all agree with `pyproject.toml`, and that `cmake/minihost_version.h.in` still contains placeholders rather than a literal that escaped into the template. The native-binary checks skip when there is no standalone build tree, since `make test` builds a wheel.

  Checked against the failure it is meant to catch, not only against the fix: bumping `pyproject.toml` to a version nothing else knows about fails five of the seven tests, one per location that would otherwise have drifted silently.

- **`--plugin-browser-selftest`, and `tests/test_desktop_pluginbrowser.py` to drive it.** The Plugin Browser had no coverage, which is how a use-after-free on its most ordinary interaction survived. Clicking through the dialog cannot be automated here, so this follows the pattern the undo and autosave selftests already set: the load-bearing part -- the window's ownership across open, dismiss and reopen -- is driven end-to-end through a mode in the binary. Dismissal is asynchronous (the modal manager deletes on a later message-loop pass), so the steps run one per timer tick with the loop free to run in between, and dismissal goes through `setVisible(false)`, the exact path the close button and Escape take. It asserts that opening creates a window, that re-requesting while open reuses it rather than stacking a second, that the tracking pointer nulls itself once dismissed, and that reopening then yields a live, fully-formed window.

  The test was checked against the bug rather than only against the fix: reverting to the old owning pointer and rebuilding makes it report `window still tracked after dismissal -- pointer is dangling or delete never ran` and then die of SIGSEGV, which is the reported crash. It deliberately does **not** compare window addresses across a dismissal -- the old window is freed before the new one is allocated, so the allocator may hand back the same block and address identity would prove nothing either way. An earlier draft asserted exactly that and failed for precisely this reason.

  Unlike the other desktop selftests it maps real windows and so needs a window server rather than merely tolerating one. CI runs it on Linux only, under the existing xvfb prefix; it skips cleanly with no display and can still be run by hand on any platform that has one.

## [0.7.0]

Closing the gap between what the library can do and what the shipped binaries expose. A survey of all 135 C API functions found the CLI using 41 of them and the desktop app 18, against 126 for the Python bindings -- with whole tiers (chains, buses) reachable from Python and from nothing else, and several entry points reachable from nothing at all.

The CLI gains MIDI-file input in the C binary (its help had advertised the flag as unsupported since the command existed), and `chain` and `bus` commands in both binaries, loaded through a shared session. The desktop app gains factory-program, bypass and `.vstpreset` controls, and stops rendering offline bounces in the realtime code path. The bindings gain the two entry points that had no Python equivalent, and `morph.lerp` stops reimplementing the C interpolation in Python. Where two binaries do the same job, they were checked against each other by rendering the same material and nulling the results, not by reading the code.

The CLI binaries also gain test coverage, which they had almost none of: their one suite compared stdout for the data commands, skipped everything when no plugin was present, and was run by no CI job at all.

The C ABI moves to **2.8.0**, in three additive steps: MIDI file reading, plugin discovery and the scan cache, then supervised scanning.

Scanning is also no longer something to supervise by hand: each plugin is probed in a
child process the scan is willing to lose, which is what it takes to get through a real
collection -- five of the ~350 installed here hang or crash on load.

Plugin discovery closes the last gap between the two front-ends: the binaries take plugin names rather than paths, `scan` finds this platform's plugin locations on its own, and both go through a cache shared with the Python CLI. Probing an AudioUnit while the message thread was running turned out to deadlock, which is what made a full AU scan impossible; it is fixed here.

### Added

- **`mh_midi_file_load` / `mh_midi_file_free`** -- reading a standard MIDI file had no C entry point at all. Python could load one through `MidiFile`, but a C consumer could only drive a plugin with MIDI it had built by hand, which is why the C CLI's `-m` flag had been sitting behind a "not yet supported" message. The loader merges tracks, applies the file's tempo map, drops meta events, and hands back one time-ordered `MH_MidiEvent` array with absolute sample offsets at a given rate -- the form the `mh_process*` entry points consume once rebased per block. Implemented on JUCE's own MIDI reader, so it adds no dependency.

- **`minihost_c process -m FILE`** -- the C CLI can now play a MIDI file through a plugin, which its own help text had advertised as unsupported since the command existed. Audio input is no longer required: with only `-m`, an instrument is fed silence for the duration of the MIDI. Verified against the C++ CLI, which has always had this: rendering `bach.mid` through Dexed from both binaries produces **bit-identical** output (-240 dB residual), so the two are genuinely at parity rather than merely both working.

- **`chain` command in both CLI binaries** -- processes audio and/or MIDI through several plugins in series, which is where the routing tier finally becomes reachable without Python. Takes the plugins in signal order, with `--mix INDEX:VALUE` for per-plugin dry/wet, plus the usual `-i` / `-m` / `-o` / `--tail` / `--bpm` / `--non-realtime` / `--bit-depth`. Because it goes through `mh_chain_process_midi_io`, the 0.6.0 MIDI routing comes with it: `chain "Chord Prism 2.component" Dexed.vst3 -m bach.mid -o out.wav` has the chorder drive the synth, which is the case that rendered silence before 0.6.0. The two binaries were checked against each other on a deterministic chain (bit-identical) and on a reverb chain, where they differ by -38 dB -- the same figure the C binary differs from *itself* by across two runs, i.e. FabFilter Pro-R 2's own nondeterminism rather than a difference between the ports.

- **`Session.open_desc` and `PluginGraph.set_node_midi` in the Python bindings.** Parity runs both ways: the audit that found the CLI and desktop behind the library also found the bindings missing entry points, and these two had no Python equivalent at all. `Session.open_desc` is the AudioUnit load path through a session's shared format manager -- AUs are identified by an id rather than a path, so a descriptor is the only way to load one, and a session is what stops several loads from re-registering the plugin formats each time. `PluginGraph.set_node_midi` stages MIDI straight onto a plugin node with no MIDI_INPUT node and no edge, for graphs that drive a plugin directly rather than wiring a MIDI topology.

- **The CLI's `chain` and `bus` load their plugins through one session.** `mh_open` builds and registers a JUCE plugin-format manager on every call, so a four-plugin chain built four of them. Both commands now open through a single `MH_Session` and close it once the plugins are up (plugins outlive the session that loaded them), which takes a four-plugin chain from ~0.85 s to ~0.74 s and scales with the plugin count. Verified behaviour-neutral: output before and after differs by -115 dB, which is exactly what the same binary differs from itself by across two runs with Saturn 2 in the chain, and chains of deterministic plugins are bit-identical.

- **`bus` command in both CLI binaries** -- splits one input across parallel branches and sums them, which is the layering shape: one MIDI part driving several instruments at once. Each argument is a branch, and commas inside an argument chain plugins in series within it, so `bus synth.vst3 "chorder.component,synth.vst3"` layers a chorded synth against a plain one. `--gain INDEX:VALUE` sets per-branch gain. Instrument branches expose no audio input, so the bus is created zero-width in that case -- the 0.6.0 bus relaxation is what makes this expressible at all. Verified three ways: the summed output matches the two branches rendered separately through `chain` (-142 dB, the 24-bit floor of the re-summed files), `--gain 1:0.0` reproduces branch 0 bit-exactly, and the two binaries agree bit-exactly with each other.

- **`.vstpreset` import and export in the desktop's plugin window.** A plugin's own editor saves in whatever private format it likes; `.vstpreset` is the interchange format other hosts read, and the app could neither write nor load one. "Load preset" reads a file and pushes its component chunk through `mh_set_state`, reporting a rejection rather than failing quietly (the usual cause being a chunk from a different plugin), and refreshes the program selector afterwards since the plugin may have jumped. "Save preset" takes the class ID from the VST3 bundle's `moduleinfo.json`; AudioUnits have none, so it says so instead of writing a file no host can load.

- **Factory-program selector and bypass toggle in the desktop's plugin window.** The app shows each plugin's own editor, but a plugin's editor generally offers no way to step through the factory programs the host can see, and several expose no bypass at all. The toolbar now lists the programs when there is more than one (`mh_get_num_programs` / `mh_get_program_name` / `mh_set_program`) and carries a bypass toggle backed by `mh_set_bypass`, which drives the plugin's own bypass parameter where it publishes one rather than merely muting the node.

- **Plugin names instead of paths in the native CLIs.** Every command that took a plugin took an absolute path typed out in full. They now accept a name from the scan cache as well, matched without regard to case -- `minihost_c probe dexed`, `minihost_c chain dexed gigaverb -m song.mid -o out.wav`, and each element of a `bus` branch. An existing path always wins, so nothing that worked before changes meaning. Both failure modes are explicit: an unmatched name names the cache file and says to scan first, and an ambiguous one lists the candidates rather than picking for you. Plugins that failed to probe are never offered by name, since one that will not load cannot be loaded by name either.

  Two of the matching rules came out of measuring a real collection (343 plugins) rather than reasoning about a toy one. **Substring matching is opt-in, behind `--fuzzy`**: `reverb` matches 5 of those plugins, `delay` 9 and `filter` 31, so as a default it mostly bought an ambiguity error, and the example first written into the docs -- `pro-q` finding `FabFilter Pro-Q 4` -- in fact matched three. **Same-name installs are collapsed rather than refused**: 16 of the 343 were installed as both an AudioUnit and a VST3 under one name, which made even a fully typed exact name ambiguous and left those plugins unreachable by name entirely. When every match is one name differing only by format, VST3
  is preferred instead of erroring, and `--format au|vst3` pins the choice explicitly.

- **`scan` with no argument scans this platform's plugin locations.** It previously required a directory, which meant knowing where plugins live before you could find out what was installed -- and name resolution would have inherited that. The directory argument is now an override. macOS covers `/Library/Audio/Plug-Ins/{VST3,Components}` and the same two under `~/Library`; Windows the Common Files VST3 directories; Linux `/usr/lib/vst3`, `/usr/local/lib/vst3` and `~/.vst3`. Directories that do not exist are skipped, and the ones being scanned are printed. Note that scanning probes each plugin, which means loading it: a first pass over a large collection takes minutes, and the measured cost on the development machine was over ten.

- **`mh_get_default_plugin_dir`, `mh_plugin_cache_path` / `_scan` / `_lookup` / `_match`** (C ABI 2.7.0) -- the library layer behind the two entries above, so both CLI binaries share one implementation rather than each growing its own index. The cache file and its JSON schema are **the same ones `minihost.plugincache` writes**, so a scan from either front-end serves the other, `MINIHOST_CACHE_DIR` included. Entries are keyed by path with an mtime + size fingerprint, so a repeat scan re-probes only what changed, and scanning one directory does not discard entries from another. `_lookup` and `_match` take a format preference and a substring flag. The cache is written as the scan proceeds rather than once at the end, so a scan that dies part way keeps what it had and a re-run resumes from there -- which matters, because some plugins take the scanning process down with them (see supervised scanning below).

- **`mh_plugin_cache_scan_supervised` / `mh_plugin_scan_worker_main`** (C ABI 2.8.0) -- scanning probes every plugin, probing means loading it, and an installed collection can be relied on to contain a plugin that never comes back. Probing all 333 AudioUnits on the development machine, one process each, found five: Apple's own `AppleAES3Audio.component` spins forever at 100% CPU inside JUCE's bus enumeration, `KV_Element` / `KV_ElementFX` / `KV_ElementMFX` segfault on load, and `JE8086` aborts on a corrupted heap. In one process the first of those ends the scan -- a full AudioUnit scan stopped at entry 66 of 333.

  The supervisor gives each plugin a child process and a deadline, so one that hangs or crashes costs a cache entry rather than the run. Two outcomes join `ok` and `error` -- `timeout` and `crash` -- fingerprinted like any other entry, so a re-scan skips those plugins instead of paying for them again. The same directory now scans start to finish: 331 ok, 2 that are not plugins.

  There is no new binary. The worker is whatever executable the caller names, defaulting to the calling process's own: both CLI binaries answer `--mh-probe-one` in the first lines of `main` and exit. An embedder whose executable is not ours -- a DAW, or Python -- passes its own worker command instead, implementing the protocol in the header: one JSON object on stdout between two markers. The markers are there because plugins print pages of their own diagnostics while loading, so the answer has to be findable inside that noise rather than assumed to be all of it. `MINIHOST_SCAN_WORKER` and `MINIHOST_SCAN_TIMEOUT_MS` override the command and the deadline, which is also what makes this testable without owning a plugin that misbehaves: `tests/test_scan_supervised.py` drives a fake worker that hangs, aborts, fails and succeeds to order.

  A side effect worth having: the worker probes on its own main thread, having never started the message thread. Several AudioUnits that hang or crash when probed on our message thread -- `AppleAES3Audio` and the three `KV_Element` plugins among them -- probe cleanly there, so supervised scanning finds more plugins as well as surviving more of them.

- **`minihost.plugincache.scan` probes in a child process too**, so the Python side is not the one front-end left holding the old failure mode. Same statuses, same cache file, same `MINIHOST_SCAN_WORKER` / `MINIHOST_SCAN_TIMEOUT_MS` overrides; `supervised=False` (and `minihost scan --in-process`) keeps the old path. The worker is `python -m minihost._scan_worker`, which writes the same marker protocol as the C one, so either supervisor can drive either worker. `plugincache.info` still probes in-process on purpose: one named plugin is the caller's own choice, so a hang there is visible and interruptible, unlike the same hang buried in a scan of several hundred. Scanning now also flushes the cache periodically rather than only at the end, and `desc.path` is filled in as the C scanner already did (`probe()` reports it empty, since it is told the path rather than discovering it).

- **`scan --in-process` in both CLI binaries** -- probes in the scanning process, as `scan` did before, for a directory you trust or a machine where process launches are expensive. The default is supervised.

### Fixed

- **`minihost.morph.lerp` reimplemented the C interpolation in Python.** The same arithmetic and the same clamping existed twice, once in `mh_morph_lerp` / `mh_morph_lerp_per_param` and once in `morph.py`, with nothing checking that the two agreed -- so either could have drifted unnoticed, and the C side had no coverage from Python at all. `morph.lerp` now delegates to the C implementation (exposed as `_core.morph_lerp` / `_core.morph_lerp_per_param`); the length checks stay in Python because their messages name the offending lengths. The C functions cannot simply be deleted instead: they are public ABI and the CLI's `morph` command uses them.

  The existing morph tests, written against the Python implementation, pass unchanged -- which is what shows the two implementations did agree.

  One observable consequence: results now come back at **parameter precision (float32)** rather than double, since that is what a plugin holds -- `get_param` returns a float and `set_param` takes one. Snapshots from `capture()` are float32 already and round-trip exactly through `lerp` at any blend, but a hand-written double literal such as `0.2` comes back as its float32 neighbour (a difference around 1e-8, well under float32 resolution, and exactly the value the plugin would end up storing). One test asserted equality against such a literal; it now pins the precision contract explicitly rather than being loosened to an approximate comparison, and a new test covers the rounding directly. `mh_morph` itself keeps no consumer on purpose: both call sites need the interpolated snapshot for other reasons, so routing through the one-call convenience would mean computing it twice.

- **`resample` took different arguments in the two CLI binaries**, found by the new conformance tests below on their first run. `minihost_c` accepted `resample IN OUT --rate N` (and also `-i` / `-o`), while `minihost_cpp` required `resample IN -o OUT -r N` -- so a command line written for one failed against the other, which is the opposite of interchangeable. The C++ binary now also accepts the output as a second positional argument and `--rate` as an alias for `--target-rate`. (The positional is declared as `output_path`: CLI11 aborts at startup if a positional's name collides with an option's long form, which is exactly what the first attempt did.)

- **The desktop app rendered offline bounces in the realtime code path.** `renderProject` never called `mh_set_non_realtime`, so plugins with quality or oversampling modes tied to that flag -- and plugins that drop work under realtime pressure -- produced a worse file than the project deserved. Offline renders now switch every plugin to non-realtime for the duration and restore it afterwards, so the same loaded project can go back to live playback unchanged. Desktop render-parity tests still pass.

- **Probing an AudioUnit deadlocked once the message thread was running.** `mh_probe` was the only thread-affine entry point in the library that did not go through `runOnMsg` -- every other one has. It ran on the caller's thread, and reading an AU's description means instantiating it, which on macOS dispatches to the message thread and waits. Our message thread deliberately services only its own task queue and never pumps JUCE's dispatch loop, so the probe waited on a dispatch nothing would ever run: stopped forever at 0% CPU, not merely slow. VST3 needs no such dispatch, which is why only AUs hung.

  The trigger was the message thread *running*, not the probe itself, so what you saw depended on what was already loaded. A probe with nothing open ran inline and was fine -- which is why `minihost info <au> --probe` worked while `minihost info <au>` hung: the latter loads the plugin first (starting the thread), then probes a second time to fill in the listing. Both native CLI binaries start the thread at launch, so for them every AU probe hung and `scan` stopped at its first AudioUnit. `MINIHOST_MESSAGE_THREAD=0` was a workaround and is no longer needed for this. Probing now goes through `runOnMsg` like everything else, and `tests/test_probe_message_thread.py` pins it -- in a subprocess with a timeout, since the failure mode is a hang rather than a wrong answer.

- **Two Windows build failures, both from the work above.** `cmd_bus` used POSIX `strtok_r`, which MSVC spells `strtok_s`: the C compiler assumed an int-returning function (two C4047 warnings that were the real message) and the link failed on an unresolved symbol, so no Windows CLI was produced at all. And `MINIHOST_SCAN_WORKER` was split with POSIX `shlex` rules on every platform, where a backslash is an escape rather than a path separator -- `C:\Users\me\python.exe` arrived as `C:Usersmepython.exe` and every supervised scan failed with "cannot find the file specified", taking 12 wheel tests with it. Splitting is now platform-aware and quote-aware on both sides (the C library's tokeniser gained quote handling too, since the obvious worker path on Windows is under `C:\Program Files`), and `tests/test_plugincache.py` pins both modes on whatever platform is running -- the broken one being, necessarily, the one the development machine never takes. `tests/test_scan_supervised.py` now runs in CI on all three platforms, because spawning a process per plugin is the part of this most likely to differ per platform.

- **`make cli` produced an unoptimized build.** The target ran `cmake ..` with no build type and relied on `cmake --build . --config Release`, but `--config` is read only by multi-config generators (Xcode, Visual Studio); with the Unix Makefiles generator it is ignored, and an unset `CMAKE_BUILD_TYPE` means no optimization flags at all. Every local CLI binary on macOS and Linux was therefore built at `-O0`. The target now sets `-DCMAKE_BUILD_TYPE=Release` at configure time and keeps `--config Release` for the generators that want it instead. `minihost_c` went from 13 MB to 4.5 MB and the CLI conformance suite from 7.05 s to 3.55 s. CI was never affected -- the workflow configures the build type itself rather than going through `make cli` -- which is why the gap survived: it existed only where the binaries were being measured by hand.

- **`make clean` left `build-*/` behind, and stale trees decided what got tested.** `tests/test_cli_conformance.py` locates the CLI binaries by taking the most recently built candidate across `build/`, `build-cli/` and `build-desktop/`. A leftover tree from an earlier configuration could therefore supply the binaries under test, silently, and `build-desktop/` builds them non-headless. `clean` now removes `build-*/` as well, so the next `make desktop` is a full rebuild.

- **`test_process_releases_the_gil` was flaky, and its reasoning about why it could not be was wrong.** The test spins a counter in a background thread and samples it either side of one `process()` call: a held GIL means the counter cannot advance, so a zero reading was treated as the regression. It failed once on a loaded machine with "0 advances during a 0.3 ms call" while the GIL was being released perfectly.

  The 0.3 ms is the point. Timed properly, 65536 frames through the default test plugin is about 0.3 ms of work -- far shorter than Python's 5 ms switch interval, so whether the spinner is scheduled inside that window is luck, and a busy machine loses. What made this hard to see is that the call *appears* to take ~6.5 ms under the default interval regardless of frame count, because the measurement includes reacquiring the GIL from the spinner; the same call measures 0.03 ms at 512 frames and 0.31 ms at 65536 once the interval is shortened, which is what exposed the real cost.

  The two outcomes are not symmetric: a held GIL is deterministic (every sample reads zero) while a released GIL only offers the spinner an opportunity. The test now takes the best of up to twenty samples, shortens the switch interval for the measurement so each call offers several scheduling points, and clamps the frame count to the plugin's actual maximum block size -- asking for more frames than a plugin accepts makes the call a no-op that measures nothing. The all-zero assertion is unchanged, so the regression it exists to catch still fails it; only the luck is gone.

### Documentation

- **Plugin discovery and the scan cache** are documented in the C API reference (the five new functions, the per-platform directory table, the cache location and schema sharing, and the three-way return of `mh_plugin_cache_lookup`), and in the CLI reference and README as the *Naming plugins* and *`scan`* sections -- including that a first scan is slow because it loads every plugin.

- **The native CLI binaries were undocumented, and the 0.7.0 features were undocumented everywhere.** `docs/cli.md` described only the Python `minihost` command, so `minihost_c` and `minihost_cpp` -- the binaries the `cli` release archive actually ships -- appeared nowhere, `chain` and `bus` included. Both files now cover them: a new *Native CLI binaries* section in the CLI reference with the routing commands, their shared options and the MIDI-effect ordering rule, and a shorter version in the README next to the Python CLI. Every command line shown was run before being written down.

  Also documented: `mh_midi_file_load` / `mh_midi_file_free` in the C API reference and the README's C section, including the per-block rebasing the absolute sample offsets need; `Session` in the Python reference and the README, with `open_desc` for the AudioUnit path; `PluginGraph.set_node_midi` and the edge-takes-precedence rule; the desktop's new program, bypass and `.vstpreset` controls and its non-realtime offline renders; and the float32 precision contract `morph.lerp` now carries.

### Testing

- **The CLI binaries' only test suite never ran in CI, and covered no command that produces audio.** `tests/test_cli_conformance.py` compared stdout for nine data commands (probe, info, params, presets, morph); `process`, `chain`, `bus`, `play`, `scan` and `resample` were untested, and the `build-cli` job built both binaries without running a single test against them. The wheels job runs the whole suite but builds no CLI, so the file skipped there too -- nothing in CI had ever executed it.

  It now covers the rendering commands as well: `process` (audio and MIDI), `chain` and `bus` each render through both binaries and the outputs are compared. Comparing renders has to allow for plugins that are not reproducible against themselves, so each test measures that floor first -- rendering twice with one binary, after a throwaway pass -- and then requires the two binaries to differ by no more than the plugin differs from itself. With a deterministic plugin the floor is zero and the comparison is exact. One test goes past conformance into correctness: a two-branch bus of one plugin must equal that plugin doubled, which catches a dropped or double-counted branch that comparing the binaries against each other cannot.

- **The suite now does useful work without a plugin, which is what makes it runnable in CI.** No GitHub runner has an audio plugin installed, so every plugin-gated test skips there. The module-wide skip meant that made the whole file a no-op; the plugin gate is now per-test, and new plugin-free cases cover `resample` conformance (byte-identical output from both binaries), five missing-file error paths, and an unknown command -- seven tests that run anywhere. The `build-cli` job runs the file on all three platforms; the binaries are located by the test itself, which handles both the Unix and the Windows `Release/*.exe` layouts.

- **`tests/test_api_coverage.py`** -- eight tests over the two newly exposed entry points, which previously had no consumer and no coverage anywhere. `Session.open_desc` is checked against the stock Apple AudioUnits rather than a configured plugin, so it runs on any Mac instead of skipping: one AU loads, several load from one session, each keeps working after the session is closed, and a malformed descriptor raises. `set_node_midi` is checked to drive an instrument with no MIDI node present, to produce audio identical to the same events delivered over a MIDI edge, to be ignored when an edge is connected (the documented precedence rule), and to reject a non-plugin node.

### Known gaps

Still reachable only from Python: graphs and projects from the CLI (chains and buses are covered now, and the desktop binary can already render a project with `--render-project`), `mh_open_async`, and sidechain routing in the desktop -- the last of which is a library gap first, since the graph has no sidechain node. The desktop's bypass, program and preset controls are per-window rather than per-node in the graph editor. Three functions still have no consumer: `mh_bus_process_midi`, `mh_chain_get_plugin` and `mh_morph`. The first two have Python equivalents that reach the same result another way; the third is a C-side convenience whose only would-be callers need the interpolated snapshot anyway. Six functions -- `mh_bus_process_midi`, `mh_chain_get_plugin`, `mh_graph_set_node_midi`, `mh_morph`, `mh_morph_lerp_per_param`, `mh_session_open_desc` -- have no consumer anywhere, including the tests.

## [0.6.0]

MIDI routing: it now flows through a chain instead of stopping at the first plugin, and a bus can hold the instrument branches its fan-out was built for. Both were features that existed on paper and could not be used. A minor rather than a patch release because callers can observe the change: `process_midi` on a chain now returns the MIDI leaving its **last** plugin rather than its first.

The C ABI moves to **2.5.0**: no symbols added or removed and no struct layout change, but MIDI now flows through a chain instead of stopping at its first plugin, and a bus can finally hold the instrument branches its MIDI fan-out was built for -- see below.

### Fixed

- **`PluginBus` could not hold an instrument branch, which is the only thing its MIDI fan-out is for.** `mh_bus_create` rejected a zero input-channel count outright, and `mh_bus_add_branch` demanded a branch's input width equal the bus's exactly. An instrument driven by MIDI alone exposes no audio input bus and reports zero input channels -- Dexed and TyrellN6 both do -- so every layering topology was rejected at construction: `PluginBus(2, 2)` refused the branch, and `PluginBus(0, 2)` refused to exist. The documented headline use, "one MIDI part drives N parallel instruments whose audio is summed", could not be built with a typical synth at all.

  A bus may now have zero input channels, and a branch may read fewer channels than the bus carries. Wider than the bus stays an error: the caller supplies exactly `num_in_channels` pointers and a wider branch would read past them. Output width and sample rate still have to match exactly. Combined with the chain fix above, the split topology works: one MIDI part into a bus whose first branch is an instrument and whose second is `[midi_effect, instrument]`, summed -- verified to produce output identical to the same topology wired by hand in `PluginGraph`, down to the spectrum of each leg.

  This one hid behind a skip. `test_bus_fans_midi_to_all_branches`, whose own docstring calls itself "the headline use case", began with `if in_ch < 1: pytest.skip(...)` -- true for every MIDI-driven instrument, so with the default test plugin it never ran once. The same skip masked two more: `test_bus_process_midi_reports_overflow` and `test_bus_merges_branch_midi_sorted_by_offset` built a `PluginBus(2, 2)` around zero-input MIDI-effect chains that `add_branch` would have rejected, but they bailed out earlier still on a single-block probe that saw no MIDI (see below). All three now run.

- **A MIDI effect in a `PluginChain` silently swallowed the notes behind it.** `mh_chain_process_midi_io` handed `midi_in` to plugin 0, reported that plugin's MIDI output back to the caller, and processed every plugin after it with `mh_process` -- no MIDI at all. For the arrangement this routing exists to serve, `PluginChain([arpeggiator, synth])`, that meant the arpeggiator consumed the incoming note, emitted its own, and the chain dropped it on the floor; the instrument was never told anything and the chain rendered digital silence. Nothing errored, so the symptom was a quiet render with no indication of why.

  MIDI now travels with the audio: `midi_in` enters the first plugin that accepts MIDI, and every plugin reporting `produces_midi` replaces the stream for the plugins behind it. Verified with a chorder ahead of an FM synth -- feeding note 60 into `PluginChain([Chord Prism 2, Dexed])` produces the spectrum of note 72, matching what the synth renders when handed that note directly, where the pre-fix build produced -240 dBFS.

  A plugin reporting `produces_midi == 0` ends the stream. JUCE's contract is that whatever a plugin leaves in the `MidiBuffer` is its output, and that a plugin should clear what it consumes -- but a plugin answering `producesMidi() == false` has declared it emits nothing, so leftovers are input it neglected to clear rather than output, and forwarding them would retrigger a downstream instrument with notes an upstream one already played. The practical consequence is that MIDI effects must come before the instrument: in `[midi_fx, audio_fx, instrument]` the audio effect terminates the stream. That is also the only ordering that makes audio sense, since an effect ahead of the instrument processes silence.

  Two knock-on details. `midi_out` now reports the MIDI leaving the **last** plugin rather than the first -- what the chain emits, consistent with treating a chain as one composite plugin; for the single-plugin chains that dominate MIDI use, nothing changes. And the per-stage `mh_get_info` calls the process loop used for channel counts are gone: that function takes the plugin's mutex, which the audio thread must not do. Channel counts and the two MIDI capability flags are now cached at chain construction, and the inter-stage MIDI buffers are pre-allocated (256 events per stage, excess dropped rather than allocating on the audio thread).

  `PluginGraph` needed no change here: it has always routed MIDI as explicit edges, including plugin to plugin. `PluginBus` inherits the fix inside each branch, since it drives branches through `mh_chain_process_midi_io` -- but it had a separate problem of its own, covered in the entry above.

### Documentation

- **New page: MIDI Routing** (`docs/midi_routing.md`, added to the nav). MIDI routing had no prose documentation at all -- `connect_midi`, the MIDI processor and merge nodes, and the bus fan-out lived only in C header comments and test files, which is a poor place to discover that a MIDI effect must precede its instrument. The page covers what each of the four routing objects does with MIDI, the chain's ordering rule and the reasoning behind it, the bus's zero-width input for layering, and the graph's edge model with a worked split topology. Every code example in it was run against real plugins. It also records three things that cost time to rediscover: a MIDI effect need not answer in the block it was fed, the high-level file renderers accept a plugin or a chain but not a graph, and a routing mistake shows up as silence rather than an error. `PluginChain` and `PluginGraph` in the Python API reference now link to it.

### Testing

- **`tests/test_graph_v2_midi.py::test_plugin_midi_input_from_graph_edge` never ran.** It skipped whenever the plugin reported no audio input channels, on the stated grounds that "graph_v2 requires plugin input port 0 to be connected". That has not been true for some time -- `mh_graph_add_plugin` gives instruments (`num_input_ch == 0`) zero input ports, compile tolerates them unwired, and the render path feeds them silence. Since instruments are exactly the plugins one drives over a MIDI edge, and the default test plugin is one, the graph's plugin MIDI edge went unexercised. The test now wires an audio input only when the plugin has one, and asserts the reference render is audible: it previously compared two buffers without checking either held anything, so it would have passed on silence == silence had the edge delivered nothing.

- **Three bus MIDI tests never ran, for two different reasons.** `test_bus_fans_midi_to_all_branches` skipped on any instrument reporting zero input channels, which the bus fix above makes moot; it now runs. `test_bus_process_midi_reports_overflow` and `test_bus_merges_branch_midi_sorted_by_offset` probed the MIDI effect for exactly one block and skipped on "emitted no MIDI" -- but a MIDI effect need not answer in the block it was fed, and the plugin they were run against (Chord Prism 2) replies one block later. Both now drive the effect until it emits and compare at that block, so the branch MIDI merge and its overflow flag are finally exercised. The whole file runs with no skips when `MINIHOST_TEST_MIDI_FX` is set.

- **New: `test_bus_accepts_a_branch_narrower_than_itself` and `test_bus_layers_a_direct_instrument_against_a_midi_effect_leg`.** The second is the full split topology -- a direct instrument branch summed against a `[midi_effect, instrument]` branch -- asserting the sum equals the two legs rendered separately. It depends on both fixes in this release and fails without either. The two pre-existing validation tests were updated rather than removed: `test_graph_create_rejects_bad_channels` now pins that a zero-width bus constructs while negative input and non-positive output are still refused, and `test_graph_add_branch_rejects_channel_mismatch` pins output width as an equality and input width as a ceiling.

- **Two graph topologies had no test at all**, both added here. `test_midi_fanout_drives_several_instruments_and_audio_sums` fans one MIDI source to two instruments and sums their audio through a mix node, asserting the result is exactly twice a single instance's render -- the layering shape. `test_plugin_to_plugin_midi_edge_drives_an_instrument` wires a MIDI effect's output into an instrument and compares against pumping the effect's MIDI into the instrument by hand, block by block; it needs `MINIHOST_TEST_MIDI_FX` and skips without one. Both cover code that was already correct, so neither is a regression pin -- they close the gap that let the equivalent `PluginChain` bug ship unnoticed. The second renders sixteen blocks rather than one because a MIDI effect need not answer in the block it was fed: Chord Prism 2 emits its transformed note a block later, and a single-block version of this test skipped on "emitted nothing".

- **`tests/test_chain_midi_routing.py`** -- seven tests over the routing rules: a MIDI effect drives an instrument behind it, the chain matches pumping the effect's output into the instrument by hand block by block, an audio effect after the instrument does not disturb it, a non-producing plugin ends the stream, `midi_out` is empty when the last plugin produces none, a single-plugin chain still matches the bare plugin, and instrument-into-effect still hears its MIDI. Four fail against the pre-fix build; the other three are the no-regression pins and pass against both. The three needing a MIDI-emitting plugin skip unless `MINIHOST_TEST_MIDI_FX` names one -- they were written against Chord Prism 2, but nothing depends on its particular transformation.

### Added

- **`examples/midi_split_routing.py`** -- splits one MIDI part across two instrument legs, one of them behind a MIDI effect, and sums them; builds that same topology twice, once with `PluginBus` and once with `PluginGraph`, and nulls the two renders against each other. Both legs also render alone so the split can be auditioned. It doubles as the reference implementation for something the high-level API does not cover: neither a bus nor a graph can be driven by `render_midi_to_file`, so the block loop here converts the file with `midi_file_to_events`, buckets the events per block with a reusable `events_by_block`, and feeds each router a block at a time. Renders to `build/output/routing/`.

  The demo self-checks before it compares: summing is what both routers do, so each route's two-leg render must equal its own legs added together, and whichever route fails that is the one at fault. With that check in place the two routes null at -240 dBFS, correlation 1.000000 -- the bus and the graph render the same topology sample for sample.

  Getting there turned up something worth writing down. **The first substantial render in a process leaves plugin state behind that changes every render after it.** Comparing two routes means rendering twice, so the route measured first came out different from the one measured second -- by a whole chord voicing, a residual only 1.3 dB under the signal, which reads exactly like a routing bug in whichever route happened to run first. It is not one. The MIDI delivered to the instruments is byte-identical in both routes (1226 events, compared event by event), two routes rendered back to back agree bit-exactly, and a fresh process renders the same result regardless of how long the plugins are given to settle. The effect is the plugins', not the host's; it was seen with Dexed plus Chord Prism 2 and reproduced outside the demo. The cure is a throwaway full render before anything is measured -- a short silent warm-up does not do it, and neither does a 64-block pass. It costs about half a second at these render speeds. Any A/B that renders the same material twice through separate plugin instances needs the same precaution.

- **`examples/midi_render_instrument.py`** -- renders a MIDI file through an instrument plugin, AudioUnit or VST3, in four passes: straight (`render_midi` into memory, checked, then `write_audio`), one file per factory preset (`render_midi_to_file` with `Plugin.program`), instrument into an effect as a single `PluginChain` with a progress callback, and a transposed variant built with `MidiFile`'s write API and saved alongside the audio. Renders to `build/output/midi/`; roughly three seconds end to end against Dexed. `--instrument` names a plugin explicitly instead of searching, `--programs` and `--transpose` size or disable the corresponding passes, `--no-chain` drops the effect, and `--tail` / `--tail-threshold` / `--max-tail` control tail detection. Every render reports its channel count and level, and says so plainly when the result is silent.

  Note that its default input, `tests/_midi/bach.mid`, is not currently tracked in git, so the example needs `--midi` pointed at a file of your own until that asset is committed. The effects example's input, `tests/_wav/piano.wav`, is tracked.

  The tail is the part that carries over to other work. `tail_seconds="auto"` renders past the last note-off until the output falls under `tail_threshold`, capped by `max_tail_seconds`, and every render reports the tail it actually needed. That length is a property of the patch rather than something a caller can know: across four Dexed presets driven by the same 50.5 s file the detected tail ranged from 0.07 s to 7.06 s, so any fixed value would either truncate a decay or pad most renders with silence.

  **Identifying an instrument turns out to need a behavioural test, not metadata.** The example originally filtered candidates on `accepts_midi`, output channel count and `is_midi_effect`, which is not sufficient in either direction: Dexed and TyrellN6 report zero audio inputs while Surge XT and TAL-NoiseMaker report two, exactly like an effect, and FabFilter Pro-R 2 answers True to `accepts_midi` because it accepts MIDI for parameter control -- so a reverb passed every metadata check and rendered fifty seconds of silence. Candidates are now sent a single test note and rejected if nothing comes out, which also catches instruments that load but cannot sound.

  Two smaller things it makes visible. Renders are as wide as the instrument's output and nothing downmixes, so a multi-bus instrument yields a file wider than stereo -- Surge XT produces six channels (main plus a stereo pair per scene) and the example prints the bus layout so the extra channels are identifiable. And the in-memory first pass is checked for NaN and Inf before anything is written, because integer output formats cannot represent them: of the instruments tried while writing this, Quanta 2 renders non-finite samples headless, which a 24-bit file would have silently buried.

- **`examples/fx_chain_au_vst3.py`** -- renders one input file through the same six-stage effects chain (EQ, saturation, compressor, delay, reverb, limiter) twice, once as AudioUnits and once as VST3s, at three parameter settings, then nulls the two results against each other. It covers the offline-rendering surface in one place: format-agnostic `Plugin` loading, `PluginChain` with `set_non_realtime(True)`, `process_audio_to_file` for block iteration, sample-rate conversion (the bundled input is mono 22.05 kHz), mono-to-stereo duplication, latency compensation and tail rendering, plus `read_audio` / `write_audio` / `resample` / `get_audio_info` for the measurements. Cumulative per-stage stems are written so each effect can be auditioned in isolation, and every output's peak is printed with a clipping flag. Renders into `build/output/<preset>/`; the whole run takes about 24 seconds and produces 52 files.

  Parameters are addressed **by name with real-unit text** (`plugin.find_param("Ratio")` then `param_from_text(index, "3:1")`) rather than by index or normalized value. This is not stylistic: FabFilter's VST3 builds expose more parameters than their AudioUnit builds (Pro-Q 4: 737 against 600), so indices do not correspond between the two formats while names do. Setting the chain by name was verified to produce numerically identical parameter state in both formats. Any stage whose plugin is not installed is dropped with a warning, and a format with none installed is skipped, so the example degrades rather than failing.

  The three presets -- `light`, `medium`, `aggressive`, selectable with `--presets` -- vary along two axes at once. The audible one is how hard the signal is worked: short-term dynamic range (the spread of 100 ms block RMS) falls from 19.4 dB dry to roughly 16, 12 and 6 dB. The measurable one is how much of the output comes from the delay and the reverb, which is what decides whether the null test still measures anything.

  That second axis is the finding the example exists to document. **Timeless 3 and Pro-R 2 are not reproducible against themselves**: rendering the same chain twice through the same plugin instances, with `reset()` between passes, leaves a residual around -25 dBFS, because free-running modulation and analog-style drift carry internal state that `reset()` does not reseed. Pro-Q 4, Pro-C 3 and Pro-L 2 null at -240 dBFS under the same test. An AU-versus-VST3 null therefore says nothing on its own, and the first version of this example drew the wrong conclusion from one -- "the formats differ, correlation 0.87" was measuring the delay and reverb disagreeing with themselves. The example now renders each format twice to establish that run-to-run floor first and reports the format residual against it. Under `light` and `medium` the residual lands 15 dB or more below the signal and the comparison is decisive; under `aggressive` the 70 percent delay feedback and 150 percent reverb decay amplify that internal state until the residual rivals the signal (correlation 0.42) and the chain-level null stops discriminating, which the script says explicitly and flags as a `usable` column in its closing table. Cumulative-stem nulls stay informative at every setting and locate the transition exactly: EQ -155 dB, saturation -122 dB, compressor -98 dB at correlation 1.000000, then the delay.

  Two things fall out of this that are worth knowing when using minihost for offline work. Dry stages can be driven as hard as you like without costing measurability; wet, self-feeding stages are what destroy it. And plugin nondeterminism is not noise in the dither sense -- it is deterministic DSP running from unreproducible initial state, so averaging more takes does not clean it up.

  The example also gives incidental positive evidence for host-side latency compensation. Reduced to deterministic plugins (Pro-Q 4 plus Pro-L 2, which reports 3115 samples of latency), the AU and VST3 renders null at -149 dBFS -- the 24-bit files' own quantization floor -- which they could not do if `compensate_latency` were misaligning either format.

## [0.5.2]

Follow-ups from a review pass over the Python bindings and CI. No C ABI change (stays at **2.4.0**): no symbols added or removed and no behaviour change on the C boundary -- the GIL fix is entirely on the Python binding side.

### Fixed

- **`Session.open` and `Plugin.from_descriptor` held the GIL across the blocking plugin load.** The direct `Plugin(...)` constructor releases the GIL around the multi-second native instantiation (added in 0.5.0), but its two siblings did not. So loading a plugin through a `Session` -- the shared-format-manager path meant for multi-plugin and directory-scanning workflows -- or from a serialized `PluginDescription` (`from_descriptor`, the AudioUnit path) stalled every other Python thread for the whole load. The advertised "load one plugin on a background thread while another runs" pattern silently serialized on exactly these entry points. Both now carry the same `gil_scoped_release` call guard as the constructor. `open_async`, which already went through the constructor, was never affected.

### Testing

- **The desktop test suite now runs in CI.** The `build-desktop` job compiled the `minihost_desktop` binary and smoke-checked it with `--save-roundtrip`, but never ran pytest against it; the `build-wheels` job ran the full suite but built no desktop binary. The two never intersected, so the entire `@skip_if_no_desktop` set -- undo, autosave / crash-recovery, headless-render smoke, and C++/Python render parity -- executed in no job at all. `build-desktop` now runs it, split by cost: the binary-only tests (undo, autosave -- the platform-sensitive recovery paths) run on all three platforms, while the tests that import the `minihost` package to compare against `render_project` (smoke, render parity) run on Linux, where a single extra headless build backs them. Plugin-gated cases (MIDI-chain parity, plugin scan) skip cleanly on runners with no plugin installed.

## [0.5.1]

Continues the code-review pass: correct sidechain channel accounting, a host playhead that actually moves during offline renders, and two more silent-discard paths turned into errors. The C ABI moves to **2.4.0**: no symbols added or removed, but `MH_Info.num_input_ch` changes meaning -- see below.

### Fixed

- **Sidechain audio was written to channels the plugin never reads.** `MH_Plugin::inCh` was set from JUCE's `getTotalNumInputChannels()`, which already sums every enabled input bus *including* the sidechain; `sidechainCh` then counted the sidechain a second time. For a compressor opened as `mh_open_ex(main_in=2, sidechain=2)` this meant `MH_Info.num_input_ch` reported 4 rather than 2, so callers had to hand `process_sidechain` an over-provisioned 4-channel "main" buffer *plus* a redundant 2-channel sidechain buffer -- and `mh_process_sidechain` then wrote the sidechain at channels [4, 6) while the plugin's sidechain bus lives at [2, 4). The sidechain signal landed in scratch channels the plugin never reads, and the real sidechain bus received whatever happened to be sitting in `main_in[2..3]`. The single overloaded count is now split in two: `mainInCh` (input bus 0 -- what the caller supplies, and what `num_input_ch` reports) and `totalInCh` (every enabled input channel, used only to size the internal process buffer). The plain `mh_process*` paths copy `mainInCh` channels and zero everything above, so a sidechain-configured plugin processed through the normal path is fed sidechain silence rather than reading past the caller's buffer.

  Verified end to end: a loud external sidechain now ducks a compressor by -7.3 dB where the pre-fix build measured 0.00 dB, with an internal-sidechain control confirming the change is attributable to sidechain routing.

  On the Python side, `process_audio` now sizes the sidechain buffer from `plugin.sidechain_channels` instead of the main input width, and `process_audio_to_file` matches a sidechain *file* to the sidechain bus rather than the main bus.

- **Offline renders never advanced the host playhead.** `MidiRenderer` did not call `set_transport` at all -- despite already parsing the MIDI file's tempo map to place events -- so anything tempo-synced (a synced delay, an arpeggiator, an LFO, a step sequencer) ran at its own default tempo with the playhead pinned at sample 0, and a rendered file did not follow its own tempo. `process_audio(..., bpm=...)` was only marginally better: it called `set_transport` exactly once, before the block loop, with `position_samples=0`, so the tempo was right but time stood still. Both now push an advancing transport before every block. `MidiRenderer` derives the tempo *and* the beat position from the file's tempo map through a new inverse of the tick-to-seconds conversion it already used, so musical position follows the map rather than wall-clock time scaled by a single tempo -- a file that changes tempo stays in sync. `PluginChain` still cannot be driven this way (there is no chain-level `set_transport`) and is skipped.

- **`process_audio` silently discarded MIDI when a sidechain was supplied.** `mh_process_sidechain` is the only process entry point with no MIDI parameter, so the sidechain block loop collected each block's events and then dropped them. The combination is now rejected with an explanation instead. The underlying gap remains: a MIDI-driven plugin with a sidechain cannot be rendered offline until `mh_process_sidechain` grows MIDI input.

- **Plugin scanning missed single-file VST3 plugins.** `mh_scan_directory` searched only for *directories* matching `*.vst3`. That is right on macOS, where a VST3 is a bundle, but on Windows and Linux a VST3 is very often a single shared library file -- and every such plugin was invisible to `scan_directory`, `Session.scan_directory` and `minihost scan`. Both scan paths now search files as well. Untested on the platforms it affects: every VST3 on the development machine is a bundle.

- **Two- and one-byte MIDI messages were built as three-byte messages.** Program Change (0xC0) and Channel Pressure (0xD0) take one data byte, and System Real-Time (0xF8-0xFF) take none, but every `MH_MidiEvent` was handed to `juce::MidiMessage`'s three-byte constructor, which hard-codes `size = 3` and trips JUCE's own length assertion in debug builds. Message construction now picks the right constructor from `getMessageLengthFromFirstByte`. In a release build this is not observable with the plugins tested -- Program Change, Channel Pressure and Pitch Bend round-trip identically through a MIDI-effect plugin either way -- so the practical impact is the debug-build assertion and malformed `MidiBuffer` contents that a strict plugin could reject.

- **`AudioDevice.send_midi` raced the MIDI input thread.** It pushed onto the same lock-free ring buffer the libremidi input thread owns. That structure is single-producer/single-consumer, so a connected MIDI input plus programmatic sends made two producers on it -- corrupting its indices and losing or duplicating events. This is precisely the combination `MidiMapper`'s own documentation recommends (mapping a pad to `audio.send_midi(...)` while a `MidiIn` is open). Sends now use a separate ring, drained alongside the input ring by the audio thread. `send_midi` must still be called from a single thread; that contract is now documented in both the C header and the Python binding.

- **Desktop: stale audio leaked into live input buffers.** `LiveEngine` cleared its planar input buffers with one contiguous `memset` of `n * channels` floats, which only covers the first channel's worth when the device callback is smaller than the project's block size -- the normal case. Any input node with two or more channels therefore kept the previous block's samples in every channel after the first. Now cleared per channel.

- **Desktop: a device/project sample-rate mismatch was ignored.** The engine logged the device rate and carried on, while transport, metronome and MIDI-clock maths all used the project rate and the plugins had been instantiated at it -- so a 48 kHz project on a 44.1 kHz device played at the wrong speed and pitch with the tempo drifting, and the only symptom was "it sounds wrong". The mismatch is now detected, logged as an error naming both rates and what to change, and exposed via `hasSampleRateMismatch()` so the app can surface it.

### Changed

- **`MH_Info.num_input_ch` / `Plugin.num_input_channels` now report the main input bus only** (C ABI 2.4.0; struct layout unchanged, meaning changed). Sidechain channels are reported separately by `mh_get_sidechain_channels` / `Plugin.sidechain_channels`. Code that sized its main input buffer from this value against a sidechain-configured plugin was over-provisioning; it will now allocate the correct width. Plugins without a sidechain or aux input bus are unaffected -- for them the main bus *is* the total.

- **Supplying sidechain audio to a plugin with no sidechain bus is now an error.** `process_audio(..., sidechain=...)` raises `ValueError` instead of silently discarding the audio. Note that `Plugin(..., sidechain_channels=N)` still succeeds for such a plugin (it simply reports `sidechain_channels == 0` afterwards), so "it opened" was never evidence that a sidechain existed.

### Testing

- **`tests/test_sidechain_channels.py`** -- six tests pinning the per-bus invariants: `num_input_channels` equals the main bus, `sidechain_channels` equals the sidechain bus, the two are disjoint, and buffers sized by the reported counts are accepted. Three fail against the pre-fix build. They need a plugin that genuinely has a sidechain bus and skip otherwise -- no VST3 on the development machine has one, but several MeldaProduction AudioUnits do, and the suite passes against those.

- **`tests/test_scan_and_midi_bytes.py`** -- sixteen tests over scanning and MIDI message lengths. Deliberately modest: each docstring states what it actually pins, since the scan fix is not observable on macOS and the MIDI length fix is not observable in a release build.

- **`tests/test_sidechain_signal.py`** -- the behavioural counterpart to the accounting tests: with a 440 Hz main tone and an 80 Hz sidechain tone, an external sidechain must duck the output (measured -7.3 dB on FabFilter Pro-C 3), while the same audio with the sidechain source set to internal must change nothing (control). Against the pre-fix build the external case measures +0.00 dB -- the sidechain never reached the detector. Needs a compressor that acts on an external sidechain; override with `MINIHOST_TEST_SIDECHAIN_PLUGIN` or it skips.

- **`tests/test_transport_advance.py`** -- nine tests covering the tempo-map inverse (including across a tempo change), a monotonically advancing playhead, beats tracking the configured tempo, no transport being fabricated when no BPM is given, and the MIDI+sidechain rejection. Five fail against the pre-fix code. They assert on what minihost *sends*: `MH_PlayHead` is write-only from the host side and nothing reads it back, so a plugin visibly reacting to host position is not covered.

- Two pre-existing sidechain tests in `tests/test_process_audio_extended.py` were passing against Dexed, which has no sidechain bus at all, so they were asserting that a path which quietly discarded the sidechain "runs". They now skip unless the configured plugin really has a sidechain, and a new test covers the rejection case.

## [0.5.0]

From a code-review pass over the native layer, the Python bindings, the CLI and the desktop app. All three crash-class findings are fixed, along with two subsystems that reported success while doing nothing: MIDI ports and `.vstpreset` interchange. Block-size and sample-rate contracts are now validated where they are configured rather than failing per-block, and the Python bindings finally release the GIL around native work. The C ABI bumps to **2.3.0** (additive: `mh_get_max_block_size`). Behaviour changes are called out under *Changed*.

### Fixed

- **`AudioDevice` corrupted memory whenever the device and plugin channel counts disagreed.** `MH_AudioDevice` allocated its non-interleaved `input_buffers` / `output_buffers` with exactly the *device's* channel count, then handed those pointer tables straight to `mh_process` / `mh_chain_process`, which read the plugin's input count and write its output count -- neither bounded by the device's. Any plugin with more channels than the device walked off the end of a `float**` on the audio thread. Two documented routes in: `AudioDevice(plugin, output_channels=N)` with `N` below the plugin's count (a supported override), and the device simply negotiating fewer channels than requested, since the count is read back from the *actual* device. `AudioDevice(stereo_plugin, output_channels=1).start()` segfaulted the interpreter within one audio callback. Fixed by sizing the buffers to `max(device channels, plugin inputs, plugin outputs)` and zero-filling any channel the device does not supply; the public `channels` property still reports the device's own count, because `write_input` interleaves against it. Surplus plugin outputs beyond the device's channel count are truncated at the interleave step (a downmix is left as future work).

- **`MidiIn.open` / `MidiIn.open_virtual` registered a dangling pointer -- the whole standalone MIDI-input feature was a use-after-free.** Both factories built a `MidiIn` on the stack, passed `&m` to `mh_midi_in_open` as the callback's `user_data`, then returned *by value*; nanobind move-constructed the result into its heap instance, leaving the C layer pointing at destroyed stack memory. The first MIDI byte received dereferenced it, acquired the GIL, and called through a garbage `nb::callable`. Affected `MidiIn`, `MidiMapper`, `minihost midi -m`, and the control-surface workflow. Fixed by heap-allocating in the factory, registering the final address, and transferring ownership to Python (`nb::rv_policy::take_ownership`); `MidiIn`'s move operations are now `= delete`d alongside its copy operations, so the address can never change after registration -- a compile-time guard against the same mistake returning.

- **MIDI was non-functional on every platform: the back-end was never compiled in.** `libminihost_audio` builds libremidi in header-only mode, which bypasses libremidi's own CMakeLists and therefore never defined the platform back-end macro its `cmake/libremidi.{macos,winmm,alsa}.cmake` would have set. `backends.hpp` fell through to `#define LIBREMIDI_DUMMY`, so every shipped build -- macOS, Windows, Linux, wheel, CLI and desktop app -- compiled a stub: `midi_get_input_ports()` / `midi_get_output_ports()` always returned `[]`, every open failed, and `AudioDevice(midi_input_port=...)` silently never connected. Linking `-framework CoreMIDI` / `winmm` / `libasound` is necessary but not sufficient; the macro is what pulls the back-end in. Enabling it exposed three further defects, all fixed here:

  - **Virtual and software ports were filtered out.** libremidi defaults to `track_hardware=true, track_virtual=false`, and its CoreMIDI classifier treats any endpoint without a hardware entity as virtual -- which covers macOS IAC buses, ALSA/JACK software ports, and endpoints published by other applications. minihost constructed the observer with all defaults, so enumeration returned nothing while the platform reported ports. It now passes an explicit `observer_configuration` tracking hardware, virtual and "any", and enumeration matches the platform's own view.

  - **Enumeration and port opening crashed or spuriously failed in a long-lived process.** libremidi's CoreMIDI back-end pumps the *calling thread's* CoreFoundation run loop (`CFRunLoopRunInMode`) at six sites, including `observer::get_{input,output}_ports` and `midi_{in,out}::open_port`. On an application main thread that is also driving JUCE, CoreAudio and plugin code, that single pass dispatches whatever those subsystems have queued -- including work whose owner has already been torn down. Enumeration killed the process (`SIGABRT` / `SIGBUS`), uncatchably, because `get_input_ports()` is `noexcept` and a throw inside it reaches `std::terminate` before any handler can unwind; `open_port` threw `std::bad_function_call` from an unrelated queued callback, surfacing as a spurious "failed to open MIDI port". Both are fixed by running these calls on a fresh thread whose run loop has nothing queued, so the pump is a no-op. Message delivery is unaffected -- CoreMIDI invokes `MIDIReadProc` on its own thread, not the run loop of whichever thread created the port.

  - **Failures reported "Unknown error".** libremidi signals with `stdx::error`, which deliberately does not derive from `std::exception`, so the `catch (const std::exception&)` arm missed it entirely and the `catch (...)` fallback discarded the reason -- which is what made the above undiagnosable. All four open paths now share a helper that handles `stdx::error` explicitly, falls back to `std::exception`, and otherwise names the dynamic exception type.

  Verified on macOS/CoreMIDI only. The WinMM and ALSA macros mirror libremidi's own cmake but have not been built or exercised; neither platform has the run-loop hazard that motivated the isolation work. Both want a smoke test before release.

- **`.vstpreset` files were not interoperable, and loading a real one silently did nothing.** JUCE's VST3 host does not expose the raw component state through `getStateInformation`: it emits its own `<VST3PluginState>` XML (with the raw `IComponent` / `IEditController` chunks base64'd inside) wrapped in `copyXmlToBinary`'s `'VC2!'` container. minihost wrote that whole container into the file's `Comp` chunk, where the format specifies the *raw* component state -- so the resulting file was readable only by minihost, and any other host would hand those bytes to `IComponent::setState` and get garbage. The reverse was worse: `load_vstpreset` fed a genuine preset's raw chunk to `set_state()`, JUCE's `setStateInformation` failed its `getXmlFromBinary` check and simply returned, and the user saw success with the default patch still loaded. Both directions now convert between the two representations, so saved files are spec-shaped and third-party presets actually apply. Presets written by older minihost versions are detected and passed through unchanged -- the discriminator is the `<VST3PluginState>` root element rather than the `VC2!` marker, because a JUCE-*built* plugin returns its own `copyXmlToBinary` blob from `IComponent::getState` and the marker alone does not separate the layers.

- **`set_state` / `set_program_state` could never report failure.** `setStateInformation` is `void`, so a plugin (or format wrapper) that rejects a blob does so silently and minihost returned 1 regardless -- `Plugin.set_state(b"garbage")` "succeeded". This is what let the `.vstpreset` defect above stay invisible, and it applies to every state path (`--state`, `--vstpreset`, `load_chain`, project `state_b64`, and the batch loop's per-file reset). There is no format-agnostic way to ask a plugin whether it accepted a state, so the call is now *observed*: parameter values are snapshotted before and after, falling back to comparing the serialized state only when parameters cannot tell the cases apart; if nothing moved and the caller asked for something different from what was already loaded, the blob was ignored and the function returns 0. False negatives remain possible for a plugin whose serialized state is non-deterministic -- the safe direction, since it never reports failure for a state that *was* applied. Costs one extra `getStateInformation` per call.

- **`mh_chain_process_auto` dereferenced null channel pointers on the documented "silence" contract.** `minihost.h` documents a `NULL` input/output pointer table as "supply silence / discard output", and `mh_process_midi_io` honors it, but the chain's automation path built a non-null table *of null pointers* and passed it down, so `mh_process_midi_io` saw a truthy table and `memcpy`'d from `NULL`. Not reachable from Python (which always materializes arrays), but it is a documented C API contract with shipped C/C++ consumers. Null-ness is now propagated.

- **Block-size and sample-rate mismatches failed per-block, or not at all.** Nothing could ask a plugin what block size it had been prepared for, so no layer validated and every mismatch surfaced late and unhelpfully. `PluginChain` hard-coded `max_block_size = 8192`, advertising a ceiling no member could honour: a caller sizing blocks against it passed the Python shape check and then failed inside `mh_process` with "Chain process failed", naming neither the real limit nor the plugin imposing it (and every intermediate and dry-mix buffer was allocated for 8192 frames regardless of need). `AudioDevice` never compared the device period to the plugin's limit, so every process call was refused, the return value was ignored, and -- because the output buffers are allocated once and never cleared -- the device replayed the previous block indefinitely as a buzz, with no error anywhere. `PluginGraph` documented that plugin nodes must match its sample rate and block size but checked neither, so a rate mismatch rendered silently at the wrong rate. All three now validate at configuration time with messages naming both numbers and what to change; the chain derives its limit as the minimum across its members, and the audio callback additionally zero-fills on a refused block so an unexpected failure degrades to silence rather than a buzz. Validation is against the device *period*, not the 2x internal headroom on the conversion buffers -- the common case of a plugin and device sized alike keeps working.

- **The Python bindings never released the GIL around native work.** Every process call held it for the full duration of `processBlock`, so a multi-threaded host got no parallelism whatsoever (measured 1.00x on 4 threads), and plugin construction, state save/restore and audio-file I/O all blocked the interpreter. It also meant a `MidiIn` callback -- which must acquire the GIL -- could not run until the in-flight process call returned, delaying or dropping MIDI in exactly the live-control scenario the API is for. The pure-native bindings now carry `nb::call_guard<nb::gil_scoped_release>`; the ones that also handle Python lists or bytes (`process_midi`, `process_auto`, `render_block`, `get_state` / `set_state`, the audio-file functions) release it around just the native call, with errors raised only after it is reacquired. Same workload now measures 3.47x on 4 threads.

### Added

- **`mh_get_max_block_size` (C API, additive -- ABI 2.3.0).** Reports the largest block a plugin was prepared for. Exposed in Python as `Plugin.max_block_size` and `PluginChain.max_block_size` (the latter being the minimum across its plugins), so callers can size blocks against the real limit instead of guessing. `process_audio`'s internal block size now derives from it rather than a hard-coded 512.

- **`vst3_state_split` / `vst3_state_join` (Python bindings).** Convert between a JUCE VST3 plugin-state blob (what `Plugin.get_state()` returns) and the raw component / controller chunks the `.vstpreset` format specifies. Used by `save_vstpreset` / `load_vstpreset`, and useful directly for anyone moving state between minihost and another host. Implemented in C++ rather than Python because JUCE's base64 is a custom variant -- a `"<size>."` prefix followed by LSB-first 6-bit groups over its own alphabet -- so `juce::MemoryBlock`'s own codec is used and stays in step with JUCE by construction. `vst3_state_split` raises for anything that is not a JUCE VST3 *host* blob, which is also the discrimination the preset loader relies on.

### Changed

- **`Plugin.set_state` / `set_program_state` now raise on a blob the plugin demonstrably ignored** (see *Fixed*). Previously every call reported success. Code that was unknowingly feeding a plugin state it could not use will now see a `RuntimeError` where it used to see a silent no-op. Valid states -- including restoring the state a plugin is already in -- are unaffected.

- **`save_vstpreset` now writes the raw component chunk** rather than JUCE's container, making the files readable by other VST3 hosts. Files written by earlier versions still load (they are detected and passed through), so this is forward-compatible in the direction that matters; presets written by this version are *not* readable by older minihost.

- **`PluginChain.max_block_size` is now the minimum across its plugins**, not a fixed 8192. Chains whose members were opened with a smaller block size will reject an oversized block up front (with the real limit named) where they previously accepted it and failed mid-process. `AudioDevice` likewise now refuses to open when the plugin cannot span the device period, instead of opening and playing a buzz.

- **MIDI port enumeration now includes virtual and software ports** (IAC buses, ALSA/JACK software ports, endpoints published by other applications), where previously it reported hardware ports only -- moot in practice, since the back-end was a stub and nothing was reported at all.

### Testing

- **`tests/test_audiodevice_channels.py`** -- seven tests over the `AudioDevice` channel-count regression: narrower and wider device counts, the default path, duplex capture, and the `enable_input()` ring-buffer path. Confirmed adversarially (reverting the fix makes the suite crash pytest with SIGSEGV).

- **`tests/test_midiin_lifetime.py` + `tests/coremidi_loopback.py`** -- structural tests for the `MidiIn` lifetime fix plus an end-to-end reproduction that drives real MIDI through a minihost *virtual* input port and asserts the callback fires with an intact payload. The loopback is self-contained (a small ctypes CoreMIDI sender; no IAC bus, no second application, no new dependency), so it runs on any macOS machine. Confirmed adversarially: reintroducing the lifetime bug segfaults pytest at exactly that test. Four further tests need a real MIDI input port and skip without one.

- **`tests/test_vstpreset_interop.py`** -- nine tests against a real plugin covering chunk extraction, a split/join round-trip through the plugin, save/load parameter restoration, a foreign-style preset built from a raw chunk, legacy pass-through, and a corrupt preset that must now raise instead of silently doing nothing. Both interop tests fail against the pre-fix code. Interop with an actual *third-party* preset file remains unverified -- a filesystem-wide search found none on the development machine -- so that claim rests on the written chunk being byte-identical to what the plugin's own `IComponent::getState` produced.

- **`tests/test_block_size_contracts.py`** -- nine tests pinning the new behaviour across all three layers: the chain's derived limit and its up-front rejection, both graph preconditions, the device refusing an undersized plugin with an actionable message, and -- importantly -- that a plugin and device sized alike still opens, which an earlier draft of the validation wrongly rejected.

- **`tests/test_concurrency.py::test_process_releases_the_gil`** -- deliberately not a threaded-vs-serial speedup assertion, which would conflate minihost's behaviour with the plugin's: Dexed (the default test plugin) serializes internally and measures 0.89x on 4 threads where three other VST3s measure 3.3-3.7x. It instead spins a counter in a background thread and samples it either side of one long `process()` call -- 0 increments before the fix, ~250k after, for both a self-serializing and a well-behaved plugin. It has no "too fast to measure" guard on purpose: holding the GIL makes the call *faster*, so such a guard skipped exactly the regression it exists to catch.

- Mock-based tests in `tests/test_vstpreset.py` and `tests/test_cli.py` were asserting the old pass-through behaviour with arbitrary fake state bytes; they now use realistic JUCE-format blobs and assert the corrected contract.

## [0.4.2]

### Added

- **Autosave / crash recovery in the desktop app.** Plugins run in-process, so a misbehaving plugin can crash the app and lose unsaved canvas edits. The working `ProjectDocument` is now snapshotted to a sidecar (`autosave.json` + an `autosave.meta` recording its origin path, next to the desktop settings) on a heartbeat timer whenever it has unsaved edits, so a crash costs at most a few seconds of editing. A clean exit and every explicit Save delete the sidecar; a *surviving* sidecar on the next launch means the previous session ended without a clean shutdown, and the app offers to recover it (re-associating the original path so Save writes back to the right file). The About dialog and README now document the in-process limitation. The sidecar write/parse/clear mechanics are covered headlessly by `minihost_desktop --autosave-selftest=<project.json>` (`tests/test_desktop_autosave.py`); the timer and recovery dialog are GUI-thread orchestration and stay manual, as with undo/redo. All file paths in the schema are absolute, so the sidecar's own location does not affect reload.

- **Save-before-quit prompt in the desktop app.** Quitting with unsaved changes (window close, File > Quit, Cmd+Q, OS logout) now shows a Save / Don't Save / Cancel dialog instead of silently discarding the work; Save quits only after the write lands (routing through the Save As chooser for an untitled project), Cancel keeps the app open. This tracks a distinct "changed since last explicit Save" flag, separate from the autosave heartbeat's dirty flag, and is reset by Save / Save As / New / Open. Headless and single-plugin modes are unaffected (they never arm it).

- **Opt-in input resampling in the project renderer.** Project inputs are still strict about sample rate by default (a file whose rate differs from the project rate is an error), but an input node now accepts an optional `resample: true` field that converts a mismatched file to the project rate at load time. Both loaders implement it over the same C resampler (`mh_audio_resample`, miniaudio linear + 4th-order anti-aliasing low-pass): Python's `render_project` resamples via `audio_io.resample`; the C++ desktop resamples in `loadProject`. Because it is the same underlying function, a resampled render is bit-identical between the two pipelines (asserted in `tests/test_desktop_smoke.py::test_resample_render_parity`). The desktop's "Add Input..." flow offers a Resample / Add-as-is choice on a rate mismatch instead of the old warning, and the per-input Properties dialog exposes the flag. The schema stays version 1 (additive; the field is only emitted when true). No new dependency -- `libsamplerate` is not required.

- **`morph` command in the Python CLI.** Brings the `minihost` CLI to parity with the C/C++ front-ends and the library API: `minihost morph PLUGIN [-t T]` captures snapshots A and B from factory programs (`--a-program` / `--b-program`) or saved state files (`--a-state` / `--b-state`), interpolates at blend `-t` (default 0.5), prints an A/B/blend table (or `--json`), and optionally `--apply` / `--save`s the result. Built on the native `Plugin.morph_capture` / `morph_apply` bindings plus `minihost.lerp_params`. Documented in the CLI reference.

### Fixed

- **C and C++ front-ends aborted (SIGABRT) at process exit after loading a plugin.** Neither `minihost_c` nor `minihost_cpp` shut the dedicated JUCE plugin thread down, so on exit its `std::thread` was still joinable and `std::terminate` fired -- any command that loaded a plugin (`params`, `load-preset`, `process`, `morph`, ...) exited with code 134 after printing correct output. Latent since the plugin-thread work in 0.3.0 (the front-ends were last synced at 0.2.x). Fixed by bringing the thread up with `mh_message_thread_init()` and registering `mh_message_thread_shutdown()` via `atexit` at the top of `main`; constructing the thread before the registration makes C++ teardown ordering run the shutdown before the thread's own destructor. Surfaced by the new CLI conformance test's exit-code check.

### Testing

- **CLI conformance test (`tests/test_cli_conformance.py`).** Runs the same deterministic data commands (`probe`, `info`, `params`, `presets`, `morph`) through both `minihost_c` and `minihost_cpp` and asserts their stdout is byte-identical and both exit cleanly, so the two independently-written front-ends can no longer drift apart silently. Discovers the binaries via `MINIHOST_C_BIN` / `MINIHOST_CPP_BIN` or the build tree (preferring the most recently built), and skips when the binaries or a test plugin are absent (same plugin-gated pattern as the other integration tests).

## [0.4.0]

Additive release: a callable composition layer over the existing routing classes (Python), plus parameter morphing pushed down into the C library and its C/C++ front-ends. The C ABI bumps to **2.2.0** (additive).

### Added

- **Parameter morphing in the C library (`libminihost`).** The A/B parameter-interpolation capability that previously existed only as the pure-Python `minihost.morph` module is now a first-class part of the C API: `mh_morph_capture` (snapshot every parameter's normalized value), `mh_morph_apply` (restore a snapshot, clamped to `[0, 1]`), `mh_morph_lerp` / `mh_morph_lerp_per_param` (interpolate two snapshots with a scalar or per-parameter blend), and `mh_morph` (interpolate and apply in one call). Composed from the existing `mh_get_param` / `mh_set_param` / `mh_get_num_params` entry points, so each per-parameter access keeps class-2 thread safety (safe to overlap `mh_process`); the lerp helpers are pure array math. Additive symbols only; the C ABI moves to 2.2.0.

- **`morph` command in the C and C++ front-ends.** Both `minihost_c` and `minihost_cpp` gain a `morph PLUGIN` subcommand exposing the new API: capture snapshots A and B from factory programs (`--a-program` / `--b-program`) or saved state files (`--a-state` / `--b-state`), interpolate at a blend `-t` (default 0.5), print an A/B/blend table (or `--json`), and optionally `--apply` and `--save` the morphed state. Defaults to morphing factory programs 0 and 1 when no source is given. The two front-ends produce byte-identical output.

- **Native morph bindings on `Plugin` (Python).** For parity with the C/C++ front-ends, the morph C API is now bound at the nanobind layer: `Plugin.morph_capture()` (snapshot as a list, one value per parameter), `Plugin.morph_apply(values)` (restore, clamped; `ValueError` on length mismatch), and `Plugin.morph(a, b, t)` (interpolate, apply, and return the applied snapshot). These run natively in a single call and are distinct from the existing duck-typed pure-Python `minihost.morph` module, which is unchanged.

- **`minihost.Compose` -- callable, composable audio pipelines.** An [audiomentations](https://github.com/iver56/audiomentations)-style layer over the native `Plugin` / `PluginChain` / `PluginBus` classes. Where those model *real-time* signal routing, `Compose` models an *offline* pipeline: an ordered list of transforms applied to a whole buffer and returned as a new one. A pipeline is callable in the audiomentations idiom (`fx(samples, sample_rate=...)`), preserves the input container family (`AudioBuffer` in -> `AudioBuffer` out; numpy in -> numpy out; 1-D in -> 1-D out), and owns/closes the plugins it holds (`close_children=True` by default) so an effect chain collapses to a single `with`. A `.to_file(input, output)` convenience reads, processes, and writes in one call. Tails are handled once at the pipeline boundary: a numeric `tail_seconds` pads the input up front so every element rings out, and `tail_seconds="auto"` over-renders then trims trailing silence. Sample rate is validated against each native processor's construction rate and never silently resampled (a mismatch raises).

  A *transform* is a native processor, a nested `Compose`, one of the pure-python transforms below, one of the stochastic combinators below, or any callable `fn(audio, sample_rate) -> audio`. The working type is `AudioBuffer`, so numpy stays an optional dependency (imported only for numpy input, `tail_seconds="auto"`, or the transforms that need it).

- **Pure-python transforms.** Deterministic, `AudioBuffer`-native, usable inside or outside a pipeline: `Gain(db)`, `Normalize(peak_dbfs=-1.0)` (silence passes through), `Trim(start, duration)` (time window in seconds), and `Fade(fade_in, fade_out)` (linear fades in seconds). Each returns a new buffer.

- **Stochastic combinators for data augmentation.** `Maybe(transform, p=)`, `OneOf([...], weights=)`, `SomeOf(n, [...])` (fixed count or a `(min, max)` range), `RandomParam(plugin, param, lo, hi)` (set a plugin parameter at random in normalized units, then process), and `AddGaussianNoise(min_amplitude, max_amplitude)`. `Compose(seed=...)` seeds the pipeline RNG and `Compose(shuffle=True)` randomizes transform order per call, so a pipeline is reproducible across runs but varied across calls. The routing combinators use Python's `random` module (no numpy); only `AddGaussianNoise` uses numpy, seeded deterministically from the pipeline RNG.

  All exported at the package top level. Coverage in `tests/test_compose.py` (34 numpy-only tests plus 6 gated on a real plugin). A runnable walkthrough in `examples/compose.py` and a dedicated [Composition Pipelines](composition.md) documentation page.

## [0.3.2]

### Fixed

- **Process-exit hang on Linux from the dedicated plugin thread (completes the 0.3.1 fix).** 0.3.1 fixed the *import* hang, but the plugin thread could still be started by a plugin-load *attempt* -- including a load of a nonexistent path -- which created a JUCE `MessageManager` on a background thread. Left alive, that MessageManager deadlocked process exit on Linux: the test suite passed, then the process hung until killed. (macOS/Windows tolerated it.) Two changes: (1) the plugin thread is now only started for a plugin that actually exists on disk, so a failing/probing load never touches JUCE; and (2) the thread is cleanly stopped and its MessageManager torn down on its own thread at interpreter exit, via an `atexit` handler (`mh_message_thread_shutdown`). Well-behaved processes now exit promptly after using a plugin.

## [0.3.1]

### Fixed

- **Headless / CI hang from the dedicated plugin thread (regression in 0.3.0).** 0.3.0 started the plugin thread eagerly at `import minihost` and initialized it with `juce::initialiseJuce_GUI()`. That pulls in GUI/display setup which blocks in a headless environment with no X server (e.g. a Linux manylinux/CI container), so `import minihost` -- and therefore any process using the package -- could hang. macOS and Windows were unaffected. Two fixes: (1) the plugin thread now creates only the JUCE `MessageManager` (`MessageManager::getInstance()`), not the GUI subsystem -- the same MessageManager that plugin construction already created on the headless path before 0.3.0; and (2) the thread starts lazily on the first plugin load instead of at import, so a process that never loads a plugin does no JUCE initialization at all. `open_async` and cross-thread plugin use are unchanged. (The CI wheel job also gained a `timeout-minutes` backstop so a future hang fails fast instead of running to the 6-hour ceiling.)

## [0.3.0]

Bug fixes on flagship paths (sample-accurate automation, honest channel counts), additive features (preset morphing, zero-copy channel views, BWF metadata, a `MIDI_OUT_CAPACITY` constant), and a threading overhaul: minihost now runs a dedicated native plugin thread so plugins are safe to use across threads and `open_async` works for real. Additive C symbols only (`mh_message_thread_init` in `minihost.h`, `mh_audio_write_bwf` in the audio-file library); the process/parameter ABI is unchanged. From a code-review pass over the native layer, the Python bindings, and the test suite.

### Fixed

- **Sample-accurate automation dropped in-block parameter changes.** `mh_process_auto` and `mh_chain_process_auto` computed the chunk boundary from the pending parameter change *before* the apply-loop advanced past changes already due at the chunk start. When two or more changes fell in a single process block, every change after the first was silently swallowed (never applied) rather than mis-timed. Fixed by applying all due changes first, then setting the chunk boundary from the next still-pending change. Regression coverage in `tests/test_process_auto_automation.py` (reproduced against the pre-fix build, passes after).

- **Honest channel counts.** A plugin now reports its true JUCE channel counts instead of an inflated minimum of one: a synthesizer with no audio input reports `num_input_channels == 0` (was `1`), and a rare 0-output plugin reports `0`. The internal processing buffer keeps a one-channel floor so a pure-MIDI plugin still receives a valid buffer, and the Python process pipeline was made synth-input-aware. **Potentially breaking** for callers that assumed at least one input channel; buffer validation is "at least N", so over-provisioned callers are unaffected. Relatedly, the offline MIDI renderer now honors a plugin's real output channel count (`max(num_output_channels, 1)`), so a genuine mono plugin renders one channel instead of a stereo file with a silent second channel.

### Added

- **`AudioBuffer.channel_view(start, count)`.** Returns a new `AudioBuffer` that aliases (zero-copy) the contiguous channel range `[start, start+count)` of the parent. Writes are visible in both directions; the parent is pinned for the view's lifetime. Channel ranges only (channels are stored contiguously); frame slicing would require strided views and is not offered. Also available on `AudioBufferD`.

- **Preset morphing (`minihost.morph`).** A small utility for A/B parameter interpolation: `capture(plugin)` snapshots normalized parameter values, `apply(plugin, snapshot)` restores them (clamped to `[0, 1]`), and `lerp(a, b, t)` interpolates two snapshots (scalar or per-parameter `t`). `morph(plugin, a, b, t)` interpolates and applies in one call. Re-exported as `capture_params` / `apply_params` / `lerp_params` / `morph_params`. Operates on per-parameter values, not opaque VST/AU state blobs.

- **Broadcast Wave (BWF) metadata.** `write_audio(path, data, sr, bwf=dict(...))` embeds an EBU Tech 3285 `bext` chunk in WAV output (`description`, `originator`, `originator_reference`, `origination_date`, `origination_time`, `time_reference`). New C entry point `mh_audio_write_bwf` in the audio-file library; `mh_audio_write` is now a NULL-metadata wrapper over it. WAV only (FLAC raises). `smpl` sampler-loop chunks are out of scope.

- **`minihost.MIDI_OUT_CAPACITY` constant.** Publishes the default 256-event MIDI-output buffer capacity used by `process_midi` / `process_auto`; a returned MIDI-out list whose length equals the capacity signals possible truncation (raise the per-call `midi_out_capacity` for dense streams).

### Verified

- **DLPack export confirmed zero-copy.** A review flagged `AudioBuffer. __dlpack__` as possibly not returning a DLPack capsule; empirical testing refuted it -- it returns a proper `"dltensor"` capsule, shares memory with `as_ndarray()`, and buffer mutations are observed through the view (no hidden copy). No code change. Added `tests/test_dlpack_interop.py` (numpy always; torch/jax via `importorskip`). Documented nuance: `numpy. from_dlpack` imports read-only (numpy 2.x default); use `as_ndarray()` for a writable zero-copy view.

### Documentation

- **Process-vs-control threading contract on `Plugin`.** Added a class docstring making the Python-facing contract explicit: the lock-free process methods must be called from a single thread and must not overlap the reconfiguring setters (`sample_rate`, `set_state`, `set_processing_precision`, `set_non_realtime`, `reset`), which reconfigure the audio pipeline and are not protected by the internal control-thread mutex.

### Changed

- **Dedicated native plugin thread: `open_async` now works, and plugins are thread-safe for control operations.** JUCE VST3/AU instances are thread-affine -- construction, destruction, and control-plane queries (state, parameter text, program names, reset, sample-rate, precision) must all run on one thread -- which made the old `open_async` deadlock (it built the plugin on a short-lived thread and used/closed it from another). minihost now runs one persistent JUCE plugin thread and marshals every thread-affine control op onto it via an internal request queue, so a plugin may be constructed on one thread and used or closed from any other. The real-time `process*()` path stays lock-free on the caller's thread. Enabled by default; opt out with the `MINIHOST_MESSAGE_THREAD=0` environment variable (after which cross-thread plugin use is unsafe again).

  Consequently `open_async` is now a plain, safe async loader: it returns a `Future` resolving to a real `Plugin` (no proxy wrapper, no warning), usable and closable from any thread. Loads are serialized on the plugin thread, so this is non-blocking rather than parallel loading. The prior investigation (a persistent-worker proxy, then a background JUCE dispatch loop) is superseded; the working design marshals via a plain condition-variable queue rather than JUCE's own `callFunctionOnMessageThread` / `CallbackMessage`, which proved unreliable on macOS.

## [0.2.1]

All changes below are additive or bug fixes relative to the published 0.2.0; the C ABI is bumped to **2.1.0** (additive).

### Added

- **Persistent plugin-scan cache.** A new `minihost.plugincache` module keeps a JSON index of probe metadata keyed by plugin path, with a filesystem fingerprint (mtime + size) so stale entries are re-probed automatically. A repeat scan of an unchanged directory probes nothing and returns instantly; failed probes are remembered (not retried every scan). API: `scan()`, `info()`, `query(...)` (filter by format / name / vendor / MIDI / I/O), `all_entries()`, `prune()`, `clear()`, `stats()`, `cache_file()`. Convenience re-exports `minihost.scan_plugins` and `minihost.query_plugins`. The CLI `scan` now uses the cache by default (`--refresh` re-probes, `--no-cache` bypasses); `info --probe` is served from it; and a new `cache` subcommand (`path` / `stats` / `list` / `prune` / `clear`) manages and queries the index. Cache location honors `MINIHOST_CACHE_DIR` (defaults to the platform cache dir). Probe-level metadata only -- parameter lists need a full load and are not cached.

- **`AudioBufferD` (float64 audio buffer).** A double-precision sibling of `AudioBuffer` with the identical surface (indexing, slicing, DSP ops, numpy interop, DLPack). It completes the numpy-optional story for float64: `AudioBufferD.dtype == "float64"`, `as_ndarray()` returns a float64 view, and its DLPack export is float64 -- so it feeds `Plugin.process_double(in, out)` directly, with no numpy arrays involved. Implemented as a single C++ template (`MhAudioBufferT<T>`) instantiated for float and double, so the two classes never drift. `AudioBuffer` (float32) is unchanged. Exported as `minihost.AudioBufferD`.

- **MIDI routing in the project schema.** Offline project files (`minihost.render_project`) can now express MIDI graphs, not just audio. New node kinds: `midi_input` (offline `source` .mid path), `midi_output` (offline `sink` .mid path), `midi_filter` / `midi_transpose` / `midi_velocity_curve` (per-event processors), and `midi_merge` (fan-in). MIDI edges share the `edges` list, tagged `"kind": "midi"` (audio edges remain the default). This mirrors the node/edge schema the desktop app already parses, so projects round-trip between the two; the offline `source`/`sink` fields are additive (the desktop uses live `port_name`). The renderer reads each `source` at the project sample rate, stages events per block, and drains `midi_output` nodes to their `.mid` sink (written on a canonical 120 BPM / 480-tpq grid; sample timing preserved). New helper `minihost.midi_file_to_events(midi_file, sample_rate)` exposes the tempo-mapped flatten used internally. Schema version stays `1` (additive); audio-only projects are unaffected. Still deferred: parameter automation in the schema, and offline `.mid` reading in the desktop's headless `--render-project` (its renderer is live-port based).

- **`PluginBus` MIDI-out merge.** `PluginBus.process_midi` now collects the MIDI produced by each branch (from each branch's first plugin) and returns it as an `(events, overflow)` tuple -- `events` is the merged stream, a list of `(sample_offset, status, data1, data2)` tuples stably sorted by sample offset (events at the same offset keep branch order), and `overflow` flags truncation (see the Changed entry below). Previously it returned `None` and discarded branch MIDI. This completes the bus for parallel MIDI effects (e.g. a layer of arpeggiators driven by one part); 0.2.0 had shipped MIDI fan-*in* but not the *out* merge. The audio fan-out-and-sum is unchanged.

  New C ABI entry point `mh_bus_process_midi_io` (additive). It appends each branch's MIDI into the caller's buffer with no internal allocation, then stably insertion-sorts by offset on the audio thread, and reports truncation via an optional `midi_out_overflow` flag (conservative: it may flag an exact fill, but never misses a real drop). The existing `mh_bus_process_midi` (no MIDI out) is unchanged.

- **ThreadSanitizer stress harness for the lock-free ring buffers** (`tests/tsan/`, run with `make tsan`). Drives the SPSC MIDI and audio ring buffers from two threads under ThreadSanitizer and asserts SPSC correctness (exactly-once, in-order delivery with no field tearing). Contributor tooling; the instrumented binary is not shipped in the wheel.

### Changed

- **Configurable MIDI-out capacity.** `Plugin` / `PluginChain` / `PluginBus` `process_midi` and `process_auto` take an optional `midi_out_capacity` argument (default 256), so callers with dense MIDI output (e.g. MPE) are no longer silently capped. A returned event count equal to the capacity means output may have been truncated -- raise the cap if so. **`PluginBus.process_midi` now returns an `(events, overflow)` tuple** (the bus merge was added in this same release, so this is not a break vs 0.2.0): `overflow` is `True` when the merge filled `midi_out_capacity` and events may have been dropped. `Plugin` / `PluginChain` `process_midi` still return a plain list (single-plugin output; use the count-equals-capacity check).

### Fixed

- **Data race on the audio input callback.** `mh_audio_set_input_callback` wrote the callback pointer (and user-data) from the app thread while the audio thread read it unsynchronized (`minihost_audio.c`), risking a torn pointer on weakly-ordered CPUs. The pointer is now published / read through portable acquire/release atomics (`__atomic` builtins on Clang/GCC, `Interlocked` intrinsics on MSVC -- no C11 `<stdatomic.h>`, which MSVC gates behind an opt-in flag), with user-data published before the pointer, so the audio thread never observes a torn or mismatched callback. Callers must still clear (set NULL) before installing a different callback -- the existing live-source start/stop contract.

- **`MidiFile.save()` crash on empty tracks.** The vendored midifile library's `write()` called `vector::back()` on a track with no events (undefined behaviour -> segfault). A fresh `MidiFile` has one empty track, so even `MidiFile().save()` crashed; likewise `add_track()` followed by writing events only to the new track left track 0 empty and crashed on save. Patched `projects/midifile/src/MidiFile.cpp` to emit the end-of-track marker without dereferencing a non-existent last event. (Local patch to the vendored BSD-2 library; re-apply on any re-vendor.)

## [0.2.0]

### Changed

- **BREAKING (0.2.0): routing types renamed, top to bottom.** The parallel-branches-summed type formerly called `PluginGraph` is now **`PluginBus`**, and the general-DAG executor formerly called `GraphV2` is now **`PluginGraph`**. This gives the three routing primitives a clean tier: `PluginChain` (series), `PluginBus` (parallel, summed), `PluginGraph` (arbitrary DAG). As an alpha (0.x) project this is a clean break with no deprecation aliases; package version bumped to 0.2.0.

  The rename goes all the way down to the C ABI (**`MH_API_VERSION` bumped to 2.0.0**, a deliberate incompatible-ABI major bump per the policy in `minihost.h`):
  - bus: `mh_graph_*` -> `mh_bus_*`, `MH_PluginGraph` -> `MH_PluginBus`;

  - DAG: `mh_graph_v2_*` -> `mh_graph_*`, `MH_GraphV2` -> `MH_PluginGraph`, C++ RAII wrapper `minihost::GraphV2` -> `minihost::PluginGraph`.

  The source file names (`minihost_graph.{h,cpp}` for the bus, `minihost_graph_v2.{h,cpp,hpp}` for the DAG) are retained for git history; a header note in each maps the file to its symbol family. Both consumers (the Python wheel and the `minihost_desktop` binary) were updated and build clean. Earlier changelog entries that mention `GraphV2` / `mh_graph_v2_*` describe what is now `PluginGraph` / `mh_graph_*`.

### Added

- **MIDI fan-out on `PluginBus`** -- `PluginBus.process_midi(input, output, midi_in)` delivers the same MIDI events to every branch (each branch's first plugin), making the bus a layering primitive: one MIDI part drives N parallel instruments whose audio is summed with per-branch gain. Muted branches (gain 0.0) are skipped. New C API `mh_graph_process_midi` in `projects/libminihost/minihost_graph.{h,cpp}` (shares the fan-out-and-sum core with `mh_graph_process`; delegates per branch to `mh_chain_process_midi_io` with MIDI output discarded). Branch MIDI *output* is not collected -- that remains a `PluginGraph` (DAG) capability and a possible follow-up. New tests in `tests/test_chain_mix_and_graph.py` (audio-only path parity with empty MIDI; two-branch layering equals the sum of two independent single renders).

- **MIDI routing in `GraphV2`** -- `graph_v2` gains first-class MIDI as a sibling of audio routing. Two new node kinds (`MH_NODE_MIDI_INPUT`, `MH_NODE_MIDI_OUTPUT`) and a separate MIDI edge list (`mh_graph_v2_connect_midi`) let callers express MIDI fan-out, MIDI effect chains (e.g. arpeggiator -> synth), and per-plugin MIDI sources without the old global "fan MIDI to every plugin" hack. One MIDI edge per dst (fan-out from a source is allowed); the topo-sort indegree includes both audio and MIDI edges so dependencies are respected. Plugin nodes whose `produces_midi=1` *and* have an outgoing MIDI edge are dispatched via `mh_process_midi_io` (or `mh_process_auto` with midi_out when automation is also active); their MIDI output is captured into a per-node buffer (default capacity 1024 events) and forwarded along the edge. New C API: `mh_graph_v2_add_midi_input`, `mh_graph_v2_add_midi_output`, `mh_graph_v2_connect_midi`, `mh_graph_v2_set_midi_input_events` (stages caller events for a MIDI_INPUT node per block; borrowed pointer, cleared after `render_block`), `mh_graph_v2_get_midi_output_events` (drains a MIDI_OUTPUT node after render; truncation-aware count). The legacy `mh_graph_v2_set_node_midi` remains for direct staging on plugin nodes that aren't wired through the new routing; if a MIDI edge is connected, the edge takes precedence. C++ wrapper (`minihost::GraphV2::addMidiInput` / `addMidiOutput` / `connectMidi` / `setMidiInputEvents` / `getMidiOutputEvents`) and Python bindings (`GraphV2.add_midi_input`, `add_midi_output`, `connect_midi`, `set_midi_input_events`, `get_midi_output_events`) mirror the C API. 12 new tests in `tests/test_graph_v2_midi.py` (topology validation, passthrough, staging clear-on-render, fan-out, post-compile rejection, MIDI-edge-overwrites-on-same-dst, plugin MIDI input via graph edge matches direct `process_midi` staging).

- **MIDI input / output nodes in `minihost_desktop` project format** -- `ProjectDocument` gains `midi_inputs: [MidiInputNodeSpec]` and `midi_outputs: [MidiOutputNodeSpec]`. Each carries an optional `port_name` (MIDI port name as enumerated by `mh_midi_get_input_name` / `mh_midi_get_output_name`; empty = engine default). `EdgeSpec` gains a `kind` field (`audio` | `midi`); audio remains the default for back-compat. The loader maps MIDI nodes to `mh_graph_v2_add_midi_input/_output` and MIDI edges to `mh_graph_v2_connect_midi`. Migration: pre-MIDI-routing projects that used the per-plugin `receives_midi=true` flag get a synthesized MIDI_INPUT node at load time, wired by MIDI edge to every opted-in plugin -- the in-memory graph behaves identically to the old "fan-out via `mh_graph_v2_set_node_midi`" path. The on-disk schema is left untouched so the migration is non-destructive; new saves use explicit MIDI nodes + edges. `LoadedProject::midi_input_node_ids` exposes the graph node ids (declared + migrated) so `LiveEngine` knows where to stage live device MIDI.

- **Live MIDI input via `libminihost_audio` instead of JUCE** -- `LiveEngine` (`projects/minihost_desktop/src/live.{h,cpp}`) drops `juce::MidiInput` + `MidiInputCallback` in favor of `mh_midi_in_open` (libremidi-backed via `libminihost_audio`). Port enumeration also goes through the C API (`mh_midi_get_num_inputs` / `mh_midi_get_input_name`), used by both the engine (port-name lookup at `setMidiInputDevice`) and `main.cpp`'s MIDI input menu. The lock-free SPSC ring + audio-thread drain is unchanged; only the producer side moved. Settings persistence key renamed `identifier` -> `port_name`; legacy `identifier` values are still read and treated as port names so existing settings files don't break (they'll just miss the device if the JUCE identifier never matched a libremidi port name -- a one-time silent regression rather than a load error). At render time, drained events go to every project `MIDI_INPUT` node via `mh_graph_v2_set_midi_input_events` instead of per-plugin `mh_graph_v2_set_node_midi` calls -- the routing inside the graph then fans out as the user wired it.

- **Canvas support for MIDI nodes and edges** -- `CanvasComponent` renders the new node kinds (`midi_input` in purple, `midi_output` in magenta) and draws MIDI edges as dashed lilac strokes (distinguishable from audio edges even on grayscale displays). Right-click menu gains "Add MIDI Input" / "Add MIDI Output" entries. The edge-create drag is kind-aware: if either endpoint is a MIDI node it creates a MIDI edge with no channel-count validation; the graph compiler enforces MIDI capability at load time. The node property dialog gains a `port_name` field for MIDI nodes, and the canvas-index -> spec-index mapping was extended to handle the two new sections (added after outputs in `rebuildLayout`, preserved by `showNodePropertiesDialog`'s segmentation). `removeNodeFromDoc` sweeps `midi_inputs` / `midi_outputs` alongside the audio specs so deletes leave no orphan references.

- **`DeviceOutputNodeSpec` for live audio device routing (speakers)** -- new project node kind that represents the system audio output device as an explicit graph destination, symmetric with the MIDI nodes. Carries `channels` (default 2) and an optional informational `device_name`. At load time each `device_output` becomes an additional `MH_NODE_OUTPUT` in the graph appended after the file-sink outputs; `LoadedProject::device_output_buffer_indices` records the position in `output_buffers[]` for each so `LiveEngine` can find them. The audio callback sums per-channel across all `device_output` buffers into the device's output channels (extras silenced, overflow dropped). When a project has *no* `device_output` nodes, `LiveEngine` falls back to the legacy "first file-sink output node also plays through speakers" rule, so existing projects continue to work without edits. File rendering allocates a discardable scratch buffer for each `device_output` (the renderer still has to provide every audio output buffer the compiled graph expects) but never writes their samples to disk. Canvas: distinct burnt-orange colour, "Add Device Output (speakers)" context-menu entry, property dialog edits channels + device name, `removeNodeFromDoc` sweeps `device_outputs`, and the channel-validation helper / property-dialog segmentation were extended to cover the new section.

- **`DeviceInputNodeSpec` for live audio device capture (mic / line-in)** -- the input-side counterpart to `device_output`. Carries `channels` (default 2) and an optional informational `device_name`. At load time each `device_input` becomes an additional `MH_NODE_INPUT` in the graph appended after the file-source inputs; `LoadedProject::device_input_buffer_indices` records the slot in `input_buffers[]` for each. The audio callback copies the live device's `inputChannelData` into those buffers each block (extras silenced, surplus device channels ignored); file-source inputs continue to receive silence during live (their WAV data is only consumed by the file renderer). Multiple `device_input` nodes share the same physical device channels, so the same mic feed can drive several independent paths. `LiveEngine::start()` widens the `AudioDeviceManager` setup to enable the required number of input channels (JUCE defaults to 0 inputs); if the device-setup change fails the engine logs and continues with silence rather than refusing to start. File rendering keeps `device_input` buffers zero-filled. Canvas: distinct sky-blue colour, "Add Device Input (mic/line-in)" context-menu entry, property dialog edits channels + device name, `removeNodeFromDoc` sweeps `device_inputs`, channel-validation helper and property-dialog segmentation extended.

- **Node kind registry refactor in `minihost_desktop`** -- the 19 node kinds previously had per-kind logic scattered across ~13-17 sites in 4 files. The new `node_registry.{h,cpp}` consolidates the dispatch into a single `NodeKindEntry` table; adding a new kind is now one entry in `nodeRegistry()` (in canonical order), not a 17-file safari.

  - **`NodeKindEntry`** bundles every per-kind hook: `kind_string`, `colour`, `count` / `id_at` / `erase_by_id` over the doc, `parse` (with `project_dir` for relative-path resolution) / `serialize_all`, `canvas_info` (label + port counts) / `channels_for` (asymmetric for pick_channel / merge_channels), `dialog_title` / `dialog_emit` / `dialog_apply`, `menu_label` / `menu_add`, `is_midi_source` / `is_midi_sink`, and `load_one` (translates a spec into one or more graph nodes + records side effects on `LoadedProject`).

  - **Registry order is the canonical iteration order** for parse, serialize, rebuildLayout, `mapCanvasIndex`, and `loadProject`. Audio inputs (`input`, then `device_input`, then `metronome`) come first, then `plugin` and `mix`, then audio outputs (`output`, `device_output`, `meter`), then MIDI nodes and routing kinds. This ordering is what determines `input_buffers[]` / `output_buffers[]` slot positions in the compiled graph, so the buffer-index bookkeeping (`device_input_buffer_indices`, `metronome_buffer_indices`, etc.) lives inside each kind's `load_one` rather than in `loadProject`.

  - **Collapsed sites in `project.cpp`**: the parser's 19-arm if/else chain (~220 lines) is now `findKind(kind)->parse(n, id, out, project_dir)`. The serializer's 19 `for (... doc.<vec>)` blocks (~140 lines) is `for (entry : nodeRegistry()) entry.serialize_all(doc, push)`. The layout-known-id sweep (19 `for` lines) is one `for` over the registry calling `entry.count` + `entry.id_at`. The loader's 18 hand-coded `addInput` / `addPlugin` / `addMidiInput` / `mh_graph_v2_add_*` chains (~170 lines) is `for (entry : nodeRegistry()) for (i : entry.count) entry.load_one(doc, i, g, id_to_node, *loaded)`. Plugin pre-pass (opening MH_Plugins and reading state) stays outside the loop; `load_one` for `plugin` attaches the already-opened instance to the graph.

  - **Collapsed sites in `canvas.cpp`**: `kindColour` is `findKind(kind)->colour`. `rebuildLayout`'s 19 `addNode` calls (~95 lines) is one `for` over the registry. `removeNodeFromDoc`'s 19 `eraseById` calls (~21 lines) is one `for` calling `entry.erase_by_id`. `addEdgeToDoc`'s 70-line `channels_for` lambda + 12-line MIDI-source/sink detection (~85 lines) become `mapCanvasIndex` + `entry.channels_for` and direct reads of `entry.is_midi_source` / `is_midi_sink`. `showNodePropertiesDialog`'s 466-line per-kind segmentation + dialog UI + OK handler shrinks to ~60 lines using `mapCanvasIndex` + `entry.dialog_title` / `dialog_emit` / `dialog_apply`. The context menu's 22-item hand-coded list collapses to a single loop over the registry (plus 4 hard-coded items for file-chooser flows and the channel-split convenience helper).

  - **Net effect**: `project.cpp` and `canvas.cpp` shrank by ~700 lines combined. `node_registry.cpp` is ~1100 lines (one ~50-line block per kind) but everything per-kind is now collocated in one entry rather than scattered. Adding the next node kind: declare the spec in `project.h`, add it to `ProjectDocument`'s vectors, write one `makeXxx()` builder in `node_registry.cpp`, append it to the registry list. No edits to `parseProjectFile`, `saveProjectFile`, `loadProject`, `rebuildLayout`, `showNodePropertiesDialog`, `addEdgeToDoc`, `showContextMenu`, or `removeNodeFromDoc`. Test suite still **651 passed, 71 skipped** -- no behavioral changes.

- **MIDI processor nodes in `GraphV2` and `minihost_desktop`** -- two new libminihost node kinds underpin four new project-level MIDI processing primitives:

  - **`MH_NODE_MIDI_PROCESSOR`** (libminihost): single MIDI input / single MIDI output. An `MH_MidiProcessorParams` struct selects one of three ops via `MH_MidiOp`:

    - `MH_MIDI_OP_FILTER` -- pass events whose channel bit is set in `channel_mask`. Note On/Off (status `0x80`/`0x90`) additionally require `data1` in `[min_note, max_note]`. System messages (status `>= 0xF0`) always pass.

    - `MH_MIDI_OP_TRANSPOSE` -- add `transpose_semitones` to `data1` of Note On/Off events; results outside `[0, 127]` drop the event. Other event kinds pass unchanged.

    - `MH_MIDI_OP_VELOCITY_CURVE` -- for Note On with non-zero velocity, remap `vel := round(pow(vel/127, gamma) * 127)` clamped to `[1, 127]`. Note On with `vel=0` (MIDI's wire-format Note Off) passes unchanged so downstream consumers still see the note-off.

    - C API: `mh_graph_v2_add_midi_processor(params)`, `mh_graph_v2_set_midi_processor_params(node, params)`. C++ wrapper `addMidiProcessor` / `setMidiProcessorParams`. Python `GraphV2.add_midi_processor(params_dict)` / `set_midi_processor_params(node, params_dict)`.

  - **`MH_NODE_MIDI_MERGE`** (libminihost): N MIDI input ports (one per port), single MIDI output. Concatenates events from all ports into the output buffer, then stable-sorts by `sample_offset` (insertion sort -- typical event counts per block are small and stability matters). C API: `mh_graph_v2_add_midi_merge(num_inputs)`. C++ `addMidiMerge`, Python `GraphV2.add_midi_merge`.

  - **MIDI edges gain `dst_port`**: the existing "one MIDI edge per dst" rule generalizes to "one per `(dst, dst_port)`". Single-port MIDI consumers (plugin, MIDI_OUTPUT, MIDI_PROCESSOR) accept only `dst_port == 0`; MIDI_MERGE accepts `0..num_inputs-1`. New C API `mh_graph_v2_connect_midi_port(src, dst, dst_port)`; existing `mh_graph_v2_connect_midi(src, dst)` becomes a wrapper calling the new function with `dst_port=0`. C++ `connectMidiPort`, Python `connect_midi_port`. The compile pass validates per-port: every required MIDI input port on MIDI_OUTPUT / MIDI_PROCESSOR / MIDI_MERGE must be connected.

  - **Internal**: the `Node::midi_src` single-source field becomes `std::vector<MH_NodeId> midi_srcs` indexed by port; `num_midi_input_ports` is now per-node (0 for sources, 1 for single-port consumers, N for merge). `MidiEdge` carries `dst_port`. The render loop adds two new cases (`MH_NODE_MIDI_PROCESSOR` runs the per-op switch over upstream events; `MH_NODE_MIDI_MERGE` concatenates and sorts). Both use the existing `midi_out_buf` capture-buffer infrastructure; `needs_buf` extends to allocate one for any MIDI source with `has_outgoing_midi_edge`. The audio-edge validator now rejects all four MIDI node kinds (not just MIDI_INPUT/MIDI_OUTPUT) -- only `connect_midi[_port]` can wire them.

  - **Project format**: four new specs (`MidiFilterNodeSpec`, `MidiTransposeNodeSpec`, `MidiVelocityCurveNodeSpec`, `MidiMergeNodeSpec`) map to the two libminihost kinds. Loader translates filter/transpose/velocity_curve to a `MH_NODE_MIDI_PROCESSOR` with the appropriate op; merge maps directly. Edge spec's existing `dst_port` field is now used for MIDI edges too (serialized only when non-zero on MIDI edges to keep audio-only project files backward-compatible).

  - **Canvas**: four new colours (purple-violet family), four context-menu entries ("Add MIDI Filter" / "Add MIDI Transpose" / "Add MIDI Velocity Curve" / "Add MIDI Merge (2 in)"), property dialogs for each (min_note/max_note/channel_mask for filter, semitones for transpose, gamma for velocity_curve, num_inputs for merge). The MIDI-edge-detection in `addEdgeToDoc` treats all four new kinds as MIDI sources / sinks (processors and merges are both). Edge-drag into a `midi_merge` auto-assigns to the lowest unconnected input port (or shows a "merge full" alert when all ports are used). `removeNodeFromDoc` sweeps all four new spec vectors.

  - **Tests**: 13 new tests in `tests/test_graph_v2_midi_processors.py` covering filter note-range and channel-mask behavior, transpose with out-of-range drop, velocity curve identity / compress / zero-velocity preservation, merge concatenation + sort + port-range rejection + per-port compile validation, processor topology validation, `set_midi_processor_params` live updates, and a filter→transpose chain. Test suite: **651 passed, 71 skipped**.

- **Transport-driven `metronome` and `midi_clock` nodes** -- two new project-level node kinds that emit audio / MIDI synchronized to `LiveEngine`'s transport (`transport_bpm_`, `transport_playing_`, `transport_pos_samples_`). No libminihost changes: each rides on an existing graph node kind, and `LiveEngine` fills its buffer / event list per block.

  - **`MetronomeNodeSpec`**: audio source. Maps to an `MH_NODE_INPUT` at the graph layer; `LoadedProject::metronome_buffer_indices` records the input-buffer slot per metronome. `LoadedProject::renderMetronomes(planar_inputs, block_size, nframes, pos, sr, bpm, playing)` runs each block on the audio thread: zero-fills the buffer, identifies beat onsets that fall within the block (using `samples_per_beat = sr * 60 / bpm`), and paints a `freq_hz` sine windowed by an exponential envelope with `decay_ms` half-life. Carries click phase across blocks via `MetronomeState::phase_samples`. Defaults: 1 ch, gain 0.5, 1000 Hz, 20 ms decay. Silent during file rendering and when transport is paused.

  - **`MidiClockNodeSpec`**: MIDI source emitting 24-PPQN Clock (`0xF8`) ticks. Maps to a dedicated `MH_NODE_MIDI_INPUT` per clock (intentionally *not* added to `LoadedProject::midi_input_node_ids`, so device MIDI does not mix with clock pulses). `LoadedProject::stageMidiClocks(...)` builds the per-block event list -- 24-PPQN ticks plus `0xFA` (Start) / `0xFC` (Stop) on transport rising/falling edges via `MidiClockState::was_playing` -- and stages it via `mh_graph_v2_set_midi_input_events`. Connect to a `midi_output` to drive external gear.

  - **Canvas**: distinct teal (metronome) and rust (midi_clock) colours, "Add Metronome" / "Add MIDI Clock" context-menu entries. Metronome property dialog edits channels / gain / freq_hz / decay_ms; midi_clock dialog edits only the id (the node is parameter-free at the project layer). `channels_for` returns the metronome's `channels` on its output port; midi_clock has no audio channels and is rejected by the audio-edge validator (the canvas's MIDI-edge branch routes it through `connectMidi` instead). `addEdgeToDoc` recognizes `midi_clock` as a MIDI source. `removeNodeFromDoc` sweeps both spec vectors.

- **Routing node kinds in `GraphV2` and `minihost_desktop`** -- four new node kinds bring sample-level routing control into the graph without requiring a plugin:

  - **`MH_NODE_PICK_CHANNEL`** (libminihost): takes an N-channel input and outputs a 1-channel signal at `channel_index`. Single input port (N ch), single output port (1 ch). C API: `mh_graph_v2_add_pick_channel(in_channels, channel_index)`. C++ wrapper `addPickChannel`, Python `GraphV2.add_pick_channel`. Validates `0 <= channel_index < in_channels` at add time.

  - **`MH_NODE_MERGE_CHANNELS`** (libminihost): interleaves `out_channels` separate 1-channel inputs into one `out_channels`-channel output. Each input port consumes a single channel and writes it as output channel `dst_port`. Distinct from `mix` (which sums); merge_channels just `memcpy`s each port into its slot. C API: `mh_graph_v2_add_merge_channels(out_channels)`. C++ wrapper `addMergeChannels`, Python `GraphV2.add_merge_channels`.

  - **`GainNodeSpec` and `BusNodeSpec`** (`minihost_desktop`): single-input, single-output gain and labeled passthrough. Both map to `mh_graph_v2_add_mix(1, channels)` at the graph layer (a 1-input mix with a settable gain is a gain stage; with gain=1.0 it's a bus). The project format keeps them distinct so the canvas can present them differently and the bus has no editable gain.

  - **`PickChannelNodeSpec` and `MergeChannelsNodeSpec`** (`minihost_desktop`): direct project-format projections of the new libminihost node kinds.

  - **Canvas**: distinct colours for each, "Add Gain (stereo)" / "Add Bus (stereo)" / "Add Channel Split (stereo -> 2 mono)" / "Add Channel Merge (2 mono -> stereo)" context-menu entries. "Channel Split" is a convenience that creates two `pick_channel` nodes (channel_index 0 and 1) with auto-generated ids `L1`/`R1` -- there's no grouped doc-level "split" entity. Property dialog edits channels / gain / in_channels / channel_index / out_channels per kind. The `channels_for` channel-validation helper handles pick_channel and merge_channels' asymmetric port shapes (pick_channel: input port carries `in_channels`, output is 1; merge_channels: each input port is 1, output is `out_channels`). `removeNodeFromDoc` sweeps all four new spec vectors.

  - **Tests**: 11 new tests in `tests/test_graph_v2_channels.py` covering pick_channel index validation, channel extraction (L and R), connect-time channel-mismatch rejection, merge_channels interleaving, mono-only input port enforcement, the pick->merge identity round-trip, and the gain / bus behaviors via `mix(1, channels)`.

- **`MeterNodeSpec` for real-time per-channel peak visualization** -- new audio sink that captures `max |sample|` per channel each block and surfaces it for canvas display. Carries `channels` (default 2). Graph-wise it's a regular `MH_NODE_OUTPUT` appended after `device_outputs`; its samples are computed but never written to disk and never routed to a device. `LoadedProject::MeterState` holds one `std::atomic<float>` per channel; `LoadedProject::updateMeters(out_buffers, nframes)` is called from `LiveEngine`'s audio callback right after `renderBlock` to refresh the atomics. File renderer allocates scratch for meters but skips the peak update (no GUI is watching). `LiveEngine::loadedProject()` exposes the running project so the canvas can read meter state; both `start()` and `stop()` (including the load-project and close-project paths in `main.cpp`) wire / unwire the canvas via `setLiveProject()`. Canvas: distinct slate-grey colour, "Add Meter" context-menu entry, property dialog edits channels, `removeNodeFromDoc` sweeps `meters`. While a live project is set, the canvas runs a 30 Hz `juce::Timer` that triggers `repaint()`; the paint routine overlays per-channel vertical level bars (green / yellow / red, sqrt-scaled) on the bottom 55% of every meter node by reading the atomics with `memory_order_relaxed`. No measurable audio-thread overhead beyond the per-channel `max(|x|)` reduction.

### Changed

- **`PluginNodeSpec`** -- added cached `accepts_midi` / `produces_midi` flags (best-effort, set at canvas-add time; authoritative values come from `mh_get_info` at load time). The legacy `receives_midi` field is preserved for back-compat with on-disk projects but is marked deprecated in the header comment; new projects should express live MIDI routing via `MidiInputNodeSpec` + MIDI edges.

- **`EdgeSpec`** -- now carries an `EdgeKind kind` field (defaults to `Audio`); MIDI edges set `kind=Midi` and ignore `dst_port`. Serializer emits `"kind": "midi"` only for MIDI edges (audio edges remain wire-compatible with the v1 schema).

### Internal

- New test file `tests/test_graph_v2_midi.py` -- 12 tests (topology + passthrough + plugin parity, see above). Full suite remains green: 627 passed, 71 skipped.

- `projects/libminihost/minihost_graph_v2.{h,cpp,hpp}`: new node kinds, edge list, plugin MIDI-out capture, staging/drain functions.

- `src/minihost/_core.cpp`: GraphV2 Python wrapper gains MIDI methods + per-node MIDI input scratch buffer that outlives Python call boundaries (mirrors the existing automation scratch pattern).

- `projects/minihost_desktop`: `project.{h,cpp}`, `live.{h,cpp}`, `canvas.{h,cpp}`, `main.cpp` updated as described above; CMake already linked `minihost_audio_gui` so no build-system changes were needed.

## [0.1.7]

### Added

- **`process_audio(plugin, audio, ..., in_place=True)`** -- new kwarg that writes output into the input buffer instead of allocating a new one, saving one buffer's worth of memory for the stereo-in / stereo-out case. Returns the same buffer object as `audio` (the input is mutated). Requires `audio` to be an `AudioBuffer` (numpy / buffer-protocol producers go through a coercion path and cannot alias), matching input / output channel counts, and `tail_seconds == 0` (a tail would need extra frames the input doesn't have). The existing block loop is already structured to snapshot each input block into a scratch buffer before processing, so writing output into the input's storage at a latency-lagged position is safe. 6 tests in `tests/test_in_place_and_session.py`.

- **`minihost.Session`** -- new type that holds one shared JUCE `AudioPluginFormatManager` and reuses it across loads, probes, and directory scans. Most useful for multi-plugin and directory-scanning workflows where the per-call format registration cost otherwise stacks up. API: `Session()` + `open(path, sample_rate, max_block_size, in_channels, out_channels, sidechain_channels)` returns a `Plugin`; `probe(path)` returns the same dict as the module-level `probe()`; `scan_directory(directory_path)` returns the same list-of-dicts as the module-level `scan_directory()`. Context-manager friendly. New C API in `projects/libminihost/minihost.{h,cpp}`: `mh_session_create` / `mh_session_close` / `mh_session_open` / `mh_session_probe` / `mh_session_scan_directory`. Sessions are thread-safe via an internal mutex (JUCE's `AudioPluginFormatManager` is not). **Refactor:** removed the per-plugin `AudioPluginFormatManager fm` field from `MH_Plugin` (the manager is only used at plugin construction; the `AudioPluginInstance` is self-contained afterwards). `mh_open_ex` now constructs a stack-local manager via the existing `initFormatManager` helper; the construction core was factored into `createPluginWithFm(fm, ...)` which both `mh_open_ex` and `mh_session_open` call. `mh_scan_directory`'s body was extracted similarly into `scanDirectoryWithFm(fm, ...)`. Plugins created via `Session.open` survive the session that loaded them (closing the session does not invalidate them). 8 tests in `tests/test_in_place_and_session.py`.

- **`process_audio_stream(plugin_or_chain, audio, ...)`** -- new generator that mirrors `render_midi_stream` for the audio-in case. Yields user-visible output blocks (post-latency-compensation, post-trim) so a consumer that concatenates every yielded block reproduces `process_audio`'s return value. Same kwargs as `process_audio` (`midi=`, `sidechain=`, `param_changes=`, `bpm=`, synth-mode `audio=None`); `as_=numpy.ndarray` selector matches `render_midi_stream`. `normalize=` is intentionally absent here -- peak normalization requires the full output's peak, which a streaming consumer doesn't have. Refactor: extracted `_prepare_render` (setup + validation + transport) and `_iter_blocks` (block loop) from `process_audio` so both the in-memory and streaming entry points share one implementation. `_iter_blocks` yields independent buffer copies by default (`copy=True`) so streaming consumers don't see the reused internal buffer get overwritten on the next iteration; `process_audio` opts out (`copy=False`) since it memcpys each block straight into a pre-allocated output. 9 tests in `tests/test_process_audio_stream.py` cover concat-equals-`process_audio` across effect and synth-mode paths, block-size cap, `as_=` selector, progress callback, validation paths, and lazy-generator behavior.

- **Dry/wet mix on `PluginChain`** -- new `chain.set_mix(plugin_index, mix)` / `chain.get_mix(plugin_index)` (C API: `mh_chain_set_mix` / `mh_chain_get_mix`). `mix` is in `[0.0, 1.0]`: `1.0` (default) is full wet (current behavior), `0.0` is full dry (plugin output bypassed; its input forwards to the next stage), `0.5` is an equal blend. Applied uniformly across `process` / `process_midi` / `process_auto` (the auto chunker delegates to `process_midi_io`, so mix application happens transparently). **Restriction:** the plugin's input and output channel counts must match for mix to be enabled -- ineligible plugins stay locked at `1.0` forever and `set_mix` raises. Dry-signal snapshot storage is pre-allocated at chain construction (per eligible plugin, sized `channels * max_block_size`); the audio thread never allocates. 7 tests in `tests/test_chain_mix_and_graph.py`.

- **`PluginGraph` parallel-branches-summed** (`minihost.PluginGraph`) -- new type for parallel plugin routing beyond the serial `PluginChain`. `PluginGraph(in_ch, out_ch, max_block_size, sample_rate)` + `add_branch(chain, gain=1.0)` + `process(input, output)`. Each branch is a `PluginChain` that receives the same input; their outputs are summed with per-branch linear gain into the graph's output. Per-branch `set_branch_gain` / `get_branch_gain` lets a branch be muted (gain=0 skips processing entirely) or attenuated dynamically. Pre-allocates one scratch output buffer per branch at `add_branch` time; the audio thread is allocation-free. Branches must agree with the graph on channel counts and sample rate -- `add_branch` rejects mismatches with descriptive errors. Covers parallel compression, dry-bus + reverb-send, multi-band-style routing (when each band is wrapped in its own chain). Scope v1: fan-out + summed mix only; arbitrary DAG topology (multi-output, per-edge gain, sidechain into branches) is a possible v2. New C API in `projects/libminihost/minihost_graph.{h,cpp}`; Python wrapper in `src/minihost/_core.cpp`. 13 tests in `tests/test_chain_mix_and_graph.py`.

- **`process_audio_to_file` / `process_audio` absorb the rest of `cmd_process`** -- both gained `midi=` (file path, `MidiFile`, or pre-resolved event list), `sidechain=` (file path or in-memory buffer; `Plugin` only -- `PluginChain` has no sidechain method and is rejected up front), `param_changes=` (sample-accurate automation: `(sample, param_idx, value)` for `Plugin`, `(sample, plugin_idx, param_idx, value)` for `PluginChain`), and `bpm=` (transport tempo; `Plugin` only) kwargs. `audio=None` enables synth-mode renders driven entirely by MIDI -- length is derived from `max(midi_max_sample, src_frames) + tail_seconds`. Per-block routing chooses `process_sidechain` / `process_auto` / `process_midi` / `process` based on which inputs are supplied; latency compensation, normalize, and progress-callback contracts are unchanged. New private helpers: `process._load_midi_events` (lazy-imports the MIDI helpers from `render.py`), `process._slice_block_events`, `process._read_optional_audio`, `process._maybe_duplicate_to_match`. 18 new tests in `tests/test_process_audio_extended.py` cover the pure-Python helpers, validation paths (sidechain-on-chain, bpm-on-chain, missing input/midi/tail), synth mode, MIDI+audio, automation, sidechain, BPM, and `process_audio_to_file` synth + sidechain end-to-end.

- **`minihost process --progress`** -- per-block progress bar on stderr for single-file renders and each file in batch mode. CLI helper `_ProgressBar` in `cli.py` matches the `progress_callback=(current, total)` signature used by the library so it can be passed directly. Library: `progress_callback` kwarg on `process_audio`, `process_audio_to_file`, and `render_midi_to_file`. Disabled by default; opt-in per invocation.

- **`minihost process --normalize [dBFS]`** -- peak-normalize the output to a target dBFS (0 dBFS = full scale; default 0 when the flag is given without a value, e.g. `--normalize -1.0` for 1 dB headroom). Library: `normalize=<dbfs>` kwarg on `process_audio`, `process_audio_to_file`, and `render_midi_to_file`. Silent buffers are left untouched. Helper: `minihost.process._normalize_peak(buf, target_dbfs)` uses `AudioBuffer.magnitude()` + `apply_gain` (JUCE-backed, no numpy). LUFS normalization is a follow-up.

- **`minihost process --chain SPEC` and `minihost.load_chain(spec_path, sample_rate, block_size)`** -- declarative plugin chains from JSON (stdlib only) or YAML (PyYAML imported lazily; clear ImportError when absent). Schema: top-level `plugins: [...]` with `path`, optional `params: {name: value}` (case-insensitive name lookup via `Plugin.find_param`), `preset: N`, `vstpreset: PATH`, `state: PATH` (mutually exclusive; validated up front), and optional `in_channels` / `out_channels` per-plugin or top-level `sample_rate` / `block_size`. Returns a `_OwningPluginChain` subclass (in `src/minihost/chain.py`) that pins the constructed plugins so callers only need to close the chain. CLI: `plugin` positional becomes optional; `--chain` combined with `--state` / `--vstpreset` / `--preset` / `--param` / `--param-file` / `--out-channels` / `--bpm` / `--non-realtime` is rejected (the spec is the source of truth); sidechain input with `--chain` is rejected (chain has no sidechain method). Works in both single-file and batch modes. 10 spec-parsing tests + 1 plugin-gated construction test in `tests/test_tier1_features.py`.

### Changed

- **`cmd_process` collapsed from ~410 to ~200 lines** -- the non-batch CLI path is now a thin shim that validates args, expands globs, peeks audio metadata for plugin construction, parses `--param` / `--param-file` into a `param_changes` list, and delegates everything else to `process_audio_to_file`. Block iteration, MIDI / sidechain / automation routing, latency compensation, normalize, and write all live in the library; the CLI no longer carries a bespoke block loop. Single-test-mock breakage was minimal: `tests/test_cli.py::TestCmdProcessErrors::test_process_plugin_load_error` was updated to include `channels` / `frames` keys in its `get_audio_info` mock (the old code only read `sample_rate`).

- **`process_audio_to_file` signature** -- `input_path` is now optional (defaults to `None` for synth mode). `output_path` remains required and is validated up front. Existing positional callers continue to work unchanged.

- **`process_audio` internals refactored** -- block loop extracted into `_prepare_render` + `_iter_blocks` to share with `process_audio_stream`. `process_audio`'s observable behavior is unchanged (verified by the existing test suite); the change is purely internal.

### Internal

- New test file `tests/test_tier1_features.py` -- 12 tests covering `_normalize_peak` math (3), `_ProgressBar` enabled/disabled behavior (2), `load_chain` validation paths (7: missing file, unknown extension, non-mapping top, empty plugins list, missing plugin path, multiple state sources, YAML without PyYAML).

- New test file `tests/test_process_audio_extended.py` -- 18 tests (see above).

- New test file `tests/test_chain_mix_and_graph.py` -- 20 tests (7 for `PluginChain.set_mix` / `get_mix`, 13 for `PluginGraph`); see the dry/wet and `PluginGraph` entries above.

- New test file `tests/test_process_audio_stream.py` -- 9 tests; see the `process_audio_stream` entry above.

- New test file `tests/test_in_place_and_session.py` -- 14 tests (6 in-place: object identity, equivalence with out-of-place, mutation, type / tail / channel-mismatch rejection; 8 session: construction, open, multi-open, probe equivalence with module-level, scan equivalence, bad-path error, plugin-outlives-session, context manager).

- Build: `projects/libminihost/CMakeLists.txt` adds `minihost_graph.cpp` to the static library; top-level `CMakeLists.txt`'s install rule adds `minihost_graph.h` alongside the other public headers.

### Test summary

`make test`: 576 passed, 71 skipped, 0 failed -- up from 515 before the Tier 1 / extension / mix+graph / streaming / in-place+session work (+61 net new tests across five new files).

## [0.1.6]

### Added

- **numpy is now an optional dependency** (BREAKING CHANGE for installs that relied on numpy being pulled in transitively). See [docs/migration.md](docs/migration.md). Moved from `dependencies` to `[project.optional-dependencies]` as `numpy`. `pip install minihost` no longer installs numpy; `pip install minihost[numpy]` does. The default API surface (`AudioBuffer`, `read_audio`, `write_audio`, `resample`, `process_audio`, `process_audio_to_file`, `render_midi`, `render_midi_stream`, `render_midi_to_file`, `MidiRenderer`, all `Plugin` / `PluginChain` process methods) works without numpy installed. numpy-typed code paths (`as_=numpy.ndarray`, `AudioBuffer.as_ndarray()`, `AudioBuffer.from_numpy()`, passing numpy arrays as inputs) lazy-import numpy on first use and raise a clear `ImportError` directing the user to `pip install minihost[numpy]` when it is absent. Required refactors: `_core.audio_read` / `_core.audio_resample` now return `AudioBuffer` directly (skipping the previous numpy detour); `audio_io.py`, `render.py`, and `process.py` lazy-import numpy and use AudioBuffer-native ops where possible (`AudioBuffer.clear` / `magnitude` / `__setitem__`) instead of `np.zeros` / `np.max(np.abs(...))` / numpy slice assignment. Internal `MidiRenderer` scratch buffers are now `AudioBuffer` instead of `np.ndarray`. New `tests/test_numpy_optional.py` runs a sub-Python process with numpy hidden via a meta-finder and exercises the AudioBuffer-only path end-to-end.

- **`AudioBuffer` class** (`minihost.AudioBuffer`) -- planar float32 audio container, stdlib-only (no numpy required), backed by `juce::AudioBuffer<float>` via a thin C++ wrapper that enforces contiguous memory by construction. Exposes the DLPack and `__array__` protocols so instances can be passed directly to `Plugin.process` / `PluginChain.process` / `write_audio` / `numpy.asarray` without an explicit `.as_ndarray()` conversion. Numpy-style 2-axis indexing supported (`buf[ch, fr_slice]`), with documented limits: strided slices, fancy indexing, boolean indexing, and Ellipsis raise `TypeError` directing the user to `.as_ndarray()` for those. JUCE-backed DSP ops exposed: `clear`, `apply_gain`, `apply_gain_ramp`, `apply_gain_per_channel`, `add_from`, `add_from_with_ramp`, `get_rms_level`, `reverse`, `reverse_channel`, `magnitude`, `copy`. Zero-initialized on construction. Conversion to numpy is via `.as_ndarray()` (zero-copy view, requires numpy installed); construction from numpy is via `AudioBuffer.from_numpy(arr)`.

- **`process_audio()` and `process_audio_to_file()` (`minihost.process`)** -- high-level offline processing helpers that collapse the typical block-iteration loop. `process_audio(plugin_or_chain, audio, tail_seconds=...)` returns a new `AudioBuffer`; `process_audio_to_file(plugin_or_chain, input_path, output_path, tail_seconds=..., bit_depth=24)` reads, optionally resamples and channel-duplicates to match the chain, processes, and writes. Both functions handle latency compensation (extends render by `latency_samples` and trims the matching head from output) when `compensate_latency=True` (default).

- **`read_audio(path, as_=...)`** -- new `as_` selector chooses the returned container type. Default `as_=AudioBuffer` (BREAKING CHANGE: previously returned `numpy.ndarray`). Pass `as_=numpy.ndarray` to keep the previous behavior. `write_audio` and `resample` accept either type transparently; `resample` returns the same type as its input.

- **`render_midi`, `render_midi_stream`, `MidiRenderer.render_block`, `MidiRenderer.render_all` now return `AudioBuffer`** (BREAKING CHANGE). `render_midi`, `render_midi_stream`, and `render_all` accept the same `as_=...` selector as `read_audio` (default `AudioBuffer`; pass `as_=numpy.ndarray` for the previous behavior). `render_block` always returns `AudioBuffer` -- call `.numpy()` on the result if you need a numpy view. Internally `render_midi_to_file` now allocates an `AudioBuffer` for the staging area; the public return type (frame count `int`) is unchanged.

- **Latency compensation in `MidiRenderer`** -- previously, a plugin reporting `latency_samples > 0` produced output time-shifted by that many samples relative to the rendered MIDI tempo positions. The renderer now renders `latency_samples` extra input frames past the user-visible end and discards the first `latency_samples` of output, so the returned audio is time-aligned with MIDI events. User-visible properties (`duration_seconds`, `total_samples`, `progress`) continue to report the user-visible duration; the new read-only `MidiRenderer.latency_samples` property exposes the compensation amount. Auto-tail detection runs against post-skip output and uses the latency-corrected MIDI-end boundary. No-op for plugins reporting zero latency.

- **CMake install rules for libminihost** -- standalone CMake builds (where `SKBUILD` is undefined) now install `libminihost.a` to `${prefix}/lib/` and the public headers (`minihost.h`, `minihost_chain.h`, `minihost_vstpreset.h`) to `${prefix}/include/minihost/`. Gated by the new `MINIHOST_INSTALL` option (default ON for standalone, OFF for the Python wheel build so the wheel is unaffected). No `find_package(minihost)` config target is generated: the static library has PRIVATE link dependencies on JUCE modules that this project vendors via `add_subdirectory` of a downloaded JUCE source tree, so a clean export is not achievable. External C/C++ consumers should rebuild minihost from source as a subdirectory, or link the installed `libminihost.a` together with their own JUCE build.

- **JUCE pinned to a commit SHA in `download_juce.py`** -- previously downloaded by tag (mutable on the server side). Now resolves the default `JUCE_VERSION` to a content-addressed commit SHA (`29396c22c93392d6738e021b83196283d6e4d850` for 8.0.12) and downloads the SHA archive for reproducible builds. `JUCE_SHA` env var overrides the pinned SHA; `JUCE_ALLOW_TAG=1` falls back to tag-based download (use only for ad-hoc bumps where the SHA is not yet known).

- **ABI versioning** -- the C library now exposes a stable ABI version distinct from the project's release version, seeded at `1.0.0`. Header macros `MH_API_VERSION_MAJOR` / `MINOR` / `PATCH` / `NUMBER` / `STRING` describe the version the header was generated for; runtime `mh_api_version()` and `mh_api_version_string()` return the version the linked implementation was built against. Major bumps signal incompatible changes, minor bumps are backward-compatible additions, patch bumps are non-API fixes. Public structs evolve by appending fields; callers should `memset` to zero before passing in. Same surface re-exported in Python as `minihost.api_version()`, `minihost.api_version_string()`, and the `MH_API_VERSION_*` attributes.

- **`MidiMapper`** (`minihost.MidiMapper`) -- maps incoming MIDI events from a USB MIDI control surface (knobs, faders, pads) to plugin parameter writes or user callbacks. Designed to be passed as the callback to `MidiIn.open` / `MidiIn.open_virtual`. `map_cc(channel, cc, param, value_range=(0,1), curve="linear"|"exp"|"log")` translates CCs to `plugin.set_param`; `map_note(channel, note, callback)` invokes a user callback on note-on (zero-velocity note-ons treated as note-offs and not dispatched, per convention); unmapped events fall through to an optional `on_unmapped` callback (useful for forwarding hybrid controllers' notes to the plugin via `audio_device.send_midi`). `set_on_unmapped(callback)` lets the fallback be replaced after construction (the CLI uses this to late-bind the AudioDevice forwarder). Internal `threading.RLock` makes `map_*` / `unmap_*` / `clear()` safe to call from another thread while the MIDI callback fires on the libremidi thread. Parameter names are resolved at mapping time via `Plugin.find_param`, so typos fail fast before MIDI starts flowing. 17 tests in `tests/test_midi_mapper.py` cover dispatch, value-range translation, curves, channel filtering, fall-through, and concurrent remap-vs-dispatch.

- **`minihost play --map "channel:cc:param[:lo:hi[:curve]]"`** -- new CLI flag (repeatable) that wires a `MidiMapper` between the MIDI input and the plugin. When `--map` is set, the MIDI port is owned by a standalone `MidiIn` driving the mapper; mapped CCs become `plugin.set_param` writes; unmapped events (notes, unmapped CCs) are forwarded to the plugin via `AudioDevice.send_midi` so the user can still play notes through the same controller. `--map` requires `--midi N` or `--virtual-midi NAME` to provide an input source. Examples: `--map 0:7:Volume`, `--map 3:10:Pan:-1:1`, `--map 0:74:Cutoff:0:1:exp`. 10 tests in `tests/test_cli.py` (`TestParseMapSpec`, `TestCmdPlayMapping`) cover spec parsing and the play-command error paths (unknown param, malformed spec, --map without --midi).

- **`minihost play --map-file PATH`** -- load CC->parameter mappings from a JSON file for reuse across sessions. Format: `{"mappings": [{"channel": 0, "cc": 7, "param": "Volume"}, {"channel": 0, "cc": 10, "param": "Pan", "value_range": [-1.0, 1.0]}, {"channel": 0, "cc": 74, "param": "Cutoff", "curve": "exp"}]}`. Required fields: `channel`, `cc`, `param`. Optional: `value_range` (default `[0.0, 1.0]`), `curve` (default `"linear"`; one of `linear`/`exp`/`log`). Combinable with `--map` (file is loaded first, CLI args appended). 8 tests in `TestLoadMapFile`.

- **`minihost play --loop-midi PATH`** -- play a MIDI file in a loop while playback runs. A Python thread schedules events at wall-clock-correct times via `audio.send_midi`, using `time.monotonic()` for pacing. All Notes Off (CC 123) is sent on every channel between loop iterations to silence sustained notes from the previous pass. Combinable with `--midi` (live and file MIDI both reach the plugin); responsive to Ctrl+C / SIGTERM via a `threading.Event` checked between every event and during waits.

- **`minihost play --loop-audio PATH`** -- loop an audio file into the plugin's input ring buffer at real time. Auto-enables `audio.enable_input()`; the file is resampled to the device sample rate if needed. A Python thread paces `audio.write_input(chunk)` at 0.9x real-time block period to keep the ring buffer fed without overflow. Mutually exclusive with `--input` (both write to the same ring buffer). 1 test for the mutual-exclusion check; the looping threads themselves are not unit-tested (they need real audio hardware).

- **`Plugin.close()` and context-manager support** -- `Plugin` now supports the `with` statement and exposes an idempotent explicit `close()` method. The same surface is added to `PluginChain` (its `close()` releases only the chain's resources; member plugins remain owned by the caller). Operations on a closed Plugin raise `RuntimeError` instead of crashing.

- **`Plugin.poll_callbacks()`** -- new method to drain pending callback events from a non-audio thread. Change, parameter-value, and gesture callbacks are now queued internally and dispatched only when `poll_callbacks()` is called, returning the number of events dispatched.

### Changed

- **Threading-contract documentation expanded in `minihost.h`** -- the original two-class model ("audio thread only" vs. "thread-safe") was misleading because some "thread-safe" functions call `releaseResources` / `prepareToPlay` and are NOT safe to overlap with `mh_process`. The header now distinguishes three classes explicitly: (1) audio-thread-only process calls, (2) functions that take an internal lock and are safe to overlap with audio (param get/set, queries, transport, callbacks), and (3) functions that reconfigure the plugin and must not overlap with audio (`mh_set_state`, `mh_set_sample_rate`, `mh_set_processing_precision`, `mh_reset`, etc.). Lifecycle ordering between `MH_AudioDevice` and `mh_close` is also documented.

- **`render_midi_to_file()` no longer triple-buffers its output** -- previously did `list(render_midi_stream(...))` (block 1) + `np.concatenate(blocks, axis=1)` (block 2) + `write_audio` (block 3), with peak memory ~3x the rendered audio size. Now allocates a single contiguous output array against `MidiRenderer.total_samples`, writes each block directly into the appropriate slice, and trims to the actual sample count before writing (auto-tail detection may finish early). Peak memory drops from ~3x to ~1x of the final audio size.

- **JUCE moved from `JUCE/` to `thirdparty/JUCE/`** -- the vendored JUCE source tree now lives alongside other third-party code instead of cluttering the repo root. `JUCE_PATH` default in `CMakeLists.txt` and `JUCE_DIR` default in `scripts/download_juce.py` updated accordingly. `pyproject.toml`'s `sdist.include` updated. The `JUCE_PATH` cmake var and `JUCE_DIR` env var still let users override; CI uses the script's default and is unaffected. Existing checkouts: run `mv JUCE thirdparty/JUCE` (or just `rm -rf JUCE && python scripts/download_juce.py` to re-download into the new location).

- **`minihost process` batch worker now uses `process_audio_to_file`** -- the per-file batch path (`_process_single_file` in `cli.py`) previously open-coded the read / pad / block-loop / latency-compensation / write pipeline. It now delegates to `process_audio_to_file`. Batch-mode invariants (sample-rate / channel-count consistency across files) are preserved via a cheap `get_audio_info` pre-check before the worker fires; exceptions are translated into the existing int return contract. Net: ~70 lines of bespoke loop replaced with a one-call delegation. The non-batch `cmd_process` path is unchanged because it has MIDI / sidechain / automation / transport features that are out of scope for `process_audio_to_file`.

- **Deduplicated `find_param_by_name`** -- `minihost.automation.find_param_by_name()` was a pure-Python loop over `plugin.get_param_info(i)`, duplicating the case-insensitive search in `Plugin.find_param`. The Python helper now delegates to `Plugin.find_param` and only translates the C++ binding's `RuntimeError` into the documented `ValueError` plus a CLI-discovery hint. Behavior is unchanged.

- **Deduplicated `.vstpreset` parser** -- `minihost.vstpreset` previously reimplemented the binary parser in pure Python alongside the C implementation in `projects/libminihost/minihost_vstpreset.c`. The Python module now delegates `read_vstpreset()` and `write_vstpreset()` to new `_core.vstpreset_read` / `_core.vstpreset_write` nanobind bindings (which call `mh_vstpreset_read` / `mh_vstpreset_write` directly). The user-facing `VstPreset` dataclass and the high-level helpers (`load_vstpreset`, `save_vstpreset`, `read_class_id_from_bundle`) are unchanged. The C parser is now the single source of truth; bug fixes land once.

- **Callback dispatch moved off the audio thread** -- `set_change_callback()`, `set_param_value_callback()`, and `set_param_gesture_callback()` no longer acquire the Python GIL from the audio thread. Callback events from the plugin (via JUCE's `AudioProcessorListener`) are pushed to a lightweight mutex-protected queue and dispatched to Python only when `poll_callbacks()` is called. This eliminates a class of audio dropouts caused by GIL contention.

- **`mh_open()` now delegates to `mh_open_ex()`** with `sidechain_in_ch=0`, removing ~50 lines of duplicated plugin-loading logic. `tryConfigureBuses()` removed (subsumed by `tryConfigureBusesEx()` which already handles zero sidechain correctly).

- **`mh_process_sidechain()` no longer heap-allocates on the audio thread** -- the combined main+sidechain buffer is pre-allocated once in `mh_open_ex()` and reused across calls, matching the zero-allocation pattern used by all other process functions.

- **`mh_process_double()` no longer heap-allocates on the audio thread** -- previously allocated `AudioBuffer<double>`/`AudioBuffer<float>` and `MidiBuffer` per call, violating the header's documented RT-safety contract. Added a persistent `AudioBuffer<double> processBufD` to `MH_Plugin` (sized once in `mh_open_ex` to match the float `processBuf`); the float-fallback path reuses `processBuf`. Both branches now reuse `p->midi`. Combined-buffer pattern also resolves the same `inCh > outCh` data-loss bug as the float path.

- **`mh_chain_process_auto()` no longer heap-allocates on the audio thread** -- previously allocated four `std::vector`s per chunk (`chunk_inputs`, `chunk_outputs`, `chunk_midi`, a 256-element `chunk_midi_out`); a block with 16 param changes did 64 heap allocations. Added persistent `autoChunkIn` / `autoChunkOut` / `autoChunkMidiIn` / `autoChunkMidiOut` members to `MH_PluginChain`, pre-sized at construction; the chunk loop now uses `clear()` + `push_back` against preserved capacity.

- **Listener trampoline no longer allocates on the audio thread** -- `Plugin::set_*_callback()` trampolines previously did `lock_guard<mutex>` + `vector::push_back`, which can reallocate. The queue is now reserved to a fixed capacity (1024) at `Plugin` construction, and `poll_callbacks()` uses `clear()` instead of `swap()` so capacity is preserved across drains. The mutex is retained (briefly held for one bounded `push_back`, microseconds), giving multi-producer safety without the maintenance cost of a hand-rolled MPSC ring buffer. When the queue is full, events are dropped and counted; the new `Plugin.callback_events_dropped()` method returns and resets the count for diagnostics.

- **Unified processing buffer in `MH_Plugin`** -- removed `inBuf`, `outBuf`, `sidechainBuf`, `combinedBuf`, `autoChunkIn`, `autoChunkOut` (six members). Replaced with a single `processBuf` (and its `processBufD` double-precision mirror) used uniformly by `mh_process_midi_io`, `mh_process_auto`, `mh_process_sidechain`, and `mh_process_double`. Reduces struct size and gives every audio path the same correctness guarantees.

- **Extracted shared helpers in Python bindings** (`_core.cpp`):

  - `planar_to_interleaved()` / `interleaved_to_planar()` replace 4 inline loop nests across `audio_read`, `audio_write`, and `audio_resample`

  - `plugin_desc_to_dict()` replaces 2 identical 10-field dict constructions in `probe()` and `scan_directory()`

### Fixed

- **`mh_set_sample_rate()` now fails loudly on rate rejection** -- previously returned 1 unconditionally even if the plugin internally rejected or clamped the requested rate. Negative, zero, and NaN inputs are now rejected up front, and after `prepareToPlay` the function verifies `getSampleRate()` matches the requested rate (within 0.5 Hz). On mismatch it rolls back its own bookkeeping (so subsequent `mh_get_sample_rate()` reflects reality) and returns 0. The Python wrapper raises `RuntimeError` accordingly.

- **`mh_set_processing_precision()` now fails loudly on precision rejection** -- previously returned 1 even when the plugin silently kept its current precision (some plugins decline `doublePrecision` even when `supportsDoublePrecisionProcessing()` returns true). The function now verifies `getProcessingPrecision()` matches the requested value after `setProcessingPrecision` + `prepareToPlay`; on mismatch, restores state to the plugin's chosen precision and returns 0.

- **`PluginChain` and `AudioDevice` no longer dangle on anonymous Python inputs** -- `PluginChain([Plugin(...)])` and `AudioDevice(Plugin(...))` previously stored raw `Plugin*` pointers without holding a Python reference, so once the temporary `Plugin` / list went out of scope the wrappers could be garbage-collected, leaving the chain or device with dangling pointers. Added `nb::keep_alive<1, 2>()` to both constructors (and the `AudioDevice(PluginChain&, ...)` overload) so the inputs' Python lifetime is pinned to the new instance. Crash was nondeterministic and depended on GC timing.

- **`mh_process_midi_io()` silently dropped input channels when `inCh > outCh`** -- the buffer passed to JUCE's `processBlock` was sized only to `outCh` channels, so plugins configured with more inputs than outputs (e.g. 4-in / 2-out downmix) only ever saw the first `outCh` input channels. Replaced the dual `inBuf`/`outBuf` setup with a single persistent `processBuf` sized to `max(inCh + sidechainCh, outCh)`; main inputs are copied into channels `[0, inCh)`, remaining channels are zeroed, and outputs are copied back from channels `[0, outCh)`. The same fix is applied per chunk in `mh_process_auto()`. Symmetric (`inCh == outCh`) plugins pay one extra `memcpy` per block; the previous code already did the equivalent copy for the in-place pre-fill.

- **Channel-count validation on `Plugin.process*` and `PluginChain.process*`** -- passing a numpy array with fewer channels than the plugin requires previously dereferenced past the internal `std::vector<const float*>` (undefined behavior). All eight process methods now validate via a new `validate_process_shape()` helper and raise `RuntimeError` with a message naming the actual vs. required channel counts. Extra channels remain accepted (harmless; the C layer only references the first N).

- **`mh_probe()` MIDI flag heuristic documented** -- `MH_PluginDesc.accepts_midi` was previously set from `desc.isInstrument` with no caveat, mislabeling MIDI effects, MIDI generators, and analyzer plugins. The implementation is unchanged in this version of JUCE (no authoritative MIDI flags available without instantiation), but the field comments and the implementation comment now state explicitly that probe-time MIDI flags are a best-effort heuristic and that callers needing authoritative values must call `mh_open` + `mh_get_info`.

- **MIDI event tuple validation** -- `process_midi()` and `process_auto()` (on both `Plugin` and `PluginChain`) now validate that each MIDI event tuple has at least 4 elements before indexing, producing a clear `RuntimeError` instead of an opaque `IndexError` from the nanobind layer.

- **Null plugin guard in `PluginChain`** -- the `PluginChain` constructor now checks each `Plugin` for a valid internal pointer. Passing a moved-from or otherwise invalid `Plugin` now raises a descriptive `RuntimeError` instead of causing undefined behavior.

- **MIDI output buffer limit documented** -- `process_midi()` and `process_auto()` docstrings now state that MIDI output is capped at 256 events per call, with excess events silently dropped. The hard-coded buffer size is consolidated into a named constant (`MIDI_OUT_CAPACITY`).

### Internal

- **New regression tests** under `tests/`:

  - `test_chain_gc.py` -- `PluginChain`/`AudioDevice` lifetime pinning

  - `test_channel_validation.py` -- shape validation on all process entry points

  - `test_asymmetric_channels.py` -- combined-buffer correctness for `process` and `process_auto`

  - `test_rt_allocations.py` -- repeated-call stability for `process_double` and chain `process_auto`

  - `test_context_manager.py` -- `Plugin` / `PluginChain` close + `with` semantics

  - `test_api_version.py` -- header / runtime ABI-version agreement

  - `test_concurrency.py` -- `set_param` racing `process` from multiple threads, callback-queue ordering and overflow reporting

  - `test_setters_fail_loud.py` -- `set_sample_rate` rejects negative/zero/NaN; `set_processing_precision` rejects unsupported double precision

  - `test_render_latency_compensation.py` -- mock-plugin-based verification that `MidiRenderer` skips the first `latency_samples` of output and emits time-aligned audio

  - `test_audiobuffer.py` (20 tests) -- AudioBuffer construction, indexing semantics (positive / negative / slice / scalar), rejection of strided / fancy / Ellipsis / single-axis access, DSP ops (clear, apply_gain, magnitude, copy), numpy zero-copy interop, direct `Plugin.process` consumption via DLPack

  - `test_audiobuffer_dsp.py` (19 tests) -- extended JUCE DSP ops on AudioBuffer (`apply_gain_ramp`, `apply_gain_per_channel`, `add_from`, `add_from_with_ramp`, `get_rms_level`, `reverse`, `reverse_channel`), each verified against a numpy reference plus bounds-checking error paths

  - `test_process_audio.py` (7 tests) -- `process_audio` shape/tail/channel-validation, `process_audio_to_file` round-trip, automatic resampling, mono->stereo channel duplication

## [0.1.5]

### Added

- **Audio device selection** -- enumerate and select specific playback/capture audio devices

  - C API: `MH_AudioDeviceInfo`, `mh_audio_enumerate_playback_devices()`, `mh_audio_enumerate_capture_devices()`; new `playback_device_index` and `capture_device_index` fields on `MH_AudioConfig`

  - Python: `minihost.audio_get_playback_devices()`, `minihost.audio_get_capture_devices()`; `AudioDevice(..., playback_device_index=N, capture_device_index=N)`

  - Python CLI: new `minihost devices` subcommand lists available devices; `minihost play --playback-device` and `--capture-device` accept either an index or a case-insensitive substring of the device name

  - C/C++ CLIs: new `devices` subcommand (text + `-j`/`--json` output). The C/C++ CLIs have no real-time `play` command, so device-selection flags are not applicable there.

- **Preset management CLI** -- `presets <plugin>` subcommand extended across all three frontends

  - Default: lists all factory presets (no longer truncated at 10 like `info`); `-j/--json` for structured output

  - `--save FILE.vstpreset` saves the current plugin state as a `.vstpreset`

  - Combinable with `--program N`, `-s/--state FILE`, or `--load-vstpreset FILE` to apply an input state before saving; when `--load-vstpreset` is used, the source file's class_id is preserved in the output

  - `-y`/`--overwrite` allows overwriting an existing `--save` target

  - Byte-exact round-trip verified between Python, C, and C++ CLIs writing/reading the same `.vstpreset`

- **`minihost_vstpreset.h/.c` in libminihost** -- new C module exposing `mh_vstpreset_read()`, `mh_vstpreset_write()`, and `mh_vstpreset_free()` for programmatic .vstpreset I/O from C/C++ callers (portable little-endian packing, no external dependencies)

- **`write_vstpreset()` / `save_vstpreset()` in `minihost.vstpreset`** -- programmatic .vstpreset writer to complement the existing reader

- **C/C++ CLI feature parity with Python frontend** -- brought `minihost_c` and `minihost_cpp` up to date with features already available in libminihost and the Python CLI

  - `process` (both CLIs): audio file I/O via `minihost_audiofile.h` -- process WAV, FLAC, MP3 input and write WAV/FLAC output (raw float32 retained as fallback for non-audio extensions)

  - `process` (both CLIs): `-i`/`--input` and `-o`/`--output` named arguments for input/output files (C CLI retains legacy positional syntax)

  - `process` (both CLIs): latency compensation -- output automatically trimmed using `mh_get_latency_samples()`

  - `process` (both CLIs): `-p`/`--preset N` -- load factory preset before processing via `mh_set_program()`

  - `process` (both CLIs): `--param "Name:value"` -- set parameters by name or index (repeatable), dispatched via `mh_process_auto()` for sample-accurate application

  - `process` (both CLIs): `--sidechain FILE` -- sidechain input via `mh_open_ex()` + `mh_process_sidechain()`

  - `process` (both CLIs): `--non-realtime` -- enable higher-quality offline processing via `mh_set_non_realtime()`

  - `process` (both CLIs): `--bpm BPM` -- set transport tempo for tempo-synced plugins via `mh_set_transport()`

  - `process` (both CLIs): `--bit-depth 16|24|32` -- control output bit depth (default: 24)

  - `process` (C++ CLI only): `-m`/`--midi-input FILE` -- render MIDI files through synth/effect plugins via midifile library + `mh_process_midi()`

  - `process` (C++ CLI only): `-t`/`--tail SECONDS` -- configurable tail length for MIDI-only rendering (default: 2.0s)

  - `params` (both CLIs): `-V`/`--verbose` -- extended parameter display with ranges, defaults, and flags using `mh_param_to_text()`

  - `info` (both CLIs): `--probe` -- lightweight metadata-only mode (no full plugin load)

  - `info` (both CLIs): `-j`/`--json` -- JSON output with merged probe and runtime info

- **Parameter access by name** on the Python `Plugin` class -- `plugin.find_param("Cutoff")`, `plugin.get_param_by_name("Cutoff")`, `plugin.set_param_by_name("Cutoff", 0.5)`. Case-insensitive lookup, raises `RuntimeError` if not found. The index-based API remains for hot-path use.

- **`minihost.open_async()`** -- load a plugin in a background thread, returns a `concurrent.futures.Future` that resolves to a `Plugin`. Useful for large sample-library plugins that take seconds to load.

- **`VENDORED.md`** -- documents vendored dependency versions (miniaudio 0.11.24, tflac, libremidi 5.3.1, midifile) with upstream URLs and update instructions.

### Changed

- `minihost_cpp` now links against `minihost_audio` and `midifile` libraries

- `minihost_c` now links against `minihost_audio` library

- **`save_vstpreset` now produces valid VST3 FUIDs.** When called with `class_id=None` (the default), the FUID is auto-detected from the plugin bundle's `Contents/Resources/moduleinfo.json` instead of writing a placeholder string. This requires the plugin to be built against VST3 SDK 3.7.5+ (which all modern plugins ship). For legacy plugins, callers must pass `class_id` explicitly or use `load_vstpreset()` to inherit one from an existing preset; there is no silent fallback. The same change applies to the `presets <plugin> --save` subcommand across all three CLI frontends.

- `Plugin` Python class and `MH_Plugin` C struct now expose the constructor's plugin path via `Plugin.path` (Python) / `mh_get_path()` (C).

- **`mh_scan_directory()` reuses a single `AudioPluginFormatManager`** instead of creating one per plugin via `mh_probe()`. Reduces overhead for large plugin collections.

- **`render_midi_stream()` now delegates to `MidiRenderer`** instead of reimplementing the render loop. Eliminates ~60 lines of duplicated setup and block-processing logic between the two code paths.

### Fixed

- **Python wheel included entire JUCE SDK and midifile install artifacts** -- CMake's `install()` rules from the JUCE and midifile subdirectories were propagating into the scikit-build-core wheel, bundling ~2200 extraneous files (headers, `juceaide` binary, CMake configs, static libraries) and inflating the wheel from ~3 MB to ~60 MB. Fixed by adding `EXCLUDE_FROM_ALL` to the `add_subdirectory()` calls for JUCE and midifile so their install targets are excluded from the default install.

- **`.vstpreset` files written by `save_vstpreset()` (and the `presets --save` CLI) previously contained a bogus class ID** -- either the literal `"minihost_unknown"` or an 8-character hash from JUCE's `PluginDescription.uniqueId`, neither of which is a valid 32-character VST3 FUID. Files written this way round-tripped through minihost's own loader but were unrecognised by other VST3 hosts. Fixed by reading the real processor FUID from the plugin bundle's `moduleinfo.json` (see Changed). New `mh_vstpreset_read_class_id_from_bundle()` C function and `minihost.vstpreset.read_class_id_from_bundle()` Python helper expose the underlying lookup.

- **`mh_audio_read()` opened the audio file twice** -- once via `ma_decode_file()` to decode audio, then again via `ma_decoder_init_file()` just to read channel count and sample rate. The second open was unnecessary: `ma_decode_file()` already populates `config.channels` and `config.sampleRate` upon return. Removed the redundant decoder open.

- **CI workflow did not run the test suite** -- `build-wheels` job built Python wheels on all platforms but never ran `pytest`. Added `CIBW_TEST_REQUIRES` and `CIBW_TEST_COMMAND` so cibuildwheel runs `pytest tests/ -v` against each built wheel. Plugin-dependent integration tests skip gracefully via `MINIHOST_TEST_PLUGIN` guard.

- **`mh_set_transport()` data race** -- transport fields (`bpm`, `positionSamples`, etc.) were written from the control thread without synchronisation while `getPosition()` read them from the audio thread, risking torn reads. Replaced with a seqlock: the writer snapshots all fields into an `MH_PlayHead::State` struct and bumps an atomic sequence counter before/after the copy; the reader retries if the counter changed mid-read. Zero overhead on the audio thread (no mutex, no CAS loop -- just two relaxed loads and a compare).

- **`mh_process_auto()` buffer overread with >64 channels** -- chunk pointer arrays were hard-coded to 64 entries, but `setDataToReferTo` was passed the plugin's actual channel count, causing an overread into uninitialised stack memory for plugins with more than 64 channels. Replaced with persistent `std::vector` members on `MH_Plugin`, sized once on first call and reused to avoid per-call heap allocation.

- **FLAC encoding crashed on Windows (`STATUS_STACK_BUFFER_OVERRUN`)** -- tflac's bitwriter always writes a full 8-byte `tflac_uint` at the current buffer position, even when only a few logical bytes remain. In `write_flac()`, the 38-byte stack buffer for STREAMINFO was too small: near the end (e.g. `pos=34`), the 8-byte write overflowed by 3+ bytes, corrupting the MSVC `/GS` stack cookie. On macOS/Linux the overflow went undetected due to stack layout differences. Fixed by padding the buffer to 46 bytes (`38 + 8`).

### Tests

- **Audio processing data-pipeline tests** (`tests/test_audio_processing.py`) -- 33 new tests covering MIDI event conversion, timing accuracy, render pipeline data flow, and automation interpolation edge cases, all runnable without a real plugin. Additional 8 plugin-dependent integration tests (gated behind `MINIHOST_TEST_PLUGIN`) verify process/process_midi/process_auto output correctness, multi-block state continuity, transport stability, and render_midi output shape.

## [0.1.4]

### Added

- **Audio input for effect processing** -- lock-free ring buffer API for feeding audio through effect plugins in real time, without GIL contention on the audio thread

  - C API: `mh_audio_enable_input()`, `mh_audio_disable_input()`, `mh_audio_write_input()`, `mh_audio_input_available()`

  - C internals: `MH_AudioRingBuffer` -- SPSC lock-free ring buffer (`audio_ringbuffer.h/.cpp`)

  - Python: `AudioDevice.enable_input(capacity_frames=0)`, `disable_input()`, `write_input(data)`, `input_available` property

  - Example: `examples/audio_input.py` -- sine wave and file-through-effect demos

- **Offline bounce with automatic tail detection** -- `tail_seconds="auto"` in render functions monitors output amplitude after MIDI ends and stops when it decays below a threshold

  - `render_midi_stream()`, `render_midi()`, `render_midi_to_file()`, `MidiRenderer` all accept `tail_seconds="auto"`

  - Configurable `tail_threshold` (default: -80 dB / `1e-4` linear) and `max_tail_seconds` (default: 30s safety cap)

  - Stops after 4 consecutive blocks below threshold to avoid cutting during brief silences

  - Example: `examples/auto_tail.py` -- compares fixed vs auto tail at different thresholds

- **Duplex audio (capture) for real-time effect processing** -- system audio input routed through plugin via miniaudio duplex mode

  - C layer: `MH_AudioConfig.capture` field; when set, audio device opens in duplex mode and the audio callback de-interleaves capture input directly into the plugin's input buffers (zero additional latency)

  - Python: `AudioDevice(plugin, capture=True)` -- new `capture` parameter on both `Plugin` and `PluginChain` constructors

  - CLI: `minihost play /path/to/effect.vst3 --input` (`-i`) enables duplex mode for live effect processing (guitar through amp sim, vocal processing, etc.)

- **Batch / multi-file processing in CLI** -- glob pattern expansion and directory output for `minihost process`

  - `-i "*.wav"` expands glob patterns; `-o output/` writes each result to the output directory

  - Batch mode detected when output path is a directory (exists or ends with `/`) and input contains audio files (no MIDI)

  - Plugin loaded once, state saved and restored between files for consistent processing

  - Skips existing output files unless `-y`/`--overwrite` is set

  - Example: `minihost process reverb.vst3 -i "drums/*.wav" -o processed/`

- **Sample rate conversion / resampling** -- built-in resampling using miniaudio's `ma_resampler` (linear interpolation with 4th-order low-pass anti-aliasing filter)

  - C API: `mh_audio_resample()` in `minihost_audiofile.h` -- resamples interleaved float32 audio between any two sample rates

  - Python: `minihost.resample(data, sr_in, sr_out)` -- takes `(channels, frames)` numpy array, returns resampled array

  - CLI: `minihost process` and batch mode automatically resample mismatched input files to match the plugin's sample rate; use `--no-resample` to error instead

  - CLI: `minihost resample input.wav -o output.wav -r 48000` -- standalone resampling subcommand with `--bit-depth` and `-y`/`--overwrite` options

  - No-op fast path when source and target rates are equal (memcpy, no resampler init)

### Tests

- **Render internals unit tests** (`tests/test_render_internals.py`) -- 55 tests covering `_build_tempo_map`, `_tick_to_seconds`, `_collect_midi_events`, `_event_to_midi_tuple`, `_seconds_to_samples`, and end-to-end tempo map integration

- **CLI unit tests** (`tests/test_cli.py`) -- 84 tests covering argument parsing for all 7 subcommands, global options, error paths, `--input` capture flag for `play`, glob expansion, batch output detection, batch error paths, `--no-resample` flag, and `resample` subcommand (arg parsing + functional tests)

- **Resampling tests** (`tests/test_audio_io.py`) -- 7 tests covering upsample, downsample, identity (same rate), stereo, silence preservation, large ratio (8k to 48k), and round-trip frame count

## [0.1.3]

### Added

- `mh_chain_process_auto()` for sample-accurate parameter automation across plugin chains

  - New `MH_ChainParamChange` struct with `plugin_index` field to target specific plugins in the chain

  - Python: `PluginChain.process_auto(input, output, midi_in, param_changes)` with 4-tuple param changes `(sample_offset, plugin_index, param_index, value)`

- FLAC write support in `mh_audio_write()` via vendored [tflac](https://github.com/jprjr/tflac) encoder (BSD-0, single-header C89)

  - Supports 16-bit and 24-bit FLAC output; 32-bit raises an error (FLAC max is 24-bit)

  - Format selected by file extension: `.wav` for WAV, `.flac` for FLAC

  - Python: `write_audio("out.flac", data, sr, bit_depth=24)` works without any API changes

### Fixed

- Fixed stale version assertion in `test_minihost.py` (`"0.1.1"` -> `"0.1.2"`)

- Removed dead bit-depth auto-detection code in CLI `process` subcommand that relied on `get_audio_info()` returning a `subtype` key (removed in 0.1.2); now defaults to 24-bit when `--bit-depth` is not specified

- Fixed `render_midi_to_file()` docstring listing unsupported output formats (FLAC, AIFF, OGG); only WAV is supported

## [0.1.2]

### Added

- Audio file I/O via miniaudio (C layer): `mh_audio_read()`, `mh_audio_write()`, `mh_audio_get_file_info()`

  - Python: `minihost.read_audio()`, `minihost.write_audio()`, `minihost.get_audio_info()` now backed by miniaudio

  - Read support: WAV, FLAC, MP3, Vorbis

  - Write support: WAV only (16-bit PCM, 24-bit PCM, 32-bit float)

- `MidiIn` class for standalone MIDI input monitoring (no plugin required)

  - `MidiIn.open(port_index, callback)` -- open a hardware MIDI input port with raw bytes callback

  - `MidiIn.open_virtual(name, callback)` -- create a virtual MIDI input port with raw bytes callback

  - `close()` method and context manager (`with`) support

  - Python: `minihost.MidiIn`

- MIDI monitor mode in `minihost midi` CLI subcommand

  - `minihost midi -m N` -- monitor incoming MIDI on hardware port N

  - `minihost midi --virtual-midi NAME` -- create a virtual MIDI port and monitor it

  - Human-readable output: Note On/Off (with note names), CC, Pitch Bend, Program Change, Channel Pressure, Poly Aftertouch, SysEx

### Changed

- Merged `probe` CLI subcommand into `info` -- use `minihost info <plugin> --probe` for lightweight metadata-only mode

  - `minihost info` now shows full runtime details by default (was already doing this)

  - `minihost info --probe` replaces the old `minihost probe` (no full plugin load)

  - `minihost info --json` outputs combined probe + runtime data as JSON

- Merged `render` CLI subcommand into `process` -- use `minihost process <plugin> -m song.mid -o output.wav` instead of `minihost render`

- Added `-t, --tail` option to `process` subcommand (default: 2.0s) for configurable tail length in MIDI-only synth mode

- Renamed `midi-ports` CLI subcommand to `midi`

### Removed

- `soundfile` runtime dependency -- audio file I/O now uses miniaudio (already vendored)

  - AIFF and OGG write support removed (miniaudio encoder is WAV-only)

  - `get_audio_info()` no longer returns `format` or `subtype` keys

## [0.1.1]

### Added

- MIDI capability queries on live plugin instances via `MH_Info` fields: `accepts_midi`, `produces_midi`, `is_midi_effect`, `supports_mpe`

  - Python: `Plugin.accepts_midi`, `Plugin.produces_midi`, `Plugin.is_midi_effect`, `Plugin.supports_mpe` read-only properties

- Stable parameter IDs via `MH_ParamInfo.id` field (uses `getParameterID()` for version-safe state management)

  - Python: `get_param_info()` dict now includes `"id"` key

- Parameter categories via `MH_ParamInfo.category` field with `MH_PARAM_CATEGORY_*` constants

  - Python: `get_param_info()` dict now includes `"category"` key

- Bus layout validation via `mh_check_buses_layout()` -- query whether a bus layout is supported before attempting to apply it

  - Python: `Plugin.check_buses_layout(input_channels, output_channels) -> bool`

- Change notifications via `AudioProcessorListener` integration

  - `mh_set_change_callback()` -- processor-level changes (latency, param info, program, non-param state) with `MH_CHANGE_*` bitmask flags

  - `mh_set_param_value_callback()` -- plugin-initiated parameter value changes

  - `mh_set_param_gesture_callback()` -- parameter gesture begin/end from plugin UI

  - Python: `Plugin.set_change_callback()`, `Plugin.set_param_value_callback()`, `Plugin.set_param_gesture_callback()`

  - Python: module-level constants `MH_CHANGE_LATENCY`, `MH_CHANGE_PARAM_INFO`, `MH_CHANGE_PROGRAM`, `MH_CHANGE_NON_PARAM_STATE`

- Parameter gesture bracketing via `mh_begin_param_gesture()` / `mh_end_param_gesture()` -- signal start/end of automation drags to plugins

  - Python: `Plugin.begin_param_gesture(index)`, `Plugin.end_param_gesture(index)`

- Current program state save/restore via `mh_get_program_state_size()` / `mh_get_program_state()` / `mh_set_program_state()` -- lighter-weight per-program state

  - Python: `Plugin.get_program_state() -> bytes`, `Plugin.set_program_state(data)`

- Processing precision selection via `mh_get_processing_precision()` / `mh_set_processing_precision()` -- explicitly select float vs double processing mode

  - Python: `Plugin.processing_precision` read/write property

  - Module-level constants `MH_PRECISION_SINGLE`, `MH_PRECISION_DOUBLE`

- Track properties via `mh_set_track_properties()` -- pass track name/color metadata to plugins

  - Python: `Plugin.set_track_properties(name=None, colour=None)`

- LV2 plugin format support (`JUCE_PLUGINHOST_LV2=1`)

  - Load and process LV2 plugins on all platforms

  - `mh_scan_directory()` now finds `.lv2` bundles

  - CLI updated with LV2 examples for `probe`, `info`, and `scan` commands

- Headless build mode (`MINIHOST_HEADLESS`, default ON)

  - Builds without GUI dependencies using JUCE's `juce_audio_processors_headless` module (requires JUCE 8.0.11+)

  - Uses headless format classes (`VST3PluginFormatHeadless`, `AudioUnitPluginFormatHeadless`, `LV2PluginFormatHeadless`)

  - Disable with `cmake -DMINIHOST_HEADLESS=OFF`

### Changed

- `addFormat()` calls now use `std::make_unique` instead of raw `new`

- Removed unused `KnownPluginList` variable from `findFirstTypeForFile`

- Added cross-platform Python script (`scripts/download_juce.py`) for downloading JUCE

  - Works on Windows, macOS, and Linux without requiring bash

  - Uses only Python standard library (no external dependencies)

  - Handles Python 3.14+ tarfile deprecation warning

- Bumped default JUCE version from 8.0.6 to 8.0.12 (required for `juce_audio_processors_headless`)

- Updated CI workflow to use Python script instead of bash for JUCE download

- Updated Makefile to prefer Python script with bash fallback

## [0.1.0]

### Added

#### Plugin Chaining

- `MH_PluginChain` opaque struct for managing chains of plugins

- `mh_chain_create()` - Create a chain from an array of plugins (all must have same sample rate)

- `mh_chain_close()` - Close chain and free resources (does not close individual plugins)

- `mh_chain_process()` - Process audio through the chain

- `mh_chain_process_midi_io()` - Process audio with MIDI I/O (MIDI goes to first plugin only)

- `mh_chain_get_latency_samples()` - Get total chain latency (sum of all plugin latencies)

- `mh_chain_get_num_plugins()` - Get number of plugins in the chain

- `mh_chain_get_plugin()` - Get plugin from chain by index

- `mh_chain_get_num_input_channels()` - Get input channel count (from first plugin)

- `mh_chain_get_num_output_channels()` - Get output channel count (from last plugin)

- `mh_chain_get_sample_rate()` - Get sample rate (all plugins share same rate)

- `mh_chain_get_max_block_size()` - Get maximum block size

- `mh_chain_reset()` - Reset all plugins in the chain

- `mh_chain_set_non_realtime()` - Set non-realtime mode for all plugins

- `mh_chain_get_tail_seconds()` - Get maximum tail length (max of all plugin tails)

- `mh_audio_open_chain()` - Open audio device for real-time playback through a plugin chain

- Python `PluginChain` class with `process()`, `process_midi()`, `reset()`, `set_non_realtime()`, `get_plugin()` methods

- Python `PluginChain` properties: `num_plugins`, `latency_samples`, `num_input_channels`, `num_output_channels`, `sample_rate`, `tail_seconds`

- `AudioDevice` now accepts either `Plugin` or `PluginChain`

- `render_midi()`, `render_midi_stream()`, `render_midi_to_file()`, and `MidiRenderer` now accept either `Plugin` or `PluginChain`

#### Real-time Audio Playback (miniaudio integration)

- `MH_AudioDevice` opaque struct for audio device management

- `MH_AudioConfig` struct for device configuration (sample_rate, buffer_frames, output_channels, midi_input_port, midi_output_port)

- `MH_AudioInputCallback` typedef for effect plugin input audio

- `mh_audio_open()` - Open audio device for real-time playback through a plugin

- `mh_audio_close()` - Close audio device

- `mh_audio_start()` / `mh_audio_stop()` - Start/stop audio playback

- `mh_audio_is_playing()` - Check if audio is currently playing

- `mh_audio_set_input_callback()` - Set input callback for effect plugins

- `mh_audio_get_sample_rate()` - Get actual device sample rate

- `mh_audio_get_buffer_frames()` - Get actual buffer size

- `mh_audio_get_channels()` - Get number of output channels

- New `libminihost_audio` library using miniaudio for cross-platform audio I/O

#### Real-time MIDI I/O (libremidi integration)

- `MH_MidiPortInfo` struct for MIDI port information

- `MH_MidiPortCallback` typedef for port enumeration callbacks

- `mh_midi_enumerate_inputs()` / `mh_midi_enumerate_outputs()` - Enumerate available MIDI ports

- `mh_midi_get_num_inputs()` / `mh_midi_get_num_outputs()` - Get MIDI port count

- `mh_midi_get_input_name()` / `mh_midi_get_output_name()` - Get MIDI port name by index

- `mh_audio_connect_midi_input()` / `mh_audio_connect_midi_output()` - Connect MIDI ports to AudioDevice

- `mh_audio_disconnect_midi_input()` / `mh_audio_disconnect_midi_output()` - Disconnect MIDI

- `mh_audio_get_midi_input_port()` / `mh_audio_get_midi_output_port()` - Query connected MIDI ports

- Lock-free ring buffer for thread-safe MIDI transfer between MIDI and audio threads

#### Virtual MIDI Ports

- `mh_midi_in_open_virtual()` - Create a virtual MIDI input port (other apps can send MIDI to it)

- `mh_midi_out_open_virtual()` - Create a virtual MIDI output port (other apps can receive MIDI from it)

- `mh_audio_create_virtual_midi_input()` - Create virtual MIDI input for AudioDevice

- `mh_audio_create_virtual_midi_output()` - Create virtual MIDI output for AudioDevice

- `mh_audio_is_midi_input_virtual()` / `mh_audio_is_midi_output_virtual()` - Check if MIDI port is virtual

- `mh_audio_send_midi()` - Send MIDI events programmatically during real-time playback

- Virtual ports appear in system MIDI port lists, allowing DAWs and other apps to connect

- Platform support: macOS (CoreMIDI), Linux (ALSA); not supported on Windows

#### MIDI File Read/Write (midifile integration)

- Integrated `midifile` library for Standard MIDI File (SMF) read/write capability

- Python `MidiFile` class for creating, loading, and saving MIDI files

  - Create MIDI files programmatically with note on/off, tempo, control change, program change, pitch bend events

  - Load existing MIDI files and iterate through events

  - Save MIDI files to disk

  - Access event timing in both ticks and seconds

#### MIDI File Rendering

- `render_midi()` - Render MIDI file through plugin to numpy array

- `render_midi_stream()` - Generator yielding audio blocks for streaming/large files

- `render_midi_to_file()` - Render MIDI file directly to WAV file (16/24/32-bit)

- `MidiRenderer` class - Stateful renderer with progress tracking and fine-grained control

  - Properties: `duration_seconds`, `progress`, `is_finished`, `current_time`

  - Methods: `render_block()`, `render_all()`, `reset()`

- Automatic tempo map handling for correct timing

- Configurable tail length for reverb/delay tails

#### Core Utilities

- `mh_reset()` - Reset plugin internal state (clears delay lines, reverb tails, filter states)

- `mh_set_non_realtime()` - Enable higher-quality algorithms for offline/batch processing

- `mh_probe()` - Get plugin metadata without full instantiation

- `MH_PluginDesc` struct for plugin metadata (name, vendor, version, format, unique_id, MIDI flags, channel counts)

- `mh_set_sample_rate()` - Change sample rate without reloading plugin (preserves parameter state)

- `mh_get_sample_rate()` - Query current sample rate

- `MH_ScanCallback` typedef for plugin scanning callback

- `mh_scan_directory()` - Recursively scan directory for VST3/AudioUnit/LV2 plugins

- `MH_PluginDesc.path` field added for scan results

- `mh_process_double()` - Process audio with 64-bit double precision

- `mh_supports_double()` - Check if plugin supports native double precision

- `MH_LoadCallback` typedef for async loading callback

- `mh_open_async()` - Load plugin in background thread

#### Parameter & Preset Access

- `mh_param_to_text()` - Convert normalized parameter value to display string (e.g., "2500 Hz")

- `mh_param_from_text()` - Convert display string to normalized value

- `mh_get_num_programs()` - Get number of factory presets

- `mh_get_program_name()` - Get factory preset name by index

- `mh_get_program()` / `mh_set_program()` - Get/set current factory preset

#### Bus Layout & Sidechain

- `MH_BusInfo` struct for bus information (name, channels, is_main, is_enabled)

- `mh_get_num_buses()` - Query number of input/output buses

- `mh_get_bus_info()` - Get detailed bus information

- `mh_open_ex()` - Open plugin with sidechain channel configuration

- `mh_process_sidechain()` - Process audio with sidechain input

- `mh_get_sidechain_channels()` - Query configured sidechain channel count

### Fixed

#### Linux Compilation

- Added Linux build dependencies to README.md (JUCE requires freetype, fontconfig, webkit2gtk, gtk3, etc.)

- Fixed `addFormat()` calls to use raw pointers instead of `std::make_unique<>()` (JUCE's API expects raw pointers)

- Added `POSITION_INDEPENDENT_CODE ON` to libminihost CMakeLists.txt for linking into shared libraries (e.g., Python module)

### Command Line Interface

- `minihost` CLI tool with subcommands for common operations:

  - `probe` - Get plugin metadata without full instantiation

  - `scan` - Recursively scan directory for VST3/AudioUnit/LV2 plugins

  - `info` - Show detailed plugin info (buses, presets, latency)

  - `params` - List plugin parameters with current values

  - `midi` - List available MIDI input/output ports

  - `play` - Real-time audio playback with MIDI input

  - `process` - Offline audio processing through effects, or MIDI-to-audio rendering for synths

- Global options: `--sample-rate`, `--block-size`

- JSON output support (`--json`) for probe, scan, params, midi

- Plugin state and preset loading for process command

- Virtual MIDI port creation for play command

### Python Bindings

All C API additions are exposed in the Python `minihost` module:

- `minihost.AudioDevice` class for real-time audio playback with MIDI

  - Constructor: `AudioDevice(plugin, sample_rate=0, buffer_frames=0, output_channels=0, midi_input_port=-1, midi_output_port=-1)`

  - Methods: `start()`, `stop()`, `connect_midi_input()`, `connect_midi_output()`, `disconnect_midi_input()`, `disconnect_midi_output()`, `create_virtual_midi_input()`, `create_virtual_midi_output()`, `send_midi()`

  - Properties: `is_playing`, `sample_rate`, `buffer_frames`, `channels`, `midi_input_port`, `midi_output_port`, `is_midi_input_virtual`, `is_midi_output_virtual`

  - Context manager support (`with AudioDevice(plugin) as audio:`)

- `minihost.midi_get_input_ports()` - Get list of available MIDI input ports

- `minihost.midi_get_output_ports()` - Get list of available MIDI output ports

- `minihost.MidiFile` class for MIDI file read/write

  - Methods: `load()`, `save()`, `add_track()`, `add_tempo()`, `add_note_on()`, `add_note_off()`, `add_control_change()`, `add_program_change()`, `add_pitch_bend()`, `get_events()`, `join_tracks()`, `split_tracks()`

  - Properties: `num_tracks`, `ticks_per_quarter`, `duration_seconds`

- `minihost.probe(path)` - Module-level function for plugin metadata

- `minihost.scan_directory(path)` - Scan directory for VST3/AudioUnit/LV2 plugins, returns list of metadata dicts

- `Plugin` constructor now accepts `sidechain_channels` parameter

- New properties: `non_realtime`, `num_programs`, `program`, `sidechain_channels`, `num_input_buses`, `num_output_buses`, `sample_rate` (read/write), `supports_double`

- New methods: `reset()`, `param_to_text()`, `param_from_text()`, `get_program_name()`, `get_bus_info()`, `process_sidechain()`, `process_double()`

- Note: For async loading in Python, use Python's `threading` module with the regular `Plugin()` constructor
