// main_build_options.h: the options about the build itself rather than the program
// (--time); internal to main.cpp, which includes it once, after main_options.h.
#pragma once
#include "llvm/Support/CommandLine.h"
#include "phase_timer.h"

static llvm::cl::opt<bool> TimePhases("time",
    llvm::cl::desc("Print the wall time of each compile phase (and the link) and the total on stderr"),
    llvm::cl::cat(EskiuCat));
