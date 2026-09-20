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
//
// What it lends is one private mode, not a common one. JUCE registers its
// source for kCFRunLoopCommonModes, and running a common mode services every
// other main-loop client in the process: sources and observers registered the
// same way, blocks from CFRunLoopPerformBlock, and the main dispatch queue,
// which CoreFoundation drains whenever the running mode is a common mode.
// That is how an AudioToolbox CAEventReceiver block came to throw
// std::bad_function_call -- from AudioToolbox's own ABI-tagged libc++, whose
// std::exception no catch in this binary matches -- out of poll_callbacks().
//
// So scripts/download_juce.py patches juce_MessageQueue_mac.h to add the
// source to the mode named below as well. Nothing else is ever added to it,
// and it is deliberately not a common mode. A plugin's own bundled JUCE is
// unpatched, so its queue stays out too. CMakeLists.txt fails the configure
// if the patch is missing; without it this mode is empty and every pump
// silently delivers nothing.

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <pthread.h>

#include "minihost_diag.h"

// Must match PUMP_MODE in scripts/download_juce.py.
#define MH_PUMP_MODE CFSTR("net.minihost.pump")

// Returns the number of messages delivered; 0 off the main thread.
extern "C" int mh_pump_main_runloop(int max_messages)
{
    if (pthread_main_np() == 0)
        return 0;

    int n = 0;
    // JUCE delivers its messages from a noexcept callback, so nothing should
    // reach here. The catch is what keeps a C API from propagating a C++ or
    // Objective-C exception if that ever stops being true.
    try
    {
        while (n < max_messages
               && CFRunLoopRunInMode(MH_PUMP_MODE, 0, true) == kCFRunLoopRunHandledSource)
        {
            ++n;
        }
    }
    catch (...)
    {
        minihost::reportSwallowedException("the macOS message pump");
    }
    return n;
}

#endif  // __APPLE__
