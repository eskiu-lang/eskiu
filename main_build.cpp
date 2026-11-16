// main_build.cpp: predefined macros, loading the program, the C driver and linker, static libraries and running executables (split from main_support.cpp).
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

void seedPredefinedMacros(std::map<std::string, Macro>& macros, const std::string& triple,
                          bool freestanding) {
    // Predefine a platform macro so stdlib can #ifdef per OS (the event-loop
    // backend and sockaddr_in layout differ between macOS and Linux). It follows
    // the --target triple when cross-compiling, else the build host — otherwise a
    // `--target x86_64-linux-gnu` build on macOS would still select the kqueue
    // path and emit unresolved BSD symbols.
    Macro os; os.body = "1";
    const std::string& tt = triple;
    bool tgtLinux = tt.find("linux") != std::string::npos;
    bool tgtApple = tt.find("apple") != std::string::npos ||
                    tt.find("darwin") != std::string::npos ||
                    tt.find("macos") != std::string::npos;
    bool tgtWindows = tt.find("windows") != std::string::npos ||
                      tt.find("win32") != std::string::npos ||
                      tt.find("mingw") != std::string::npos;
    // _WIN64 accompanies _WIN32 on 64-bit Windows (MSVC keeps _WIN32 defined
    // for both widths and adds _WIN64 only when 64-bit).
    bool tgt64 = tt.find("x86_64") != std::string::npos ||
                 tt.find("amd64") != std::string::npos ||
                 tt.find("aarch64") != std::string::npos ||
                 tt.find("arm64") != std::string::npos;
    if (tgtLinux)        { macros["__linux__"] = os; }
    else if (tgtApple)   { macros["__APPLE__"] = os; }
    else if (tgtWindows) {
        macros["_WIN32"] = os;
        if (tgt64) macros["_WIN64"] = os;
    }
    else if (tt.empty()) {
        // Native build: follow the build host.
#if defined(_WIN32)
        macros["_WIN32"] = os;
#if defined(_WIN64)
        macros["_WIN64"] = os;
#endif
#elif defined(__APPLE__)
        macros["__APPLE__"] = os;
#elif defined(__linux__)
        macros["__linux__"] = os;
#endif
    }
    // else: an explicit bare-metal or otherwise non-hosted triple (e.g. the
    // 3DS's armv6k-none-eabihf) defines no OS macro. Bare metal has no host OS,
    // so portable code guards that path explicitly rather than falling through
    // to the build host's.

    // The architecture macro C compilers predefine (__aarch64__ / __x86_64__ /
    // __arm__), from the --target triple or else the build host. The self-hosted
    // compiler reads it at its own build to pick its C-ABI lowering.
    std::string arch;
    if (tt.rfind("aarch64", 0) == 0 || tt.rfind("arm64", 0) == 0) arch = "__aarch64__";
    else if (tt.rfind("x86_64", 0) == 0 || tt.rfind("amd64", 0) == 0) arch = "__x86_64__";
    else if (tt.rfind("arm", 0) == 0 || tt.rfind("thumb", 0) == 0) arch = "__arm__";
    else if (tt.empty()) {
#if defined(__aarch64__) || defined(_M_ARM64)
        arch = "__aarch64__";
#elif defined(__x86_64__) || defined(_M_X64)
        arch = "__x86_64__";
#elif defined(__arm__)
        arch = "__arm__";
#endif
    }
    if (!arch.empty()) macros[arch] = os;

    // WebAssembly, Android and iOS, spelled as clang predefines them.
    if (tt.rfind("wasm32", 0) == 0) {
        macros["__wasm__"] = os;
        macros["__wasm32__"] = os;
        if (tt.find("emscripten") != std::string::npos) macros["__EMSCRIPTEN__"] = os;
        if (tt.find("wasi") != std::string::npos) macros["__wasi__"] = os;
    }
    if (tt.find("android") != std::string::npos) macros["__ANDROID__"] = os;
    if (tt.find("-ios") != std::string::npos) {
        macros["TARGET_OS_IPHONE"] = os;
        macros["TARGET_OS_IOS"] = os;
        if (tt.find("simulator") != std::string::npos) macros["TARGET_IPHONE_SIMULATOR"] = os;
    }
    // visionOS has UIKit as iOS does: TARGET_OS_IPHONE and TARGET_OS_VISION, as Apple's
    // TargetConditionals.h sets them (TARGET_OS_IOS stays 0 there).
    if (tt.find("-xros") != std::string::npos || tt.find("-visionos") != std::string::npos) {
        macros["TARGET_OS_IPHONE"] = os;
        macros["TARGET_OS_VISION"] = os;
        if (tt.find("simulator") != std::string::npos) macros["TARGET_OS_SIMULATOR"] = os;
    }

    // Predefine __ESKIU_FREESTANDING__ under --freestanding so stdlib (e.g.
    // <mem>'s alloc/free) can target esk_alloc/esk_free instead of libc.
    if (freestanding) {
        Macro fs; fs.body = "1";
        macros["__ESKIU_FREESTANDING__"] = fs;
    }
}

