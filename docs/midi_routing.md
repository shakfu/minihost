# MIDI Routing

Four objects in minihost carry MIDI, and they differ in how far it
travels rather than in what it carries.

| Object | What it does with MIDI |
| --- | --- |
| `Plugin` | Delivers events to one plugin and returns whatever it emits |
| `PluginChain` | Carries the stream down a series of plugins, so a MIDI effect can drive an instrument behind it |
| `PluginBus` | Fans the same events to every parallel branch and merges what the branches emit |
| `PluginGraph` | Routes MIDI as explicit edges between any nodes, including plugin to plugin |

Two capability flags decide what happens at each step. `accepts_midi`
marks a plugin that receives events; `produces_midi` marks one that
emits them. Both come from the plugin's own declaration, and both are
worth checking before wiring anything, because they are not what
intuition suggests: many audio effects accept MIDI for parameter
control, and instruments frequently declare MIDI output they never use.

## A single plugin

`Plugin.process_midi` takes the events for one block and returns the
events the plugin produced during it. The return value is empty for
plugins that emit nothing.

```python
out_events = plugin.process_midi(input_audio, output_audio, [(0, 0x90, 60, 100)])
```

Events are `(sample_offset, status, data1, data2)` tuples, with the
offset measured from the start of the block.

## A chain

MIDI enters the first plugin in the chain that accepts it, and every
plugin declaring `produces_midi` replaces the stream for the plugins
behind it. That is what allows an arpeggiator or a chorder to drive an
instrument further down:

```python
with minihost.PluginChain([arpeggiator, synth, reverb]) as chain:
    chain.process_midi(silence, out, [(0, 0x90, 60, 100)])
```

A plugin declaring no MIDI output ends the stream. Anything left in a
plugin's buffer is treated as leftover input rather than output, since
forwarding it would retrigger a downstream instrument with notes an
upstream one already played. The practical consequence is an ordering
rule: **MIDI effects must come before the instrument**. In
`[midi_effect, audio_effect, instrument]` the audio effect ends the
stream and the instrument stays silent. Order it
`[midi_effect, instrument, audio_effect]`, which is also the only
arrangement that makes audio sense.

What `process_midi` returns is the MIDI leaving the last plugin, so a
chain ending in an instrument or an audio effect returns nothing. At
most 256 events pass from one plugin to the next in a single block.

## A bus

A bus sends the same events to every branch and sums the branch audio.
Each branch is a chain, so the routing rules above apply inside it.

Instruments driven by MIDI alone have no audio input, so a bus that
layers them has no input width either:

```python
bus = minihost.PluginBus(0, 2, max_block_size=512, sample_rate=48000.0)
bus.add_branch(minihost.PluginChain([synth_a]))
bus.add_branch(minihost.PluginChain([chorder, synth_b]))

events, overflow = bus.process_midi(np.zeros((0, 512), np.float32), out, note)
```

Branches may be narrower than the bus but never wider, and their output
width and sample rate must match it exactly. `process_midi` returns the
branches' MIDI output merged into one stream ordered by sample offset,
along with a flag that is true when the merge filled the capacity and
events may have been dropped.

## A graph

`PluginGraph` keeps MIDI on its own edge list, separate from audio. Any
node that produces MIDI can feed any node that accepts it, a source may
fan out to several destinations, and each destination takes one incoming
edge. Audio fan-in needs a mix node; MIDI fan-in needs a merge node.

This is the only routing object that expresses a split: one part driving
an instrument directly and, through a MIDI effect, a second instrument,
with the audio summed.

```python
g = minihost.PluginGraph(512, 48000.0)
mi = g.add_midi_input()
direct, fx, layered = g.add_plugin(synth_a), g.add_plugin(chorder), g.add_plugin(synth_b)
mix, out = g.add_mix(2, 2), g.add_output(2)

g.connect_midi(mi, direct)      # one source, two destinations
g.connect_midi(mi, fx)
g.connect_midi(fx, layered)     # the effect drives the second instrument
g.connect(direct, mix, 0)
g.connect(layered, mix, 1)
g.connect(mix, out)
g.compile()

g.set_midi_input_events(mi, [(0, 0x90, 60, 100)])
g.render_block([], [buffer], 512)
```

Events are staged per block on a MIDI input node and cleared after each
render. A MIDI output node collects whatever reaches it, drained with
`get_midi_output_events` after `render_block`.

Three node types reshape MIDI without a plugin, selected by the `op`
field: a filter (by channel mask and note range), a transpose, and a
velocity curve. Omitted fields default to pass-through.

```python
up_an_octave = g.add_midi_processor({"op": 1, "transpose_semitones": 12})
g.connect_midi(mi, up_an_octave)
g.connect_midi(up_an_octave, synth_node)
```

Plugin nodes with no audio input are fine: compile does not require
their input port to be wired, and they are fed silence.

## Choosing between them

Reach for a chain when MIDI runs in a straight line, which covers most
work: effects into an instrument, then audio effects after it. Reach for
a bus to layer one part across parallel instruments. Reach for the graph
when the MIDI path branches or rejoins, or when you need the processor
and merge nodes.

## Practical notes

- **A MIDI effect need not answer in the block it was fed.** Several
  reply a block later. Code that sends a note and inspects the same
  block's output will conclude, wrongly, that nothing was emitted.
- **The high-level renderers take a plugin or a chain, not a graph.**
  `render_midi` and `render_midi_to_file` cannot drive a `PluginGraph`.
  Rendering a MIDI file through a graph means your own block loop:
  convert the file with `midi_file_to_events`, slice the events per
  block, stage them with `set_midi_input_events`, and call
  `render_block`.
- **Silence is the usual symptom of a routing mistake**, not an error.
  If an instrument produces nothing, check that the plugins ahead of it
  report `produces_midi`, and that no audio-only stage sits between the
  MIDI source and the instrument.
