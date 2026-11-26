// cli.h: the subcommands' usage texts, --help and usage errors (cli.cpp). Exit codes of
// every command (docs/lang/build.md, "Exit codes"): 0 success, 1 the work failed (a
// compile error, a failed test, a missing file), 2 the command line is wrong (an unknown
// option, a missing or bad option value, wrong arguments).
#pragma once
#include <string>

constexpr int kExitUsage = 2;

// `--help`, `-help` or `-h`.
bool isHelpFlag(const std::string& a);

// The usage text of the subcommand `cmd` (new, add, build, run, test, fetch, fmt, repl,
// doctor), or "" when `cmd` is not one.
std::string cliUsage(const std::string& cmd);

// When argv[1] is a subcommand and its own arguments ask for help (for `run`, only the
// flags before the script: what follows is the program's), print its usage on stdout
// and return true; `rc` is then 0.
bool cliSubcommandHelp(int argc, char** argv, int& rc);

// `error: CMD: MSG (see 'eskiuc CMD --help')` on stderr; returns kExitUsage.
int cliUsageError(const std::string& cmd, const std::string& msg);

// Whether `arg` (`-name`, `--name`, `-name=value`, `-O2`, `-lm`) names one of the
// compiler's options; `takesValue` tells whether the next argument is its value.
bool isCompilerOption(const std::string& arg, bool* takesValue = nullptr);

// Rewrite the errors of llvm::cl's parser (`eskiuc: Unknown command line argument
// '--x'.  Try: ...`) as `error: unknown option '--x' (see 'eskiuc --help')` lines.
void printOptionErrors(const std::string& text);
