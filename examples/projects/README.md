# Example graphs for minihost-desktop

Generate the source audio first. The JSON files reference it by relative path.

```bash
uv run python examples/projects/make_examples.py
make run-desktop    # then File > Open Project...
```

Each graph writes to `build/out/projects/<name>.wav` at the repo root and plays to the speakers. Press Play in the status bar to hear and record, or Cmd+R to render offline.

| File | Graph | What to check |
|-|-|-|
| `01_passthrough` | drums -> out, speakers | Basic Play / Stop / Loop. `drums` reads PLAYING, then ENDED after 2 s unless Loop is on. `out` reads REC. Play File on `out` plays the recording back. |
| `02_gain_meter` | pad -> gain 0.5 -> out, meter, speakers | The meter moves in live mode. The render peaks at about 0.18 instead of 0.37. |
| `03_mix_two_files` | drums + pad -> mix [1.0, 0.6] -> out, meter, speakers | Inputs of different lengths. Drums read ENDED at 2 s while the pad plays to 5 s. Loop repeats both every 5 s. |
| `04_swap_channels` | beeps -> pick L, pick R -> merge (R, L) -> out, speakers | Left and right swap: 880 Hz on the left, 440 Hz on the right. Play File on `beeps` plays the original order for comparison. |
| `05_resample_input` | 44.1 kHz sweep (`resample: true`) -> out, speakers | Pitch matches Play File on `sweep`. Remove `resample` and loading fails with a rate-mismatch error. |
| `06_metronome` | click + drums -> mix -> out, speakers | Clicks follow Audio > Set BPM in live mode. The metronome is silent in offline renders, so the render contains drums only. |
| `07_au_chain` (macOS) | drums -> AULowpass -> AUDelay -> out, speakers | Serial plugins. Double-click a plugin to open its editor. Render with a 1 s tail to keep the echoes. |
| `08_parallel_fx` (macOS) | pad -> dry + AUDelay + AUReverb2 -> mix -> out, meter, speakers | Fan-out and fan-in. Use a 3 s tail. |
| `09_mic_delay` (macOS) | device input -> AUDelay -> out, meter, speakers | Live only. Use headphones to avoid feedback. Play records the delayed mic signal. Offline renders are silent, with length set by `duration_seconds`. |
| `11_sequencer` (macOS) | sequencer -> AUMIDISynth -> AUReverb2 -> out, speakers | A generated minor-pentatonic line at 110 BPM. The node shows its 16 steps and outlines the one playing. Edit it with Properties...: a new seed gives a new melody at once, without restarting Live. |
| `12_two_sequencers` (macOS) | bass seq, lead seq -> 2 x AUMIDISynth -> mix -> AUDelay -> out, meter, speakers | Patterns of 8 and 12 steps drift against each other. The lead mutates 25% of its steps each pass. |
| `13_sequencer_dexed` (needs Dexed) | sequencer -> Dexed -> out, speakers | A third-party VST3 driven by the sequencer. |
| `10_midi_synth` (needs Dexed) | melody.mid -> transpose +12 -> Dexed -> out, speakers | Play runs the MIDI file from the playhead. Device MIDI from Audio > MIDI Input is mixed in. With Loop on, the arpeggio repeats without hung notes. `notes` reads PLAYING, LOOPING or ENDED. |

Known gaps these examples expose:

- The metronome is not rendered offline (`06_metronome`).
- Graph edits during live mode take effect only after Stop Live and Start Live.
