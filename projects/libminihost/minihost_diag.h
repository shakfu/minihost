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
#include <cxxabi.h>
#include <typeinfo>

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

    const char* name = "unknown";
    if (auto* type = abi::__cxa_current_exception_type())
        name = type->name();

    int status = 0;
    char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    std::fprintf(stderr, "minihost: %s discarded an exception of type '%s'%s\n",
                 where, (status == 0 && demangled != nullptr) ? demangled : name,
                 (n + 1 == kMaxReports) ? " (further reports suppressed)" : "");
    std::free(demangled);
}

} // namespace minihost
