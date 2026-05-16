// minihost_desktop - GUI host application (stub entry point)
//
// Status: scaffold only. The real application starts here once Phase 0
// of docs/dev/desktop_app_todo.md is underway. For now this is just
// enough to link and exit cleanly, so the CMake target is buildable
// and CI can exercise the GUI-mode libminihost path end-to-end.

#include <juce_gui_basics/juce_gui_basics.h>

namespace minihost_desktop {

class StubApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return "minihost"; }
    const juce::String getApplicationVersion() override { return "0.0.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String&) override {
        // TODO(desktop_app_todo.md Phase 1): construct MainWindow,
        // ProjectModel, GraphCanvas. For now exit immediately so the
        // stub is harmless to launch.
        quit();
    }

    void shutdown() override {}
};

} // namespace minihost_desktop

START_JUCE_APPLICATION(minihost_desktop::StubApplication)
