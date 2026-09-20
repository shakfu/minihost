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

// Returns the number of messages delivered; 0 off the main thread.
extern "C" int mh_pump_main_runloop(int max_messages)
{
    if (pthread_main_np() == 0)
        return 0;

    int n = 0;
    while (n < max_messages
           && CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) == kCFRunLoopRunHandledSource)
    {
        ++n;
    }
    return n;
}

#endif  // __APPLE__
