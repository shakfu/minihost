// minihost_desktop
//
// Three modes, selected on the command line:
//
//   minihost_desktop
//       App shell: empty MainWindow with File > Open Plugin / Quit.
//       Editor windows are opened on demand via the file chooser; each
//       loads its own MH_Plugin and is independently closeable.
//
//   minihost_desktop <plugin_path> [output.wav]
//       Interactive single-plugin shortcut: opens an EditorWindow on
//       the supplied plugin (no MainWindow). The window has a
//       "Render 5s" button that writes a WAV.
//
//   minihost_desktop --auto-render <plugin_path> [output.wav]
//       Renders once at startup and quits. Exit 0 on success.
//
//   minihost_desktop --probe [--iterations=N] <plugin_path> [output.wav]
//       Phase 0 lifetime/locking probe. Opens the editor, runs N
//       (default 10) consecutive 5 s renders on a worker, and
//       concurrently tweaks random parameters at ~60 Hz from the
//       message thread + at high rate from a worker thread. A
//       watchdog kills the process if the probe exceeds an upper
//       time bound. Exit 0 on clean completion.
//
//   minihost_desktop --render-project=<project.json>
//       Headless render. Same code path as the File > Render Project
//       menu but with no window; prints progress to stderr and exits
//       with the render's success/failure status.
//
//   minihost_desktop --save-roundtrip=<project.json>
//       Headless parse + re-save to <input>.resaved.json. Exercises the
//       C++ project writer against the Python parser without UI input.
//
//   minihost_desktop --undo-selftest=<project.json>
//       Headless undo/redo self-test over the loaded document. Asserts
//       the UndoHistory contract (snapshot/undo/redo/redo-clear) via the
//       production serializer. Exit 0 on success, 1 on any broken
//       invariant. Driven by tests/test_desktop_undo.py.
//
//   minihost_desktop --scan-plugins[=<dir1;dir2>] [--scan-out=<file.xml>]
//                     [--scan-format=<VST3|AudioUnit|LV2>] [--scan-oop]
//       (--scan-oop routes each probe through a disposable child process for
//       crash containment -- the same path the interactive GUI scan uses.)
//       Headless in-process plugin scan. Registers the host formats and
//       scans the given directories (default: each format's standard
//       locations), writing the KnownPluginList to <file.xml> (default:
//       known_plugins.xml next to the desktop settings). --scan-format
//       restricts to one format (AudioUnit ignores the directory list and
//       enumerates all system AUs, so a hermetic single-dir scan should
//       pass --scan-format=VST3). Prints per-format found/failed counts.
//       Exit 0 on a completed scan. Driven by tests/test_desktop_pluginscan.py.
//
// Note on long options: JUCE's ArgumentList only matches "--opt=value"
// form for long options (the space-separated form parses as two
// separate args). All long options here use the "=" form.
//
// Default output path is /tmp/minihost_smoke.wav.

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>

#include "minihost.h"
#include "minihost_audiofile.h"
#include "minihost_midi.h"
#include "canvas.h"
#include "live.h"
#include "plugin_scanner.h"
#include "project.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace minihost_desktop {

namespace {
constexpr double  kSampleRate = 48000.0;
constexpr int     kBlockSize  = 512;
constexpr int     kChannels   = 2;
constexpr double  kRenderSec  = 5.0;
constexpr double  kNoteOffSec = 4.0;
constexpr const char* kDefaultOut = "/tmp/minihost_smoke.wav";
} // namespace

// Per-window mode. The smoke / probe / auto-render bits drive
// EditorWindow's startup-time behaviour; they're meaningful in
// "single-plugin shortcut" mode but unused for editors opened from
// the MainWindow menu.
enum class EditorMode { Interactive, AutoRenderAndQuit, ProbeAndQuit };

struct EditorOptions {
    juce::String plugin_path;
    juce::String descriptor_xml;   // AU: open via mh_open_desc when set
    juce::String output_path;
    EditorMode   mode        = EditorMode::Interactive;
    int          probe_iters = 10;

    // Optional toolbar override. If toolbar_label is set, EditorWindow
    // shows that button instead of the default "Render 5s -> ...".
    // toolbar_action runs on click (message thread).
    juce::String          toolbar_label;
    std::function<void()> toolbar_action;
};

class EditorWindow;  // fwd
class DesktopApplication;  // fwd

// -------------------------------------------------------------------- //
// Render path                                                          //
// -------------------------------------------------------------------- //

static bool renderToFile(MH_Plugin* plugin, const juce::String& outPath,
                         juce::String& error)
{
    const int totalFrames = (int) (kRenderSec * kSampleRate);
    const int noteOffFrame = (int) (kNoteOffSec * kSampleRate);

    std::vector<float> inL(kBlockSize, 0.0f), inR(kBlockSize, 0.0f);
    std::vector<float> outL(kBlockSize, 0.0f), outR(kBlockSize, 0.0f);
    const float* inputs[2]  = { inL.data(), inR.data() };
    float*       outputs[2] = { outL.data(), outR.data() };

    std::vector<float> interleaved((size_t) totalFrames * (size_t) kChannels,
                                   0.0f);

    int frame = 0;
    bool noteOnSent = false;
    bool noteOffSent = false;

    while (frame < totalFrames)
    {
        const int n = std::min(kBlockSize, totalFrames - frame);

        std::vector<MH_MidiEvent> events;
        if (!noteOnSent)
        {
            MH_MidiEvent ev{};
            ev.sample_offset = 0;
            ev.status        = 0x90;
            ev.data1         = 60;
            ev.data2         = 100;
            events.push_back(ev);
            noteOnSent = true;
        }
        if (!noteOffSent && frame + n > noteOffFrame)
        {
            MH_MidiEvent ev{};
            ev.sample_offset = std::max(0, noteOffFrame - frame);
            ev.status        = 0x80;
            ev.data1         = 60;
            ev.data2         = 0;
            events.push_back(ev);
            noteOffSent = true;
        }

        const int ok = mh_process_midi(plugin, inputs, outputs, n,
                                       events.empty() ? nullptr
                                                      : events.data(),
                                       (int) events.size());
        if (!ok)
        {
            error = "mh_process_midi failed at frame "
                  + juce::String(frame);
            return false;
        }

        for (int i = 0; i < n; ++i)
        {
            interleaved[(size_t)(frame + i) * 2 + 0] = outL[i];
            interleaved[(size_t)(frame + i) * 2 + 1] = outR[i];
        }
        frame += n;
    }

    char err[256] = {0};
    const int wrote = mh_audio_write(outPath.toRawUTF8(),
                                     interleaved.data(),
                                     (unsigned) kChannels,
                                     (unsigned) totalFrames,
                                     (unsigned) kSampleRate,
                                     /*bit_depth=*/24,
                                     err, sizeof(err));
    if (!wrote)
    {
        error = juce::String("mh_audio_write failed: ")
              + juce::String(static_cast<const char*>(err));
        return false;
    }
    return true;
}

class ParamTweakTimer : public juce::Timer
{
public:
    ParamTweakTimer(MH_Plugin* plugin, int numParams)
        : plugin_(plugin), numParams_(numParams) {}

    void timerCallback() override
    {
        if (numParams_ <= 0) return;
        const int idx = rng_.nextInt(numParams_);
        const float val = rng_.nextFloat();
        mh_set_param(plugin_, idx, val);
        ++tweakCount_;
    }

    int tweakCount() const noexcept { return tweakCount_; }

private:
    MH_Plugin*    plugin_;
    int           numParams_;
    juce::Random  rng_;
    int           tweakCount_ = 0;
};

static void startWatchdog(int seconds)
{
    std::thread([seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::fprintf(stderr,
            "watchdog: probe exceeded %d seconds, forcing exit\n",
            seconds);
        std::_Exit(2);
    }).detach();
}

// -------------------------------------------------------------------- //
// EditorWindow                                                         //
// -------------------------------------------------------------------- //

// Owns one MH_Plugin instance and its JUCE editor. closeButtonPressed
// invokes `on_close_` so the owner (DesktopApplication or whoever
// constructed it) can drop the unique_ptr and trigger destruction.
class EditorWindow : public juce::DocumentWindow
{
public:
    EditorWindow(EditorOptions opts,
                 MH_Plugin* plugin,
                 juce::AudioProcessor* proc,
                 std::function<void(EditorWindow*)> on_close)
        : juce::DocumentWindow(juce::String("minihost: ")
                               + juce::File(opts.plugin_path).getFileName(),
                               juce::Colours::darkgrey,
                               juce::DocumentWindow::closeButton),
          opts_(std::move(opts)),
          plugin_(plugin),
          proc_(proc),
          on_close_(std::move(on_close))
    {
        setUsingNativeTitleBar(true);

        auto* container = new juce::Component();
        container->setName("container");

        if (auto* editor = proc->createEditorIfNeeded())
        {
            editor_.reset(editor);
            container->addAndMakeVisible(editor);
            editor->setTopLeftPosition(0, kToolbarHeight);
        }
        else
        {
            auto* msg = new juce::Label({},
                "Plugin has no editor (hasEditor=false).");
            msg->setJustificationType(juce::Justification::centred);
            msg->setSize(420, 80);
            msg->setTopLeftPosition(0, kToolbarHeight);
            container->addAndMakeVisible(msg);
            placeholder_.reset(msg);
        }

        if (opts_.toolbar_label.isNotEmpty() && opts_.toolbar_action)
        {
            renderBtn_.setButtonText(opts_.toolbar_label);
            renderBtn_.onClick = [this] {
                if (opts_.toolbar_action) opts_.toolbar_action();
            };
        }
        else
        {
            renderBtn_.setButtonText("Render 5s -> " + opts_.output_path);
            renderBtn_.onClick = [this] {
                triggerRender(/*quitWhenDone=*/false);
            };
        }
        renderBtn_.setBounds(8, 6, 480, kToolbarHeight - 12);
        container->addAndMakeVisible(renderBtn_);

        const int editorW = editor_ ? editor_->getWidth()  : 480;
        const int editorH = editor_ ? editor_->getHeight() : 100;
        container->setSize(std::max(editorW, 500),
                           editorH + kToolbarHeight);

        setContentOwned(container, /*resizeToFit=*/true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);

        if (opts_.mode == EditorMode::AutoRenderAndQuit)
        {
            juce::Timer::callAfterDelay(250, [this] {
                triggerRender(/*quitWhenDone=*/true);
            });
        }
        else if (opts_.mode == EditorMode::ProbeAndQuit)
        {
            juce::Timer::callAfterDelay(250, [this] { startProbe(); });
        }
    }

