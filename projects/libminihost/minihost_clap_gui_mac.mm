// minihost_clap_gui_mac.mm
// The NSView a CLAP plugin's editor is parented to. AppKit is kept out of
// minihost_clap.cpp for the reason minihost_pump_mac.cpp gives. Built into
// the GUI library only: the headless one does not link AppKit.

#if defined(__APPLE__) && ! defined(MINIHOST_HEADLESS)

#import <AppKit/AppKit.h>

// +1 retained; release with mh_clap_nsview_release.
extern "C" void* mh_clap_nsview_create(int width, int height)
{
    NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    return (void*) view;
}

extern "C" void mh_clap_nsview_release(void* view)
{
    if (view != nullptr)
        [(NSView*) view release];
}

#endif
