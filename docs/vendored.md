# Vendored Dependencies

This file tracks the vendored C/C++ libraries included in `projects/`. JUCE is not vendored in the repository. It is downloaded on first build by `scripts/download_juce.py` (invoked automatically by `make`) and extracted into the `JUCE/` directory at the project root.

## Libraries

| Library | Version | Directory | Upstream | License |
|---------|---------|-----------|----------|---------|
| miniaudio | 0.11.24 | `projects/miniaudio/` | <https://github.com/mackron/miniaudio> | MIT-0 |
| tflac | unversioned (2024) | `projects/tflac/` | <https://github.com/jprjr/tflac> | BSD-0 |
| libremidi | 5.3.1 | `projects/libremidi/` | <https://github.com/jcelerier/libremidi> | BSD-2-Clause |
| midifile | unversioned (2021) | `projects/midifile/` | <https://github.com/craigsapp/midifile> | BSD-2-Clause |
| py2tosc `check_json.py` | 0.6.0 | `tests/check_json.py` | <https://github.com/shakfu/py2tosc> | MIT |

## Update Process

1. Download the new release from the upstream repository.

2. Replace the contents of the corresponding `projects/<name>/` directory.

3. Build and run tests: `make clean && make build && make test`.

Note that `tests/check_json.py` is a test-only vendoring, not a build
dependency: a single stdlib-only file that py2tosc publishes for projects
which *generate* `.ui.json` descriptions and want to validate them without
depending on the compiler. It is re-copied on a py2tosc schema bump.

4. Check for API changes in headers consumed by minihost (`minihost_audiofile.c`, `minihost_audio.c`, `minihost_midi.cpp`, `_core.cpp`).

5. Update the version in this file.
