# Architecture — what shares what

Status: descriptive. Captures the current layering between the C library, the Python wheel, and the `minihost_desktop` binary so future refactors don't accidentally couple them in surprising ways.

The TL;DR: **three artifacts, one C core.** The wheel and the desktop binary are largely independent codebases that both consume the same `libminihost` C ABI. The only meaningful coupling outside the C ABI is the JSON project schema, which has two independent parsers (one Python, one C++) kept in sync by a parity test.

## Layer diagram

```text
                ┌───────────────────────────────────────────────────────┐
                │  libminihost  (C ABI: mh_open, mh_process,            │
                │   mh_graph_*, mh_set_node_midi,                    │
                │   mh_graph_set_node_automation, ...)               │
                │   minihost.cpp / minihost_chain.cpp /                 │
                │   minihost_graph.cpp / minihost_graph_v2.cpp          │
                └───────────┬───────────────────────────┬───────────────┘
                            │                           │
            libminihost.a (headless)         libminihost_gui.a (full JUCE)
                            │                           │
                            ▼                           ▼
            ┌───────────────────────┐     ┌───────────────────────┐
            │ libminihost_audio.a   │     │ libminihost_audio_gui │
            │ (mh_audio_read,       │     │  (same .c sources,    │
            │  mh_audio_write,      │     │   PUBLIC-links _gui)  │
            │  miniaudio + tflac)   │     │                       │
            └───────────────────────┘     └───────────────────────┘
                            │                           │
                            ▼                           ▼
            ┌───────────────────────┐     ┌───────────────────────┐
            │  src/minihost/_core.cpp│     │ projects/minihost_   │
            │  (nanobind binding)   │     │  desktop/src/*        │
            │                       │     │  (main, canvas, live, │
            │  src/minihost/*.py    │     │   project)            │
            │  (project.py, cli.py, │     │                       │
            │   render.py, ...)     │     │                       │
            └──────────┬────────────┘     └──────────┬────────────┘
                       ▼                              ▼
                 Python wheel                   minihost_desktop binary
```

## What is shared (deepest to thinnest)

1. **The C ABI in `libminihost`.** Every type and function -- `MH_Plugin`, `MH_PluginGraph`, `MH_PluginChain`, `MH_PluginBus`, `mh_open`, `mh_process*`, `mh_graph_*`, the Phase-2 additions (`mh_graph_set_node_midi`, `mh_graph_set_node_automation`), `mh_get_juce_processor` -- is the single source of truth. Both consumers call into this. The header is valid C; only consumers that link JUCE cast the `void*` returned by `mh_get_juce_processor`.

2. **`libminihost_audio` (and `_gui` sibling).** Audio file I/O and MIDI ringbuffers. Consumed by both the wheel and the desktop. The two variants exist only so the wheel-side build stays headless while the desktop links the GUI flavour of `libminihost` -- both compile the same C source files.

