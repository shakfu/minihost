// minihost_diag.h
// Reporting for the catch(...) blocks that guard the C API boundary.
//
// A C API returns codes, so an exception a plugin throws has to stop before it
// crosses one. Discarding it silently costs a debugger session the next time:
// the NSException AppKit raises when a plugin builds a window off the main
// thread took a breakpoint on __cxa_throw to identify. Naming the type on
// stderr would have said it in one line.
//
// Header-only and dependency-free on purpose: minihost_pump_mac.cpp cannot
// include JUCE, and minihost.cpp cannot include CoreFoundation.

#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <typeinfo>

// The Itanium ABI can name the exception in flight whatever its type, which is
// how an Objective-C NSException gets reported rather than "unknown". MSVC and
// clang-cl have no equivalent and no <cxxabi.h>, so they take the portable
// path below; MinGW has both and does not. __has_include decides, because the
// compiler is the wrong question -- clang-cl is clang without cxxabi.
// Define MINIHOST_DIAG_PORTABLE to compile the portable path on a toolchain
// that has cxxabi, which is the only way to exercise it outside Windows.
#if !defined(MINIHOST_DIAG_PORTABLE) && defined(__has_include)
 #if __has_include(<cxxabi.h>)
  #define MINIHOST_DIAG_CXXABI 1
 #endif
#endif
#ifdef MINIHOST_DIAG_CXXABI
 #include <cxxabi.h>
#endif

namespace minihost {

// Call from inside a catch(...). Prints at most kMaxReports lines per process,
// so a plugin that throws on every call cannot flood the output.
inline void reportSwallowedException(const char* where)
{
    constexpr int kMaxReports = 3;
    static std::atomic<int> reported{0};
    const int n = reported.fetch_add(1, std::memory_order_relaxed);
    if (n >= kMaxReports)
        return;

    const char* const suffix =
        (n + 1 == kMaxReports) ? " (further reports suppressed)" : "";

#ifdef MINIHOST_DIAG_CXXABI
    const char* name = "unknown";
    if (auto* type = abi::__cxa_current_exception_type())
        name = type->name();

    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::fprintf(stderr, "minihost: %s discarded an exception of type '%s'%s\n",
                 where, (status == 0 && demangled != nullptr) ? demangled : name,
                 suffix);
    std::free(demangled);
#else
    // Rethrowing is the portable way to ask what is in flight, and names
    // anything derived from std::exception. The guard matters: a bare throw
    // with no exception active calls std::terminate.
    const char* name = "unknown";
    if (std::current_exception() == nullptr)
        return;
    try
    {
        throw;
    }
    catch (const std::exception& e)
    {
        name = typeid(e).name();
    }
    catch (...)
    {
    }
    std::fprintf(stderr, "minihost: %s discarded an exception of type '%s'%s\n",
                 where, name, suffix);
#endif
}

} // namespace minihost
