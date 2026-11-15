// quick_add.h
//
// Type-to-search box that lists every addable node kind and scanned
// plugin. Up / Down pick a row, Return runs it, Escape closes.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace minihost_desktop {

struct QuickAddItem {
    juce::String          label;   // shown, e.g. "Gain (stereo)"
    juce::String          detail;  // searched and shown dimmed, e.g. "gain"
    std::function<void()> run;
};

// True when every whitespace-separated word of `query` occurs in the
// item's label or detail, ignoring case. An empty query matches.
bool quickAddMatches(const QuickAddItem& item, const juce::String& query);

class QuickAddPopup : public juce::Component,
                      private juce::ListBoxModel
{
public:
    // `close` is called when the popup should go away (after a run, on
    // Escape); the owner deletes it asynchronously.
    QuickAddPopup(std::vector<QuickAddItem> items, std::function<void()> close);
    ~QuickAddPopup() override;

    void focus();
    // Runs the first item matching `query`; false when none does.
    bool runFirstMatch(const juce::String& query);

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    class SearchField;

    void refilter();
    void run(int row);

    int  getNumRows() override { return (int) shown_.size(); }
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { run(row); }
    void returnKeyPressed(int row) override { run(row); }

    std::vector<QuickAddItem>    items_;
    std::vector<int>             shown_;
    std::function<void()>        close_;
    std::unique_ptr<SearchField> search_;
    juce::ListBox                list_;
};

} // namespace minihost_desktop