3. **JSON project schema.** The *spec* is shared, but the **parsers are not the same code**. See [The schema-duplication pinch](#the-schema-duplication-pinch) below.

## What is NOT shared

- **The Python wheel doesn't use `libminihost_gui` or any GUI code.** It links `libminihost.a` (headless), which substitutes `juce_audio_processors_headless` for `juce_audio_processors`. The wheel has no `juce::Component`, no editor windows, no `AudioDeviceManager`. (See the build / link layer in [docs/dev/desktop_app.md](desktop_app.md).)

- **The desktop binary doesn't embed Python.** It calls `mh_graph_*` natively from C++. No Python interpreter, no `nanobind`, no IPC seam.

- **`minihost::PluginGraph`** -- the header-only RAII wrapper in `projects/libminihost/minihost_graph_v2.hpp` -- is used only by the desktop's `LiveEngine` and project loader. The Python binding talks straight to the C ABI through `nanobind`; the C++ wrapper is not on its path.

- **App-level code** -- `main.cpp`, `canvas.{h,cpp}`, `live.{h,cpp}`, the C++ `project.{h,cpp}`, `rt_param_queue.h` -- is desktop-only.

- **Python-level code** -- `process.py`, `chain.py`, `render.py`, `audio_io.py`, `cli.py`, `project.py`, `automation.py`, `vstpreset.py` -- is wheel-only.

## The schema-duplication pinch

The JSON project file format (schema v1, declared in `src/minihost/project.py` and re-declared in `projects/minihost_desktop/src/project.{h,cpp}`) is the **one load-bearing coupling outside the C ABI**.

Both parsers must accept the same documents and produce equivalent in-memory shapes. A schema change requires editing both:

| Code path | File |
|---|---|
| Python load | `src/minihost/project.py` (`load_project`, `save_project`) |
| C++ load    | `projects/minihost_desktop/src/project.cpp` (`parseProjectFile`, `loadProject`, `saveProjectFile`) |

The contract is enforced by parity tests, not by code generation:

- **Python load + render parity:** `tests/test_project.py` (13 tests) covers validation, round-trip, and render-vs-`process_audio_to_file`.

- **C++ vs Python parity:** `tests/test_desktop_render_parity.py` (2 tests) drives `minihost_desktop --render-project=...` headlessly and asserts the output is bit-identical to `minihost.render_project()` over the same project. Also asserts `--save-roundtrip=...` (C++ load + save) preserves layout for the Python loader.

If you change the schema and forget one parser, those tests fail.

Generating one parser from the other (e.g. via a JSON-schema codegen) would close the gap structurally; the current hand-maintained mirror is acceptable because the schema is small (4 node kinds, 1 edge kind, optional layout) and changes infrequently.

## Version identity

Two versions run on independent schedules and should not be confused.

**The release version** is what a user means by "which minihost is this". It has exactly one source of truth: the `version` field in `pyproject.toml`. Everything else derives from it, so bumping a release is a one-line edit plus a mirror:

| Consumer | How it gets the version |
|---|---|
| Python package | `__version__` in `src/minihost/__init__.py` -- the one hand-maintained copy, kept a literal so `import minihost` costs no metadata lookup |
| Wheel build | `SKBUILD_PROJECT_VERSION`, handed to CMake by scikit-build-core |
| Standalone CMake build | the top-level `CMakeLists.txt` parses the same `version = "..."` line |
| `minihost_c`, `minihost_cpp`, `minihost_desktop` | `MINIHOST_VERSION` from the generated `minihost_version.h` |
| macOS app bundle | `MACOSX_BUNDLE_SHORT_VERSION_STRING` / `..._BUNDLE_VERSION`, set from `MINIHOST_VERSION` |
| CI artifact and archive names | the tag on `refs/tags/v*` builds, otherwise the same `pyproject.toml` line |

`cmake/minihost_version.h.in` is configured into `${CMAKE_BINARY_DIR}/generated/minihost_version.h` and exposed by the `minihost_version` INTERFACE target; a target that wants to report its version links that target and includes the header. `pyproject.toml` is in `CMAKE_CONFIGURE_DEPENDS`, so editing it re-runs configure on the next build.

All three CLIs answer `--version` with the release version and the linked library's ABI version. The long form is used everywhere because `-V` is already `--verbose` in `minihost_c`.

**The C ABI version** is `MH_API_VERSION_{MAJOR,MINOR,PATCH}` in `projects/libminihost/minihost.h`, currently 2.8.0 against a 0.7.1 release. It moves when struct layouts or entry points change, which is what lets a wheel detect a mismatched separately-installed library. Tying it to the release version would force an ABI bump on every release and a release on every ABI bump, so the two are kept apart.

### Bumping a release

1. Edit `version` in `pyproject.toml`.

2. Mirror it into `__version__` in `src/minihost/__init__.py`.

3. Add the matching `## [X.Y.Z]` section to `CHANGELOG.md`.

`tests/test_release_version.py` fails if any of these disagree, or if a CLI binary in the build tree reports something else. Nothing else needs editing -- the generated header picks the new value up on the next configure.

## Where changes propagate

| Change in… | Affects Python wheel? | Affects desktop? |
|---|---|---|
| `minihost.h` / `minihost.cpp`                       | Yes (via `_core.cpp`) | Yes (via C++ call) |
| `minihost_graph_v2.{h,cpp}`                         | Yes (via `nanobind`)  | Yes (via C++ wrapper) |
| `minihost_graph_v2.hpp` (C++ wrapper, header-only)  | No                    | Yes (used by `LiveEngine`, project loader) |
| `src/minihost/project.py`                           | Yes                   | No                  |
| `projects/minihost_desktop/src/project.{h,cpp}`     | No                    | Yes                 |
| Schema doc-of-truth (the JSON shape itself) | Both -- must hand-port to both parsers |  |
| `src/minihost/_core.cpp` (nanobind bindings)        | Yes                   | No                  |
| `version` in `pyproject.toml`                        | Yes                   | Yes (via `minihost_version.h`) |
| `projects/minihost_desktop/src/canvas.{h,cpp}` etc. | No                    | Yes                 |

## Build target topology

`CMakeLists.txt` produces two sibling static libraries for each C library that has a GUI vs headless distinction:

- `libminihost.a` -- always built. Used by the Python wheel and the CLI tools.

- `libminihost_gui.a` -- built when `MINIHOST_BUILD_DESKTOP=ON` (or `MINIHOST_BUILD_GUI_LIB=ON`). Same `.cpp` sources, links `juce_audio_processors` and `juce_gui_basics` instead of the `_headless` variant.

- `libminihost_audio.a` / `libminihost_audio_gui.a` -- the same pattern, gated on the same option.

The Python wheel's `setup` only sees `libminihost.a` / `libminihost_audio.a`. The desktop sees the `_gui` variants. They coexist in one `build/` tree; flipping between them is a configure- time toggle, not a clean-rebuild event.

## Runtime independence

A user can:

- Install the wheel (`uv pip install minihost`) and never touch the desktop binary. They get the C ABI exposed as `minihost.Plugin`, `minihost.PluginGraph`, `minihost.process_audio_to_file`, the CLI tools, etc.

- Build the desktop binary and never use Python. The Python wheel is not a runtime dependency of `minihost_desktop`. The binary is self-contained except for the OS and the user's plugin files.

- Use both, independently. Projects produced by one are consumed by the other byte-identically; that's what the schema parity tests enforce.

The two artifacts share lineage but not lifetime. Releasing a new Python wheel doesn't require a desktop rebuild and vice versa, as long as the C ABI in `libminihost` stays stable.

## Related design docs

- [desktop_app.md](desktop_app.md) -- desktop application design (phases, build configuration, project file format).

- [desktop_app_todo.md](desktop_app_todo.md) -- per-phase task list with current state.

- [graph.md](graph.md) -- the original v1 graph-executor sketch (predates `mh_graph_*` and the C++ wrapper; preserved as a historical note).
