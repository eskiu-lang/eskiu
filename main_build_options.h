// main_build_options.h: the options about the build itself rather than the program
// (--time, the object cache) and the cache key of a compile; internal to main.cpp, which
// includes it once, after main_options.h.
#pragma once
#include <filesystem>
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/TargetParser/Host.h"
#include "build_cache.h"
#include "phase_timer.h"

static llvm::cl::opt<bool> TimePhases("time",
    llvm::cl::desc("Print the wall time of each compile phase (and the link) and the total on stderr"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> UseCache("cache",
    llvm::cl::desc("Reuse the object of an identical earlier compile (as ESKIU_CACHE=1; "
                   "ESKIU_CACHE_DIR, ESKIU_CACHE_SIZE)"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> NoCache("no-cache",
    llvm::cl::desc("Do not use the object cache, even with ESKIU_CACHE=1 (as ESKIU_NO_CACHE=1)"),
    llvm::cl::cat(EskiuCat));

// The command line the options were parsed from (argv[0] left out), for the cache key.
static std::vector<std::string> g_cacheArgv;

// The key of this compile's object (build_cache.h): the compiler, the target, the
// command line without what only names the output or reports on the build, the working
// directory, the root of the files that get warnings, and the sources the loader read.
// "" when the compiler's identity is unknown: the build is then not cached.
static std::string objectCacheKey() {
    std::string id = objcache::compilerIdentity();
    if (id.empty()) return "";
    objcache::Key k;
    k.add(VERSION);
    k.add(LLVM_VERSION_STRING);
    k.add(id);
    k.add(std::string(TargetTriple));
    k.add(llvm::sys::getDefaultTargetTriple());
    k.add(objcache::hostOSVersion());
    k.add("args");
    for (size_t i = 0; i < g_cacheArgv.size(); ++i) {
        const std::string& a = g_cacheArgv[i];
        if (a == "-o" || a == "--o") { ++i; continue; }
        if (a.rfind("-o=", 0) == 0 || a.rfind("--o=", 0) == 0) continue;
        if (a == "-time" || a == "--time" || a == "-cache" || a == "--cache" ||
            a == "-no-cache" || a == "--no-cache")
            continue;
        k.add(a);
    }
    // What the dropped -o still decides: a shared library is compiled for one.
    std::string o = std::string(OutputFilename);
    bool shared = !g_runMode && (endsWith(o, ".so") || endsWith(o, ".dylib") || endsWith(o, ".dll"));
    k.add(shared ? "shared" : "not-shared");
    k.add(g_runMode ? "run" : "build");
    std::error_code ec;
    k.add(std::filesystem::current_path(ec).generic_string());
    k.add(TypeChecker::lintRootFor(std::string(InputFilename)));
    return k.finish();
}
