# CLI Reference

The `minihost` command provides a CLI for plugin inspection, real-time playback, offline processing, parameter morphing, and audio conversion.

```bash
minihost [-r SAMPLE_RATE] [-b BLOCK_SIZE] <command> [options]
```

## Global Options

| Option | Default | Description |
|--------|---------|-------------|
| `-r, --sample-rate` | 48000 | Sample rate in Hz |
| `-b, --block-size` | 512 | Block size in samples |
| `--version` | | Print the release version and exit |

`--version` reports two independent things: the minihost release version, and the C ABI
version of the `libminihost` the binary is linked against. Quote both when filing a bug.
It is spelled the same way in all three CLIs -- long form only, since `-V` is already
`--verbose` in `minihost_c`.

```console
$ minihost --version
minihost 0.7.1
libminihost ABI 2.8.0
```

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

### `morph` -- Interpolate between two parameter snapshots

```bash
# Morph 25% between factory programs 0 and 1 (the default sources)
minihost morph /path/to/synth.vst3 -t 0.25

# Morph between two explicit programs, as JSON
minihost morph /path/to/synth.vst3 --a-program 0 --b-program 5 -t 0.5 --json

# Morph between two saved state files, apply, and save the result
minihost morph /path/to/synth.vst3 \
  --a-state a.state --b-state b.state -t 0.3 --save morphed.state
```

Captures two parameter snapshots (A and B), linearly interpolates them at blend `-t` (0..1, default 0.5), and prints an A/B/blend table (or `--json`). Each snapshot comes from a factory program (`--a-program` / `--b-program`) or a saved state file (`--a-state` / `--b-state`); with no source given it defaults to factory programs 0 and 1. Pass `--apply` to write the blended values back to the plugin, or `--save FILE` to apply and persist the resulting state.

Morphing operates on normalized per-parameter values (not opaque state blobs), so only continuous parameters glide smoothly; stepped/boolean parameters are quantized by the plugin. This is the CLI counterpart of the `minihost.morph` module and the native `Plugin.morph_*` methods, and mirrors the `morph` command in the C and C++ front-ends.

| Option | Description |
|--------|-------------|
| `--a-program N` / `--b-program N` | Snapshot A / B from factory program N |
| `--a-state FILE` / `--b-state FILE` | Snapshot A / B from a saved state file |
| `-t, --blend T` | Blend amount 0..1 (default 0.5) |
| `--apply` | Apply the morphed snapshot to the plugin |
| `--save FILE` | Apply and save the morphed state to FILE |
| `-j, --json` | Output as JSON |

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

---

## Native CLI binaries

The `minihost` command documented above is the Python one. The project also ships two
native binaries, `minihost_c` (pure C) and `minihost_cpp` (C++), built from
`projects/minihost_c` and `projects/minihost_cpp` and published in the `cli` release
archive. They are independent implementations over the same C API and are meant to be
interchangeable; `tests/test_cli_conformance.py` runs them against each other and fails if
they diverge.

They cover the single-plugin commands (`probe`, `scan`, `info`, `params`, `get-param`,
`set-param`, `presets`, `devices`, `midi`, `play`, `load-preset`, `save-state`,
`load-state`, `process`, `morph`, `resample`) plus the two routing commands below. Run
either binary with no arguments for its full option list, or with `--version` to identify
the build -- both report the same release version as the Python CLI, since all three read
it from the `version` field in `pyproject.toml`.

### Naming plugins

Anywhere a command takes a plugin you can give a **path** or a **name from the scan
cache**, matched without regard to case:

```bash
minihost_c scan                     # index the plugins installed on this machine
minihost_c probe dexed              # by name, any case
minihost_c probe "pro-q"            # unique substring -> FabFilter Pro-Q 4
minihost_c chain dexed gigaverb -m song.mid -o out.wav
```

An existing path always wins, so anything that worked before keeps working. Otherwise the
whole name must match, ignoring case. Substring matching is opt-in via `--fuzzy`, because
it is rarely decisive on a real collection -- on a machine with 343 plugins installed,
`reverb` matches 5, `delay` 9 and `filter` 31, so it mostly buys an ambiguity error:

```bash
minihost_c --fuzzy probe "pro-q 3"     # substring, when you want it
```

The same plugin is often installed in two formats under one name (16 of those 343 were),
which would make even an exact name ambiguous. When every match is one name differing only
by format, one is chosen rather than refused: VST3 in preference to AudioUnit, or whatever
`--format` asks for.

```bash
minihost_c --format au probe "FabFilter Pro-Q 4"    # pin the format
```

Both failure modes are explicit:

```
$ minihost_c probe nosuch
Error: no plugin named 'nosuch' in the scan cache (~/Library/Caches/minihost/plugins.json)
       run 'scan' first, or pass a path, or --fuzzy to match part of a name

$ minihost_c --fuzzy probe pro-q
Error: 'pro-q' matches 3 plugins:
       /Library/Audio/Plug-Ins/Components/FabFilter Pro-Q 3.component
       /Library/Audio/Plug-Ins/Components/FabFilter Pro-Q 4.component
       /Library/Audio/Plug-Ins/VST3/FabFilter Pro-Q 4.vst3
       name it more precisely, pass a path, or pick a format with --format
```

