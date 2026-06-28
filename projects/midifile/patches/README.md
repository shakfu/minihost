# Local patches to the vendored midifile library

`projects/midifile/` is a **vendored copy** of craigsapp/midifile (BSD-2),
tracked directly in this repo. The patches here are already applied to the
checked-in sources -- they are kept as a record so the fixes can be
re-applied if the library is ever re-vendored from upstream (re-pulling
upstream would clobber them).

## Re-applying after a re-vendor

From the repo root:

```bash
git apply projects/midifile/patches/0001-write-empty-track-guard.patch
```

If the patch no longer applies cleanly (upstream changed the surrounding
code), apply the change by hand using the description below.

## Patches

### 0001-write-empty-track-guard.patch

`MidiFile::write()` (in `src/MidiFile.cpp`) added the per-track
end-of-track marker by calling `m_events[i]->back()` unconditionally. On a
track with no events that calls `std::vector::back()` on an empty vector --
undefined behaviour, observed as a segfault. A fresh `MidiFile` has one
empty track, so even `MidiFile().save()` crashed; likewise `add_track()`
followed by writing events only to the new track left the original track
empty and crashed on save.

The fix guards the `back()` access: when the track is empty, emit the
end-of-track marker without inspecting a non-existent last event.

Regression test: `tests/test_minihost.py::TestMidiFile::test_midifile_save_with_empty_tracks`.
