// `eskiuc doctor`: what this machine has for each kind of build, and what is missing.
// It only looks (no download, no build): the C driver that links native programs, the
// tools packages need, OpenSSL for <fetch>'s https, SDL3 for the gpu and audio packages
// (found as a build finds it: a package's [native.SDL3] archive, else the system library),
// and the toolchain each cross target uses (the same lookups as findCDriver/crossDriver).
#include "main_support.h"
#include "pkg.h"
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

// SDL3 as a build gets it: the archive a package declares in [native.SDL3] (already in
// $ESKIU_HOME/native, or downloaded by the first build that links it), else the system's
// libSDL3. Prints its own lines; true when a build can link it.
bool sdl3() {
    std::vector<KnownNative> archives = pkgKnownNatives("SDL3");
    std::string file;
    std::string sysDir = pkgFindSystemLib("SDL3", &file);
    bool preferSys = pkgPreferSystemLib("SDL3");
    const KnownNative* cached = nullptr;
    for (const auto& a : archives) if (a.cached && !cached) cached = &a;
    std::string what = "SDL3 (gpu, audio packages)";
    std::string sysText = sysDir.empty() ? "" : "system library " + file;
    std::string version;
    if (!sysDir.empty()) {
        std::string pc = onPath("pkg-config");
        if (!pc.empty()) version = firstLine(pc, {"--modversion", "sdl3"});
        if (!version.empty() && version.find_first_not_of("0123456789.") == std::string::npos)
            sysText += " (" + version + ")";
    }
    bool ok = true;
    if (preferSys) {
        ok = !sysDir.empty();
        report(ok, what, ok ? sysText + ", used in place of the package archive (ESKIU_SYSTEM_LIBS)"
                            : "ESKIU_SYSTEM_LIBS names SDL3 but no libSDL3 is installed");
    } else if (cached) {
        report(true, what, "prebuilt archive of package '" + cached->pkg + "' in " + cached->dir);
    } else if (!archives.empty()) {
        report(true, what, "prebuilt archive of package '" + archives[0].pkg +
                               "', downloaded by the first build that links it (needs curl)");
        std::cout << std::string(39, ' ') << "from " << archives[0].url << "\n";
    } else if (!sysDir.empty()) {
        report(true, what, sysText);
    } else {
        ok = false;
        report(false, what, "no package archive and no system libSDL3");
    }
    std::string pad(39, ' ');
    const char* install =
#if defined(__APPLE__)
        "brew install sdl3";
#elif defined(_WIN32)
        "pacman -S mingw-w64-x86_64-sdl3 (MSYS2)";
#else
        "apt install libsdl3-dev, or dnf install SDL3-devel";
#endif
    if (!preferSys && !sysDir.empty() && !archives.empty())
        std::cout << pad << "also " << sysText << ": ESKIU_SYSTEM_LIBS=SDL3 links it instead\n";
    if (!preferSys && sysDir.empty() && !cached && !archives.empty())
        std::cout << pad << "to build offline, run eskiuc fetch in the package first, or install SDL3: "
                  << install << "\n";
    if (!ok && !preferSys)
        std::cout << pad << "build in a package that depends on gpu or audio (the first build downloads\n"
                  << pad << "their SDL3), or install SDL3: " << install << "\n";
    if (!ok && preferSys)
        std::cout << pad << "install SDL3, or unset ESKIU_SYSTEM_LIBS to use the package archive\n";
    return ok;
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
    sdl3();

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
