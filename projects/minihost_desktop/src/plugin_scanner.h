// plugin_scanner.h -- out-of-process plugin scanning.
//
// Plugin instantiation-during-scan is inherently unsafe: a plugin can crash
// or call exit() while being probed, and in-process that takes down the whole
// app (observed live with a commercial AU). This moves the actual
// instantiation into a disposable child process. The parent detects a dead or
// hung child, blacklists the offending plugin, respawns, and continues.
//
// This is JUCE's first-class pattern (KnownPluginList::CustomScanner +
// ChildProcessCoordinator / ChildProcessWorker), adapted from the reference in
// thirdparty/JUCE/extras/AudioPluginHost (HostStartup.cpp worker side,
// MainHostWindow.cpp coordinator + scanner side).
//
// Wiring:
//   - The same executable relaunches itself as the child. Very early in
//     DesktopApplication::initialise(), construct a PluginScannerSubprocess and
//     call initialiseFromCommandLine(cmdLine, kScannerProcessUID); if it
//     returns true, this process IS the worker -- keep it alive and return.
//   - On the parent, call known_plugins.setCustomScanner(
//     std::make_unique<OutOfProcessPluginScanner>()). Both the interactive
//     PluginListComponent scan and a headless PluginDirectoryScanner loop then
//     dispatch through the child automatically (KnownPluginList::scanAndAddFile
//     invokes the custom scanner and blacklists on failure).

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

namespace minihost_desktop {

// Command-line token that marks a relaunch of this binary as a scan worker.
constexpr const char* kScannerProcessUID = "minihostPluginScanner";

// Child side. Lives only in the worker process. Receives (formatName,
// identifier) messages, instantiates the plugin in THIS (disposable) process,
// and returns the discovered PluginDescriptions as XML. If this process
// crashes mid-scan, only the child dies.
class PluginScannerSubprocess final : private juce::ChildProcessWorker,
                                      private juce::AsyncUpdater
{
public:
    PluginScannerSubprocess();

    using juce::ChildProcessWorker::initialiseFromCommandLine;

private:
    void handleMessageFromCoordinator (const juce::MemoryBlock& mb) override;
    void handleConnectionLost() override;
    void handleAsyncUpdate() override;

    juce::OwnedArray<juce::PluginDescription> doScan (const juce::MemoryBlock& block);
    void sendResults (const juce::OwnedArray<juce::PluginDescription>& results);

    std::mutex mutex_;
    std::queue<juce::MemoryBlock> pending_blocks_;
    juce::AudioPluginFormatManager format_manager_;
};

// Parent side. One child process, reused across the plugins of a scan. Sends a
// scan request and waits (with a short timeout) for the child's reply or its
// death.
class ScannerCoordinator final : private juce::ChildProcessCoordinator
{
public:
    ScannerCoordinator();

    enum class State { timeout, gotResult, connectionLost };

    struct Response {
        State state;
        std::unique_ptr<juce::XmlElement> xml;
    };

    // Blocks up to ~50 ms for a reply. `timeout` means "still working, ask
    // again"; `connectionLost` means the child died (crash/exit).
    Response getResponse();

    using juce::ChildProcessCoordinator::sendMessageToWorker;

private:
    void handleMessageFromWorker (const juce::MemoryBlock& mb) override;
    void handleConnectionLost() override;

    std::mutex mutex_;
    std::condition_variable condvar_;
    std::unique_ptr<juce::XmlElement> plugin_description_;
    bool connection_lost_ = false;
    bool got_result_      = false;
};

// The KnownPluginList hook. findPluginTypesFor dispatches one plugin to the
// child and collects its descriptions. Returning false tells KnownPluginList
// the plugin is unrecoverable, so it gets blacklisted and the scan continues.
class OutOfProcessPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    // scan_in_process = true bypasses the child (direct in-process scan); the
    // default runs every probe out-of-process.
    explicit OutOfProcessPluginScanner (bool scan_in_process = false);

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override;
    void scanFinished() override;

private:
    bool addPluginDescriptions (const juce::String& formatName,
                                const juce::String& fileOrIdentifier,
                                juce::OwnedArray<juce::PluginDescription>& result);

    std::unique_ptr<ScannerCoordinator> superprocess_;
    const bool scan_in_process_;
};

} // namespace minihost_desktop