    ~EditorWindow() override
    {
        if (tweaker_) tweaker_->stopTimer();
        workerTweakStop_.store(true);
        if (workerTweaker_.joinable())
            workerTweaker_.join();
        editor_.reset();
        if (plugin_)
        {
            mh_close(plugin_);
            plugin_ = nullptr;
        }
    }

    void closeButtonPressed() override
    {
        // Notify the owner. If we're in an exit-on-completion mode the
        // owner will request app quit; otherwise it just deletes us.
        if (on_close_) on_close_(this);
    }

    MH_Plugin* plugin() noexcept { return plugin_; }

private:
    static constexpr int kToolbarHeight = 32;

    void triggerRender(bool quitWhenDone)
    {
        if (renderBusy_.exchange(true)) return;
        renderBtn_.setEnabled(false);
        renderBtn_.setButtonText("Rendering...");

        std::thread([this, quitWhenDone] {
            juce::String error;
            const bool ok = renderToFile(plugin_, opts_.output_path, error);
            juce::MessageManager::callAsync(
                [this, ok, error, quitWhenDone] {
                renderBusy_.store(false);
                renderBtn_.setEnabled(true);
                renderBtn_.setButtonText(ok
                    ? juce::String("Rendered -> " + opts_.output_path)
                    : juce::String("Render failed: " + error));
                if (quitWhenDone)
                {
                    auto* app = juce::JUCEApplication::getInstance();
                    if (app)
                    {
                        app->setApplicationReturnValue(ok ? 0 : 1);
                        app->systemRequestedQuit();
                    }
                }
            });
        }).detach();
    }

    // Toggles the editor (close then re-create) every kEditorToggleMs
    // during a probe run. Exercises the ctor/dtor path while audio is
    // rendering on the worker.
    void toggleEditorOnce()
    {
        if (proc_ == nullptr) return;
        editor_.reset();
        if (auto* nestedContent = dynamic_cast<juce::Component*>(getContentComponent()))
        {
            if (auto* e = proc_->createEditorIfNeeded())
            {
                editor_.reset(e);
                nestedContent->addAndMakeVisible(e);
                e->setTopLeftPosition(0, kToolbarHeight);
            }
        }
        repaint();
    }

    void startProbe()
    {
        const int numParams = mh_get_num_params(plugin_);
        std::fprintf(stderr,
            "probe: %d iterations, %d parameters\n",
            opts_.probe_iters, numParams);

        // Editor open/close toggler every 250 ms during the probe.
        // juce::Timer wraps a lambda for cleanup parity with
        // tweaker_.
        class EditorToggleTimer : public juce::Timer {
        public:
            EditorToggleTimer(EditorWindow* w) : w_(w) {}
            void timerCallback() override { if (w_) w_->toggleEditorOnce(); }
        private:
            EditorWindow* w_;
        };
        editor_toggler_ = std::make_unique<EditorToggleTimer>(this);
        editor_toggler_->startTimer(250);

        tweaker_ = std::make_unique<ParamTweakTimer>(plugin_, numParams);
        tweaker_->startTimer(16);

        workerTweakStop_.store(false);
        workerTweaker_ = std::thread([this, numParams] {
            juce::Random rng;
            while (!workerTweakStop_.load(std::memory_order_relaxed))
            {
                if (numParams > 0)
                {
                    mh_set_param(plugin_,
                                 rng.nextInt(numParams),
                                 rng.nextFloat());
                    ++workerTweakCount_;
                }
                std::this_thread::sleep_for(
                    std::chrono::microseconds(100));
            }
        });

        renderBtn_.setEnabled(false);
        renderBtn_.setButtonText("Probing...");

        std::thread([this] {
            int completed = 0;
            bool ok = true;
            juce::String error;
            for (int i = 0; i < opts_.probe_iters; ++i)
            {
                std::fprintf(stderr,
                    "probe: iter %d / %d starting\n",
                    i + 1, opts_.probe_iters);
                if (!renderToFile(plugin_, opts_.output_path, error))
                {
                    ok = false;
                    break;
                }
                ++completed;
            }

            workerTweakStop_.store(true);
            if (workerTweaker_.joinable())
                workerTweaker_.join();

            juce::MessageManager::callAsync(
                [this, ok, error, completed] {
                int editorWrites = 0;
                if (tweaker_)
                {
                    editorWrites = tweaker_->tweakCount();
                    tweaker_->stopTimer();
                }
                if (editor_toggler_)
                {
                    editor_toggler_->stopTimer();
                    editor_toggler_.reset();
                }
                std::fprintf(stderr,
                    "probe: %d iterations completed, "
                    "%d editor-thread writes, "
                    "%d worker-thread writes\n",
                    completed, editorWrites, workerTweakCount_.load());
                renderBtn_.setEnabled(true);
                renderBtn_.setButtonText(ok
                    ? juce::String("Probe OK")
                    : juce::String("Probe failed: " + error));
                auto* app = juce::JUCEApplication::getInstance();
                if (app)
                {
                    app->setApplicationReturnValue(ok ? 0 : 1);
                    app->systemRequestedQuit();
                }
            });
        }).detach();
    }

    EditorOptions                               opts_;
    MH_Plugin*                                  plugin_ = nullptr;
    juce::AudioProcessor*                       proc_   = nullptr;
    std::function<void(EditorWindow*)>          on_close_;
    std::unique_ptr<juce::AudioProcessorEditor> editor_;
    std::unique_ptr<juce::Component>            placeholder_;
    juce::TextButton                            renderBtn_;
    std::atomic<bool>                           renderBusy_{ false };
    std::unique_ptr<ParamTweakTimer>            tweaker_;
    std::unique_ptr<juce::Timer>                editor_toggler_;
    std::thread                                 workerTweaker_;
    std::atomic<bool>                           workerTweakStop_{ true };
    std::atomic<int>                            workerTweakCount_{ 0 };
};

// -------------------------------------------------------------------- //
// Plugin loading helper -- shared by MainWindow's File > Open Plugin   //
// and by the command-line single-plugin shortcut.                      //
// -------------------------------------------------------------------- //

struct LoadedPlugin {
    MH_Plugin*            plugin = nullptr;
    juce::AudioProcessor* proc   = nullptr;
    juce::String          error;
};

// Load a plugin for a standalone editor. When descriptor_xml is non-empty it
// opens via mh_open_desc (AudioUnits, which have no file path); otherwise by
// path via mh_open.
static LoadedPlugin loadPlugin(const juce::String& path,
                               const juce::String& descriptor_xml = {})
{
    LoadedPlugin out;
    char err[512] = {0};
    out.plugin = descriptor_xml.isNotEmpty()
        ? mh_open_desc(descriptor_xml.toRawUTF8(), kSampleRate, kBlockSize,
                       kChannels, kChannels, err, sizeof(err))
        : mh_open(path.toRawUTF8(), kSampleRate, kBlockSize,
                  kChannels, kChannels, err, sizeof(err));
    if (!out.plugin)
    {
        out.error = juce::String(descriptor_xml.isNotEmpty()
                                     ? "mh_open_desc failed: "
                                     : "mh_open failed: ")
                  + juce::String(static_cast<const char*>(err));
        return out;
    }
    out.proc = static_cast<juce::AudioProcessor*>(
        mh_get_juce_processor(out.plugin));
    if (!out.proc)
    {
        mh_close(out.plugin);
        out.plugin = nullptr;
        out.error = "mh_get_juce_processor returned NULL";
    }
    return out;
}

// -------------------------------------------------------------------- //
// MainWindow                                                           //
// -------------------------------------------------------------------- //

