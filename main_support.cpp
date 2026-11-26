#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <climits>
#include <set>
#include <cerrno>
#include <filesystem>
#ifndef _WIN32
  #include <unistd.h>
  #include <sys/wait.h>
#endif
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#elif defined(__linux__)
  #include <unistd.h>
#elif defined(_WIN32)
  // Declare just GetModuleFileNameA instead of including <windows.h>, whose macros
  // (VOID, CONST, TRUE, FALSE, FLOAT, IN, INTERFACE, ...) collide with our TokenType
  // enum. Plain types: HMODULE = void*, LPSTR = char*, DWORD = unsigned long.
  extern "C" __declspec(dllimport) unsigned long __stdcall
      GetModuleFileNameA(void* hModule, char* lpFilename, unsigned long nSize);
#endif
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Object/ArchiveWriter.h"
#include "llvm/Support/Path.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/TargetParser/Triple.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "ast/ast_printer.h"
#include "sema/type_checker.h"
#include "sema/async_transform.h"
#include "codegen/codegen.h"
#include "main_support.h"
#include "pkg.h"

// Definition of the shared stdlib-root global (declared in main_support.h).
std::string stdlibRoot;

// Resolve stdlib root: $ESKIU_ROOT env var, or dirname(argv[0])/../lib/eskiu
std::string resolveStdlibPath() {
    const char* env = std::getenv("ESKIU_ROOT");
    if (env && *env) return std::string(env);

    // Deduce from binary location
    char buf[4096] = {};
#ifdef __APPLE__
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
#elif defined(__linux__)
    if (readlink("/proc/self/exe", buf, sizeof(buf) - 1) > 0) {
#elif defined(_WIN32)
    if (GetModuleFileNameA(nullptr, buf, (unsigned long)sizeof(buf)) > 0) {
#else
    if (false) {
#endif
        std::string binPath(buf);
        // Accept either separator: GetModuleFileNameA returns backslashes.
        size_t slash = binPath.find_last_of("/\\");
        if (slash != std::string::npos) {
            // binary is at <prefix>/bin/eskiuc → look for <prefix>/lib/eskiu
            std::string binDir = binPath.substr(0, slash);
            size_t parentSlash = binDir.find_last_of("/\\");
            if (parentSlash != std::string::npos) {
                std::string prefix = binDir.substr(0, parentSlash);
                std::string candidate = prefix + "/lib/eskiu";
                std::ifstream probe(candidate + "/stdlib/result.esk");
                if (probe.good()) return candidate;
            }
            // Also try sibling directory (dev: build/ next to stdlib/)
            std::string devCandidate = binDir + "/..";
            std::ifstream probe2(devCandidate + "/stdlib/result.esk");
            if (probe2.good()) return devCandidate;
        }
    }
    return "";
}

// Read file contents into string
std::string readFile(const std::string& filename) {
    std::error_code ec;
    if (std::filesystem::is_directory(filename, ec)) {
        std::cerr << "error: '" << filename << "' is a directory, not a source file" << std::endl;
        exit(1);
    }
    std::string text;
    if (!Parser::readSource(filename, text)) {
        std::cerr << "error: could not open file '" << filename << "'" << std::endl;
        exit(1);
    }
    return text;
}

// Directory portion of a path (up to its last '/' or, for Windows paths, '\\'), or "." if
// there is none.
std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "." : path.substr(0, slash);
}
