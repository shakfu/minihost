// minihost_clap_bundle_mac.cpp
// Loads a macOS .clap bundle and resolves its clap_entry symbol.
//
// A translation unit of its own for the reason minihost_pump_mac.cpp gives:
// CoreFoundation's MacTypes.h collides with JUCE's Point and Rect.

#if defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <cstring>

// Returns a retained CFBundleRef, or nullptr if `path` is not a loadable bundle.
extern "C" void* mh_clap_bundle_open(const char* path)
{
    if (path == nullptr) return nullptr;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(path),
        (CFIndex) std::strlen(path), true);
    if (url == nullptr) return nullptr;
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, url);
    CFRelease(url);
    if (bundle == nullptr) return nullptr;
    if (! CFBundleLoadExecutable(bundle))
    {
        CFRelease(bundle);
        return nullptr;
    }
    return (void*) bundle;
}

extern "C" void* mh_clap_bundle_symbol(void* bundle, const char* name)
{
    if (bundle == nullptr || name == nullptr) return nullptr;
    CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, name, kCFStringEncodingUTF8);
    if (s == nullptr) return nullptr;
    void* sym = CFBundleGetDataPointerForName((CFBundleRef) bundle, s);
    CFRelease(s);
    return sym;
}

// Releases the bundle reference. The executable stays mapped: unloading code
// a plugin may still have threads or atexit handlers in is not safe.
extern "C" void mh_clap_bundle_close(void* bundle)
{
    if (bundle != nullptr)
        CFRelease((CFBundleRef) bundle);
}

#endif  // __APPLE__
