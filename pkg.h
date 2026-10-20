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

// The directories of the prebuilt native libraries ([native.NAME] in eskiu.toml, NAME the
// library -lNAME links) that the package of the compile's first input and its dependencies
// declare for this host, one per name in `linked` that has one. An archive not in
// $ESKIU_HOME/native yet is downloaded and checked first; a failure throws.
std::vector<std::string> pkgNativeLibDirs(const std::vector<std::string>& linked);

// Copy the DLLs of `dirs` next to `exe`, where Windows looks for them first.
void copyNativeDlls(const std::vector<std::string>& dirs, const std::string& exe);
