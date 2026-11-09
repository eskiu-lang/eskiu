#pragma once

// Compile-phase timing for docs/dev/compile-speed.md: with ESKIU_TIME_PHASES set in the
// environment, each phaseMark(name) prints to stderr the wall time since the previous mark
// (or since the first call). Off otherwise, at the cost of one cached getenv.

#include <chrono>
#include <cstdio>
#include <cstdlib>

inline bool phaseTimingOn() {
    static const bool on = std::getenv("ESKIU_TIME_PHASES") != nullptr;
    return on;
}

inline void phaseMark(const char* name) {
    if (!phaseTimingOn()) return;
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    static clock::time_point first = last;
    clock::time_point now = clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - last).count();
    double total = std::chrono::duration<double, std::milli>(now - first).count();
    std::fprintf(stderr, "phase %-16s %8.1f ms  (total %8.1f ms)\n", name, ms, total);
    last = now;
}
