// minihost_pump_mac.cpp
// The macOS/iOS half of pumpJuceMessages (see minihost.cpp).
//
// In a translation unit of its own so <CoreFoundation/CoreFoundation.h>
// never meets JUCE's headers: it pulls in MacTypes.h, whose Point and Rect
// would collide with JUCE's under the `using namespace juce` in minihost.cpp.
//
// Why the main thread: JUCE adds its message queue's run-loop source to the
// process's MAIN run loop (juce_MessageQueue_mac.h) regardless of which
// thread posted the message or created the MessageManager, so no other
// thread can deliver these. A headless host never runs that loop, which is
// why the caller has to lend it -- Plugin.poll_callbacks() does.

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

namespace {

// Private run-loop mode, registered as a common mode.
//
// The main run loop is shared with every other library in the process, so
// running kCFRunLoopDefaultMode delivers their work too. AudioToolbox's
// CAEventReceiver timer is one such client, and its block throws
// std::bad_function_call -- from AudioToolbox's own ABI-tagged libc++, whose
// std::exception is a different type from ours, so no catch in this binary
// can match it and the caller sees an untranslatable foreign exception.
//
// A common mode gets every source registered with kCFRunLoopCommonModes,
// which is how JUCE registers its queue, and CFRunLoopAddCommonMode also
// back-fills the ones already registered, so the call need not precede JUCE's.
// A source or timer added for kCFRunLoopDefaultMode alone stays out.
CFStringRef pumpMode()
{
    static CFStringRef mode = []
    {
        CFStringRef m = CFSTR("net.minihost.pump");
        CFRunLoopAddCommonMode(CFRunLoopGetMain(), m);
        return m;
    }();
    return mode;
}

} // namespace

// Returns the number of messages delivered; 0 off the main thread.
extern "C" int mh_pump_main_runloop(int max_messages)
{
    if (pthread_main_np() == 0)
        return 0;

    int n = 0;
    // A callout can still throw something this binary cannot name -- a
    // foreign C++ exception as above, or an Objective-C one. Stop pumping
    // and report what was delivered rather than let it cross the C API.
    try
    {
        while (n < max_messages
               && CFRunLoopRunInMode(pumpMode(), 0, true) == kCFRunLoopRunHandledSource)
        {
            ++n;
        }
    }
    catch (...)
    {
    }
    return n;
}

#endif  // __APPLE__
