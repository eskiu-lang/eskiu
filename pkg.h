#pragma once

// The package manager: eskiu.toml (the manifest), eskiu.lock (the resolved
// dependencies), git fetching into $ESKIU_HOME/pkgs, and the `new`, `add`, `build`,
// `run`, `test` and `fetch` commands. See pkg.cpp and docs/lang/build.md.

#include <string>
#include <vector>

// Handle a package command (`eskiuc new|add|build|fetch|test ...`, and `eskiuc run`
// inside a package when no .esk script is named). Returns true and sets `rc` when
// argv was a package command; false leaves argv to the compiler's own options.
bool pkgCommand(int argc, char** argv, int& rc);

// Fill `packageRoots` (parser.h) for a compile whose first input is `input`: when an
// eskiu.toml is in its directory or above, the dependencies of that package's
// eskiu.lock, and the [native.NAME] archives matching `triple` (or the host when empty).
// Nothing is fetched here.
void pkgLoadRoots(const std::string& input, const std::string& triple = "");

// The directories of the prebuilt native libraries ([native.NAME] in eskiu.toml, NAME the
// library -lNAME links) that the package of the compile's first input and its dependencies
// declare for the target (or host), one per name in `linked` that has one. An archive not in
// $ESKIU_HOME/native yet is downloaded and checked first; a failure throws, unless the build
// is for this machine and the system has the library: then a warning, and its directory goes
// to `systemDirs`. A name in $ESKIU_SYSTEM_LIBS skips the archive (its system directory, when
// found, goes to `systemDirs`).
std::vector<std::string> pkgNativeLibDirs(const std::vector<std::string>& linked,
                                          std::vector<std::string>* systemDirs = nullptr);

// Is `lib` named in $ESKIU_SYSTEM_LIBS (separated by commas or spaces; "*" names every one),
// so that it is linked from the system instead of its package's [native.NAME] archive?
bool pkgPreferSystemLib(const std::string& lib);

// The directory where this machine has lib<NAME> for the linker ($LIBRARY_PATH, pkg-config's
// libdir, then the usual library directories), or "" when none has it; `file` gets the file.
std::string pkgFindSystemLib(const std::string& lib, std::string* file = nullptr);

// Copy the [web] assets of the compile's package and its dependencies next to `page`, the
// output of a WebAssembly (emscripten) build.
void pkgCopyWebAssets(const std::string& page);

// A [native.NAME] archive for this host that a package declares: the package of the current
// directory and its dependencies, and every package fetched into $ESKIU_HOME/pkgs.
struct KnownNative {
    std::string pkg, url, dir;   // dir: its library directory once fetched
    bool cached = false;         // already in $ESKIU_HOME/native
};
std::vector<KnownNative> pkgKnownNatives(const std::string& lib);

// Copy the DLLs of `dirs` next to `exe`, where Windows looks for them first.
void copyNativeDlls(const std::vector<std::string>& dirs, const std::string& exe);
