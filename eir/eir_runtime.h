#pragma once

#include "eir.h"
#include <string>

namespace eir {

// The C runtime the native EIR backends call for what the EVM does internally:
// exceptions (the pending exception, the caught stack, exception_text and the uncaught
// report) and threads. Exceptions use the EVM's object layout: size at 0, type name at 8,
// payload at 16.

// A translation unit of its own, with the system headers (the assembly backend links it).
std::string runtimeStandaloneSource();

// The runtime for the inside of a generated C file: no system headers, and each C
// function it calls is reached through a cast, so a program declaring the same function
// with its own types does not conflict.
std::string runtimeInlineSource(const Module* mod);

// True when the module calls into the runtime.
bool moduleNeedsRuntime(const Module* mod);

// True when the module can raise an exception (it throws).
bool moduleThrows(const Module* mod);

// True when the module starts threads (the link needs pthread where libc lacks it).
bool moduleUsesThreads(const Module* mod);

} // namespace eir
