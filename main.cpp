#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <climits>
#include <set>
#include <filesystem>
#ifdef __APPLE__
  #include <mach-o/dyld.h>
#elif defined(__linux__)
  #include <unistd.h>
#endif
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#ifdef _WIN32
  #include "llvm/Support/thread.h"
#else
  #include <pthread.h>
#endif
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/TargetParser/Triple.h"
#include "lexer/lexer.h"
#include "parser/parser.h"
#include "ast/ast_printer.h"
#include "sema/type_checker.h"
#include "sema/async_transform.h"
#include "codegen/codegen.h"
#include "phase_timer.h"
#include "eir/eir_builder.h"
#include "eir/eir_vm.h"
#include "eir/eir_asm.h"
#include "eir/eir_c.h"
#include "eir/eir_obj.h"
#include "eir/eir_runtime.h"
#include "main_support.h"
#include "diag_json.h"
#include "repl.h"
#include "pkg.h"
#include "cli.h"
#include "main_options.h"
#include "main_build_options.h"
#include "main_modes.h"
#include "main_tools.h"
#include "main_compile.h"

// Parse the compiler's options; an error is printed as `error: ...` lines (cli.cpp) and
// returns false (exit code 2). `-h` is `--help`.
static bool parseOptions(int argc, char** argv) {
    std::vector<char*> args(argv, argv + argc);
    g_cacheArgv.assign(argv + 1, argv + argc);
    static char help[] = "--help";
    for (int i = 1; i < argc; ++i) {
        if (std::string(args[i]) == "--") break;
        if (std::string(args[i]) == "-h") args[i] = help;
    }
    llvm::cl::HideUnrelatedOptions(EskiuCat);
    std::string errText;
    llvm::raw_string_ostream errs(errText);
    if (llvm::cl::ParseCommandLineOptions(argc, args.data(), OVERVIEW, &errs)) return true;
    errs.flush();
    printOptionErrors(errText);
    return false;
}

