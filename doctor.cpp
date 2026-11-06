// `eskiuc doctor`: what this machine has for each kind of build, and what is missing.
// It only looks (no download, no build): the C driver that links native programs, the
// tools packages need, OpenSSL for <fetch>'s https, SDL3 for the gpu and audio packages,
// and the toolchain each cross target uses (the same lookups as findCDriver/crossDriver).
#include "main_support.h"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

std::string androidNdk();

namespace {

int problems = 0;

void report(bool ok, const std::string& what, const std::string& detail, bool required = false) {
    std::cout << "  " << (ok ? "ok       " : "missing  ") << what;
    for (size_t n = what.size(); n < 28; ++n) std::cout << ' ';
    std::cout << detail << "\n";
    if (!ok && required) ++problems;
}

// The first line `program args` prints (stdout or stderr), or "" when it cannot run.
std::string firstLine(const std::string& program, const std::vector<std::string>& args) {
    llvm::SmallString<128> outFile;
    if (llvm::sys::fs::createTemporaryFile("eskiu-doctor", "out", outFile)) return "";
    std::vector<llvm::StringRef> argv = {program};
    for (const auto& a : args) argv.push_back(a);
    std::optional<llvm::StringRef> redirects[] = {llvm::StringRef(""), llvm::StringRef(outFile),
                                                  llvm::StringRef(outFile)};
    int rc = llvm::sys::ExecuteAndWait(program, argv, std::nullopt, redirects, 20);
    std::string line;
    if (auto buf = llvm::MemoryBuffer::getFile(outFile)) {
        llvm::StringRef text = (*buf)->getBuffer();
        line = text.split('\n').first.trim().str();
    }
    llvm::sys::fs::remove(outFile);
    return rc < 0 ? "" : line;
}

std::string onPath(const char* name) {
    if (auto p = llvm::sys::findProgramByName(name)) return *p;
    return "";
}

void tool(const char* name, const char* versionArg, const std::string& why) {
    std::string p = onPath(name);
    if (p.empty()) report(false, name, "not on PATH (" + why + ")");
    else report(true, name, firstLine(p, {versionArg}));
}

bool exists(const std::string& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

bool openssl(std::string& where) {
#if defined(_WIN32)
    const char* names[] = {"libssl-3-x64.dll", "libssl-3.dll", "libssl-1_1-x64.dll"};
#elif defined(__APPLE__)
    const char* names[] = {"libssl.3.dylib", "/opt/homebrew/opt/openssl@3/lib/libssl.3.dylib",
                           "/usr/local/opt/openssl@3/lib/libssl.3.dylib", "/opt/local/lib/libssl.3.dylib",
                           "libssl.1.1.dylib", "/opt/homebrew/opt/openssl@1.1/lib/libssl.1.1.dylib",
                           "/usr/local/opt/openssl@1.1/lib/libssl.1.1.dylib"};
#else
    const char* names[] = {"libssl.so.3", "libssl.so.1.1", "libssl.so"};
#endif
    for (const char* n : names) {
        std::string err;
        auto lib = llvm::sys::DynamicLibrary::getLibrary(n, &err);
        if (lib.isValid()) {
            where = n;
            return true;
        }
    }
    return false;
}

bool sdl3(std::string& where) {
    std::vector<std::string> dirs = {"/opt/homebrew/include", "/usr/local/include", "/usr/include"};
    if (const char* p = std::getenv("SDL3_DIR"); p && *p) dirs.insert(dirs.begin(), std::string(p) + "/include");
    for (const auto& d : dirs)
        if (exists(d + "/SDL3/SDL.h")) {
            where = d + "/SDL3";
            return true;
        }
    std::string pc = onPath("pkg-config");
    if (!pc.empty() && llvm::sys::ExecuteAndWait(pc, {pc, "--exists", "sdl3"}) == 0) {
        where = "pkg-config sdl3 " + firstLine(pc, {"--modversion", "sdl3"});
        return true;
    }
    return false;
}

}  // namespace

int doctorCommand(const std::string& version) {
    std::cout << "Eskiu " << version << "\n\nCompiler:\n";
    std::string root = resolveStdlibPath();
    bool stdOk = !root.empty() && exists(root + "/stdlib/io.esk");
    report(stdOk, "standard library",
           stdOk ? root + "/stdlib" : "not found (set ESKIU_ROOT to the directory holding stdlib/)", true);
    std::vector<std::string> ccArgs;
    std::string cc = findCDriver(false, &ccArgs, "");
    report(!cc.empty(), "C driver (links programs)",
           cc.empty() ? "none: install clang or gcc (Windows: MinGW-w64 or llvm-mingw), or set $CC"
                      : cc + ": " + firstLine(cc, {"--version"}),
           true);

    std::cout << "\nPackages and the standard library:\n";
    tool("git", "--version", "eskiuc add/fetch of git packages");
    tool("curl", "--version", "[native.*] libraries of packages");
#ifdef _WIN32
    tool("tar", "--version", "unpacking native libraries");
#else
    tool("tar", "--version", "unpacking native libraries");
    tool("unzip", "-v", "native libraries shipped as .zip");
#endif
    std::string where;
    report(openssl(where), "OpenSSL (https in <fetch>)",
           where.empty() ? "libssl 3 or 1.1 not loadable: https fetches fail, http works" : where);
    where.clear();
    report(sdl3(where), "SDL3 (gpu, audio packages)",
           where.empty() ? "headers not found: a [native.SDL3] package entry, or install SDL3" : where);

    std::cout << "\nCross targets:\n";
    std::string emcc = onPath("emcc");
    report(!emcc.empty(), "WebAssembly (emscripten)",
           emcc.empty() ? "emcc not on PATH (install the Emscripten SDK)" : "emcc: " + firstLine(emcc, {"--version"}));
    std::string ndk = androidNdk();
    report(!ndk.empty(), "Android",
           ndk.empty() ? "no NDK: set ANDROID_NDK_HOME, or install it with the SDK manager" : "NDK " + ndk);
#ifdef __APPLE__
    std::string xcrun = onPath("xcrun");
    std::string sdk = xcrun.empty() ? "" : firstLine(xcrun, {"--sdk", "iphoneos", "--show-sdk-path"});
    bool iosOk = !sdk.empty() && sdk.find("error") == std::string::npos && exists(sdk);
    report(iosOk, "iOS", iosOk ? sdk : "no iPhoneOS SDK: install Xcode");
#endif
    std::string dka = std::getenv("DEVKITARM") ? std::getenv("DEVKITARM") : "";
    bool dkOk = !dka.empty() && (exists(dka + "/bin/arm-none-eabi-gcc") || exists(dka + "/bin/arm-none-eabi-gcc.exe"));
    report(dkOk, "Nintendo 3DS (devkitARM)",
           dkOk ? dka : "DEVKITARM not set to a devkitARM install (freestanding builds still work)");

    std::cout << "\n" << (problems == 0 ? "Native builds are ready." : "Native builds cannot link yet: see above.")
              << "\n";
    return problems == 0 ? 0 : 1;
}
