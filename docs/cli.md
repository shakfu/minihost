# CLI Reference

The `minihost` command provides a CLI for plugin inspection, real-time playback, offline processing, and audio conversion.

```bash
minihost [-r SAMPLE_RATE] [-b BLOCK_SIZE] <command> [options]
```

## Global Options

| Option | Default | Description |
|--------|---------|-------------|
| `-r, --sample-rate` | 48000 | Sample rate in Hz |
| `-b, --block-size` | 512 | Block size in samples |

## Commands

### `scan` -- Scan directory for plugins

```bash
minihost scan /Library/Audio/Plug-Ins/VST3/
minihost scan ~/Music/Plugins --json
```

| Option | Description |
|--------|-------------|
| `directory` | Directory to scan (required) |
| `-j, --json` | Output as JSON |

### `info` -- Show plugin info

```bash
minihost info /path/to/plugin.vst3          # full info (loads plugin)
minihost info /path/to/plugin.vst3 --probe  # lightweight metadata only
minihost info /path/to/plugin.vst3 --json   # JSON output
```

| Option | Description |
|--------|-------------|
| `plugin` | Path to plugin (required) |
| `--probe` | Metadata only, no full load |
| `-j, --json` | Output as JSON |

### `params` -- List plugin parameters

```bash
minihost params /path/to/plugin.vst3
minihost params /path/to/plugin.vst3 --verbose
minihost params /path/to/plugin.vst3 --json
```

| Option | Description |
|--------|-------------|
| `plugin` | Path to plugin (required) |
| `-V, --verbose` | Show ranges, defaults, flags |
| `-j, --json` | Output as JSON |

### `devices` -- List audio devices

```bash
minihost devices           # list playback and capture devices
minihost devices --json    # output as JSON
```

| Option | Description |
|--------|-------------|
| `-j, --json` | Output as JSON |

Lists available audio playback and capture devices. The system default device is marked. Use the index or a case-insensitive substring of the device name with `minihost play --playback-device` / `--capture-device`.

### `presets` -- List factory presets or export `.vstpreset` files

```bash
# List all factory presets
minihost presets /path/to/synth.vst3
minihost presets /path/to/synth.vst3 --json

# Export factory preset N as a .vstpreset
minihost presets /path/to/synth.vst3 --program 5 --save preset5.vstpreset

# Round-trip a .vstpreset through the plugin (loads, re-saves)
minihost presets /path/to/synth.vst3 --load-vstpreset in.vstpreset --save out.vstpreset

# Convert a raw state blob to .vstpreset
minihost presets /path/to/synth.vst3 --state state.bin --save out.vstpreset
```

| Option | Description |
|--------|-------------|
| `plugin` | Path to plugin (required) |
| `--save FILE.vstpreset` | Save current plugin state as a `.vstpreset` file |
| `--program N` | Select factory program N before saving |
| `--state FILE` | Load raw state blob into plugin before saving |
| `--load-vstpreset FILE` | Load a `.vstpreset` before saving. When combined with `--save`, the source file's `class_id` is preserved in the output |
| `-y, --overwrite` | Overwrite the `--save` target if it already exists |
| `-j, --json` | Output the preset listing as JSON |

Without `--save`, `presets` lists all factory presets (no truncation). With `--save`, the subcommand exports the plugin's current state (optionally after applying `--program`, `--state`, or `--load-vstpreset`) to a `.vstpreset` file.

The output `.vstpreset`'s processor class ID (FUID) is determined as follows, in order:

1. If `--load-vstpreset` was used, the source file's `class_id` is preserved in the output.
2. Otherwise, the FUID is auto-detected from the plugin bundle's `Contents/Resources/moduleinfo.json` (requires VST3 SDK 3.7.5+, which all modern plugins ship).
3. If neither path yields a valid FUID, the command fails with a helpful error rather than writing a `.vstpreset` with a bogus class ID. For legacy plugins, use `--load-vstpreset` to inherit a real FUID from an existing preset.

### `midi` -- List or monitor MIDI ports

```bash
minihost midi                          # list all MIDI ports
minihost midi --json                   # list as JSON
minihost midi -m 0                     # monitor MIDI input port 0
minihost midi --virtual-midi "Monitor" # create virtual port and monitor
```

