// plugin_scanner.cpp -- see plugin_scanner.h for design.
//
// Adapted from JUCE's AudioPluginHost reference
// (extras/AudioPluginHost/Source/HostStartup.cpp and
// Source/UI/MainHostWindow.cpp).

#include "plugin_scanner.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace minihost_desktop {

// ------------------------------------------------------------------ //
// Child side                                                         //
// ------------------------------------------------------------------ //

PluginScannerSubprocess::PluginScannerSubprocess()
{
    // JUCE 8.0.11 moved AudioPluginFormatManager to the headless module and
    // deleted addDefaultFormats(); the free function registers the real
    // hosting formats.
    juce::addDefaultFormatsToManager (format_manager_);
}

void PluginScannerSubprocess::handleMessageFromCoordinator (const juce::MemoryBlock& mb)
{
    if (mb.isEmpty())
        return;

    const std::lock_guard<std::mutex> lock (mutex_);

    if (const auto results = doScan (mb); ! results.isEmpty())
    {
        sendResults (results);
    }
    else
    {
        // Empty could mean "genuinely no plugins" or "this format needs the
        // message thread and we're on the connection thread" -- re-run on the
        // message thread to be sure before reporting an empty result.
        pending_blocks_.emplace (mb);
        triggerAsyncUpdate();
    }
}

void PluginScannerSubprocess::handleConnectionLost()
{
    juce::JUCEApplicationBase::quit();
}

void PluginScannerSubprocess::handleAsyncUpdate()
{
    for (;;)
    {
        const std::lock_guard<std::mutex> lock (mutex_);

        if (pending_blocks_.empty())
            return;

        sendResults (doScan (pending_blocks_.front()));
        pending_blocks_.pop();
    }
}

juce::OwnedArray<juce::PluginDescription>
PluginScannerSubprocess::doScan (const juce::MemoryBlock& block)
{
    juce::MemoryInputStream stream { block, false };
    const auto formatName = stream.readString();
    const auto identifier = stream.readString();

    juce::PluginDescription pd;
    pd.fileOrIdentifier = identifier;
    pd.uniqueId = pd.deprecatedUid = 0;

    juce::AudioPluginFormat* matchingFormat = nullptr;
    for (auto* format : format_manager_.getFormats())
        if (format->getName() == formatName)
            matchingFormat = format;

    juce::OwnedArray<juce::PluginDescription> results;

    if (matchingFormat != nullptr
        && (juce::MessageManager::getInstance()->isThisTheMessageThread()
            || matchingFormat->requiresUnblockedMessageThreadDuringCreation (pd)))
    {
        matchingFormat->findAllTypesForFile (results, identifier);
    }

    return results;
}

void PluginScannerSubprocess::sendResults (
    const juce::OwnedArray<juce::PluginDescription>& results)
{
    juce::XmlElement xml ("LIST");

    for (const auto& desc : results)
        xml.addChildElement (desc->createXml().release());

    const auto str = xml.toString();
    sendMessageToCoordinator ({ str.toRawUTF8(), str.getNumBytesAsUTF8() });
}

// ------------------------------------------------------------------ //
// Parent side                                                        //
// ------------------------------------------------------------------ //

ScannerCoordinator::ScannerCoordinator()
{
    // MINIHOST_SCANNER_WORKER overrides the child executable, so tests can
    // make the launch fail.
    auto exe = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
    if (const auto* env = std::getenv ("MINIHOST_SCANNER_WORKER");
        env != nullptr && env[0] != '\0')
        exe = juce::File (juce::String::fromUTF8 (env));
    launched_ = launchWorkerProcess (exe, kScannerProcessUID, 0, 0);
}

ScannerCoordinator::Response ScannerCoordinator::getResponse()
{
    std::unique_lock<std::mutex> lock { mutex_ };

    if (! condvar_.wait_for (lock, std::chrono::milliseconds { 50 },
                             [&] { return got_result_ || connection_lost_; }))
        return { State::timeout, nullptr };

    const auto state = connection_lost_ ? State::connectionLost : State::gotResult;
    connection_lost_ = false;
    got_result_      = false;

    return { state, std::move (plugin_description_) };
}

void ScannerCoordinator::handleMessageFromWorker (const juce::MemoryBlock& mb)
{
    const std::lock_guard<std::mutex> lock { mutex_ };
    plugin_description_ = juce::parseXML (mb.toString());
    got_result_ = true;
    condvar_.notify_one();
}

