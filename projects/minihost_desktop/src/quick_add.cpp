// quick_add.cpp -- see quick_add.h.

#include "quick_add.h"

namespace minihost_desktop {

bool quickAddMatches(const QuickAddItem& item, const juce::String& query)
{
    const auto hay = (item.label + " " + item.detail).toLowerCase();
    for (const auto& word : juce::StringArray::fromTokens(query.toLowerCase(), true))
        if (word.isNotEmpty() && !hay.contains(word)) return false;
    return true;
}

// Keeps keyboard focus while Up / Down move the list's selection.
class QuickAddPopup::SearchField : public juce::TextEditor
{
public:
    explicit SearchField(QuickAddPopup& owner) : owner_(owner)
    {
        setTitle("Add node");
        setDescription("Type to search node kinds and plugins");
        setTextToShowWhenEmpty("Add node: type to search", juce::Colours::grey);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        auto& list = owner_.list_;
        const int n = owner_.getNumRows();
        if (key.isKeyCode(juce::KeyPress::downKey) && n > 0)
        {
            list.selectRow(std::min(n - 1, list.getSelectedRow() + 1));
            return true;
        }
        if (key.isKeyCode(juce::KeyPress::upKey) && n > 0)
        {
            list.selectRow(std::max(0, list.getSelectedRow() - 1));
            return true;
        }
        return juce::TextEditor::keyPressed(key);
    }

private:
    QuickAddPopup& owner_;
};

QuickAddPopup::QuickAddPopup(std::vector<QuickAddItem> items, std::function<void()> close)
    : items_(std::move(items)), close_(std::move(close))
{
    search_ = std::make_unique<SearchField>(*this);
    search_->onTextChange = [this] { refilter(); };
    search_->onReturnKey  = [this] { run(list_.getSelectedRow()); };
    search_->onEscapeKey  = [this] { close_(); };
    addAndMakeVisible(*search_);

    list_.setModel(this);
    list_.setRowHeight(22);
    list_.setWantsKeyboardFocus(false);
    list_.setTitle("Matching nodes");
    addAndMakeVisible(list_);
    refilter();
}

QuickAddPopup::~QuickAddPopup() = default;

void QuickAddPopup::focus()
{
    search_->grabKeyboardFocus();
}

bool QuickAddPopup::runFirstMatch(const juce::String& query)
{
    search_->setText(query, true);
    refilter();
    if (shown_.empty()) return false;
    run(0);
    return true;
}

void QuickAddPopup::refilter()
{
    shown_.clear();
    const auto q = search_->getText();
    for (int i = 0; i < (int) items_.size(); ++i)
        if (quickAddMatches(items_[(size_t) i], q)) shown_.push_back(i);
    list_.updateContent();
    list_.selectRow(0);
    list_.repaint();
}

void QuickAddPopup::run(int row)
{
    if (row < 0 || row >= (int) shown_.size()) return;
    auto action = items_[(size_t) shown_[(size_t) row]].run;
    close_();
    if (action) action();
}

void QuickAddPopup::paint(juce::Graphics& g)
{
    g.setColour(juce::Colour(0xff26282c));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
    g.setColour(juce::Colour(0xff4a4d55));
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 6.0f, 1.0f);
}

void QuickAddPopup::resized()
{
    auto r = getLocalBounds().reduced(6);
    search_->setBounds(r.removeFromTop(26));
    r.removeFromTop(4);
    list_.setBounds(r);
}

void QuickAddPopup::paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) shown_.size()) return;
    const auto& item = items_[(size_t) shown_[(size_t) row]];
    if (selected)
    {
        g.setColour(juce::Colour(0xff3a5f80));
        g.fillRect(0, 0, w, h);
    }
    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(13.0f));
    g.drawText(item.label, 8, 0, w - 16, h, juce::Justification::centredLeft, true);
    g.setColour(juce::Colour(0xff9aa0a8));
    g.setFont(juce::FontOptions(11.0f));
    g.drawText(item.detail, 8, 0, w - 16, h, juce::Justification::centredRight, true);
}

} // namespace minihost_desktop
