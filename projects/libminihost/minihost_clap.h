// minihost_clap.h
// CLAP hosting as a juce::AudioPluginFormat. JUCE hosts VST3, AU and LV2 but
// not CLAP, so this adapter wraps a CLAP plugin in an AudioPluginInstance and
// every mh_* path (open, chain, graph, state, scan) treats it like the others.
//
// Scope: audio ports, note ports (MIDI and CLAP note dialects), parameters,
// state, latency, tail, render mode. No editor, no note expressions, no
// per-voice modulation, no port reconfiguration.

#pragma once

#include <memory>

namespace juce { class AudioPluginFormat; }

namespace minihost::clapfmt {

std::unique_ptr<juce::AudioPluginFormat> createFormat();

// Run the main-thread work live CLAP plugins have requested: on_main_thread,
// restart, latency re-query, parameter flush. Call only on the plugin thread.
// Returns the number of requests serviced.
int service();

}  // namespace minihost::clapfmt
