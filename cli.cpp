// cli.cpp: the subcommands' usage texts, --help and usage errors (see cli.h).
#include "cli.h"

#include <iostream>
#include <map>
#include <sstream>
#include "llvm/Support/CommandLine.h"

bool isHelpFlag(const std::string& a) {
    return a == "--help" || a == "-help" || a == "-h";
}

std::string cliUsage(const std::string& cmd) {
    static const std::map<std::string, std::string> usages = {
        {"new",
         "usage: eskiuc new NAME\n\n"
         "Create the package NAME/: eskiu.toml, src/main.esk (hello world) and .gitignore.\n"},
        {"add",
         "usage: eskiuc add NAME --git URL [--rev REV]\n"
         "       eskiuc add NAME --path DIR\n\n"
         "Add a dependency to eskiu.toml, fetch it and update eskiu.lock. REV is a tag, branch\n"
         "or commit (default: the remote's default branch); a --path dependency is used in place.\n"},
        {"build",
         "usage: eskiuc build [FILE.esk] [-o OUT] [compiler flags]\n\n"
         "Compile the package's main file to build/NAME (or OUT), fetching what eskiu.lock names\n"
         "and is missing. With FILE.esk, compile that file (an example or a tool of the package)\n"
         "to build/FILE. Other flags go to the compiler (eskiuc --help lists them).\n"},
        {"run",
         "usage: eskiuc run [compiler flags] FILE.esk [--] [ARGS...]\n"
         "       eskiuc run [--] [ARGS...]            (inside a package)\n\n"
         "Compile FILE.esk to a temporary executable, run it with ARGS and delete it; the exit\n"
         "code is the program's (128+N when signal N killed it). With no .esk file inside a\n"
         "package, build the package and run it. Flags after FILE.esk belong to the program.\n"},
        {"test",
         "usage: eskiuc test [compiler flags]\n\n"
         "Compile and run each tests/*.esk of the package; a test passes when it exits with 0.\n"
         "Compiler flags (-O2, --safe, ...) apply to every test.\n"},
        {"fetch",
         "usage: eskiuc fetch\n\n"
         "Resolve every dependency again (fetching the git mirrors), rewrite eskiu.lock and\n"
         "download this host's native libraries.\n"},
        {"fmt",
         "usage: eskiuc fmt [--check] [--width N] FILE.esk...\n\n"
         "Reindent the files in place.\n"
         "  --check     change nothing; print the files that need formatting (exit 1 if any)\n"
         "  --width N   also wrap lines longer than N columns (N >= 20)\n"},
        {"repl",
         "usage: eskiuc repl [FILE.esk...]\n\n"
         "Start an interactive session (JIT); the files are loaded first, as with :load.\n"},
        {"doctor",
         "usage: eskiuc doctor\n\n"
         "Report the tools this machine has (and lacks) for each kind of build.\n"},
    };
    auto it = usages.find(cmd);
    return it == usages.end() ? "" : it->second;
}

bool cliSubcommandHelp(int argc, char** argv, int& rc) {
    if (argc < 3) return false;
    std::string cmd = argv[1];
    if (cliUsage(cmd).empty()) return false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--") break;
        if (isHelpFlag(a)) {
            std::cout << cliUsage(cmd);
            rc = 0;
            return true;
        }
        if (cmd != "run") continue;
        bool takesValue = false;
        if (!a.empty() && a[0] == '-' && isCompilerOption(a, &takesValue)) {
            if (takesValue) ++i;
            continue;
        }
        if (a.empty() || a[0] != '-') break;   // the script: the rest is the program's
    }
    return false;
}

int cliUsageError(const std::string& cmd, const std::string& msg) {
    std::cerr << "error: " << cmd << ": " << msg << " (see 'eskiuc " << cmd << " --help')" << std::endl;
    return kExitUsage;
}

bool isCompilerOption(const std::string& arg, bool* takesValue) {
    if (takesValue) *takesValue = false;
    if (arg.size() < 2 || arg[0] != '-') return false;
    std::string s = arg.substr(arg[1] == '-' ? 2 : 1);
    size_t eq = s.find('=');
    std::string name = s.substr(0, eq);
    if (name.empty()) return false;
    auto& opts = llvm::cl::getRegisteredOptions(llvm::cl::SubCommand::getTopLevel());
    auto it = opts.find(name);
    if (it != opts.end()) {
        llvm::cl::Option* o = it->second;
        if (takesValue && eq == std::string::npos &&
            o->getValueExpectedFlag() == llvm::cl::ValueRequired)
            *takesValue = true;
        return true;
    }
    // A prefix option with its value attached: -O2, -lm, -L/usr/lib.
    for (size_t n = name.size() - 1; n > 0; --n) {
        auto p = opts.find(name.substr(0, n));
        if (p == opts.end()) continue;
        auto fmt = p->second->getFormattingFlag();
        if (fmt == llvm::cl::Prefix || fmt == llvm::cl::AlwaysPrefix) return true;
    }
    return false;
}

void printOptionErrors(const std::string& text) {
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        // llvm::cl prefixes each line with the program's name.
        size_t colon = line.find(": ");
        if (colon != std::string::npos && line.find(' ') > colon) line = line.substr(colon + 2);
        if (line.empty()) continue;
        static const std::string unknown = "Unknown command line argument '";
        if (line.compare(0, unknown.size(), unknown) == 0) {
            size_t end = line.find('\'', unknown.size());
            std::string opt = line.substr(unknown.size(), end - unknown.size());
            std::cerr << "error: unknown option '" << opt << "' (see 'eskiuc --help')" << std::endl;
        } else if (line.compare(0, 13, "Did you mean ") == 0) {
            std::cerr << "note: did you mean " << line.substr(13) << std::endl;
        } else {
            std::cerr << "error: " << line << std::endl;
        }
    }
}
