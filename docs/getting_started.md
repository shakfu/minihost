# Getting Started

## Requirements

- CMake 3.20+
- C++17 compiler
- JUCE framework (automatically downloaded if not present)

### Platform-specific

- **macOS**: Xcode command line tools
- **Windows**: Visual Studio 2019+ or MinGW
- **Linux**:
  ```bash
  sudo apt install libasound2-dev libfreetype-dev libfontconfig1-dev \
      libwebkit2gtk-4.1-dev libgtk-3-dev libgl-dev libcurl4-openssl-dev
  ```

## Building

### Python Bindings (recommended)

```bash
git clone https://github.com/shakfu/minihost.git
cd minihost
uv sync          # initial environment setup
make build       # build Python extension (downloads JUCE if needed)
make test        # run tests
```

### C/C++ Library Only

```bash
make cli         # build C library and tools

# Or manually:
cmake -B build
cmake --build build --config Release
```

### Custom JUCE Path

```bash
cmake -B build -DJUCE_PATH=/path/to/JUCE
cmake --build build
```

### Headless Mode

Headless mode (default ON) builds without GUI dependencies. To disable:

```bash
cmake -B build -DMINIHOST_HEADLESS=OFF
cmake --build build
```

## JUCE Setup

JUCE is downloaded automatically by `make`. Manual options:

```bash
# Cross-platform (recommended)
python scripts/download_juce.py

# Unix only
./scripts/download_juce.sh

# Specific version
JUCE_VERSION=8.0.12 python scripts/download_juce.py

# Point to existing installation
cmake -B build -DJUCE_PATH=/path/to/your/JUCE
```

## Verifying the Installation

```python
import minihost
print(minihost.__version__)

# List available MIDI ports
print(minihost.midi_get_input_ports())
print(minihost.midi_get_output_ports())
```

```bash
# CLI
minihost --help
minihost midi  # list MIDI ports
```