static int compilerMain(int argc, char** argv) {
    llvm::InitLLVM X(argc, argv);

    // Set version string for LLVM's built-in --version
    llvm::cl::SetVersionPrinter([](llvm::raw_ostream& os) {
        os << "Eskiu " << VERSION << " (LLVM " << LLVM_VERSION_MAJOR << "."
           << LLVM_VERSION_MINOR << "." << LLVM_VERSION_PATCH << ")\n";
    });

    // `eskiuc CMD --help`: the subcommand's usage, before it does anything (cli.cpp).
    if (int rc = 0; cliSubcommandHelp(argc, argv, rc)) return rc;

    // Package commands (new, add, build, fetch, test, and run inside a package): pkg.cpp.
    if (int rc = 0; pkgCommand(argc, argv, rc)) return rc;

    // `eskiuc doctor`: what this machine has for each kind of build (doctor.cpp).
    if (argc >= 2 && std::string(argv[1]) == "doctor") {
        if (argc > 2) {
            std::string a = argv[2];
            return cliUsageError("doctor", (a[0] == '-' ? "unknown option '" : "unexpected argument '") + a + "'");
        }
        std::string v = std::string(VERSION) + " (LLVM " + std::to_string(LLVM_VERSION_MAJOR) + "." +
                        std::to_string(LLVM_VERSION_MINOR) + "." + std::to_string(LLVM_VERSION_PATCH) + ")";
        return doctorCommand(v);
    }

    // `eskiuc fmt [--check] file.esk …` — reformat files in place. Handled before
    // option parsing; it does not use the compiler pipeline.
    if (argc >= 2 && std::string(argv[1]) == "fmt") {
        bool check = false;
        int width = 0;
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            std::string val;
            bool isWidth = false;
            if (a == "--width") {
                if (i + 1 >= argc) return cliUsageError("fmt", "--width needs a value");
                val = argv[++i];
                isWidth = true;
            }
            else if (a.rfind("--width=", 0) == 0) { val = a.substr(8); isWidth = true; }
            if (isWidth) {
                width = val.empty() || val.size() > 5 ? -1 : 0;
                for (char c : val)
                    if (width >= 0) width = std::isdigit((unsigned char)c) ? width * 10 + (c - '0') : -1;
                if (width < 20)
                    return cliUsageError("fmt", "--width needs a number of at least 20, not '" + val + "'");
            }
            else if (a == "--check") check = true;
            else if (a.size() > 1 && a[0] == '-') return cliUsageError("fmt", "unknown option '" + a + "'");
            else files.push_back(a);
        }
        return runFmt(files, check, width);
    }

    // `eskiuc repl [file.esk ...]`: the interactive session (repl.cpp). The files are
    // loaded first, as with `:load`.
    if (argc >= 2 && std::string(argv[1]) == "repl") {
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a.size() > 1 && a[0] == '-') return cliUsageError("repl", "unknown option '" + a + "'");
            files.push_back(a);
        }
        return runRepl(files);
    }

    // `eskiuc run script.esk [args...]` — compile to a temporary executable, run
    // it forwarding [args...], then delete it. Enables shebang scripts
    // (`#!/usr/bin/env -S eskiuc run`). Rewritten here before option parsing: any
    // leading flags and the first non-flag token (the script) go to the option
    // parser; everything after the script becomes the program's argv.
    if (argc >= 2 && std::string(argv[1]) == "run") {
        g_runMode = true;
        // Options that take their value as the next argument (`-o out`, `--target T`):
        // that argument is the option's value, not the script.
        static const std::set<std::string> valueOpts = {
            "-o", "-target", "--target", "-mcpu", "--mcpu", "-mattr", "--mattr",
            "-reloc", "--reloc", "-link-arg", "--link-arg", "-hover-at", "--hover-at",
            "-definition-at", "--definition-at", "-references-at", "--references-at", "-complete-at", "--complete-at", "-l", "-L",
            "-diagnostics", "--diagnostics",
        };
        std::vector<char*> clArgv = { argv[0] };
        bool gotScript = false;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (!gotScript) {
                if (a == "--") {                              // end of compiler flags
                    if (i + 1 < argc) { clArgv.push_back(argv[++i]); gotScript = true; }
                    continue;
                }
                clArgv.push_back(argv[i]);
                if (valueOpts.count(a) && i + 1 < argc) { clArgv.push_back(argv[++i]); continue; }
                if (a[0] != '-') gotScript = true;           // first non-flag = the script
            } else if (g_runArgs.empty() && a == "--" && !sawSeparator) {
                sawSeparator = true;                          // `run f.esk -- args`: drop the `--`
            } else {
                g_runArgs.push_back(argv[i]);
            }
        }
        int newArgc = (int)clArgv.size();
        if (!parseOptions(newArgc, clArgv.data())) return kExitUsage;
    } else {
        if (!parseOptions(argc, argv)) return kExitUsage;
    }
    for (const std::string& o : Overlays) {
        size_t eq = o.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == o.size()) {
            std::cerr << "error: invalid --overlay value '" << o << "' (use PATH=FILE)" << std::endl;
            return 2;
        }
        Parser::overlays[Parser::canonicalPath(o.substr(0, eq))] = o.substr(eq + 1);
    }
    if (TimePhases) enablePhaseTiming();
    if (Diagnostics == "json") installJsonDiagnostics();
    else if (Diagnostics != "text") {
        std::cerr << "error: invalid --diagnostics value '" << std::string(Diagnostics) << "' (use text or json)" << std::endl;
        return kExitUsage;
    }
    // One spelling per target from here on: `x86_64-w64-mingw32` is `x86_64-w64-windows-gnu`,
    // which every check below (the C ABI, the predefined macros, the linker) recognizes.
    if (!TargetTriple.empty()) TargetTriple.setValue(llvm::Triple::normalize(TargetTriple));

    // Resolve stdlib root once — used by all parsers for import <name>
    stdlibRoot = resolveStdlibPath();

    // Check input file provided
    if (InputFilename.empty()) {
        std::cerr << "error: no input file specified (see 'eskiuc --help')" << std::endl;
        return kExitUsage;
    }
    if (FpContract != "off" && FpContract != "fast") {
        std::cerr << "error: invalid -ffp-contract value '" << std::string(FpContract)
                  << "' (use off or fast)" << std::endl;
        return kExitUsage;
    }
    if (OptLevel > 3) {
        std::cerr << "error: invalid optimization level '-O" << OptLevel
                  << "' (use -O0, -O1, -O2 or -O3)" << std::endl;
        return kExitUsage;
    }
    unsigned safeChecks = 0;
    if (!safeChecksFromFlag(safeChecks)) return kExitUsage;
    if (!Subsystem.empty() && Subsystem != "console" && Subsystem != "windows") {
        std::cerr << "error: invalid --subsystem '" << Subsystem << "' (use console or windows)" << std::endl;
        return kExitUsage;
    }
    // Refuse an output path that names one of the inputs: `-o prog.esk` would
    // silently replace the source with an object file or executable.
    if (!OutputFilename.empty()) {
        std::string outCanon = Parser::canonicalPath(std::string(OutputFilename));
        std::vector<std::string> ins = { std::string(InputFilename) };
        for (const auto& f : ExtraInputs) ins.push_back(f);
        for (const auto& f : ins) {
            if (Parser::canonicalPath(f) == outCanon) {
                std::cerr << "error: output file '" << std::string(OutputFilename)
                          << "' would overwrite the input '" << f << "'" << std::endl;
                return 1;
            }
        }
    }

    // An output in a directory that does not exist yet (`-o build/app.html`) gets it, as the
    // package commands make build/; the C driver and emcc would fail instead.
    for (const std::string& out : { std::string(OutputFilename), std::string(EmitHeader) }) {
        if (out.empty() || out == "-") continue;
        std::filesystem::path dir = std::filesystem::path(out).parent_path();
        std::error_code ec;
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    }

    if (EirAssemble) {
        if (OutputFilename.empty()) {
            std::cerr << "error: --eir-assemble needs -o" << std::endl;
            return 1;
        }
        return eir::assembleEirFile(std::string(InputFilename), std::string(OutputFilename),
                                    std::string(TargetTriple)) ? 0 : 1;
    }

    // Handle --test-lexer
    if (TestLexer) {
        return testLexer(InputFilename);
    }

    // Handle --test-parser
    if (TestParser) {
        return testParser(InputFilename);
    }

    // Handle --test-typechecker
    if (TestTypeChecker) {
        return testTypeChecker(InputFilename);
    }

    if (!HoverAt.empty()) return hoverAtMode();

    if (!DefinitionAt.empty()) return definitionAtMode();

    if (!ReferencesAt.empty()) return referencesAtMode();

    if (!CompleteAt.empty()) return completeAtMode();

    if (Symbols) return symbolsMode();

    // Handle --test-codegen
    if (TestCodegen) {
        return testCodegen(InputFilename);
    }

    // --test-eir, --run-eir, --emit-asm, --emit-c. The assembly has no -O levels and none
    // of the code-generation options, as for the asm path of --backend=eir.
    if (EmitAsm && !checkEirOptions(safeChecks)) return 1;
    if (TestEir || RunEir || EmitAsm || EmitC) {
        return eirMode(InputFilename);
    }

    // --emit-obj is the EIR backend writing an object file.
    bool useEir = EmitObj || Backend == BackendKind::EIR;
    if (useEir && !checkEirOptions(safeChecks)) return 1;

    int rc = compileProgram(safeChecks, useEir);
    phaseTotal();
    return rc;
}

