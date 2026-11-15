// properties_panel.cpp -- see properties_panel.h.

#include "properties_panel.h"
#include "canvas.h"

namespace minihost_desktop {

namespace {

const juce::Colour kPanelBg (0xff222428);
const juce::Colour kDim     (0xffaab0b8);
const juce::Colour kProblem (0xffe5534b);

// "in_channels" -> "In channels"
juce::String fieldLabel(const juce::String& key)
{
    auto s = key.replaceCharacter('_', ' ');
    return s.substring(0, 1).toUpperCase() + s.substring(1);
}

} // namespace

PropertiesPanel::PropertiesPanel(CanvasComponent& canvas) : canvas_(canvas)
{
    setTitle("Properties");
    title_.setFont(juce::FontOptions(15.0f, juce::Font::bold));
    subtitle_.setColour(juce::Label::textColourId, kDim);
    problems_.setColour(juce::Label::textColourId, kProblem);
    hint_.setColour(juce::Label::textColourId, kDim);
    hint_.setJustificationType(juce::Justification::topLeft);
    stats_.setColour(juce::Label::textColourId, kDim);
    for (auto* l : { &title_, &subtitle_, &stats_, &problems_, &hint_ })
        addAndMakeVisible(*l);
    startTimerHz(2);
    open_editor_.onClick = [this] { canvas_.openEditorFor(id_); };
    addChildComponent(open_editor_);
    viewport_.setViewedComponent(&content_, false);
    viewport_.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport_);
    rebuild();
}

PropertiesPanel::~PropertiesPanel() = default;

void PropertiesPanel::focusFirstField()
{
    rebuild();
    if (!fields_.empty()) fields_.front().editor->grabKeyboardFocus();
}

juce::StringArray PropertiesPanel::fieldKeys() const
{
    juce::StringArray keys;
    for (const auto& f : fields_) keys.add(f.key);
    return keys;
}

bool PropertiesPanel::setField(const juce::String& key, const juce::String& value)
{
    for (auto& f : fields_)
    {
        if (f.key != key) continue;
        if (auto* te = dynamic_cast<juce::TextEditor*>(f.editor.get()))
            te->setText(value, false);
        else if (auto* cb = dynamic_cast<juce::ComboBox*>(f.editor.get()))
            cb->setText(value, juce::dontSendNotification);
        commit();
        return true;
    }
    return false;
}

void PropertiesPanel::timerCallback()
{
    juce::StringArray parts;
    if (canvas_.kindOf(id_) == "plugin")
    {
        const int lat = canvas_.latencyOf(id_);
        const int sr  = canvas_.sampleRate();
        if (lat >= 0)
            parts.add("Latency " + juce::String(lat) + " smp"
                      + (sr > 0 ? " (" + juce::String(1000.0 * lat / sr, 1) + " ms)" : juce::String()));
        const float load = canvas_.loadOf(id_);
        if (load >= 0.0f) parts.add("CPU " + juce::String(load * 100.0f, 1) + "%");
        if (const int lanes = canvas_.automationLanesOf(id_); lanes > 0)
            parts.add(juce::String(lanes) + (lanes == 1 ? " automated parameter" : " automated parameters"));
    }
    const auto text = parts.joinIntoString("\n");
    if (text != stats_.getText())
    {
        stats_.setText(text, juce::dontSendNotification);
        resized();
    }
}

