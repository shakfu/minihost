# Changelog

## [Unreleased]

### Added

#### Core Utilities

- `mh_reset()` - Reset plugin internal state (clears delay lines, reverb tails, filter states)
- `mh_set_non_realtime()` - Enable higher-quality algorithms for offline/batch processing
- `mh_probe()` - Get plugin metadata without full instantiation
- `MH_PluginDesc` struct for plugin metadata (name, vendor, version, format, unique_id, MIDI flags, channel counts)
- `mh_set_sample_rate()` - Change sample rate without reloading plugin (preserves parameter state)
- `mh_get_sample_rate()` - Query current sample rate
- `MH_ScanCallback` typedef for plugin scanning callback
- `mh_scan_directory()` - Recursively scan directory for VST3/AudioUnit plugins
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

### Python Bindings

All C API additions are exposed in the Python `minihost` module:

- `minihost.probe(path)` - Module-level function for plugin metadata
- `minihost.scan_directory(path)` - Scan directory for plugins, returns list of metadata dicts
- `Plugin` constructor now accepts `sidechain_channels` parameter
- New properties: `non_realtime`, `num_programs`, `program`, `sidechain_channels`, `num_input_buses`, `num_output_buses`, `sample_rate` (read/write), `supports_double`
- New methods: `reset()`, `param_to_text()`, `param_from_text()`, `get_program_name()`, `get_bus_info()`, `process_sidechain()`, `process_double()`
- Note: For async loading in Python, use Python's `threading` module with the regular `Plugin()` constructor