// Load → lex → parse every input file and merge their declarations into one Program,
// or nullptr on a lexical or parse error (a diagnostic is printed by the lexer or
// parser; `lexFailed` tells which). Shared by the build and every pipeline mode
// (parse/typecheck/codegen, --hover-at, --definition-at), so they all preprocess alike
// (same predefined macros, one macro table across files and imports, each import
// preprocessed at its import line, __FILE__ = the path) and see the same program.
// `macrosOut` receives the final macro table.
std::shared_ptr<Program> loadProgram(const std::vector<std::string>& inputs, const std::string& triple,
                                     bool freestanding, std::map<std::string, Macro>* macrosOut,
                                     bool* lexFailed, const std::string* primarySource) {
    std::map<std::string, Macro> macros;     // shared: #defines propagate across files
    seedPredefinedMacros(macros, triple, freestanding);
    std::set<std::string> importedFiles;     // shared: a common import is parsed once
    // Shared too: a type or generic declared by an earlier input (or an import it parsed)
    // keeps `Name<T>(x)` and `(T)x` reading as such in a later input.
    std::set<std::string> typeNames, genericNames;
    ImportCache cache;                       // imports preprocessed at their import line
    cache.stdlibPath = stdlibRoot;
    cache.macros = &macros;
    std::vector<DeclPtr> merged;
    std::vector<std::string> linkLibs;       // `#pragma link` libraries of every input
    std::vector<Program::TypeRef> typeRefs;  // the type names written in the first input
    if (lexFailed) *lexFailed = false;
    if (!inputs.empty()) pkgLoadRoots(inputs[0], triple);   // package imports from eskiu.lock
    for (const auto& fname : inputs) {
        // Register the root file itself, so an import cycle back to it (or an
        // input that an earlier input already imported) is not parsed twice.
        std::string canon = Parser::canonicalPath(fname);
        if (!importedFiles.insert(canon).second) continue;
        cache.seen.insert(canon);
        // The first input may be given as text (editor tooling: an unsaved buffer).
        std::string source = primarySource && fname == inputs.front() ? *primarySource : readFile(fname);
        PPImportHook hook = cache.hookFor(dirOf(fname));
        Lexer lexer(source, &macros, fname, &hook);
        std::vector<Token> tokens;
        Token tok = lexer.next_token();
        while (tok.type != TokenType::EOF_TOKEN) {
            tokens.push_back(tok);
            tok = lexer.next_token();
        }
        tokens.push_back(tok);
        if (lexer.hadError) { if (lexFailed) *lexFailed = true; return nullptr; }

        Parser parser(tokens);
        parser.sourceText = &lexer.text();
        parser.filename = fname;
        parser.stdlibPath = stdlibRoot;
        parser.basedir = dirOf(fname);
        parser.macros = &macros;
        parser.importedFiles = &importedFiles;
        parser.importCache = &cache;
        parser.sharedTypeNames = &typeNames;
        parser.sharedGenericNames = &genericNames;
        auto prog = parser.parse();
        if (!prog) return nullptr;
        merged.insert(merged.end(), prog->declarations.begin(), prog->declarations.end());
        linkLibs.insert(linkLibs.end(), prog->linkLibs.begin(), prog->linkLibs.end());
        if (fname == inputs.front()) typeRefs = prog->typeRefs;
    }
    // A program has one main: a script's statements are its main, so no other input may
    // define one or have top-level statements too (one file is checked by its parser).
    const FunctionDecl* scriptMain = nullptr;
    const FunctionDecl* namedMain = nullptr;
    for (const auto& d : merged) {
        auto* f = nodeCast<FunctionDecl>(d.get());
        if (!f || f->name != "main" || f->fromImport) continue;
        bool script = f->params.size() == 2 && f->params[0].second == "__argc" && f->body;
        if (!script) { if (!namedMain) namedMain = f; continue; }
        if (scriptMain) {
            std::cerr << "error: " << f->sourceFile << ":" << f->body->line << ":" << f->body->col
                      << ": only one input file may have top-level statements (they are its main)\n";
            return nullptr;
        }
        scriptMain = f;
    }
    if (scriptMain && namedMain) {
        std::cerr << "error: " << scriptMain->sourceFile << ":" << scriptMain->body->line << ":"
                  << scriptMain->body->col << ": a file with top-level statements has no 'main': the statements are its main\n";
        return nullptr;
    }
    if (macrosOut) *macrosOut = macros;
    auto program = std::make_shared<Program>(merged);
    program->linkLibs = linkLibs;
    program->typeRefs = std::move(typeRefs);
    return program;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// The Android NDK: $ANDROID_NDK_HOME, $ANDROID_NDK_ROOT, else the newest version under
// the SDK's ndk/ directory ($ANDROID_HOME, $ANDROID_SDK_ROOT, or the SDK manager's
// default place in the home directory).
std::string androidNdk() {
    for (const char* v : {"ANDROID_NDK_HOME", "ANDROID_NDK_ROOT"})
        if (const char* p = std::getenv(v); p && *p) return p;
    std::vector<std::string> sdks;
    for (const char* v : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
        if (const char* p = std::getenv(v); p && *p) sdks.push_back(p);
    if (const char* home = std::getenv("HOME"); home && *home) {
        sdks.push_back(std::string(home) + "/Library/Android/sdk");
        sdks.push_back(std::string(home) + "/Android/Sdk");
    }
    for (const auto& sdk : sdks) {
        std::error_code ec;
        std::string best;
        for (auto it = std::filesystem::directory_iterator(sdk + "/ndk", ec);
             !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
            std::string name = it->path().filename().string();
            auto key = [](const std::string& s) {
                std::vector<long> parts;
                std::stringstream ss(s);
                for (std::string p; std::getline(ss, p, '.');) parts.push_back(std::atol(p.c_str()));
                return parts;
            };
            if (best.empty() || key(name) > key(best)) best = name;
        }
        if (!best.empty()) return sdk + "/ndk/" + best;
    }
    return "";
}

// The C driver a --target build for WebAssembly, Android, iOS or the other macOS
// architecture uses when $CC is not set, with the arguments that select the target: emcc
// for emscripten, clang with --target for the other wasm32 triples (the LLVM toolchain's
// own, which has the WebAssembly backend and wasm-ld), the NDK's clang for Android (from
// $ANDROID_NDK_HOME or $ANDROID_NDK_ROOT), `xcrun -sdk iphoneos clang` for iOS (`xros` for
// visionOS), and on a
// Mac `cc -target` for macOS.
static bool crossDriver(const std::string& triple, std::string& driver, std::vector<std::string>& args) {
    auto has = [&](const char* s) { return triple.find(s) != std::string::npos; };
    if (triple.rfind("wasm32", 0) == 0 && has("emscripten")) {
        if (auto p = llvm::sys::findProgramByName("emcc")) { driver = *p; return true; }
        return false;
    }
    if (triple.rfind("wasm32", 0) == 0) {
#ifdef ESKIU_LLVM_BINDIR
        std::string llvmClang = std::string(ESKIU_LLVM_BINDIR) + "/clang";
        if (llvm::sys::fs::exists(llvmClang)) driver = llvmClang;
#endif
        if (driver.empty())
            if (auto p = llvm::sys::findProgramByName("clang")) driver = *p;
        if (driver.empty()) return false;
        args = {"--target=" + triple};
        return true;
    }
    if (has("android")) {
        std::string ndk = androidNdk();
        if (ndk.empty()) return false;
        for (const char* host : {"darwin-x86_64", "linux-x86_64", "windows-x86_64"}) {
            std::string c = ndk + "/toolchains/llvm/prebuilt/" + host + "/bin/clang";
            if (llvm::sys::fs::exists(c) || llvm::sys::fs::exists(c + ".exe")) {
                driver = llvm::sys::fs::exists(c) ? c : c + ".exe";
                args = {"--target=" + triple};
                return true;
            }
        }
        return false;
    }
    if (has("-ios")) {
        auto p = llvm::sys::findProgramByName("xcrun");
        if (!p) return false;
        driver = *p;
        args = {"-sdk", has("simulator") ? "iphonesimulator" : "iphoneos", "clang", "-target", triple};
        return true;
    }
    if (has("-xros") || has("-visionos")) {
        auto p = llvm::sys::findProgramByName("xcrun");
        if (!p) return false;
        driver = *p;
        args = {"-sdk", has("simulator") ? "xrsimulator" : "xros", "clang", "-target", triple};
        return true;
    }
#ifdef __APPLE__
    // The other macOS architecture from a Mac (x86_64 from Apple silicon, or arm64 from an
    // Intel Mac): the system cc links it when told the target.
    if (has("apple") && (has("-macos") || has("-darwin"))) {
        auto p = llvm::sys::findProgramByName("cc");
        if (!p) return false;
        driver = *p;
        args = {"-target", triple};
        return true;
    }
#endif
    return false;
}

// What a --target build needs to link and did not find, as a message; empty when the target
// links with the host's driver or what it needs is there. The host's cc cannot link these.
static std::string missingCrossTool(const std::string& triple) {
    auto has = [&](const char* s) { return triple.find(s) != std::string::npos; };
    if (triple.rfind("wasm32", 0) == 0 && has("emscripten"))
        return "emcc was not found: install Emscripten (emsdk), activate it so emcc is on PATH "
               "(source emsdk_env.sh), or set CC";
    if (has("android"))
        return "the Android NDK was not found: set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) to it, or set CC";
    if (has("-ios") || has("-xros") || has("-visionos"))
        return "xcrun was not found: install Xcode (and its command line tools), or set CC";
    return "";
}

// Locate a C linker driver: $CC first, then cc / clang / gcc on PATH.
// When `sanitized` is set, prefer the clang from the LLVM toolchain this
// compiler was built against — its compiler-rt matches the ASan instrumentation
// we emit, so the runtime versions agree (Apple's system clang ships a different
// ASan ABI and would fail to link).
std::string findCDriver(bool sanitized, std::vector<std::string>* ccArgs, const std::string& triple) {
#ifdef ESKIU_LLVM_BINDIR
    if (sanitized) {
        std::string llvmClang = std::string(ESKIU_LLVM_BINDIR) + "/clang";
        if (llvm::sys::fs::exists(llvmClang)) return llvmClang;
    }
#endif
    if (const char* cc = std::getenv("CC"); cc && *cc) {
        // $CC may carry arguments ("clang -m64"): the first word is the program.
        std::vector<std::string> words;
        std::istringstream ws(cc);
        for (std::string w; ws >> w;) words.push_back(w);
        if (!words.empty()) {
            if (auto p = llvm::sys::findProgramByName(words[0])) {
                if (ccArgs) {
                    ccArgs->assign(words.begin() + 1, words.end());
#ifdef __APPLE__
                    // A --target for macOS (the other architecture) still needs selecting
                    // when $CC names the driver, unless its words already select one.
                    bool selects = false;
                    for (const auto& w : *ccArgs)
                        if (w == "-target" || w == "-arch" || w.rfind("--target=", 0) == 0) selects = true;
                    if (!selects && triple.find("apple") != std::string::npos &&
                        (triple.find("-macos") != std::string::npos || triple.find("-darwin") != std::string::npos))
                        ccArgs->insert(ccArgs->end(), {"-target", triple});
#endif
                }
                return *p;
            }
            std::cerr << "warning: $CC ('" << cc << "') was not found; "
                         "falling back to cc/clang/gcc" << std::endl;
        }
    }
    if (!triple.empty()) {
        std::string driver;
        std::vector<std::string> args;
        if (crossDriver(triple, driver, args)) {
            if (ccArgs) *ccArgs = args;
            return driver;
        }
        // The host's cc cannot link for these: linkExecutable names what is missing.
        if (!missingCrossTool(triple).empty()) return "";
    }
    for (const char* name : {"cc", "clang", "gcc"}) {
        if (auto p = llvm::sys::findProgramByName(name)) return *p;
    }
    // A toolchain installed beside the compiler (install.ps1 puts llvm-mingw in
    // <prefix>\toolchain next to <prefix>\bin\eskiuc.exe).
    std::string exe = llvm::sys::fs::getMainExecutable(nullptr, (void*)&resolveStdlibPath);
    llvm::SmallString<256> tc(llvm::sys::path::parent_path(llvm::sys::path::parent_path(exe)));
    llvm::sys::path::append(tc, "toolchain", "bin", "clang");
    for (const char* ext : {"", ".exe"})
        if (llvm::sys::fs::can_execute(std::string(tc) + ext)) return std::string(tc) + ext;
    return "";
}

// A MinGW driver whose C++ runtime is libc++ (llvm-mingw), not libstdc++: exceptions then
// link -lc++ (its libc++abi and libunwind come with it).
bool mingwLibcxxDriver(const std::string& driver) {
    llvm::SmallString<256> root(llvm::sys::path::parent_path(llvm::sys::path::parent_path(driver)));
    for (const char* arch : {"x86_64", "aarch64", "i686", "armv7"}) {
        llvm::SmallString<256> lib(root);
        llvm::sys::path::append(lib, std::string(arch) + "-w64-mingw32", "lib", "libc++.a");
        if (llvm::sys::fs::exists(lib)) return true;
    }
    return false;
}

// Link an object file into an executable by invoking the system C driver
// (the same thing rustc/clang do internally). Returns true on success.
bool linkExecutable(const std::string& obj, const std::string& out,
                           const std::vector<std::string>& libs,
                           const std::vector<std::string>& paths,
                           const std::vector<std::string>& extra,
                           bool sanitized, const std::string& triple) {
    std::vector<std::string> ccArgs;
    std::string driver = findCDriver(sanitized, &ccArgs, triple);
    if (driver.empty() && !missingCrossTool(triple).empty()) {
        std::cerr << "error: " << missingCrossTool(triple) << ".\n"
                     "       Or use -c to emit an object file and link it yourself.\n";
        return false;
    }
    if (driver.empty()) {
        std::cerr << "error: no C linker driver found (looked for $CC, cc, clang, gcc).\n"
                     "       Install a C toolchain, or use -c to emit an object file "
                     "and link it yourself.\n";
        return false;
    }
    std::vector<std::string> argv = {driver};
    argv.insert(argv.end(), ccArgs.begin(), ccArgs.end());
    argv.insert(argv.end(), {obj, "-o", out});
    // A shared library (.so, .dylib, .dll): the object is position-independent already.
    if (endsWith(out, ".so") || endsWith(out, ".dylib") || endsWith(out, ".dll")) argv.push_back("-shared");
    for (const auto& p : paths) argv.push_back("-L" + p);
    for (const auto& l : libs)  argv.push_back("-l" + l);
    for (const auto& a : extra) argv.push_back(a);

    std::vector<llvm::StringRef> args(argv.begin(), argv.end());
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(driver, args, /*Env=*/std::nullopt,
                                       /*Redirects=*/{}, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg);
    if (rc != 0) {
        std::cerr << "error: linking failed";
        if (!errMsg.empty()) std::cerr << ": " << errMsg;
        else                 std::cerr << " (" << driver << " exited with code " << rc << ")";
        std::cerr << std::endl;
        return false;
    }
    return true;
}

bool writeStaticLibrary(const std::string& obj, const std::string& out, const std::string& tripleStr) {
    llvm::Triple t(tripleStr.empty() ? llvm::sys::getDefaultTargetTriple() : tripleStr);
    auto member = llvm::NewArchiveMember::getFile(obj, /*Deterministic=*/true);
    if (!member) {
        std::cerr << "error: " << llvm::toString(member.takeError()) << std::endl;
        return false;
    }
    // The member is named after the library, not the temporary object.
    static llvm::BumpPtrAllocator alloc;
    static llvm::StringSaver saver(alloc);
    std::string base = llvm::sys::path::stem(out).str();
    member->MemberName = saver.save(base + (t.isOSWindows() ? ".obj" : ".o"));
    llvm::object::Archive::Kind kind = t.isOSDarwin() ? llvm::object::Archive::K_DARWIN
                                     : t.isOSWindows() ? llvm::object::Archive::K_COFF
                                     : llvm::object::Archive::K_GNU;
    std::vector<llvm::NewArchiveMember> members;
    members.push_back(std::move(*member));
    if (llvm::Error e = llvm::writeArchive(out, members, llvm::SymtabWritingMode::NormalSymtab,
                                           kind, /*Deterministic=*/true, /*Thin=*/false)) {
        std::cerr << "error: cannot write " << out << ": " << llvm::toString(std::move(e)) << std::endl;
        return false;
    }
    return true;
}

std::vector<std::string> implicitLinkLibs(const std::map<std::string, Macro>& macros,
                                          bool usesExceptions, bool usesThreads) {
    std::vector<std::string> libs;
    bool apple = macros.count("__APPLE__") > 0;
    bool android = macros.count("__ANDROID__") > 0;
    bool gnu = !android && (macros.count("__linux__") > 0 || macros.count("_WIN32") > 0);
    // Android: the NDK's static libc++ and its ABI library; pthread is in bionic's libc.
    if (usesExceptions) {
        if (apple) libs.push_back("c++");
        else if (android) { libs.push_back("c++_static"); libs.push_back("c++abi"); }
        else if (gnu) libs.push_back("stdc++");
    }
    if (usesThreads && gnu) libs.push_back("pthread");
    return libs;
}

// Run an executable, forwarding `progArgs`, and return its exit code: the program's
// own status, 128+N when it was killed by signal N (the shell convention), or 1 if it
// could not be launched. Used by `eskiuc run`.
int runExecutable(const std::string& exe, const std::vector<std::string>& progArgs) {
#ifndef _WIN32
    std::vector<char*> cargv;
    cargv.push_back(const_cast<char*>(exe.c_str()));
    for (const auto& a : progArgs) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) {
        std::cerr << "error: could not run '" << exe << "': fork failed" << std::endl;
        return 1;
    }
    if (pid == 0) {
        execv(exe.c_str(), cargv.data());
        std::cerr << "error: could not run '" << exe << "'" << std::endl;
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) { std::cerr << "error: waiting for '" << exe << "' failed" << std::endl; return 1; }
    }
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
#else
    std::vector<std::string> argv = {exe};
    for (const auto& a : progArgs) argv.push_back(a);
    std::vector<llvm::StringRef> args(argv.begin(), argv.end());
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(exe, args, /*Env=*/std::nullopt,
                                       /*Redirects=*/{}, /*SecondsToWait=*/0,
                                       /*MemoryLimit=*/0, &errMsg);
    if (rc < 0) {
        std::cerr << "error: could not run '" << exe << "'";
        if (!errMsg.empty()) std::cerr << ": " << errMsg;
        std::cerr << std::endl;
        return 1;
    }
    return rc;
#endif
}