// The parser, the type checker, the async transform and codegen recurse once per nesting
// level of the source (parentheses, blocks, nested ifs and lambdas), so the pipeline runs
// on a thread with a large stack: deep input then reaches the parser's nesting limit
// (Parser::kMaxNesting) instead of overflowing a default 8 MB main-thread stack. The
// stack is reserved address space; pages are only touched as deep input needs them. If
// the thread cannot be created, the compiler runs on the current thread.
static constexpr unsigned kPipelineStackBytes = 1u << 30;   // 1 GB

#ifndef _WIN32
namespace {
struct MainArgs { int argc; char** argv; int rc; };
void* runCompilerMain(void* p) {
    auto* a = static_cast<MainArgs*>(p);
    a->rc = compilerMain(a->argc, a->argv);
    return nullptr;
}
}  // namespace
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    int rc = 1;
    llvm::thread worker(std::optional<unsigned>(kPipelineStackBytes),
                        [&] { rc = compilerMain(argc, argv); });
    worker.join();
    flushJsonDiagnostics();
    return rc;
#else
    MainArgs args{argc, argv, 1};
    pthread_attr_t attr;
    pthread_t tid;
    if (pthread_attr_init(&attr) == 0) {
        bool started = false;
        // Try requested large stack (1 GB), then graceful fallbacks (64 MB, 16 MB)
        // if virtual memory limits prevent 1 GB allocation.
        for (unsigned stackBytes : { kPipelineStackBytes, 64u << 20, 16u << 20 }) {
            if (pthread_attr_setstacksize(&attr, stackBytes) == 0 &&
                pthread_create(&tid, &attr, runCompilerMain, &args) == 0) {
                started = true;
                break;
            }
        }
        pthread_attr_destroy(&attr);
        if (started) {
            pthread_join(tid, nullptr);
            flushJsonDiagnostics();
            return args.rc;
        }
    }
    std::cerr << "warning: could not allocate dedicated stack thread; falling back to caller stack\n";
    int rc = compilerMain(argc, argv);
    flushJsonDiagnostics();
    return rc;
#endif
}
