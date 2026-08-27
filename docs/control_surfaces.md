# Control Surfaces

Driving a plugin's parameters from something other than code: a USB MIDI
controller, a tablet running TouchOSC, or anything that speaks OSC.

This page is the guide. The reference material lives in
[Python API](api_python.md) and [CLI Reference](cli.md); the design reasoning,
including what was rejected and why, is in
[docs/dev/osc_and_touch.md](dev/osc_and_touch.md).

## The short version

```bash
# Generate a surface from the plugin's own parameter list
minihost touch synth.vst3 -o synth

# Open synth.tosc on the tablet, then run the host
minihost play synth.vst3 --map-file synth.map.json \
    --osc-port 9000 --osc-feedback 192.168.1.40:9001
```

That gives a paged layout with a control per parameter, bound to both MIDI CC
and OSC, with the surface tracking the plugin rather than only driving it.

## Choosing a transport

| | reaches | resolution | needs |
|---|---|---|---|
| MIDI CC (7-bit) | any controller, any tablet app | 128 steps | a MIDI port |
| MIDI CC (14-bit) | controllers that pair CCs | 16384 steps | a MIDI port |
| OSC | tablets, phones, other hosts | float32 | a UDP port |

7-bit CC is what a hardware knob usually sends and it is fine for most things.
On a filter cutoff, 128 steps across the whole range is audibly stepped -- that
is what 14-bit pairs and OSC are for.

A note on browsers, since it comes up: a web page cannot send UDP, so it can
never speak OSC to minihost directly. Web MIDI works on desktop Chrome, Edge,
Firefox and Android, but **not on iPad or iPhone**, because WebKit has declined
to ship it. Section 7 of the design doc covers the alternatives.

## MIDI

```python
plugin = minihost.Plugin("synth.vst3", sample_rate=48000)

with minihost.AudioDevice(plugin) as audio:
    mapper = minihost.MidiMapper(plugin, device=audio)
    mapper.map_cc(channel=0, cc=74, param="Cutoff", curve="exp")
    mapper.map_cc14(channel=0, cc=1, param="Resonance")
    mapper.map_note(channel=0, note=36,
                    callback=lambda vel: audio.send_midi(0x90, 60, vel))

    audio.start()
    with minihost.MidiIn.open(0, mapper):
        input("Press Enter to stop...\n")
```

Passing `device=audio` matters. Without it a CC write calls `Plugin.set_param`,
which takes the plugin's state mutex and sets the value underneath whatever the
audio thread is doing. With it, the write goes onto a lock-free queue the audio
thread drains at a block boundary.

Curves: `"linear"`, `"exp"` (more resolution low down), `"log"` (more up top).

## OSC

Two paths, and they are for different things.

**Native, for plain automation.** Parses the address in C and takes neither a
lock nor the GIL:

```python
with minihost.AudioDevice(plugin) as audio:
    audio.connect_osc(9000)
    audio.start()
    # /mh/param/3 with one float in 0..1 now moves parameter 3
```

**`OscMapper`, for names, curves and ranges.** Costs a GIL acquisition per
message, and gives you addressing by parameter name:

```python
with minihost.AudioDevice(plugin) as audio:
    mapper = minihost.OscMapper(plugin, device=audio)
    mapper.bind_all()                                  # /mh/param/<name>
    mapper.map_address("/fx/mix", "Dry Wet", curve="exp")
    with minihost.OscServer.open(9000, mapper):
        audio.start()
        ...
```

`bind_all` derives each address from the parameter name by the same rule
`minihost touch` uses, so a generated layout and the host agree without a table
written down anywhere.

## Addressing a chain

A device opened on a `PluginChain` addresses a slot's parameters with an extra
segment. Two forms:

```
/mh/1/param/7        slot 1, by position
/mh/reverb/param/7   the slot named "reverb", wherever it sits
```

Prefer the name. A position is only stable while the chain is built the same
way, and a generated layout outlives the script that builds it -- save a
surface for `[synth, reverb, limiter]`, later edit the script to put the
limiter second, and every address silently points at a different plugin with
nothing to say so.

```python
chain = minihost.PluginChain([synth, reverb, limiter])
with minihost.AudioDevice(chain) as audio:
    audio.set_slot_name(1, "reverb")     # before connect_osc
    audio.connect_osc(9000)
```

Names are alphanumeric and start with a letter, which is what keeps them
distinct from the numeric form, and must be unique. They have to be set before
`connect_osc`, which is enforced rather than merely documented: the table is
read by the OSC socket thread and is never written while that thread exists.

## Feedback

Without it a surface is write-only: load a preset and every fader shows where
the finger left it rather than where the parameter is.

```python
with minihost.OscClient("192.168.1.40", 9001) as out:
    fb = minihost.OscFeedback(plugin, out, mapper.feedback_addresses(),
                              mapper=mapper)
    with fb:
        ...
```

Passing `mapper` suppresses the echo of that mapper's own writes for a short
window. The hazard is a loop with a human in it: the surface sends 0.5, the
poller sends 0.5 back a frame later, and during a drag that fights the finger.

## Transport

The live device has a host playhead, off by default:

```python
audio.set_transport_enabled(True)
audio.transport_set_bpm(128.0)
audio.transport_play()
```

Enable it for anything tempo-synced -- a synced delay, an arpeggiator, an LFO.
Without it the plugin is told there is no transport, and those run at their own
default with the playhead pinned at sample 0.

Over OSC: `/mh/transport/play`, `/stop`, `/bpm`, `/position` (in beats),
`/loop`, `/record`, on the same port as the parameter addresses.

## Generating a surface

```bash
minihost touch synth.vst3 -o synth --params 0-47 --size 1024x768
```

Writes three files:

- `synth.ui.json` -- the layout, in the `py2tosc.ui` dialect
- `synth.map.json` -- a `--map-file` mapping
- `synth.tosc` -- the compiled layout, when `minihost[touch]` is installed

Both bindings come from one parameter table, so the layout and the host's
mapping cannot drift. Widget choice follows the plugin's own metadata: a
boolean parameter becomes a button, a stepped one a radio, everything else a
fader.

The `.ui.json` is a source file, not an artefact. Edit it -- move a control,
change a gap, add a page -- and recompile:

```bash
py2tosc convert synth.ui.json -o synth.tosc
```

Generation itself needs no dependency: without py2tosc the command still writes
a complete, valid layout and tells you how to compile it.

MIDI has 128 controller numbers. A plugin with more parameters than that gets
the remainder bound over OSC only, and the command says so.
