// ab_window.cpp -- see ab_window.h.

#include "ab_window.h"

namespace minihost_desktop {

AbComparePanel::AbComparePanel(Hooks hooks) : hooks_(std::move(hooks))
{
    setTitle("A/B Compare");
    output_.setTitle("Output to compare");
    output_.onChange = [this] { updateDiff(); };
    render_a_.onClick = [this] { renderInto(0); };
    render_b_.onClick = [this] { renderInto(1); };
    play_.onClick = [this] {
        if (slots_[0].audio.getNumSamples() == 0 || slots_[1].audio.getNumSamples() == 0)
            return;
        hooks_.play(slots_[0].audio, slots_[1].audio, slots_[0].rate);
        hooks_.select(playing_b_);
    };
    stop_.onClick = [this] { hooks_.stop(); };
    pick_a_.setClickingTogglesState(false);
    pick_b_.setClickingTogglesState(false);
    pick_a_.onClick = [this] { choose(false); };
    pick_b_.onClick = [this] { choose(true); };
    hint_.setText("Render A, change the project, render B. Play, then press "
                  "A or B (or Tab) to switch at the same position.",
                  juce::dontSendNotification);
    hint_.setColour(juce::Label::textColourId, juce::Colour(0xffaab0b8));
    diff_.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    for (juce::Component* c : std::initializer_list<juce::Component*>{
             &output_, &render_a_, &render_b_, &info_a_, &info_b_, &diff_,
             &play_, &stop_, &pick_a_, &pick_b_, &hint_ })
        addAndMakeVisible(c);
    setWantsKeyboardFocus(true);
    refreshOutputs();
    choose(false);
    updateDiff();
    setSize(520, 250);
}

void AbComparePanel::refreshOutputs()
{
    const auto current = output_.getText();
    output_.clear(juce::dontSendNotification);
    int i = 1;
    for (const auto& [id, sink] : hooks_.outputs())
        output_.addItem(id + "  (" + sink.getFileName() + ")", i++);
    output_.setText(current, juce::dontSendNotification);
    if (output_.getSelectedId() == 0 && output_.getNumItems() > 0)
        output_.setSelectedId(1, juce::dontSendNotification);
}

void AbComparePanel::renderInto(int slot)
{
    refreshOutputs();
    if (output_.getNumItems() == 0)
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "A/B Compare",
            "The project has no file output node to render.");
        return;
    }
    hooks_.render([sp = SafePointer<AbComparePanel>(this), slot](bool ok) {
        if (ok && sp != nullptr) sp->capture(slot);
    });
}

void AbComparePanel::capture(int slot)
{
    const auto outs = hooks_.outputs();
    const int i = output_.getSelectedId() - 1;
    if (i < 0 || i >= (int) outs.size()) return;
    juce::String err;
    auto& s = slots_[slot];
    if (!readAudioFile(outs[(size_t) i].second, s.audio, s.rate, err))
    {
        juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "A/B Compare", err);
        return;
    }
    s.info = juce::String(s.audio.getNumSamples() / s.rate, 2) + " s, "
           + juce::String(s.audio.getNumChannels()) + " ch, rendered "
           + juce::Time::getCurrentTime().toString(false, true, true, true);
    (slot == 0 ? info_a_ : info_b_).setText(s.info, juce::dontSendNotification);
    updateDiff();
}

void AbComparePanel::updateDiff()
{
    const auto& a = slots_[0];
    const auto& b = slots_[1];
    if (a.audio.getNumSamples() == 0 || b.audio.getNumSamples() == 0)
    {
        diff_.setText("Render both slots to compare them.", juce::dontSendNotification);
        return;
    }
    if (a.rate != b.rate)
    {
        diff_.setText("The renders have different sample rates.", juce::dontSendNotification);
        return;
    }
    const auto d = compareAudio(a.audio, b.audio);
    juce::String text = d.nulls()
        ? juce::String("Identical: the difference is silent")
        : "Difference: RMS " + juce::String(d.rms_db, 1) + " dBFS, peak "
              + juce::String(d.peak_db, 1) + " dBFS";
    if (d.length_differs) text += " (compared over the shorter render)";
    diff_.setText(text, juce::dontSendNotification);
    diff_.setColour(juce::Label::textColourId,
                    d.nulls() ? juce::Colour(0xff74b36a) : juce::Colours::white);
}

void AbComparePanel::choose(bool b)
{
    playing_b_ = b;
    hooks_.select(b);
    pick_a_.setToggleState(!b, juce::dontSendNotification);
    pick_b_.setToggleState(b, juce::dontSendNotification);
}

bool AbComparePanel::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::tabKey) { choose(!playing_b_); return true; }
    if (key.getTextCharacter() == 'a' || key.getTextCharacter() == 'A') { choose(false); return true; }
    if (key.getTextCharacter() == 'b' || key.getTextCharacter() == 'B') { choose(true);  return true; }
    return false;
}

void AbComparePanel::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff222428));
}

void AbComparePanel::resized()
{
    auto r = getLocalBounds().reduced(14);
    output_.setBounds(r.removeFromTop(26));
    r.removeFromTop(10);
    auto row = [&r] { auto x = r.removeFromTop(28); r.removeFromTop(6); return x; };
    auto ra = row();
    render_a_.setBounds(ra.removeFromLeft(110));
    ra.removeFromLeft(10);
    info_a_.setBounds(ra);
    auto rb = row();
    render_b_.setBounds(rb.removeFromLeft(110));
    rb.removeFromLeft(10);
    info_b_.setBounds(rb);
    diff_.setBounds(row());
    auto rp = row();
    play_.setBounds(rp.removeFromLeft(70));
    rp.removeFromLeft(6);
    stop_.setBounds(rp.removeFromLeft(70));
    rp.removeFromLeft(20);
    pick_a_.setBounds(rp.removeFromLeft(44));
    rp.removeFromLeft(4);
    pick_b_.setBounds(rp.removeFromLeft(44));
    hint_.setBounds(r);
}

} // namespace minihost_desktop
