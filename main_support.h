#pragma once

// Self-contained driver-support utilities split out of main.cpp: filesystem /
// path helpers, the `fmt` reindenter, the C-linker driver + executable runner,
// and the lex+parse program loader. None of these touch the CLI option globals
// (those stay in main.cpp). See main_support.cpp.

#include <string>
#include <vector>
#include <memory>
#include <map>
#include "lexer/lexer.h"

class Program;

// stdlib root, set once at startup (main) and read by loadProgram.
extern std::string stdlibRoot;

std::string resolveStdlibPath();
std::string readFile(const std::string& filename);
std::string dirOf(const std::string& path);
std::string formatSource(const std::string& src);
int runFmt(const std::vector<std::string>& files, bool check);
// Seed the predefined macros every compile mode shares: the platform macro that
// follows `triple` (or the build host when empty) and __ESKIU_FREESTANDING__.
void seedPredefinedMacros(std::map<std::string, Macro>& macros, const std::string& triple,
                          bool freestanding);
std::shared_ptr<Program> loadProgram(const std::vector<std::string>& inputs, const std::string& triple,
                                     bool freestanding,
                                     std::map<std::string, Macro>* macrosOut = nullptr,
                                     bool* lexFailed = nullptr,
                                     const std::string* primarySource = nullptr);
bool endsWith(const std::string& s, const std::string& suffix);
// `ccArgs` receives any extra words of $CC (e.g. "-m64" in CC="clang -m64").
std::string findCDriver(bool sanitized = false, std::vector<std::string>* ccArgs = nullptr,
                        const std::string& triple = "");
bool linkExecutable(const std::string& obj, const std::string& out,
                    const std::vector<std::string>& libs,
                    const std::vector<std::string>& paths,
                    const std::vector<std::string>& extra,
                    bool sanitized = false, const std::string& triple = "");
// Write `obj` into the static library `out` (.a, or .lib for a Windows target).
bool writeStaticLibrary(const std::string& obj, const std::string& out, const std::string& triple);
// The runtime libraries a program needs that no stdlib module names, for the platform
// the predefined macros select: the C++ exception runtime when it throws or catches
// (-lc++ on Apple, -lstdc++ on Linux and mingw) and pthread when it spawns threads
// (Linux and mingw; libSystem has it on Apple). A non-hosted target gets none.
std::vector<std::string> implicitLinkLibs(const std::map<std::string, Macro>& macros,
                                          bool usesExceptions, bool usesThreads);
int runExecutable(const std::string& exe, const std::vector<std::string>& progArgs);
