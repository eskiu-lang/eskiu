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
// eskiu.lock. Nothing is fetched here.
void pkgLoadRoots(const std::string& input);

// The lib directories of the prebuilt native libraries ([native.NAME] in eskiu.toml) that
// the package and its dependencies declare for this host, as pkgLoadRoots found them in
// $ESKIU_HOME/native; `missing` names the ones not fetched yet. The driver links a host
// build against `dirs` (-L and an rpath).
struct PkgNativeLibs {
    std::vector<std::string> dirs;
    std::vector<std::string> missing;
};
extern PkgNativeLibs pkgNativeLibs;

// Copy the DLLs of `dirs` next to `exe`, where Windows looks for them first.
void copyNativeDlls(const std::vector<std::string>& dirs, const std::string& exe);