// Empty shell with a File menu. closing it quits the app. Holds no
// plugin state itself -- plugin editors live in their own DocumentWindows.
class MainWindow : public juce::DocumentWindow,
                   public juce::MenuBarModel
{
public:
    MainWindow(std::function<void(juce::String)> on_open_plugin,
               std::function<void(juce::String)> on_open_project,
               std::function<void(juce::String)> on_render_project,
               std::function<void()>             on_save_project,
               std::function<void()>             on_save_project_as,
               std::function<void()>             on_new_project,
               std::function<void(int)>          on_open_canvas_plugin_editor,
               std::function<void()>             on_audio_settings,
               std::function<void()>             on_midi_input,
               std::function<void()>             on_start_live,
               std::function<void()>             on_stop_live,
               std::function<void()>             on_transport_play,
               std::function<void()>             on_transport_stop,
               std::function<void()>             on_set_bpm,
               std::function<void()>             on_set_loop)
        : juce::DocumentWindow("minihost",
                               juce::Colours::darkgrey,
                               juce::DocumentWindow::allButtons),
          on_open_plugin_(std::move(on_open_plugin)),
          on_open_project_(std::move(on_open_project)),
          on_render_project_(std::move(on_render_project)),
          on_save_project_(std::move(on_save_project)),
          on_save_project_as_(std::move(on_save_project_as)),
          on_new_project_(std::move(on_new_project)),
          on_open_canvas_plugin_editor_(std::move(on_open_canvas_plugin_editor)),
          on_audio_settings_(std::move(on_audio_settings)),
          on_midi_input_(std::move(on_midi_input)),
          on_start_live_(std::move(on_start_live)),
          on_stop_live_(std::move(on_stop_live)),
          on_transport_play_(std::move(on_transport_play)),
          on_transport_stop_(std::move(on_transport_stop)),
          on_set_bpm_(std::move(on_set_bpm)),
          on_set_loop_(std::move(on_set_loop))
    {
        setUsingNativeTitleBar(true);
        setResizable(/*shouldBeResizable=*/true,
                     /*useBottomRightCornerResizer=*/false);
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(this);
       #else
        setMenuBar(this);
       #endif

        welcomeLabel_.setText(
            "No project loaded.\n\n"
            "File > New Project        start an empty canvas.\n"
            "File > Open Project...    load an existing project.json.\n"
            "File > Open Plugin...     host a single plugin's editor.\n"
            "File > Render Project...  render a project.json to disk.\n\n"
            "Once a project is open, right-click the canvas to add nodes;\n"
            "drag from an output port to an input port to connect them.",
            juce::dontSendNotification);
        welcomeLabel_.setJustificationType(juce::Justification::centred);
        welcomeLabel_.setColour(juce::Label::textColourId,
                                juce::Colours::lightgrey);

        // setContentNonOwned: welcomeLabel_ is a MEMBER, not a heap
        // allocation, so we must keep ownership. setContentOwned would
        // make JUCE call `delete &welcomeLabel_` when the content
        // swaps (e.g. on Open Project / New Project), corrupting the
        // heap.
        setContentNonOwned(&welcomeLabel_, /*resizeToFit=*/false);
        setSize(640, 360);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    // Swap the content area to the canvas displaying `doc`. `doc` must
    // outlive the MainWindow's display of it; we keep a copy in
    // current_doc_ to guarantee that. An empty `project_path`
    // signals an untitled (in-memory only) project.
    void showProject(project::ProjectDocument doc,
                     const juce::File& project_path)
    {
        current_doc_     = std::make_unique<project::ProjectDocument>(std::move(doc));
        current_project_ = project_path;
        has_open_project_ = true;

        if (!canvas_)
        {
            canvas_ = std::make_unique<CanvasComponent>();
            canvas_->setOnOpenPluginEditor(
                [this](int plugin_index) {
                    if (on_open_canvas_plugin_editor_)
                        on_open_canvas_plugin_editor_(plugin_index);
                });
            // Refresh the Edit menu's Undo/Redo enabled state whenever
            // the document is edited or an undo/redo is applied.
            canvas_->setOnDocumentEdited([this]() { menuItemsChanged(); });
            if (on_add_plugin_requested_)
                canvas_->setOnAddPluginRequested(on_add_plugin_requested_);
        }
        canvas_->setDocument(current_doc_.get());
        // setContentNonOwned releases the previous content (welcomeLabel)
        // without deleting it (we still own it).
        setContentNonOwned(canvas_.get(), /*resizeToFit=*/false);
        setName("minihost: "
                + (project_path == juce::File()
                       ? juce::String("(untitled)")
                       : project_path.getFileName()));
        setSize(std::max(800, getWidth()), std::max(500, getHeight()));
        menuItemsChanged();
    }

    // Called after Save As assigns a path to a previously-untitled
    // document. Updates the stored path + window title without
    // touching the canvas state.
    void retitleAfterSaveAs(const juce::File& project_path)
    {
        current_project_ = project_path;
        setName("minihost: " + project_path.getFileName());
        menuItemsChanged();
    }

    project::ProjectDocument* currentDocument() noexcept
    { return current_doc_.get(); }
    const juce::File& currentProjectPath() const noexcept
    { return current_project_; }
    CanvasComponent* canvas() noexcept { return canvas_.get(); }

    // Plugin-library callbacks are set after construction (the ctor
    // already carries many positional callbacks; setters keep the new
    // ones from adding to that list). Both may be null.
    void setOnPluginBrowser(std::function<void()> cb)
    { on_plugin_browser_ = std::move(cb); }
    void setOnAddLibraryPlugin(std::function<void()> cb)
    { on_add_library_plugin_ = std::move(cb); }

    // Canvas "Add Plugin..." delegates here (app shows its shared picker).
    // Stored so it can be attached to the canvas whenever it is created.
    void setOnAddPluginRequested(std::function<void()> cb)
    {
        on_add_plugin_requested_ = std::move(cb);
        if (canvas_)
            canvas_->setOnAddPluginRequested(on_add_plugin_requested_);
    }
    // File > Open Plugin... delegates here when set (app shows its shared
    // picker, then hosts the pick in a standalone EditorWindow). When
    // unset, showOpenPluginChooser's raw file chooser is used as a fallback.
    void setOnRequestOpenPlugin(std::function<void()> cb)
    { on_request_open_plugin_ = std::move(cb); }

    ~MainWindow() override
    {
       #if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(nullptr);
       #else
        setMenuBar(nullptr);
       #endif
    }

    void closeButtonPressed() override
    {
        if (auto* app = juce::JUCEApplication::getInstance())
            app->systemRequestedQuit();
    }

    // ----- juce::MenuBarModel ----- //
    juce::StringArray getMenuBarNames() override
    {
        return { "File", "Edit", "Plugins", "Audio", "Help" };
    }

    juce::PopupMenu getMenuForIndex(int /*topLevelMenuIndex*/,
                                    const juce::String& menuName) override
    {
        juce::PopupMenu m;
        if (menuName == "File")
        {
            m.addItem(kMenuNewProject,    "New Project");
            m.addItem(kMenuOpenProject,   "Open Project...");
            m.addItem(kMenuSaveProject,   "Save Project",
                      /*isActive=*/has_open_project_);
            m.addItem(kMenuSaveProjectAs, "Save Project As...",
                      /*isActive=*/has_open_project_);
            m.addSeparator();
            m.addItem(kMenuOpenPlugin,    "Open Plugin...");
            m.addItem(kMenuRenderProject, "Render Project...");
            m.addSeparator();
            m.addItem(kMenuQuit, "Quit");
        }
        else if (menuName == "Edit")
        {
            const bool can_undo = canvas_ != nullptr && canvas_->canUndo();
            const bool can_redo = canvas_ != nullptr && canvas_->canRedo();
            m.addItem(kMenuUndo, "Undo    Cmd+Z",       /*isActive=*/can_undo);
            m.addItem(kMenuRedo, "Redo    Cmd+Shift+Z", /*isActive=*/can_redo);
        }
        else if (menuName == "Plugins")
        {
            m.addItem(kMenuPluginBrowser, "Plugin Browser / Scan...");
            m.addItem(kMenuAddLibraryPlugin, "Add Plugin from Library...",
                      /*isActive=*/has_open_project_);
        }
        else if (menuName == "Audio")
        {
            m.addItem(kMenuAudioSettings, "Audio Device Settings...");
            m.addItem(kMenuMidiInput,     "MIDI Input...");
            m.addSeparator();
            m.addItem(kMenuStartLive,
                      live_running_ ? "Restart Live"
                                    : "Start Live",
                      /*isActive=*/has_open_project_);
            m.addItem(kMenuStopLive, "Stop Live",
                      /*isActive=*/live_running_);
            m.addSeparator();
            m.addItem(kMenuTransportPlay, "Transport: Play",
                      /*isActive=*/live_running_);
            m.addItem(kMenuTransportStop, "Transport: Stop",
                      /*isActive=*/live_running_);
            m.addItem(kMenuSetBpm, "Set BPM...",
                      /*isActive=*/live_running_);
            m.addItem(kMenuSetLoop, "Set Loop Region...",
                      /*isActive=*/live_running_);
        }
        else if (menuName == "Help")
        {
            m.addItem(kMenuAbout, "About minihost");
        }
        return m;
    }

    void menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/) override
    {
        switch (menuItemID)
        {
        case kMenuOpenPlugin:
            if (on_request_open_plugin_) on_request_open_plugin_();
            else                         showOpenPluginChooser();
            break;
        case kMenuOpenProject:   showOpenProjectChooser(); break;
        case kMenuNewProject:
            if (on_new_project_) on_new_project_();
            break;
        case kMenuSaveProject:
            if (on_save_project_) on_save_project_();
            break;
        case kMenuSaveProjectAs:
            if (on_save_project_as_) on_save_project_as_();
            break;
        case kMenuRenderProject: showRenderProjectChooser(); break;
        case kMenuUndo: if (canvas_ != nullptr) canvas_->undo(); break;
        case kMenuRedo: if (canvas_ != nullptr) canvas_->redo(); break;
        case kMenuPluginBrowser:
            if (on_plugin_browser_) on_plugin_browser_(); break;
        case kMenuAddLibraryPlugin:
            if (on_add_library_plugin_) on_add_library_plugin_(); break;
        case kMenuAudioSettings: if (on_audio_settings_) on_audio_settings_(); break;
        case kMenuMidiInput:     if (on_midi_input_)     on_midi_input_();     break;
        case kMenuStartLive:     if (on_start_live_)     on_start_live_();     break;
        case kMenuStopLive:      if (on_stop_live_)      on_stop_live_();      break;
        case kMenuTransportPlay: if (on_transport_play_) on_transport_play_(); break;
        case kMenuTransportStop: if (on_transport_stop_) on_transport_stop_(); break;
        case kMenuSetBpm:        if (on_set_bpm_)        on_set_bpm_();        break;
        case kMenuSetLoop:       if (on_set_loop_)       on_set_loop_();       break;
        case kMenuQuit:
            if (auto* app = juce::JUCEApplication::getInstance())
                app->systemRequestedQuit();
            break;
        case kMenuAbout: showAboutDialog(); break;
        default: break;
        }
    }

private:
    enum MenuId {
        kMenuOpenPlugin = 1,
        kMenuNewProject,
        kMenuOpenProject,
        kMenuSaveProject,
        kMenuSaveProjectAs,
        kMenuRenderProject,
        kMenuUndo,
        kMenuRedo,
        kMenuPluginBrowser,
        kMenuAddLibraryPlugin,
        kMenuAudioSettings,
        kMenuMidiInput,
        kMenuStartLive,
        kMenuStopLive,
        kMenuTransportPlay,
        kMenuTransportStop,
        kMenuSetBpm,
        kMenuSetLoop,
        kMenuQuit,
        kMenuAbout,
    };

    void showOpenPluginChooser()
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Choose a plugin",
            juce::File("/Library/Audio/Plug-Ins"),
            "*.vst3;*.component;*.lv2");

        const int flags = juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::canSelectDirectories;
        chooser_->launchAsync(flags,
            [this](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (file == juce::File()) return;
                if (on_open_plugin_) on_open_plugin_(file.getFullPathName());
            });
    }

    void showOpenProjectChooser()
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Choose a project JSON file",
            juce::File(),
            "*.json");
        const int flags = juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles;
        chooser_->launchAsync(flags,
            [this](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (file == juce::File()) return;
                if (on_open_project_) on_open_project_(file.getFullPathName());
            });
    }

    void showRenderProjectChooser()
    {
        chooser_ = std::make_unique<juce::FileChooser>(
            "Choose a project JSON file",
            juce::File(),
            "*.json");
        const int flags = juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles;
        chooser_->launchAsync(flags,
            [this](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (file == juce::File()) return;
                if (on_render_project_) on_render_project_(file.getFullPathName());
            });
    }

    void showAboutDialog()
    {
        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            "minihost",
            "minihost desktop (Phase 1 shell)\n\n"
            "File > Open Plugin... hosts a single plugin in its own window.\n"
            "Right-click the canvas > Add Plugin... adds one to the graph.\n"
            "Both show your scanned plugin library (with a Browse-to-file "
            "fallback); populate it via Plugins > Plugin Browser / Scan...");
    }

    std::function<void(juce::String)>  on_open_plugin_;
    std::function<void(juce::String)>  on_open_project_;
    std::function<void(juce::String)>  on_render_project_;
    std::function<void()>              on_save_project_;
    std::function<void()>              on_save_project_as_;
    std::function<void()>              on_new_project_;
    std::function<void(int)>           on_open_canvas_plugin_editor_;
    std::function<void()>              on_audio_settings_;
    std::function<void()>              on_midi_input_;
    std::function<void()>              on_start_live_;
    std::function<void()>              on_stop_live_;
    std::function<void()>              on_transport_play_;
    std::function<void()>              on_transport_stop_;
    std::function<void()>              on_set_bpm_;
    std::function<void()>              on_set_loop_;
    std::function<void()>              on_plugin_browser_;
    std::function<void()>              on_add_library_plugin_;
    std::function<void()>              on_add_plugin_requested_;
    std::function<void()>              on_request_open_plugin_;
    juce::Label                        welcomeLabel_;
    std::unique_ptr<juce::FileChooser> chooser_;
    std::unique_ptr<CanvasComponent>   canvas_;
    std::unique_ptr<project::ProjectDocument> current_doc_;
    juce::File                         current_project_;
    bool                               has_open_project_ = false;

