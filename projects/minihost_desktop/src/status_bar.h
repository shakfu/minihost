// status_bar.h
//
// Transport controls and engine state along the bottom of the project
// window: live on/off and device, transport and position, loop,
// recording, and Play File. Polls the LiveEngine on a timer and pushes
// the same state to the canvas as node markers.

#pragma once

#include "canvas.h"
#include "live.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace minihost_desktop {

class StatusBar : public juce::Component,
                  private juce::Timer
{
public:
    struct Actions {
        std::function<void()> toggle_live;
        std::function<void()> play;
        std::function<void()> stop;
        std::function<void()> toggle_loop;
        std::function<void()> render;
    };

    // `engine` returns nullptr until the app creates the engine.
    StatusBar(Actions actions,
              std::function<LiveEngine*()> engine,
              CanvasComponent& canvas);

    static constexpr int kHeight = 34;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Segment {
        juce::Colour dot;
        juce::String text;
    };

    Actions                      actions_;
    std::function<LiveEngine*()> engine_;
    CanvasComponent&             canvas_;
    std::vector<Segment>         segments_;

    juce::TextButton live_btn_   { "Start Live" };
    juce::TextButton play_btn_   { "Play" };
    juce::TextButton stop_btn_   { "Stop" };
    juce::TextButton loop_btn_   { "Loop" };
    juce::TextButton render_btn_ { "Render" };
};

} // namespace minihost_desktop
