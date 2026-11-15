// properties_panel.h
//
// Side panel that edits the selected node's properties in place. Each
// field mirrors one control of the node kind's Properties form (see
// CanvasComponent::createPropertiesFor); a field applies on Return, when
// it loses focus, or when a choice changes, as one undo step.

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace minihost_desktop {

class CanvasComponent;

class PropertiesPanel : public juce::Component,
                        private juce::AsyncUpdater,
                        private juce::Timer
{
public:
    explicit PropertiesPanel(CanvasComponent& canvas);
    ~PropertiesPanel() override;

    // Rebuild from the canvas selection. Asynchronous, so a field's own
    // callback can trigger it.
    void refresh() { triggerAsyncUpdate(); }
    // Rebuild now and move keyboard focus to the first field.
    void focusFirstField();

    // For `--canvas-script`: rebuild synchronously, list the fields, and
    // set one as the user would (text, or a choice's text), then commit.
    void rebuildNow() { rebuild(); }
    juce::StringArray fieldKeys() const;
    bool setField(const juce::String& key, const juce::String& value);

    // Extra rows for plugin nodes, supplied by the app (e.g. Bypass).
    using ExtraRowsFn = std::function<void(const juce::String& id,
                                           std::vector<std::unique_ptr<juce::Component>>&)>;
    void setExtraRows(ExtraRowsFn fn) { extra_rows_ = std::move(fn); }

    static constexpr int kWidth = 260;

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    struct Field {
        juce::String                     key;
        std::unique_ptr<juce::Label>     label;
        std::unique_ptr<juce::Component> editor;  // TextEditor or ComboBox
    };

    void handleAsyncUpdate() override { rebuild(); }
    // Refreshes the plugin stats line (latency, CPU).
    void timerCallback() override;
    void rebuild();
    void commit();
    void layoutContent();

    CanvasComponent&  canvas_;
    ExtraRowsFn       extra_rows_;
    juce::String      id_;
    juce::Label       title_, subtitle_, stats_, problems_, hint_;
    juce::TextButton  open_editor_{ "Open Editor" };
    juce::Viewport    viewport_;
    juce::Component   content_;
    std::vector<Field> fields_;
    std::vector<std::unique_ptr<juce::Component>> extras_;
    bool              committing_ = false;
};

} // namespace minihost_desktop