public:
    bool                               live_running_     = false;
    void setLiveRunning(bool r) { live_running_ = r; menuItemsChanged(); }
};

// -------------------------------------------------------------------- //
// ProjectRenderJob: juce::ThreadWithProgressWindow wrapping the C++   //
// project renderer. Modal progress bar with a Cancel button; the      //
// thread sets the cancel flag on cancellation.                         //
// -------------------------------------------------------------------- //

// Self-deleting render job. Construct on the heap; call launchThread()
// to spawn the worker and show the modal progress window. When the
// thread finishes, threadComplete fires on the message thread, posts
// a success/failure alert, and deletes the job.
class ProjectRenderJob : public juce::ThreadWithProgressWindow
{
public:
    static void launch(juce::File project_file,
                       project::RenderOptions options = {})
    {
        // Lifetime: deletes itself in threadComplete().
        auto* job = new ProjectRenderJob(std::move(project_file),
                                         options);
        job->launchThread();
    }

    void run() override
    {
        try {
            auto loaded = project::loadProject(project_file_);
            setStatusMessage("Rendering...");

            std::atomic<bool> cancel{ false };
            juce::String err;
            const int total = loaded->duration_frames;
            const bool ok = project::renderProject(
                *loaded, cancel,
                [this, total, &cancel](int done, int /*tot*/) {
                    if (threadShouldExit()) cancel.store(true);
                    setProgress(total > 0
                                ? (double) done / (double) total
                                : 0.0);
                },
                err, options_);
            if (!ok)
            {
                error_ = (err == "cancelled")
                       ? juce::String("Render cancelled.") : err;
                return;
            }
            for (const auto& on : loaded->doc.outputs)
                written_sinks_.emplace_back(on.id, on.sink);
            succeeded_ = true;
        } catch (const std::exception& e) {
            error_ = juce::String(e.what());
        }
    }

    void threadComplete(bool /*userPressedCancel*/) override
    {
        if (succeeded_)
        {
            juce::String body = "Rendered:\n";
            for (const auto& kv : written_sinks_)
                body += "  " + kv.first + "  ->  "
                     + kv.second.getFullPathName() + "\n";
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "Render complete", body);
        }
        else
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Render failed",
                error_);
        }
        delete this;
    }

private:
    ProjectRenderJob(juce::File project_file,
                     project::RenderOptions options)
        : juce::ThreadWithProgressWindow(
              juce::String("Rendering ") + project_file.getFileName(),
              /*hasProgressBar=*/true,
              /*hasCancelButton=*/true),
          project_file_(std::move(project_file)),
          options_(options)
    {
        setStatusMessage("Loading project...");
    }

    juce::File              project_file_;
    project::RenderOptions  options_;
    bool                    succeeded_ = false;
    juce::String            error_;
    std::vector<std::pair<juce::String, juce::File>> written_sinks_;
};

// -------------------------------------------------------------------- //
// DesktopApplication                                                   //
// -------------------------------------------------------------------- //

class DesktopApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "minihost"; }
    const juce::String getApplicationVersion() override { return "0.0.0"; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise(const juce::String& cmdLine) override
    {
        // This is a JUCE app: it already owns the process's MessageManager on
        // the main thread. libminihost otherwise spins up its OWN message
        // thread on the first mh_open (for headless callers -- Python/CLI --
        // that have none). In this process that second thread is redundant:
        // it never becomes the real message thread, so plugin construction is
        // no more/less on-the-message-thread with it than without, and at
        // process exit it is left joinable, which std::terminate()s (the
        // "aborts on exit after a plugin load" symptom). Disable it here,
        // before any mh_open, so construction runs inline on our own threads
        // and teardown is clean. Respect an explicit override if the user set
        // one. Must precede the scan-worker relaunch so children inherit it.
        setenv("MINIHOST_MESSAGE_THREAD", "0", /*overwrite=*/0);

        // Plugin-scan worker detection MUST come first: when the parent's
        // out-of-process scanner launches a child, it relaunches this same
        // binary with the scanner UID token. If that's us, become the worker
        // (no GUI, no other modes) and return. Any other command line makes
        // initialiseFromCommandLine return false and falls through unchanged.
        {
            auto worker = std::make_unique<PluginScannerSubprocess>();
            if (worker->initialiseFromCommandLine(cmdLine, kScannerProcessUID))
            {
                scanner_subprocess_ = std::move(worker);
                return;
            }
        }

        auto args = juce::ArgumentList("minihost_desktop", cmdLine);

        // Headless project render: short-circuits all GUI setup.
        if (args.containsOption("--render-project"))
        {
            const auto path = args.removeValueForOption("--render-project");
            if (path.isEmpty())
            {
                std::fprintf(stderr,
                    "--render-project requires a path\n");
                setApplicationReturnValue(2);
                quit();
                return;
            }
            runHeadlessProjectRender(juce::File(path));
            return;
        }

        // Headless save round-trip: parse, immediately save to
        // <input>.resaved.json. Used to verify the C++ saveProjectFile
        // path against the Python parser without needing UI input.
        if (args.containsOption("--save-roundtrip"))
        {
            const auto path = args.removeValueForOption("--save-roundtrip");
            if (path.isEmpty())
            {
                std::fprintf(stderr,
                    "--save-roundtrip requires a path\n");
                setApplicationReturnValue(2);
                quit();
                return;
            }
            runSaveRoundtrip(juce::File(path));
            return;
        }

        // Headless undo/redo self-test: exercises the UndoHistory core +
        // ProjectDocument snapshot/restore through the production
        // serializer, with no GUI. Exit 0 on success, 1 on any failed
        // invariant. Driven by tests/test_desktop_undo.py.
        if (args.containsOption("--undo-selftest"))
        {
            const auto path = args.removeValueForOption("--undo-selftest");
            if (path.isEmpty())
            {
                std::fprintf(stderr, "--undo-selftest requires a path\n");
                setApplicationReturnValue(2);
                quit();
                return;
            }
            runUndoSelfTest(juce::File(path));
            return;
        }

        // Headless plugin scan: register the host formats, scan the given
        // (or default) directories in-process, and write the resulting
        // KnownPluginList to XML. Proves addDefaultFormatsToManager yields
        // working scanners and seeds the library from the CLI. Exit 0 on a
        // completed scan (even if it found nothing), 2 on a bad argument.
        if (args.containsOption("--scan-plugins"))
        {
            const auto paths   = args.removeValueForOption("--scan-plugins");
            const auto out_arg = args.containsOption("--scan-out")
                               ? args.removeValueForOption("--scan-out")
                               : juce::String();
            const auto fmt_arg = args.containsOption("--scan-format")
                               ? args.removeValueForOption("--scan-format")
                               : juce::String();
            const bool oop = args.removeOptionIfFound("--scan-oop");
            runPluginScan(paths, out_arg, fmt_arg, oop);
            return;
        }

        EditorOptions cli{};
        cli.output_path = juce::String(kDefaultOut);

        if (args.removeOptionIfFound("--auto-render"))
            cli.mode = EditorMode::AutoRenderAndQuit;
        if (args.removeOptionIfFound("--probe"))
            cli.mode = EditorMode::ProbeAndQuit;
        if (args.containsOption("--iterations"))
        {
            cli.probe_iters =
                args.removeValueForOption("--iterations").getIntValue();
            if (cli.probe_iters < 1) cli.probe_iters = 1;
        }

        const bool has_plugin_arg = args.arguments.size() >= 1;
        if (has_plugin_arg)
        {
            cli.plugin_path = args.arguments[0].text;
            if (args.arguments.size() >= 2)
                cli.output_path = args.arguments[1].text;
        }

        // The --auto-render / --probe modes both require a plugin
        // path on the command line (they're non-interactive).
        if (cli.mode != EditorMode::Interactive && !has_plugin_arg)
        {
            std::fprintf(stderr,
                "usage: minihost_desktop "
                "[--auto-render | --probe [--iterations=N]] "
                "<plugin_path> [output.wav]\n");
            setApplicationReturnValue(2);
            quit();
            return;
        }

        if (cli.mode == EditorMode::ProbeAndQuit)
            startWatchdog(15 * cli.probe_iters + 30);

        // Restore audio device / MIDI input choice from prior runs.
        // Headless modes (auto-render / probe / single-plugin shortcut)
        // don't open a MainWindow but still benefit from the load
        // (cheap; no callback attached without Start Live).
        loadSettingsFromDisk();

        if (has_plugin_arg)
        {
            // Single-plugin shortcut: skip the MainWindow.
            openPluginEditor(cli);
        }
        else
        {
            // App shell: empty MainWindow waiting for File > Open Plugin.
            mainWindow_ = std::make_unique<MainWindow>(
                [this](juce::String path) {
                    EditorOptions opts{};
                    opts.plugin_path = path;
                    opts.output_path = juce::String(kDefaultOut);
                    opts.mode        = EditorMode::Interactive;
                    openPluginEditor(opts);
                },
                [this](juce::String project_path) {
                    openProjectInCanvas(juce::File(project_path));
                },
                [this](juce::String project_path) {
                    runProjectRender(juce::File(project_path));
                },
                [this]() { saveCurrentProject(); },
                [this]() { saveCurrentProjectAs(); },
                [this]() { newProject(); },
                [this](int plugin_index) {
                    openCanvasPluginEditor(plugin_index);
                },
                [this]() { showAudioSettings(); },
                [this]() { showMidiInputDialog(); },
                [this]() { startLive(); },
                [this]() { stopLive(); },
                [this]() { if (live_) live_->setTransportPlaying(true); },
                [this]() { if (live_) live_->setTransportPlaying(false); },
                [this]() { showBpmDialog(); },
                [this]() { showLoopDialog(); });
            mainWindow_->setOnPluginBrowser([this]() { showPluginBrowser(); });
            mainWindow_->setOnAddLibraryPlugin(
                [this]() { showAddLibraryPluginMenu(); });

            // Route the canvas "Add Plugin..." and File > Open Plugin...
            // through the shared library picker (scanned list + browse +
            // scan). Add Plugin adds a node; Open Plugin hosts the pick in
            // a standalone editor window.
            mainWindow_->setOnAddPluginRequested([this]() {
                if (mainWindow_ == nullptr) return;
                auto* canvas = mainWindow_->canvas();
                if (canvas == nullptr) return;
                showPluginPicker([canvas](const juce::PluginDescription& pd) {
                    canvas->addPluginFromDescription(pd);
                });
            });
            mainWindow_->setOnRequestOpenPlugin([this]() {
                showPluginPicker([this](const juce::PluginDescription& pd) {
                    openPluginEditorFromDescription(pd);
                });
            });

            // Plugin library: register the host formats and restore the
            // previously-scanned plugin list. Scanning is user-driven
            // via the Plugin Browser; nothing is scanned at startup.
            // (JUCE 8.0.11 moved AudioPluginFormatManager to the headless
            // module and deleted its addDefaultFormats(); the non-headless
            // free function registers the real hosting formats.)
            juce::addDefaultFormatsToManager(plugin_format_manager_);
            // Scan plugins out-of-process so a plugin that crashes or calls
            // exit() during instantiation-during-scan takes down only a
            // disposable child, not the app. Routes both the interactive
            // PluginListComponent scan and Add Plugin from Library probes.
            known_plugins_.setCustomScanner(
                std::make_unique<OutOfProcessPluginScanner>());
            loadKnownPlugins();
            owns_library_ = true;   // this process persists the library
        }
    }

    void shutdown() override
    {
        if (live_) saveSettingsToDisk();
        // Only the GUI app-shell loads and owns known_plugins_; persist it
        // only there. Headless modes (--scan-plugins writes its own local
        // list) and spawned scan-worker children have an empty member list
        // and must NOT write it back, or they clobber a freshly-scanned
        // library with an empty one.
        if (owns_library_) saveKnownPlugins();
        plugin_browser_window_.reset();
        if (live_) live_->stop();
        live_.reset();
        editors_.clear();   // closes EditorWindows; each mh_close in dtor
        mainWindow_.reset();
    }

    // Settings live next to the app data dir. MINIHOST_DESKTOP_SETTINGS_DIR
    // overrides the base directory (used by tests for hermetic isolation --
    // macOS resolves userApplicationDataDirectory via the native API, which
    // ignores $HOME, so an env override is the only way to redirect it).
    juce::File settingsFile() const
    {
        if (const auto* env = std::getenv("MINIHOST_DESKTOP_SETTINGS_DIR");
            env != nullptr && env[0] != '\0')
            return juce::File(juce::String::fromUTF8(env))
                   .getChildFile("desktop_settings.xml");
        return juce::File::getSpecialLocation(
                   juce::File::userApplicationDataDirectory)
               .getChildFile("minihost")
               .getChildFile("desktop_settings.xml");
    }

    void loadSettingsFromDisk()
    {
        const auto path = settingsFile();
        if (!path.existsAsFile()) return;
        const auto xml = path.loadFileAsString();
        if (xml.isEmpty()) return;
        ensureLiveEngine().loadSettingsFromXml(xml);
    }

    void saveSettingsToDisk()
    {
        if (!live_) return;
        const auto xml = live_->saveSettingsAsXml();
        const auto path = settingsFile();
        path.getParentDirectory().createDirectory();
        path.replaceWithText(xml);
    }

    // The scanned plugin list persists in its own file next to the
    // device settings so a scan survives restarts.
    juce::File knownPluginsFile() const
    {
        return settingsFile().getSiblingFile("known_plugins.xml");
    }

    void loadKnownPlugins()
    {
        const auto path = knownPluginsFile();
        if (!path.existsAsFile()) return;
        if (auto xml = juce::XmlDocument::parse(path))
            known_plugins_.recreateFromXml(*xml);
    }

    void saveKnownPlugins()
    {
        if (auto xml = std::unique_ptr<juce::XmlElement>(
                known_plugins_.createXml()))
        {
            const auto path = knownPluginsFile();
            path.getParentDirectory().createDirectory();
            xml->writeTo(path);
        }
    }

    // The scan/manage UI. juce::PluginListComponent handles scan-path
    // configuration, scanning (in-process; see the crash-resilience note
    // in docs/dev/desktop_app.md), and rescans, persisting into
    // known_plugins_. A dead-man's-pedal file lets a scan that crashed on
    // a bad plugin skip it on the next attempt.
    void showPluginBrowser()
    {
        if (plugin_browser_window_ != nullptr)
        {
            plugin_browser_window_->toFront(true);
            return;
        }
        const auto dead_mans_pedal =
            settingsFile().getSiblingFile("plugin_scan.deadman");
        auto* list_comp = new juce::PluginListComponent(
            plugin_format_manager_, known_plugins_, dead_mans_pedal,
            /*propertiesToUse=*/nullptr,
            /*allowAsync=*/true);
        list_comp->setSize(700, 480);

        juce::DialogWindow::LaunchOptions opts;
        opts.content.setOwned(list_comp);
        opts.dialogTitle              = "Plugin Browser";
        opts.dialogBackgroundColour   = juce::Colours::darkgrey;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar        = true;
        opts.resizable                = true;

        plugin_browser_window_.reset(opts.launchAsync());
    }

    // Shared plugin picker used by every "add / open a plugin" action.
    // Presents the scanned library list first, then a file-browse fallback
    // and a shortcut into the scanner. `on_chosen` receives the full
    // juce::PluginDescription of the pick -- consumers decide path vs
    // descriptor loading from it (AudioUnits have no file path and load via
    // their descriptor; VST3/LV2 keep loading by path). The browse fallback
    // synthesizes a description whose fileOrIdentifier is the chosen path.
    //
    // Menu IDs: the library items are based at KnownPluginList's
    // menuIdBase (0x324503f4), so the small custom IDs below never collide,
    // and they are checked before getIndexChosenByMenu (which returns -1
    // for them anyway).
    void showPluginPicker(std::function<void(const juce::PluginDescription&)> on_chosen)
    {
        enum { kPickBrowse = 1, kPickScan = 2 };

        const auto types = known_plugins_.getTypes();

        juce::PopupMenu m;
        if (types.isEmpty())
        {
            m.addItem(-1, "(no plugins scanned)", /*isActive=*/false);
        }
        else
        {
            // Static helper (the instance version is deprecated): snapshot
            // the types once and reuse the same array for the index lookup.
            juce::KnownPluginList::addToMenu(
                m, types, juce::KnownPluginList::sortAlphabetically);
        }
        m.addSeparator();
        m.addItem(kPickBrowse, "Browse to file...");
        m.addItem(kPickScan,   "Scan for plugins...");

        m.showMenuAsync(juce::PopupMenu::Options{},
            [this, types, on_chosen = std::move(on_chosen)](int chosen) {
                if (chosen == 0) return;   // dismissed
                if (chosen == kPickBrowse)
                {
                    showPluginBrowseChooser(on_chosen);
                    return;
                }
                if (chosen == kPickScan)
                {
                    showPluginBrowser();
                    return;
                }
                const int idx =
                    juce::KnownPluginList::getIndexChosenByMenu(types, chosen);
                if (idx < 0 || idx >= types.size()) return;
                on_chosen(types[idx]);
            });
    }

    // The "Browse to file..." fallback: a raw file chooser feeding the same
    // completion as a library pick. Synthesizes a description carrying the
    // chosen path so consumers take the path route. Owns plugin_chooser_ so
    // it outlives the async callback.
    void showPluginBrowseChooser(
        std::function<void(const juce::PluginDescription&)> on_chosen)
    {
        plugin_chooser_ = std::make_unique<juce::FileChooser>(
            "Choose a plugin",
            juce::File("/Library/Audio/Plug-Ins"),
            "*.vst3;*.component;*.lv2");
        const int flags = juce::FileBrowserComponent::openMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::canSelectDirectories;
        plugin_chooser_->launchAsync(flags,
            [on_chosen = std::move(on_chosen)](const juce::FileChooser& fc) {
                const auto file = fc.getResult();
                if (file == juce::File()) return;
                juce::PluginDescription pd;
                pd.fileOrIdentifier = file.getFullPathName();
                on_chosen(pd);
            });
    }

    // Back-compat entry point for Plugins > Add Plugin from Library...:
    // routes through the shared picker, adding the pick to the canvas.
    void showAddLibraryPluginMenu()
    {
        if (mainWindow_ == nullptr) return;
        auto* canvas = mainWindow_->canvas();
        if (canvas == nullptr) return;
        showPluginPicker([canvas](const juce::PluginDescription& pd) {
            canvas->addPluginFromDescription(pd);
        });
    }

private:
    // Open a standalone editor for a library/browse pick. Real files
    // (VST3/LV2) open by path; AudioUnits (no file path) open via their
    // serialized descriptor.
    void openPluginEditorFromDescription(const juce::PluginDescription& pd)
    {
        EditorOptions opts{};
        opts.output_path = juce::String(kDefaultOut);
        opts.mode        = EditorMode::Interactive;
        if (juce::File(pd.fileOrIdentifier).exists())
            opts.plugin_path = pd.fileOrIdentifier;
        else if (auto xml = pd.createXml())
            opts.descriptor_xml = xml->toString();
        else
            opts.plugin_path = pd.fileOrIdentifier;
        openPluginEditor(opts);
    }

    void openPluginEditor(EditorOptions opts)
    {
        auto loaded = loadPlugin(opts.plugin_path, opts.descriptor_xml);
        if (loaded.plugin == nullptr)
        {
            std::fprintf(stderr, "%s\n", loaded.error.toRawUTF8());
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Failed to load plugin",
                loaded.error);
            // For non-interactive modes, also exit nonzero.
            if (opts.mode != EditorMode::Interactive)
            {
                setApplicationReturnValue(1);
                quit();
            }
            return;
        }

        auto* w = new EditorWindow(
            std::move(opts), loaded.plugin, loaded.proc,
            [this](EditorWindow* victim) {
                // Find and erase; ScopedPointer-like cleanup.
                for (int i = 0; i < editors_.size(); ++i)
                {
                    if (editors_[i] == victim)
                    {
                        editors_.remove(i);  // deletes the window
                        break;
                    }
                }
                // In single-plugin-shortcut mode (no MainWindow), the
                // last editor closing means the user is done.
                if (!mainWindow_ && editors_.isEmpty())
                {
                    if (auto* app = juce::JUCEApplication::getInstance())
                        app->systemRequestedQuit();
                }
            });
        editors_.add(w);
    }

    // Headless render: blocks the message thread (it's fine here -- no
    // window is open) and exits when done.
    void runHeadlessProjectRender(juce::File project_file)
    {
        std::atomic<bool> cancel{ false };
        int last_pct = -1;
        juce::String err;
        try {
            auto loaded = project::loadProject(project_file);
            std::fprintf(stderr,
                "loaded: %d input(s), %d output(s), %d plugin(s), "
                "duration=%d frames\n",
                (int) loaded->doc.inputs.size(),
                (int) loaded->doc.outputs.size(),
                (int) loaded->doc.plugins.size(),
                loaded->duration_frames);
            const bool ok = project::renderProject(*loaded, cancel,
                [&last_pct](int done, int tot) {
                    int pct = (int) (100.0 * done
                                     / std::max(1, tot));
                    if (pct >= last_pct + 10) {
                        std::fprintf(stderr, "  %d%%\n", pct);
                        last_pct = pct;
                    }
                },
                err);
            if (!ok)
            {
                std::fprintf(stderr,
                    "render failed: %s\n", err.toRawUTF8());
                setApplicationReturnValue(1);
                quit();
                return;
            }
            for (const auto& on : loaded->doc.outputs)
                std::fprintf(stderr, "wrote %s\n",
                             on.sink.getFullPathName().toRawUTF8());
            setApplicationReturnValue(0);
            quit();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "load failed: %s\n", e.what());
            setApplicationReturnValue(1);
            quit();
        }
    }

    void runProjectRender(juce::File project_file)
    {
        // Show an options dialog before launching the render. The
        // dialog is async (JUCE modal loops are disabled); it kicks
        // off ProjectRenderJob::launch on OK.
        auto* aw = new juce::AlertWindow(
            "Render options",
            "Configure render output:",
            juce::AlertWindow::QuestionIcon);
        aw->addComboBox("bit_depth", { "16", "24", "32" }, "Bit depth");
        aw->getComboBoxComponent("bit_depth")->setSelectedItemIndex(1);
        aw->addTextEditor("normalize", "0",
                          "Normalize to dBFS (0 = off, e.g. -1.0)");
        aw->addButton("Render", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        const juce::File pf = std::move(project_file);
        aw->enterModalState(true,
            juce::ModalCallbackFunction::create(
                [aw, pf](int result) {
                    if (result == 1)
                    {
                        project::RenderOptions opts;
                        const int idx
                            = aw->getComboBoxComponent("bit_depth")
                                  ->getSelectedItemIndex();
                        opts.bit_depth_override
                            = (idx == 0) ? 16 : (idx == 1) ? 24 : 32;
                        opts.normalize_dbfs
                            = aw->getTextEditorContents("normalize")
                                  .getDoubleValue();
                        ProjectRenderJob::launch(pf, opts);
                    }
                    delete aw;
                }),
            /*deleteWhenDismissed=*/false);
    }

    void runSaveRoundtrip(juce::File project_file)
    {
        try {
            auto doc = project::parseProjectFile(project_file);
            const auto out = project_file.getSiblingFile(
                project_file.getFileNameWithoutExtension() + ".resaved.json");
            project::saveProjectFile(out, doc);
            std::fprintf(stderr, "wrote %s\n",
                         out.getFullPathName().toRawUTF8());
            setApplicationReturnValue(0);
            quit();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "save roundtrip failed: %s\n", e.what());
            setApplicationReturnValue(1);
            quit();
        }
    }

    void runUndoSelfTest(juce::File project_file)
    {
        auto fail = [this](const juce::String& m) {
            std::fprintf(stderr, "undo-selftest FAIL: %s\n", m.toRawUTF8());
            setApplicationReturnValue(1);
            quit();
        };
        try {
            // Serialize a document through the production writer so the
            // comparison is byte-exact and independent of struct layout.
            auto serialize = [](const project::ProjectDocument& d)
                             -> juce::String {
                auto tmp = juce::File::createTempFile(".json");
                project::saveProjectFile(tmp, d);
                const auto s = tmp.loadFileAsString();
                tmp.deleteFile();
                return s;
            };

            project::ProjectDocument doc =
                project::parseProjectFile(project_file);
            const juce::String s0 = serialize(doc);

            UndoHistory hist;

            // Edit 1: a content-independent mutation the serializer sees.
            hist.snapshot(doc);
            doc.block_size += 1;
            const juce::String s1 = serialize(doc);
            if (s1 == s0)      return fail("mutation did not change output");
            if (!hist.canUndo()) return fail("canUndo false after snapshot");

            // Undo restores the pre-edit document exactly.
            if (!hist.undo(doc))        return fail("undo returned false");
            if (serialize(doc) != s0)   return fail("undo did not restore");
            if (!hist.canRedo())        return fail("canRedo false after undo");

            // Redo re-applies the edit exactly.
            if (!hist.redo(doc))        return fail("redo returned false");
            if (serialize(doc) != s1)   return fail("redo did not reapply");

            // A fresh edit clears the redo stack.
            hist.snapshot(doc);
            doc.block_size += 1;
            if (hist.canRedo())
                return fail("redo stack not cleared by new edit");

            std::fprintf(stderr, "undo-selftest OK\n");
            setApplicationReturnValue(0);
            quit();
        } catch (const std::exception& e) {
            fail(juce::String("exception: ") + e.what());
        }
    }

    // Headless in-process scan. `paths_override` is a ';'-separated list of
    // directories (empty = each format's default locations). `out_override`
    // is the XML destination (empty = the real known_plugins.xml).
    // `format_filter` restricts scanning to the one format whose name
    // matches (case-insensitive, e.g. "VST3"); empty scans every format.
    // Drives a juce::PluginDirectoryScanner per format to completion.
    //
    // Note: AudioUnit scanning enumerates the whole system component
    // registry and ignores `paths_override`, so a hermetic scan of a single
    // directory must also pass --scan-format=VST3 (or LV2) to avoid pulling
    // in every installed AU. `out_of_process` routes each probe through a
    // disposable child process (crash containment) via the same custom
    // scanner the interactive GUI uses.
    void runPluginScan(const juce::String& paths_override,
                       const juce::String& out_override,
                       const juce::String& format_filter,
                       bool out_of_process)
    {
        juce::AudioPluginFormatManager fm;
        juce::addDefaultFormatsToManager(fm);

        juce::KnownPluginList list;
        if (out_of_process)
            list.setCustomScanner(
                std::make_unique<OutOfProcessPluginScanner>());
        const auto dead_mans_pedal =
            settingsFile().getSiblingFile("plugin_scan.deadman");
        dead_mans_pedal.getParentDirectory().createDirectory();

        int total_found = 0, total_failed = 0;
        for (auto* format : fm.getFormats())
        {
            if (format == nullptr) continue;
            if (format_filter.isNotEmpty()
                && !format->getName().equalsIgnoreCase(format_filter))
                continue;

            juce::FileSearchPath search;
            if (paths_override.isNotEmpty())
                search = juce::FileSearchPath(paths_override);
            else
                search = format->getDefaultLocationsToSearch();

            juce::PluginDirectoryScanner scanner(
                list, *format, search, /*searchRecursively=*/true,
                dead_mans_pedal, /*allowAsync=*/false);

            const int before = list.getNumTypes();
            juce::String being_scanned;
            while (scanner.scanNextFile(/*dontRescanIfAlreadyInList=*/true,
                                        being_scanned))
            {
                // scanNextFile advances one plugin per call.
            }
            const int found = list.getNumTypes() - before;
            const int failed = scanner.getFailedFiles().size();
            total_found  += found;
            total_failed += failed;
            std::fprintf(stderr, "scan[%s]: found %d, failed %d\n",
                         format->getName().toRawUTF8(), found, failed);
        }

        const auto out = out_override.isNotEmpty()
                       ? juce::File(out_override)
                       : knownPluginsFile();
        if (auto xml = std::unique_ptr<juce::XmlElement>(list.createXml()))
        {
            out.getParentDirectory().createDirectory();
            if (xml->writeTo(out))
                std::fprintf(stderr, "scan: wrote %d plugins to %s\n",
                             total_found, out.getFullPathName().toRawUTF8());
            else
                std::fprintf(stderr, "scan: FAILED to write %s\n",
                             out.getFullPathName().toRawUTF8());
        }
        std::fprintf(stderr, "scan: total found %d, failed %d\n",
                     total_found, total_failed);
        setApplicationReturnValue(0);
        quit();
    }

    void saveCurrentProject()
    {
        if (!mainWindow_) return;
        auto* doc  = mainWindow_->currentDocument();
        const auto& path = mainWindow_->currentProjectPath();
        if (doc == nullptr) return;
        // Untitled (no path yet) -> fall through to Save As.
        if (path == juce::File()) { saveCurrentProjectAs(); return; }
        try {
            project::saveProjectFile(path, *doc);
            std::fprintf(stderr, "saved %s\n",
                         path.getFullPathName().toRawUTF8());
        } catch (const std::exception& e) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Failed to save project",
                e.what());
        }
    }

    void saveCurrentProjectAs()
    {
        if (!mainWindow_) return;
        auto* doc = mainWindow_->currentDocument();
        if (doc == nullptr) return;
        save_chooser_ = std::make_unique<juce::FileChooser>(
            "Save project as",
            mainWindow_->currentProjectPath() == juce::File()
                ? juce::File::getSpecialLocation(
                      juce::File::userDocumentsDirectory)
                  .getChildFile("untitled.json")
                : mainWindow_->currentProjectPath(),
            "*.json");
        const int flags = juce::FileBrowserComponent::saveMode
                        | juce::FileBrowserComponent::canSelectFiles
                        | juce::FileBrowserComponent::warnAboutOverwriting;
        save_chooser_->launchAsync(flags,
            [this](const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File()) return;
                if (!file.hasFileExtension("json"))
                    file = file.withFileExtension("json");
                auto* doc = mainWindow_->currentDocument();
                if (doc == nullptr) return;
                try {
                    project::saveProjectFile(file, *doc);
                    mainWindow_->retitleAfterSaveAs(file);
                    std::fprintf(stderr, "saved %s\n",
                                 file.getFullPathName().toRawUTF8());
                } catch (const std::exception& e) {
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon,
                        "Failed to save project",
                        e.what());
                }
            });
    }

    void newProject()
    {
        if (!mainWindow_) return;
        // Stop live mode if active -- the project it's running against
        // is about to be replaced.
        if (live_ && live_->isRunning())
        {
            live_->stop();
            mainWindow_->setLiveRunning(false);
            if (auto* c = mainWindow_->canvas())
                c->setLiveProject(nullptr);
        }
        project::ProjectDocument blank;
        blank.sample_rate = 48000;
        blank.block_size  = 512;
        mainWindow_->showProject(std::move(blank), juce::File());
    }

    // Opens a transient editor for a plugin referenced by the current
    // project. The plugin is freshly mh_open'd here (independent of any
    // render-time instance); state_b64 is restored on open if present.
    // "Capture State" reads mh_get_state, base64-encodes it, and
    // writes it back to doc.plugins[plugin_index].state_b64 so
    // File > Save Project persists the edit.
    void openCanvasPluginEditor(int plugin_index)
    {
        if (!mainWindow_) return;
        auto* doc = mainWindow_->currentDocument();
        if (doc == nullptr) return;
        if (plugin_index < 0
            || plugin_index >= (int) doc->plugins.size()) return;

        const auto& spec = doc->plugins[(size_t) plugin_index];
        // AU nodes carry a base64 descriptor and no usable path; decode it
        // so the editor loads via mh_open_desc.
        juce::String desc_xml;
        if (spec.descriptor.isNotEmpty())
        {
            juce::MemoryBlock mb;
            juce::MemoryOutputStream os(mb, false);
            if (juce::Base64::convertFromBase64(os, spec.descriptor))
                desc_xml = juce::String::fromUTF8(
                    static_cast<const char*>(mb.getData()), (int) mb.getSize());
        }
        auto loaded = loadPlugin(spec.path.getFullPathName(), desc_xml);
        if (loaded.plugin == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Failed to load plugin",
                loaded.error);
            return;
        }

        if (spec.state_b64.isNotEmpty())
        {
            juce::MemoryBlock mb;
            juce::MemoryOutputStream out(mb, false);
            if (juce::Base64::convertFromBase64(out, spec.state_b64))
                mh_set_state(loaded.plugin, mb.getData(),
                             (int) mb.getSize());
        }

        EditorOptions opts{};
        opts.plugin_path    = spec.path.getFullPathName();
        opts.descriptor_xml = desc_xml;
        opts.output_path    = juce::String(kDefaultOut);
        opts.mode           = EditorMode::Interactive;
        opts.toolbar_label  = "Capture State";

        // Captured by value: plugin_index is stable for this dialog;
        // even if the user reorders the document later, the lambda
        // re-resolves by id below.
        const juce::String node_id = spec.id;
        opts.toolbar_action = [this, node_id]() {
            captureStateForCanvasPlugin(node_id);
        };

        auto* w = new EditorWindow(
            std::move(opts), loaded.plugin, loaded.proc,
            [this](EditorWindow* victim) {
                for (int i = 0; i < editors_.size(); ++i)
                    if (editors_[i] == victim) {
                        editors_.remove(i);
                        break;
                    }
            });
        editors_.add(w);
    }

    // Re-resolves the plugin by id (the user may have edited the doc
    // while the editor was open) and writes the encoded state back.
    void captureStateForCanvasPlugin(juce::String node_id)
    {
        if (!mainWindow_) return;
        auto* doc = mainWindow_->currentDocument();
        if (doc == nullptr) return;

        // Find the EditorWindow currently associated with node_id by
        // matching its window title (set from the plugin file name on
        // construction). A more direct registry would be sturdier;
        // file name matches in practice because each canvas-opened
        // plugin has a distinct path. For now we re-lookup by id
        // and use that editor's plugin instance.
        EditorWindow* match = nullptr;
        int plugin_index = -1;
        for (size_t i = 0; i < doc->plugins.size(); ++i)
            if (doc->plugins[i].id == node_id) { plugin_index = (int) i; break; }
        if (plugin_index < 0) return;

        const auto& spec = doc->plugins[(size_t) plugin_index];
        const auto basename = spec.path.getFileName();
        for (auto* w : editors_)
            if (w->getName().endsWith(basename)) { match = w; break; }
        if (match == nullptr) return;

        MH_Plugin* p = match->plugin();
        if (p == nullptr) return;

        const int sz = mh_get_state_size(p);
        if (sz <= 0)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Capture failed",
                "Plugin reported zero state size.");
            return;
        }
        std::vector<unsigned char> buf((size_t) sz);
        const int got = mh_get_state(p, buf.data(), sz);
        if (got <= 0)
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Capture failed",
                "mh_get_state returned 0.");
            return;
        }
        juce::MemoryOutputStream encoded;
        juce::Base64::convertToBase64(encoded, buf.data(), (size_t) got);
        doc->plugins[(size_t) plugin_index].state_b64
            = encoded.toString();
        if (auto* cv = mainWindow_->canvas())
            cv->notifyDocumentChanged();
        std::fprintf(stderr,
            "captured %d bytes of state for plugin '%s'\n",
            got, node_id.toRawUTF8());
    }

    void openProjectInCanvas(juce::File project_file)
    {
        // Stop any live engine first -- switching projects mid-stream
        // would invalidate the audio thread's compiled graph.
        if (live_ && live_->isRunning())
        {
            live_->stop();
            if (mainWindow_)
            {
                mainWindow_->setLiveRunning(false);
                if (auto* c = mainWindow_->canvas())
                    c->setLiveProject(nullptr);
            }
        }
        try {
            auto doc = project::parseProjectFile(project_file);
            if (mainWindow_)
                mainWindow_->showProject(std::move(doc), project_file);
        } catch (const std::exception& e) {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Failed to open project",
                e.what());
        }
    }

    LiveEngine& ensureLiveEngine()
    {
        if (!live_) live_ = std::make_unique<LiveEngine>();
        return *live_;
    }

    void showAudioSettings()
    {
        auto& engine = ensureLiveEngine();
        auto* selector = new juce::AudioDeviceSelectorComponent(
            engine.deviceManager(),
            /*minInput=*/0,  /*maxInput=*/16,
            /*minOutput=*/2, /*maxOutput=*/16,
            /*showMidiInput=*/false,
            /*showMidiOutput=*/false,
            /*showChannelsAsStereoPairs=*/true,
            /*hideAdvancedOptions=*/false);
        selector->setSize(640, 480);

        juce::DialogWindow::LaunchOptions opts;
        opts.dialogTitle = "Audio Device Settings";
        opts.content.setOwned(selector);
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = true;
        opts.resizable = true;
        opts.launchAsync();
    }

    void startLive()
    {
        if (!mainWindow_) return;
        const auto& path = mainWindow_->currentProjectPath();
        if (path == juce::File())
        {
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::InfoIcon,
                "No project",
                "Open a project first (File > Open Project...).");
            return;
        }

        // Topology swap under mute: stop -> reload -> start.
        auto& engine = ensureLiveEngine();
        juce::String err;
        if (!engine.start(path, err))
        {
            mainWindow_->setLiveRunning(false);
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                "Could not start live mode",
                err);
            return;
        }
        mainWindow_->setLiveRunning(true);
        if (auto* c = mainWindow_->canvas())
            c->setLiveProject(engine.loadedProject());
        std::fprintf(stderr, "live: started\n");
    }

    void stopLive()
    {
        if (live_) live_->stop();
        if (mainWindow_)
        {
            mainWindow_->setLiveRunning(false);
            if (auto* c = mainWindow_->canvas())
                c->setLiveProject(nullptr);
        }
        std::fprintf(stderr, "live: stopped\n");
    }

    void showMidiInputDialog()
    {
        auto& engine = ensureLiveEngine();
        // Enumerate ports via libminihost (libremidi-backed). Names are
        // the persistence key passed to LiveEngine::setMidiInputDevice.
        juce::StringArray port_names;
        const int n_ports = mh_midi_get_num_inputs();
        char buf[256];
        for (int i = 0; i < n_ports; ++i)
            if (mh_midi_get_input_name(i, buf, sizeof(buf)))
                port_names.add(juce::String::fromUTF8(buf));

        juce::PopupMenu m;
        const int kNone = 1;
        m.addItem(kNone, "None",
                  /*isActive=*/true,
                  /*isTicked=*/engine.midiInputDevice().isEmpty());
        m.addSeparator();
        for (int i = 0; i < port_names.size(); ++i)
        {
            const auto& name = port_names[i];
            m.addItem(kNone + 1 + i, name,
                      true,
                      engine.midiInputDevice() == name);
        }

        juce::PopupMenu::Options opts;
        if (mainWindow_)
            opts = opts.withTargetComponent(mainWindow_.get());
        m.showMenuAsync(opts,
            [this, port_names](int chosen) {
                if (chosen <= 0) return;
                auto& eng = ensureLiveEngine();
                if (chosen == kNone) eng.setMidiInputDevice({});
                else
                {
                    const int idx = chosen - kNone - 1;
                    if (idx >= 0 && idx < port_names.size())
                        eng.setMidiInputDevice(port_names[idx]);
                }
            });
    }

    void showLoopDialog()
    {
        if (!live_) return;
        const double sr = 48000.0;  // canonical; ideally use the
                                    // currently loaded project's rate
        auto* aw = new juce::AlertWindow(
            "Loop region",
            "Loop in seconds (set both to 0 to disable):",
            juce::AlertWindow::QuestionIcon);
        aw->addTextEditor("start",
                          juce::String(live_->loopStart() / sr),
                          "loop start (s)");
        aw->addTextEditor("end",
                          juce::String(live_->loopEnd()   / sr),
                          "loop end (s)");
        aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true,
            juce::ModalCallbackFunction::create(
                [this, aw, sr](int result) {
                    if (result == 1)
                    {
                        const double s
                            = aw->getTextEditorContents("start").getDoubleValue();
                        const double e
                            = aw->getTextEditorContents("end").getDoubleValue();
                        const long long ss = (long long) (s * sr);
                        const long long es = (long long) (e * sr);
                        live_->setLoop(ss, es, ss < es);
                    }
                    delete aw;
                }),
            /*deleteWhenDismissed=*/false);
    }

    void showBpmDialog()
    {
        if (!live_) return;
        auto* aw = new juce::AlertWindow(
            "Set BPM",
            "Tempo in beats per minute (1-960):",
            juce::AlertWindow::QuestionIcon);
        aw->addTextEditor("bpm", juce::String(live_->bpm()), {}, false);
        aw->addButton("OK",     1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true,
            juce::ModalCallbackFunction::create(
                [this, aw](int result) {
                    if (result == 1)
                    {
                        const double bpm
                            = aw->getTextEditorContents("bpm").getDoubleValue();
                        if (bpm > 0.0 && bpm < 1000.0)
                            live_->setBpm(bpm);
                    }
                    delete aw;
                }),
            /*deleteWhenDismissed=*/false);
    }

    juce::OwnedArray<EditorWindow>     editors_;
    std::unique_ptr<MainWindow>        mainWindow_;
    std::unique_ptr<LiveEngine>        live_;
    std::unique_ptr<juce::FileChooser> save_chooser_;

    // Plugin library (scanned via the Plugin Browser, persisted to
    // known_plugins.xml). The format manager owns the host format
    // objects used for scanning; instantiation for the audio graph still
    // goes through mh_open.
    juce::AudioPluginFormatManager      plugin_format_manager_;
    juce::KnownPluginList               known_plugins_;
    std::unique_ptr<juce::DialogWindow> plugin_browser_window_;
    std::unique_ptr<juce::FileChooser>  plugin_chooser_;

    // Set only when this process was relaunched as a scan worker (see the
    // top of initialise()); keeps the child's IPC connection alive.
    std::unique_ptr<PluginScannerSubprocess> scanner_subprocess_;

    // True only in the GUI app-shell, which loads/owns known_plugins_ and is
    // the only process that should persist it on shutdown.
    bool owns_library_ = false;
};

} // namespace minihost_desktop

START_JUCE_APPLICATION(minihost_desktop::DesktopApplication)