| Option | Description |
|--------|-------------|
| `-m, --monitor N` | Monitor MIDI input port N |
| `--virtual-midi NAME` | Create virtual port and monitor it |
| `-j, --json` | Output as JSON |

### `play` -- Play plugin with real-time audio/MIDI

```bash
minihost play /path/to/synth.vst3 --midi 0
minihost play /path/to/synth.vst3 --virtual-midi "My Synth"
minihost play /path/to/effect.vst3 --input              # duplex mode
minihost play /path/to/effect.vst3 --input --midi 0     # duplex + MIDI

# Select specific audio devices by index or case-insensitive name substring
minihost play /path/to/synth.vst3 --playback-device "BlackHole"
minihost play /path/to/effect.vst3 --input --capture-device 1 --playback-device 0
```

| Option | Description |
|--------|-------------|
| `plugin` | Path to plugin (required) |
| `-i, --input` | Enable audio input (duplex mode) for effect processing |
| `-m, --midi N` | Connect to MIDI input port N |
| `-v, --virtual-midi NAME` | Create virtual MIDI input |
| `--midi-out N` | Connect to MIDI output port N |
| `--virtual-midi-out NAME` | Create virtual MIDI output |
| `--playback-device INDEX_OR_NAME` | Playback device selector (default: system default) |
| `--capture-device INDEX_OR_NAME` | Capture device selector for `--input` duplex mode |

When `--input` is enabled, the audio device opens in duplex mode: system audio input is captured, processed through the plugin, and played back through speakers. This is useful for guitar amp sims, vocal processing, and live effects.

`--playback-device` and `--capture-device` accept either an integer index from `minihost devices` or a case-insensitive substring of the device name.

### `process` -- Process audio/MIDI offline

```bash
# Basic effect processing
minihost process /path/to/effect.vst3 -i input.wav -o output.wav

# With parameters
minihost process /path/to/effect.vst3 -i input.wav -o output.wav --param "Mix:0.5"

# Render MIDI through synth
minihost process /path/to/synth.vst3 -m song.mid -o output.wav --tail 3.0

# Sidechain processing (second -i is sidechain)
minihost process /path/to/compressor.vst3 -i main.wav -i sidechain.wav -o output.wav

# Batch processing (glob input, directory output)
minihost process /path/to/reverb.vst3 -i "drums/*.wav" -o processed/
minihost process /path/to/effect.vst3 -i "*.wav" -o output/ -y
```

| Option | Description |
|--------|-------------|
| `plugin` | Path to plugin (required) |
| `-o, --output` | Output file or directory (required) |
| `-i, --input FILE` | Input audio file (repeatable; second = sidechain) |
| `-m, --midi-input FILE` | Input MIDI file |
| `-t, --tail SECS` | Tail length after MIDI ends (default: 2.0) |
| `--param SPEC` | Set parameter: `"Name:value"` (repeatable) |
| `--param-file FILE` | JSON automation file |
| `-s, --state FILE` | Load plugin state from file |
| `--vstpreset FILE` | Load .vstpreset file |
| `-p, --preset N` | Load factory preset N |
| `--bit-depth {16,24,32}` | Output bit depth (default: 24) |
| `--out-channels N` | Override output channel count |
| `--non-realtime` | Enable non-realtime processing mode |
| `--bpm BPM` | Set transport BPM |
| `--no-resample` | Error on sample rate mismatch instead of auto-resampling |
| `-y, --overwrite` | Overwrite output if it exists |

#### Batch Mode

When the output path is a directory (ends with `/` or is an existing directory) and input contains glob patterns, batch mode activates:

- Each matched input file is processed independently
- Plugin is loaded once and reset between files
- Output files keep the input filename (e.g., `input/kick.wav` becomes `output/kick.wav`)
- Mismatched sample rates are automatically resampled to match the first file
- Existing output files are skipped unless `-y` is set

### `resample` -- Resample audio files

```bash
minihost resample input.wav -o output.wav -r 48000
minihost resample input.wav -o output.wav -r 44100 --bit-depth 16
minihost resample input.wav -o output.wav -r 96000 -y
```

| Option | Description |
|--------|-------------|
| `input` | Input audio file (required) |
| `-o, --output` | Output file path (required) |
| `-r, --target-rate HZ` | Target sample rate (required) |
| `--bit-depth {16,24,32}` | Output bit depth (default: 24) |
| `-y, --overwrite` | Overwrite output if it exists |
