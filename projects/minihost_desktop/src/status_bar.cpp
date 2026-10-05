// status_bar.cpp -- see status_bar.h.

#include "status_bar.h"

#include <cmath>

namespace minihost_desktop {

namespace {

const juce::Colour kIdle  (0xff606060);
const juce::Colour kGreen (0xff3aa84a);
const juce::Colour kRed   (0xffd03030);
const juce::Colour kBlue  (0xff3a78c8);
const juce::Colour kAmber (0xffd09020);

// m:ss.t
juce::String formatTime(double seconds)
{
    seconds = std::max(0.0, seconds);
    const int tenths = (int) std::floor(seconds * 10.0);
    return juce::String(tenths / 600) + ":"
         + juce::String((tenths / 10) % 60).paddedLeft('0', 2) + "."
         + juce::String(tenths % 10);
}

} // namespace

StatusBar::StatusBar(Actions actions,
                     std::function<LiveEngine*()> engine,
                     CanvasComponent& canvas)
    : actions_(std::move(actions)),
      engine_(std::move(engine)),
      canvas_(canvas)
{
    auto wire = [this](juce::TextButton& b, std::function<void()>& fn,
                       const juce::String& tip) {
        b.onClick = [&fn]() { if (fn) fn(); };
        b.setTooltip(tip);
        addAndMakeVisible(b);
    };
    wire(live_btn_,   actions_.toggle_live, "Start or stop the audio device");
    wire(play_btn_,   actions_.play,
         "Play the graph from the start and record its output nodes");
    wire(stop_btn_,   actions_.stop,
         "Stop the transport, finish recordings, rewind; stop Play File");
    wire(loop_btn_,   actions_.toggle_loop, "Repeat the input files");
    wire(render_btn_, actions_.render, "Render offline (Cmd+R)");
    loop_btn_.setColour(juce::TextButton::buttonOnColourId, kGreen);
    startTimerHz(15);
    timerCallback();
}

void StatusBar::resized()
{
    auto r = getLocalBounds().reduced(6, 4);
    for (auto* b : { &live_btn_, &play_btn_, &stop_btn_, &loop_btn_, &render_btn_ })
    {
        const int w = b == &live_btn_ ? 84 : 60;
        b->setBounds(r.removeFromLeft(w));
        r.removeFromLeft(4);
    }
}

void StatusBar::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff252525));
    g.setColour(juce::Colour(0xff3a3a3a));
    g.drawHorizontalLine(0, 0.0f, (float) getWidth());

    float x = (float) render_btn_.getRight() + 16.0f;
    const float cy = getHeight() * 0.5f;
    g.setFont(juce::FontOptions(13.0f));
    for (const auto& seg : segments_)
    {
        if (x >= (float) getWidth()) break;
        g.setColour(seg.dot);
        g.fillEllipse(x, cy - 5.0f, 10.0f, 10.0f);
        x += 16.0f;
        const float w = (float) juce::GlyphArrangement::getStringWidthInt(
                            g.getCurrentFont(), seg.text);
        g.setColour(juce::Colour(0xffe0e0e0));
        g.drawText(seg.text, juce::Rectangle<float>(x, 0.0f, w + 2.0f,
                                                    (float) getHeight()),
                   juce::Justification::centredLeft, false);
        x += w + 20.0f;
    }
}

void StatusBar::timerCallback()
{
    auto* e = engine_ ? engine_() : nullptr;
    const bool live      = e != nullptr && e->isRunning();
    const bool playing   = live && e->isTransportPlaying();
    const bool recording = live && e->isRecording();
    const bool looping   = e != nullptr && e->isLooping();
    const bool preview   = e != nullptr && e->isPreviewing();
    // Blinks at 1 Hz so a recording reads as active.
    const bool blink_on  = (juce::Time::getMillisecondCounter() / 500) % 2 == 0;

    std::vector<Segment> segs;
    if (!live)
    {
        segs.push_back({ kIdle, "Live off" });
    }
    else
    {
        auto* dev = e->deviceManager().getCurrentAudioDevice();
        const juce::String name = dev ? dev->getName() : juce::String("no device");
        if (e->hasSampleRateMismatch())
            segs.push_back({ kAmber, name + ": device "
                + juce::String((int) e->mismatchedDeviceSampleRate())
                + " Hz, project " + juce::String((int) e->projectSampleRate())
                + " Hz (wrong pitch)" });
        else
            segs.push_back({ kGreen, name + ", "
                + juce::String((int) e->projectSampleRate()) + " Hz" });

        const double sr  = std::max(1.0, e->projectSampleRate());
        const juce::String pos = formatTime((double) e->positionSamples() / sr)
                               + " / " + formatTime(e->projectFrames() / sr);
        const juce::String tempo = "  " + juce::String(e->bpm(), 1) + " BPM";
        segs.push_back(playing ? Segment{ kGreen, "Playing " + pos + tempo }
                               : Segment{ kIdle,  "Stopped " + pos + tempo });

        juce::StringArray sinks;
        if (auto* lp = e->loadedProject())
            for (const auto& on : lp->doc.outputs)
                sinks.add(on.sink.getFileName());
        if (recording)
            segs.push_back({ blink_on ? kRed : kRed.darker(0.8f),
                "REC " + sinks.joinIntoString(", ") + " "
                + formatTime((double) e->recordedFrames() / sr) });
        else if (sinks.isEmpty())
            segs.push_back({ kIdle, "No output nodes to record" });
        else
            segs.push_back({ kIdle, "Not recording" });
    }
    if (looping)
        segs.push_back({ kGreen, "Loop on" });
    if (preview)
        segs.push_back({ kBlue, "Play File " + e->previewFile().getFileName()
            + " " + formatTime(e->previewPositionSeconds())
            + " / " + formatTime(e->previewLengthSeconds()) });

    live_btn_.setButtonText(live ? "Stop Live" : "Start Live");
    live_btn_.setToggleState(live, juce::dontSendNotification);
    loop_btn_.setToggleState(looping, juce::dontSendNotification);
    play_btn_.setEnabled(!playing);

    bool changed = segs.size() != segments_.size();
    for (size_t i = 0; !changed && i < segs.size(); ++i)
        changed = segs[i].dot != segments_[i].dot || segs[i].text != segments_[i].text;
    if (changed)
    {
        segments_ = std::move(segs);
        repaint();
    }

    CanvasComponent::Activity a;
    a.live      = live;
    a.playing   = playing;
    a.recording = recording;
    a.looping   = looping;
    a.position  = live ? e->positionSamples() : 0;
    a.bpm       = live ? e->bpm() : 0.0;
    if (preview) a.preview = e->previewFile();
    canvas_.setActivity(a);
}

} // namespace minihost_desktop