Plugins that failed to probe are never offered by name -- one that will not load cannot be
loaded by name either.

### `scan` -- build the index

```bash
minihost_c scan                     # this platform's plugin locations
minihost_c scan /path/to/plugins    # or just this directory
minihost_c scan -j                  # JSON on stdout
```

With no argument, `scan` walks the canonical locations for the platform:

| Platform | Directories |
|----------|-------------|
| macOS | `/Library/Audio/Plug-Ins/{VST3,Components}` and the same two under `~/Library` |
| Windows | `C:\Program Files\Common Files\VST3`, and the x86 equivalent |
| Linux | `/usr/lib/vst3`, `/usr/local/lib/vst3`, `~/.vst3` |

Directories that do not exist are skipped, and the ones being scanned are printed.

Scanning **probes each plugin, which means loading it**, so a first pass over a large
collection takes minutes. Results are cached with an mtime + size fingerprint, so a repeat
scan re-probes only what changed, and the cache is written as the scan proceeds -- a scan
that dies part way keeps what it had, and re-running resumes. The cache is the same file
the Python CLI's `minihost cache` commands manage
(`~/Library/Caches/minihost/plugins.json` on macOS, overridable with
`MINIHOST_CACHE_DIR`), so a scan from either side serves the other.

The Python `minihost scan` supervises the same way and takes the same `--in-process`
flag, so either front-end can be pointed at an unfamiliar plugin directory.

Each plugin is probed in a child process that the scan is willing to lose. That matters
because probing means loading, and an installed collection can be relied on to hold a
plugin that spins forever or corrupts its heap on load -- five of ~350 here do. In process
the first of those ends the scan; supervised, it costs one entry:

| status | meaning |
|--------|---------|
| `ok` | probed, and usable by name |
| `error` | probed and declined the file -- not a plugin, or an unreadable one |
| `timeout` | did not finish within the deadline (60 s, or `MINIHOST_SCAN_TIMEOUT_MS`) |
| `crash` | the child died before answering |

All four are recorded with the same file fingerprint, so a re-scan skips the bad plugins
rather than paying for them again. `--in-process` probes in the scanning process instead,
which is faster by the cost of one process launch per plugin and is how it worked before.

!!! note "Probing in a child is also more reliable, not just safer"

    The worker never starts the JUCE message thread, so it probes on its own main thread.
    Several AudioUnits that hang or crash when probed on our message thread --
    `AppleAES3Audio` among them -- probe perfectly there. A full scan of 333 AudioUnits
    that stopped at entry 66 in process now completes: 331 ok, 2 that are not plugins.

    The durable fix is scanning out of process, as the desktop app does
    (`minihost_desktop --scan-plugins`); the CLI does not yet.

### `process` -- one plugin

```bash
# audio in, audio out
minihost_c process Plugin.vst3 -i input.wav -o output.wav --tail 3

# MIDI in: an instrument needs no audio input
minihost_c process Synth.vst3 -m song.mid -o output.wav --tail 2
```

`-m` renders a MIDI file through the plugin. It works in both binaries as of 0.7.0; before
that the C binary parsed the flag and refused it.

### `chain` -- plugins in series

```bash
minihost_c chain EQ.vst3 Reverb.vst3 -i input.wav -o output.wav --tail 3

# a MIDI effect ahead of an instrument
minihost_c chain Arpeggiator.component Synth.vst3 -m song.mid -o output.wav
```

Plugins are given in signal order. MIDI enters the first plugin that accepts it and is
carried onward by any plugin that produces MIDI, so a MIDI effect drives the instrument
behind it -- see [MIDI Routing](midi_routing.md) for the rules, including why MIDI effects
have to come first. `--mix INDEX:VALUE` sets one plugin's dry/wet (repeatable).

### `bus` -- branches in parallel, summed

```bash
# layer two instruments from one MIDI part
minihost_c bus SynthA.vst3 SynthB.vst3 -m song.mid -o output.wav

# a branch may itself be a chain: commas run plugins in series
minihost_c bus Synth.vst3 "Chorder.component,Synth.vst3" -m song.mid -o out.wav --gain 1:0.7
```

Each argument is one branch and the same MIDI is fanned to all of them. `--gain
INDEX:VALUE` sets a branch's gain (repeatable); `0.0` mutes it. Instrument branches carry
no audio input, which is why a bus of instruments is built zero-width.

### Options shared by the rendering commands

| Option | Description |
|--------|-------------|
| `-i, --input FILE` | Input audio file |
| `-m, --midi FILE` | Input MIDI file |
| `-o, --output FILE` | Output audio file (required) |
| `-t, --tail SECONDS` | Extra time rendered past the input, for delay and reverb tails |
| `--bpm BPM` | Transport tempo reported to the plugins |
| `--non-realtime` | Put the plugins in offline mode |
| `--bit-depth {16,24,32}` | Output bit depth |

`minihost_cpp` takes the same short options; its long forms differ in places (`-m` is
`--midi-input` rather than `--midi`), since the two binaries use different argument
parsers. Where the shapes had drifted apart they have been brought back together --
`resample` now accepts both `resample IN OUT --rate N` and `resample IN -o OUT -r N` in
either binary.