void ScannerCoordinator::handleConnectionLost()
{
    const std::lock_guard<std::mutex> lock { mutex_ };
    connection_lost_ = true;
    condvar_.notify_one();
}

// ------------------------------------------------------------------ //
// CustomScanner hook                                                 //
// ------------------------------------------------------------------ //

namespace {
// Upper bound on how long a single plugin may take before we give up on it
// and blacklist it. A hung plugin (never returns, never crashes) would
// otherwise loop forever. 50 ms per timeout tick -> 40 s.
constexpr int kMaxTimeoutTicks = 800;
} // namespace

OutOfProcessPluginScanner::OutOfProcessPluginScanner (bool scan_in_process)
    : scan_in_process_ (scan_in_process)
{
}

bool OutOfProcessPluginScanner::findPluginTypesFor (
    juce::AudioPluginFormat& format,
    juce::OwnedArray<juce::PluginDescription>& result,
    const juce::String& fileOrIdentifier)
{
    if (scan_in_process_)
    {
        superprocess_ = nullptr;
        format.findAllTypesForFile (result, fileOrIdentifier);
        return true;
    }

    if (skip_rest_) return true;

    switch (addPluginDescriptions (format.getName(), fileOrIdentifier, result))
    {
    case Outcome::ok:
        return true;
    case Outcome::pluginFailed:
        // The child crashed, hung or answered garbage on this plugin: drop it
        // so the next plugin gets a fresh child. False blacklists the plugin.
        superprocess_ = nullptr;
        return false;
    case Outcome::scannerFailed:
        break;
    }
    superprocess_ = nullptr;
    skip_rest_ = true;
    failed_.store (true);
    const juce::String msg =
        "The plugin scanner could not start its helper process, so the scan "
        "stopped at " + fileOrIdentifier + ". No plugin was blacklisted; "
        "scan again to retry.";
    std::fprintf (stderr, "scan: %s\n", msg.toRawUTF8());
    if (on_failure_) on_failure_ (msg);
    return true;   // neither added nor blacklisted
}

void OutOfProcessPluginScanner::scanFinished()
{
    superprocess_ = nullptr;
    skip_rest_ = false;
}

OutOfProcessPluginScanner::Outcome OutOfProcessPluginScanner::addPluginDescriptions (
    const juce::String& formatName,
    const juce::String& fileOrIdentifier,
    juce::OwnedArray<juce::PluginDescription>& result)
{
    if (superprocess_ == nullptr)
        superprocess_ = std::make_unique<ScannerCoordinator>();
    if (! superprocess_->launched())
        return Outcome::scannerFailed;

    juce::MemoryBlock block;
    juce::MemoryOutputStream stream { block, true };
    stream.writeString (formatName);
    stream.writeString (fileOrIdentifier);

    if (! superprocess_->sendMessageToWorker (block))
        return Outcome::scannerFailed;

    int timeout_ticks = 0;
    for (;;)
    {
        if (shouldExit())
            return Outcome::ok;

        const auto response = superprocess_->getResponse();

        if (response.state == ScannerCoordinator::State::timeout)
        {
            if (++timeout_ticks >= kMaxTimeoutTicks)
            {
                std::fprintf (stderr,
                    "scan: giving up on %s (%s) after timeout; blacklisting\n",
                    fileOrIdentifier.toRawUTF8(), formatName.toRawUTF8());
                return Outcome::pluginFailed;
            }
            continue;
        }

        // connectionLost: the child died on this plugin, so blacklist it.
        if (response.state != ScannerCoordinator::State::gotResult)
            return Outcome::pluginFailed;
        // A reply that is not XML would otherwise drop the plugin unseen.
        if (response.xml == nullptr)
        {
            std::fprintf (stderr,
                "scan: malformed reply for %s (%s); blacklisting\n",
                fileOrIdentifier.toRawUTF8(), formatName.toRawUTF8());
            return Outcome::pluginFailed;
        }
        for (const auto* item : response.xml->getChildIterator())
        {
            auto desc = std::make_unique<juce::PluginDescription>();
            if (desc->loadFromXml (*item))
                result.add (std::move (desc));
        }
        return Outcome::ok;   // even if empty
    }
}

} // namespace minihost_desktop
