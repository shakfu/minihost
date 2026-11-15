// ab_window.h
//
// Audio > A/B Compare: render the project into slot A, edit, render into
// slot B, then play either one and switch between them while playing.
// Shows the level of their difference (a null test). The app supplies
// rendering and playback through Hooks.

#pragma once

#include "ab_compare.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace minihost_desktop {

class AbComparePanel : public juce::Component
{
public:
    struct Hooks {
        // Output nodes of the open project: (id, sink file).
        std::function<std::vector<std::pair<juce::String, juce::File>>()> outputs;
        // Renders the open project; calls done(true) after the sinks are written.
        std::function<void(std::function<void(bool)> done)> render;
        std::function<void(juce::AudioBuffer<float>, juce::AudioBuffer<float>, double)> play;
        std::function<void(bool b)> select;
        std::function<void()>       stop;
    };

    explicit AbComparePanel(Hooks hooks);

    void resized() override;
    void paint(juce::Graphics& g) override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    struct Slot {
        juce::AudioBuffer<float> audio;
        double                   rate = 0.0;
        juce::String             info;
    };

    void renderInto(int slot);
    void capture(int slot);
    void refreshOutputs();
    void updateDiff();
    void choose(bool b);

    Hooks            hooks_;
    Slot             slots_[2];
    bool             playing_b_ = false;
    juce::ComboBox   output_;
    juce::TextButton render_a_{ "Render to A" }, render_b_{ "Render to B" };
    juce::Label      info_a_, info_b_, diff_;
    juce::TextButton play_{ "Play" }, stop_{ "Stop" };
    juce::TextButton pick_a_{ "A" }, pick_b_{ "B" };
    juce::Label      hint_;
};

} // namespace minihost_desktop