void PropertiesPanel::rebuild()
{
    // Keep focus on the same field across a rebuild after a commit.
    juce::String focused_key;
    for (const auto& f : fields_)
        if (f.editor->hasKeyboardFocus(true)) focused_key = f.key;

    fields_.clear();
    extras_.clear();
    const auto sel = canvas_.selectedIds();
    id_ = sel.size() == 1 ? sel[0] : juce::String();

    if (id_.isEmpty())
    {
        title_.setText(sel.isEmpty() ? "No selection"
                                     : juce::String(sel.size()) + " nodes selected",
                       juce::dontSendNotification);
        subtitle_.setText({}, juce::dontSendNotification);
        problems_.setText({}, juce::dontSendNotification);
        hint_.setText(sel.isEmpty()
            ? "Select a node to edit it.\n\n"
              "Add a node: Cmd+K, or double-click\n"
              "Connect: drag output to input\n"
              "Add files: drop them on the canvas\n"
              "Select several: drag on empty space\n"
              "Pan: scroll. Zoom: Cmd+scroll\n"
              "Fit to window: Cmd+0"
            : "Copy: Cmd+C. Duplicate: Cmd+D\n"
              "Delete: Delete\n"
              "Move: arrow keys",
            juce::dontSendNotification);
        open_editor_.setVisible(false);
        resized();
        return;
    }

    hint_.setText({}, juce::dontSendNotification);
    title_.setText(id_, juce::dontSendNotification);
    subtitle_.setText(canvas_.subtitleOf(id_),
                      juce::dontSendNotification);
    problems_.setText(canvas_.problemsOf(id_).joinIntoString("\n"),
                      juce::dontSendNotification);
    open_editor_.setVisible(canvas_.kindOf(id_) == "plugin");
    timerCallback();

    if (auto aw = canvas_.createPropertiesFor(id_))
    {
        for (int i = 0; i < aw->getNumChildComponents(); ++i)
        {
            auto* child = aw->getChildComponent(i);
            Field f;
            f.key = child->getName();
            if (auto* te = dynamic_cast<juce::TextEditor*>(child))
            {
                auto ed = std::make_unique<juce::TextEditor>(f.key);
                ed->setText(te->getText(), false);
                ed->setReadOnly(te->isReadOnly());
                if (te->isReadOnly())
                    ed->setColour(juce::TextEditor::textColourId, kDim);
                ed->onReturnKey = [this] { commit(); };
                ed->onFocusLost = [this] { commit(); };
                ed->onEscapeKey = [this] { rebuild(); };
                f.editor = std::move(ed);
            }
            else if (auto* cb = dynamic_cast<juce::ComboBox*>(child))
            {
                auto box = std::make_unique<juce::ComboBox>(f.key);
                for (int k = 0; k < cb->getNumItems(); ++k)
                    box->addItem(cb->getItemText(k), cb->getItemId(k));
                box->setSelectedId(cb->getSelectedId(), juce::dontSendNotification);
                box->onChange = [this] { commit(); };
                f.editor = std::move(box);
            }
            else continue;
            f.label = std::make_unique<juce::Label>(f.key + "_label", fieldLabel(f.key));
            f.label->setColour(juce::Label::textColourId, kDim);
            f.label->attachToComponent(f.editor.get(), false);
            f.editor->setTitle(fieldLabel(f.key));
            content_.addAndMakeVisible(*f.label);
            content_.addAndMakeVisible(*f.editor);
            fields_.push_back(std::move(f));
        }
    }
    if (extra_rows_ && canvas_.kindOf(id_) == "plugin")
    {
        extra_rows_(id_, extras_);
        for (auto& c : extras_) content_.addAndMakeVisible(*c);
    }
    resized();

    for (const auto& f : fields_)
        if (f.key == focused_key) f.editor->grabKeyboardFocus();
}

void PropertiesPanel::commit()
{
    // Rebuilding inside apply moves focus, which would commit again.
    if (committing_ || id_.isEmpty()) return;
    auto aw = canvas_.createPropertiesFor(id_);
    if (aw == nullptr) return;

    bool changed = false;
    for (const auto& f : fields_)
    {
        if (auto* src = dynamic_cast<juce::TextEditor*>(f.editor.get()))
            if (auto* dst = aw->getTextEditor(f.key))
            {
                changed = changed || dst->getText() != src->getText();
                dst->setText(src->getText(), false);
            }
        if (auto* src = dynamic_cast<juce::ComboBox*>(f.editor.get()))
            if (auto* dst = aw->getComboBoxComponent(f.key))
            {
                changed = changed || dst->getSelectedId() != src->getSelectedId();
                dst->setSelectedId(src->getSelectedId(), juce::dontSendNotification);
            }
    }
    if (!changed) return;  // no undo step for a field left as it was
    committing_ = true;
    canvas_.applyPropertiesFor(id_, *aw);
    committing_ = false;
    refresh();
}

void PropertiesPanel::paint(juce::Graphics& g)
{
    g.fillAll(kPanelBg);
    g.setColour(juce::Colour(0xff3a3d44));
    g.drawVerticalLine(0, 0.0f, (float) getHeight());
}

void PropertiesPanel::resized()
{
    auto r = getLocalBounds().reduced(12, 10);
    title_.setBounds(r.removeFromTop(24));
    subtitle_.setBounds(r.removeFromTop(20));
    stats_.setBounds(stats_.getText().isEmpty()
        ? juce::Rectangle<int>()
        : r.removeFromTop(18 * juce::StringArray::fromLines(stats_.getText()).size() + 2));
    if (problems_.getText().isNotEmpty())
    {
        const int lines = juce::StringArray::fromLines(problems_.getText()).size();
        problems_.setBounds(r.removeFromTop(18 * lines + 4));
    }
    else problems_.setBounds({});
    if (open_editor_.isVisible())
    {
        r.removeFromTop(6);
        open_editor_.setBounds(r.removeFromTop(26).removeFromLeft(120));
    }
    r.removeFromTop(8);
    hint_.setBounds(hint_.getText().isEmpty() ? juce::Rectangle<int>() : r);
    viewport_.setBounds(r);
    layoutContent();
}

void PropertiesPanel::layoutContent()
{
    const int w = std::max(0, viewport_.getWidth() - viewport_.getScrollBarThickness());
    int y = 0;
    for (auto& f : fields_)
    {
        y += 20;  // the attached label sits above
        f.editor->setBounds(0, y, w, 26);
        y += 34;
    }
    for (auto& c : extras_)
    {
        c->setBounds(0, y, w, 26);
        y += 34;
    }
    content_.setSize(w, y);
}

} // namespace minihost_desktop
