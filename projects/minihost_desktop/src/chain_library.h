// chain_library.h -- saved chains (docs/dev/chains.md).
//
// Mirrors src/minihost/chain_library.py: a change to validation or
// expansion there needs the same change here. Errors throw
// project::ProjectError.

#pragma once

#include "project.h"

#include <juce_core/juce_core.h>
#include <juce_graphics/juce_graphics.h>

#include <functional>
#include <vector>

namespace minihost_desktop::chains {

constexpr int kVersion = 1;
constexpr const char* kExtension = ".mhchain";

// `chains/` beside the desktop settings file.
juce::File libraryDir();
juce::File fileFor(const juce::String& name);
juce::StringArray names();  // sorted
// Valid as a library file name: non-empty, no path separators.
bool validName(const juce::String& name);
// Project I/O and chain kinds: not allowed inside a chain.
bool forbiddenKind(const juce::String& kind);
// audio_inlet, audio_outlet, midi_inlet, midi_outlet.
bool isPortKind(const juce::String& kind);
juce::String portKind(bool midi, bool in);
// Sort key for a chain's pins: the port's saved y, or +inf if unplaced.
// Pins run top to bottom; ties keep file order.
float pinY(const project::ProjectDocument::Layout& layout, const juce::String& id);

// Read and validate; relative plugin paths resolve against the file.
juce::var readChain(const juce::File& file);
// Validate and write atomically.
void writeChain(const juce::File& file, const juce::var& chain);

// Check a chain's structure. Inner node fields are checked on parse.
void validate(const juce::var& chain, const juce::String& where);

// Replace each `chain` node with its inner nodes (ids "<chain>/<inner>")
// and route edges addressed to a chain id to its ports. Throws for an
// edge that names a node inside a chain.
void expand(juce::Array<juce::var>& nodes, juce::Array<juce::var>& edges,
            std::vector<project::ChainSpec>& out);
// The inverse, for saving: put each chain's nodes, and the edges among
// them, back inside a `chain` node, and address edges to its ports by
// chain id and index. A chain with no nodes is dropped. Ports are
// written in pin order (pinY), so file order matches the canvas.
void fold(juce::Array<juce::var>& nodes, juce::Array<juce::var>& edges,
          const std::vector<project::ChainSpec>& chains,
          const project::ProjectDocument::Layout& layout);

// Edits on a serialized project, as serializeProject writes it. A chain's
// own layout entry is the position of its box on the canvas.
// Group top-level nodes `ids` into a new chain `cid`, with a port node per
// outside source of an edge in and per inside source of an edge out.
// `channels_of` gives a node's output channel count (-1 if unknown), for
// the audio ports this creates.
void group(juce::var& project, const juce::StringArray& ids, const juce::String& cid,
           const juce::String& source,
           const std::function<int(const juce::String&)>& channels_of = {});
// Turn chain `cid`'s nodes back into top-level nodes. Its ports are
// removed and the edges through them joined.
void ungroup(juce::var& project, const juce::String& cid);
// Add a library chain as `cid`, its layout placed at `origin`.
void insert(juce::var& project, const juce::var& chain, const juce::String& cid,
            juce::Point<float> origin);
// Swap chain `cid`'s contents for `chain`, keeping edges and automation
// that still name a node.
void replace(juce::var& project, const juce::String& cid, const juce::var& chain);
// Chain `cid` in library form, named `name`, layout relative to its corner.
juce::var extract(const juce::var& project, const juce::String& cid,
                  const juce::String& name);

} // namespace minihost_desktop::chains
