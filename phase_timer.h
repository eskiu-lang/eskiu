#pragma once

// Compile-phase timing (docs/dev/compile-speed.md): with `--time`, or ESKIU_TIME_PHASES set
// in the environment, each phaseMark(name) prints to stderr the wall time since the
// previous mark (or since the first call), and phaseTotal() the whole. Off otherwise, at
// the cost of one cached getenv.

#include <chrono>
#include <cstdio>
#include <cstdlib>

inline bool& phaseTimingFlag() {
    static bool on = std::getenv("ESKIU_TIME_PHASES") != nullptr;
    return on;
}

inline bool phaseTimingOn() { return phaseTimingFlag(); }

inline void enablePhaseTiming() { phaseTimingFlag() = true; }

struct PhaseClock {
    using clock = std::chrono::steady_clock;
    clock::time_point first, last;
    bool started = false;
    bool totalPrinted = false;
    static PhaseClock& get() {
        static PhaseClock c;
        return c;
    }
};

inline void phaseMark(const char* name) {
    if (!phaseTimingOn()) return;
    PhaseClock& c = PhaseClock::get();
    PhaseClock::clock::time_point now = PhaseClock::clock::now();
    if (!c.started) {
        c.first = c.last = now;
        c.started = true;
    }
    double ms = std::chrono::duration<double, std::milli>(now - c.last).count();
    double total = std::chrono::duration<double, std::milli>(now - c.first).count();
    std::fprintf(stderr, "phase %-16s %8.1f ms  (total %8.1f ms)\n", name, ms, total);
    c.last = now;
}

// A line without a phase (`cache hit 0123...`), in the same column layout.
inline void phaseNote(const char* what, const char* detail) {
    if (!phaseTimingOn()) return;
    std::fprintf(stderr, "phase %-16s %s\n", what, detail);
}

// The wall time since the first mark, printed once.
inline void phaseTotal() {
    if (!phaseTimingOn()) return;
    PhaseClock& c = PhaseClock::get();
    if (!c.started || c.totalPrinted) return;
    c.totalPrinted = true;
    double total = std::chrono::duration<double, std::milli>(PhaseClock::clock::now() - c.first).count();
    std::fprintf(stderr, "phase %-16s %8.1f ms\n", "total", total);
}
