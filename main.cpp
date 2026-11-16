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
#include "main_options.h"
#include "main_modes.h"

static int compilerMain(int argc, char** argv) {
    llvm::InitLLVM X(argc, argv);

    // Set version string for LLVM's built-in --version
    llvm::cl::SetVersionPrinter([](llvm::raw_ostream& os) {
        os << "Eskiu " << VERSION << " (LLVM " << LLVM_VERSION_MAJOR << "."
           << LLVM_VERSION_MINOR << "." << LLVM_VERSION_PATCH << ")\n";
    });

    // Package commands (new, add, build, fetch, test, and run inside a package): pkg.cpp.
    if (int rc = 0; pkgCommand(argc, argv, rc)) return rc;

    // `eskiuc doctor`: what this machine has for each kind of build (doctor.cpp).
    if (argc == 2 && std::string(argv[1]) == "doctor") {
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
            if (a == "--width" && i + 1 < argc) { val = argv[++i]; isWidth = true; }
            else if (a.rfind("--width=", 0) == 0) { val = a.substr(8); isWidth = true; }
            if (isWidth) {
                width = val.empty() || val.size() > 5 ? -1 : 0;
                for (char c : val)
                    if (width >= 0) width = std::isdigit((unsigned char)c) ? width * 10 + (c - '0') : -1;
                if (width < 20) {
                    std::cerr << "error: fmt: --width needs a number of at least 20, not '" << val << "'\n";
                    return 1;
                }
            }
            else if (a == "--check") check = true;
            else files.push_back(a);
        }
        return runFmt(files, check, width);
    }

    // `eskiuc repl [file.esk ...]`: the interactive session (repl.cpp). The files are
    // loaded first, as with `:load`.
    if (argc >= 2 && std::string(argv[1]) == "repl") {
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) files.push_back(argv[i]);
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
        llvm::cl::HideUnrelatedOptions(EskiuCat);
        llvm::cl::ParseCommandLineOptions(newArgc, clArgv.data(), OVERVIEW);
    } else {
        llvm::cl::HideUnrelatedOptions(EskiuCat);
        llvm::cl::ParseCommandLineOptions(argc, argv, OVERVIEW);
    }
    if (Diagnostics == "json") installJsonDiagnostics();
    else if (Diagnostics != "text") {
        std::cerr << "error: invalid --diagnostics value '" << std::string(Diagnostics) << "' (use text or json)" << std::endl;
        return 1;
    }
    // One spelling per target from here on: `x86_64-w64-mingw32` is `x86_64-w64-windows-gnu`,
    // which every check below (the C ABI, the predefined macros, the linker) recognizes.
    if (!TargetTriple.empty()) TargetTriple.setValue(llvm::Triple::normalize(TargetTriple));

    // Resolve stdlib root once — used by all parsers for import <name>
    stdlibRoot = resolveStdlibPath();

    // Check input file provided
    if (InputFilename.empty()) {
        std::cerr << "error: no input file specified" << std::endl;
        return 1;
    }
    if (FpContract != "off" && FpContract != "fast") {
        std::cerr << "error: invalid -ffp-contract value '" << std::string(FpContract)
                  << "' (use off or fast)" << std::endl;
        return 1;
    }
    if (OptLevel > 3) {
        std::cerr << "error: invalid optimization level '-O" << OptLevel
                  << "' (use -O0, -O1, -O2 or -O3)" << std::endl;
        return 1;
    }
    unsigned safeChecks = 0;
    if (!safeChecksFromFlag(safeChecks)) return 1;
    if (!Subsystem.empty() && Subsystem != "console" && Subsystem != "windows") {
        std::cerr << "error: invalid --subsystem '" << Subsystem << "' (use console or windows)" << std::endl;
        return 1;
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

    // Handle --hover-at LINE:COL
    if (!HoverAt.empty()) {
        int line = 0, col = 0;
        if (sscanf(HoverAt.c_str(), "%d:%d", &line, &col) != 2) {
            std::cerr << "error: --hover-at expects LINE:COL format\n"; return 1;
        }
        auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
        if (!program) { std::cout << "(parse error)\n"; return 0; }
        try {
            TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
            tc.sourceFile = std::string(InputFilename);
            bool ok = tc.check(program.get());
            std::string type = tc.getTypeAtPosition(line, col);
            if (type.empty()) {
                if (!ok) std::cout << "(semantic error)\n";
                else     std::cout << "(no type at " << line << ":" << col << ")\n";
            } else {
                std::cout << type << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "error: internal error during hover: " << e.what() << "\n";
            std::cout << "(internal error)\n";
            return 1;
        } catch (...) {
            std::cerr << "error: unknown internal error during hover\n";
            std::cout << "(internal error)\n";
            return 1;
        }
        return 0;
    }

    // Handle --definition-at LINE:COL
    if (!DefinitionAt.empty()) {
        int line = 0, col = 0;
        if (sscanf(DefinitionAt.c_str(), "%d:%d", &line, &col) != 2) {
            std::cerr << "error: --definition-at expects LINE:COL format\n"; return 1;
        }
        auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
        if (!program) { std::cout << "(parse error)\n"; return 0; }
        try {
            TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
            tc.sourceFile = std::string(InputFilename);
            bool ok = tc.check(program.get());
            std::string loc = tc.getDefinitionAt(line, col);
            if (loc.empty()) {
                if (!ok) std::cout << "(semantic error)\n";
                else     std::cout << "(no definition at " << line << ":" << col << ")\n";
            } else {
                std::cout << loc << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "error: internal error during definition: " << e.what() << "\n";
            std::cout << "(internal error)\n";
            return 1;
        } catch (...) {
            std::cerr << "error: unknown internal error during definition\n";
            std::cout << "(internal error)\n";
            return 1;
        }
        return 0;
    }

    // Handle --references-at LINE:COL
    if (!ReferencesAt.empty()) {
        int line = 0, col = 0;
        if (sscanf(ReferencesAt.c_str(), "%d:%d", &line, &col) != 2) {
            std::cerr << "error: --references-at expects LINE:COL format\n"; return 1;
        }
        auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
        if (!program) { std::cout << "(parse error)\n"; return 0; }
        try {
            TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
            tc.sourceFile = std::string(InputFilename);
            bool ok = tc.check(program.get());
            auto refs = tc.getReferencesAt(line, col);
            if (refs.empty()) std::cout << (ok ? "(no symbol at " + std::to_string(line) + ":" + std::to_string(col) + ")"
                                                : std::string("(semantic error)")) << "\n";
            for (const auto& r : refs) std::cout << r << "\n";
        } catch (const std::exception& e) {
            std::cerr << "error: internal error during references: " << e.what() << "\n";
            std::cout << "(internal error)\n";
            return 1;
        }
        return 0;
    }

    // Handle --complete-at LINE:COL: the identifier being typed at the cursor (or the
    // member after `recv.`) is replaced by TypeChecker::kCompleteSentinel, which
    // collects the candidates when the type checker reaches it.
    if (!CompleteAt.empty()) {
        int line = 0, col = 0;
        if (sscanf(CompleteAt.c_str(), "%d:%d", &line, &col) != 2) {
            std::cerr << "error: --complete-at expects LINE:COL format\n"; return 1;
        }
        std::string src = readFile(InputFilename);
        size_t ls = 0;
        for (int l = 1; l < line && ls != std::string::npos; ++l) {
            ls = src.find('\n', ls);
            if (ls != std::string::npos) ++ls;
        }
        if (ls == std::string::npos) ls = src.size();
        size_t le = src.find('\n', ls);
        if (le == std::string::npos) le = src.size();
        size_t at = std::min(le, ls + (size_t)std::max(0, col - 1));
        auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        size_t p = at, q = at;
        while (p > ls && isId(src[p - 1])) --p;
        while (q < le && isId(src[q])) ++q;
        bool restBlank = src.find_first_not_of(" \t\r", q) >= le;
        std::string spliced = src.substr(0, p) + TypeChecker::kCompleteSentinel +
                              (restBlank ? ";" : "") + src.substr(q);
        // A syntax error elsewhere in the file (the user is mid-edit): blank the broken
        // line and try again. An error at the first token of a line blames the line
        // before it (an unfinished statement is reported at the next token). A line that
        // opens or closes a block is kept, and so is the cursor's line.
        std::shared_ptr<Program> program;
        std::streambuf* errBuf = std::cerr.rdbuf();
        std::ostringstream quiet;
        auto lineSpan = [&](int ln, size_t& b, size_t& e) {
            b = 0;
            for (int l = 1; l < ln && b != std::string::npos; ++l) {
                b = spliced.find('\n', b);
                if (b != std::string::npos) ++b;
            }
            if (b == std::string::npos) return false;
            e = spliced.find('\n', b);
            if (e == std::string::npos) e = spliced.size();
            return true;
        };
        for (int attempt = 0; attempt < 8 && !program; ++attempt) {
            Parser::firstErrorLine() = 0;
            std::cerr.rdbuf(quiet.rdbuf());
            program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding,
                                  nullptr, nullptr, &spliced);
            std::cerr.rdbuf(errBuf);
            if (program) break;
            int bad = Parser::firstErrorLine();
            if (bad <= 0 || Parser::canonicalPath(Parser::firstErrorFile()) !=
                                Parser::canonicalPath(std::string(InputFilename))) break;
            size_t b = 0, e = 0;
            if (!lineSpan(bad, b, e)) break;
            size_t first = spliced.find_first_not_of(" \t", b);
            if (first == std::string::npos || first >= e || (int)(first - b) + 1 >= Parser::firstErrorCol()) {
                do {
                    --bad;
                } while (bad > 0 && lineSpan(bad, b, e) &&
                         spliced.find_first_not_of(" \t\r", b) >= e);
            }
            if (bad <= 0 || bad == line || !lineSpan(bad, b, e)) break;
            if (spliced.substr(b, e - b).find_first_of("{}") != std::string::npos) break;
            spliced.replace(b, e - b, std::string(e - b, ' '));
        }
        if (!program) { std::cout << "(parse error)\n"; return 0; }
        try {
            TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = false; tc.warnDefaults = false;
            tc.sourceFile = std::string(InputFilename);
            tc.check(program.get());
            for (const auto& c : tc.completions)
                std::cout << c.kind << "\t" << c.name << "\t" << c.detail << "\n";
        } catch (const std::exception& e) {
            std::cerr << "error: internal error during completion: " << e.what() << "\n";
            return 1;
        }
        return 0;
    }

    // Handle --symbols: the declarations of the input file (an editor's outline).
    if (Symbols) {
        auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
        if (!program) { std::cout << "(parse error)\n"; return 0; }
        std::string self = Parser::canonicalPath(std::string(InputFilename));
        auto mine = [&](const Decl* d) {
            return !d->fromImport && (d->sourceFile.empty() || Parser::canonicalPath(d->sourceFile) == self);
        };
        auto pos = [](int l, int c) { return std::to_string(l) + ":" + std::to_string(c); };
        auto sig = [](const FunctionDecl* f, const std::string& name) {
            std::string s = f->returnType + " " + name + "(";
            for (size_t i = 0; i < f->params.size(); ++i)
                s += (i ? ", " : "") + f->params[i].first +
                     (f->params[i].second.empty() ? "" : " " + f->params[i].second);
            return s + ")";
        };
        auto emit = [](const std::string& kind, const std::string& name, const std::string& at,
                       const std::string& container, const std::string& detail) {
            std::cout << kind << "\t" << name << "\t" << at << "\t" << container << "\t" << detail << "\n";
        };
        for (const auto& d : program->declarations) {
            if (!mine(d.get()) || d->line <= 0) continue;   // not an implicit one (a script's argc)
            if (auto* f = nodeCast<FunctionDecl>(d.get())) {
                emit("function", f->name, pos(f->line, f->col), "", sig(f, f->name));
            } else if (auto* s = nodeCast<StructDecl>(d.get())) {
                emit("struct", s->name, pos(s->line, s->col), "", "struct " + s->name);
                for (const auto& fl : s->fields)
                    emit("field", fl.name, pos(fl.line, fl.col), s->name, fl.type);
                for (const auto& m : s->methods)
                    if (auto* f = nodeCast<FunctionDecl>(m.get()))
                        emit("method", f->name, pos(f->line, f->col), s->name, sig(f, f->name));
            } else if (auto* u = nodeCast<UnionDecl>(d.get())) {
                emit("struct", u->name, pos(u->line, u->col), "", "union " + u->name);
                for (const auto& fl : u->fields)
                    emit("field", fl.name, pos(fl.line, fl.col), u->name, fl.type);
            } else if (auto* e = nodeCast<EnumDecl>(d.get())) {
                emit("enum", e->name, pos(e->line, e->col), "", "enum " + e->name);
            } else if (auto* i = nodeCast<InterfaceDecl>(d.get())) {
                emit("interface", i->name, pos(i->line, i->col), "", "interface " + i->name);
            } else if (auto* a = nodeCast<TypeAliasDecl>(d.get())) {
                emit("type", a->name, pos(a->line, a->col), "", "type " + a->name + " = " + a->aliased);
            } else if (auto* v = nodeCast<VarDecl>(d.get())) {
                emit(v->isConst ? "constant" : "variable", v->name, pos(v->line, v->col), "", v->type);
            }
        }
        return 0;
    }

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

    // Full compilation pipeline — parse every input file and merge their
    // top-level declarations into a single program (`eskiuc a.esk b.esk ...`).
    try {
        std::map<std::string, Macro> macros;
        bool lexFailed = false;
        phaseMark("start");
        auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding,
                                   &macros, &lexFailed);
        phaseMark("load+parse");
        if (!program) {
            if (!lexFailed && !g_jsonDiagnostics) std::cerr << "error: parse failed" << std::endl;
            return 1;
        }
        const std::vector<std::string>& pragmaLibs = program->linkLibs;

        TypeChecker typeChecker; typeChecker.targetTriple = std::string(TargetTriple);
        typeChecker.sourceFile = std::string(InputFilename);
        typeChecker.warnAll = Wall;
        typeChecker.warnDeprecated = !WnoDeprecated;
        typeChecker.warnExtra = Wextra;
        applyWarningFlags(typeChecker);
        if (!typeChecker.check(program.get())) {
            return 1;
        }
        phaseMark("typecheck");
        if (!EmitHeader.empty()) return writeCHeader(typeChecker, program.get());

        // Single resolver: re-resolve the post-transform AST; codegen consumes it. The EIR
        // builder needs the per-instance records only a checker with recordInstanceTypes
        // keeps, so it always gets a second run.
        std::unique_ptr<TypeChecker> postOwn;
        TypeChecker* post = &typeChecker;
        if (useEir || programHasAsync(program.get())) {
            AsyncTransform(&typeChecker.expressionTypeMap(), &typeChecker.instanceArgsMap(), &typeChecker.instanceExprTypeMap()).run(program.get());
            phaseMark("async-lower");
            postOwn = std::make_unique<TypeChecker>();
            TypeChecker& postTc = *postOwn;
            postTc.targetTriple = std::string(TargetTriple); postTc.warnDeprecated = false; postTc.warnDefaults = false; postTc.sourceFile = std::string(InputFilename);
            postTc.recordInstanceTypes = useEir;
            if (!postTc.check(program.get())) {
                std::cerr << "error: internal: the async lowering produced a program that does not type-check" << std::endl;
                return 1;
            }
            post = &postTc;
            phaseMark("re-check");
        }
        CodeGen codegen;
        std::unique_ptr<eir::Module> eirModule;
        if (useEir) {
            eirModule = lowerToEir(program.get(), *post);
            if (!eirModule) return 1;
        } else {
            codegen.resolvedExprTypes = &post->expressionTypeMap();
            codegen.semaInstanceArgs = &post->instanceArgsMap();
            if (!TargetTriple.empty()) codegen.targetTriple = std::string(TargetTriple);
            if (!TargetCPU.empty()) codegen.targetCPU = std::string(TargetCPU);
            if (!TargetFeatures.empty()) codegen.targetFeatures = std::string(TargetFeatures);
            if (!RelocModel.empty()) codegen.relocModel = std::string(RelocModel);
            codegen.freestanding = Freestanding;
            {
                std::string o = std::string(OutputFilename);
                codegen.sharedLibrary = !g_runMode && (endsWith(o, ".so") || endsWith(o, ".dylib") || endsWith(o, ".dll"));
            }
            codegen.safeChecks = safeChecks;
            codegen.asan = Asan;
            codegen.ubsan = Ubsan;
            codegen.optLevel = OptLevel;
            codegen.fpContract = FpContract == "fast";
            codegen.strictAliasing = StrictAliasing && !NoStrictAliasing;
            if (!codegen.generateCode(program)) {
                std::cerr << "error: code generation failed" << std::endl;
                return 1;
            }
            if (OptLevel) codegen.optimizeModule();
            phaseMark("optimize");
        }
        auto emitObject = [&](const std::string& path) {
            bool ok = eirModule ? eir::emitEirObject(eirModule.get(), path, std::string(TargetTriple), OptLevel)
                                : codegen.emitObjectFile(path);
            phaseMark("emit-object");
            return ok;
        };

        // `eskiuc run`: link into a temporary executable, then run it.
        std::string runExePath;
        if (g_runMode) {
            llvm::SmallString<128> tmpExe;
#ifdef _WIN32
            // The Windows linker driver appends ".exe" to a name without one.
            const char* runSuffix = "exe";
#else
            const char* runSuffix = "";
#endif
            if (llvm::sys::fs::createTemporaryFile("eskiu-run", runSuffix, tmpExe)) {
                std::cerr << "error: could not create a temporary executable" << std::endl;
                return 1;
            }
            runExePath = std::string(tmpExe.str());
        }

        std::string outFile = !runExePath.empty() ? runExePath
            : OutputFilename.empty() ? std::string(InputFilename) + ".o"
            : std::string(OutputFilename);

        // Link into an executable when the output is not an object file.
        // Object-only when: -c is given, the output ends in .o or .obj, no -o was given,
        // or --freestanding (bare-metal needs a custom linker script: link yourself).
        bool linkExe = g_runMode || (!CompileOnly && !EmitObj && !Freestanding &&
                       !OutputFilename.empty() && !endsWith(outFile, ".o") &&
                       !endsWith(outFile, ".obj") && !endsWith(outFile, ".a") &&
                       !endsWith(outFile, ".lib"));

        // A static library (.a, .lib): the object in an archive, for a host build system
        // (an iOS xcframework, an Android or emscripten build) to link.
        if (!g_runMode && !CompileOnly && (endsWith(outFile, ".a") || endsWith(outFile, ".lib"))) {
            llvm::SmallString<128> tmpObj;
            if (llvm::sys::fs::createTemporaryFile("eskiu", "o", tmpObj)) {
                std::cerr << "error: could not create a temporary object file" << std::endl;
                return 1;
            }
            std::string tmpObjPath(tmpObj.str());
            bool ok = emitObject(tmpObjPath) &&
                      writeStaticLibrary(tmpObjPath, outFile, std::string(TargetTriple));
            llvm::sys::fs::remove(tmpObjPath);
            return ok ? 0 : 1;
        }

        if (linkExe) {
            llvm::SmallString<128> tmpObj;
            if (llvm::sys::fs::createTemporaryFile("eskiu", "o", tmpObj)) {
                std::cerr << "error: could not create a temporary object file" << std::endl;
                return 1;
            }
            std::string tmpObjPath(tmpObj.str());
            if (!emitObject(tmpObjPath)) {
                llvm::sys::fs::remove(tmpObjPath);
                return 1;
            }
            std::vector<std::string> libs(LinkLibs.begin(), LinkLibs.end());
            std::vector<std::string> paths(LinkPaths.begin(), LinkPaths.end());
            std::vector<std::string> extra(LinkArgs.begin(), LinkArgs.end());
            // ASan needs its runtime linked; --ubsan traps directly (no runtime).
            if (Asan) extra.push_back("-fsanitize=address");
            // The libraries the program implies go after every object (a --link-arg
            // object may need them too), skipping any -l already given.
            if (!NoDefaultLibs) {
                bool usesEH = false, usesThreads = false;
                if (eirModule) {
                    usesThreads = eir::moduleUsesThreads(eirModule.get());
                } else {
                    llvm::Module* mod = codegen.getModule();
                    usesEH = mod->getFunction("__cxa_throw") || mod->getFunction("__cxa_rethrow") ||
                             mod->getFunction("__gxx_personality_v0") ||
                             mod->getFunction("__gxx_personality_seh0");
                    usesThreads = mod->getFunction("pthread_create") != nullptr;
                }
                std::vector<std::string> implied = pragmaLibs;
                bool libcxx = usesEH && macros.count("_WIN32") &&
                              mingwLibcxxDriver(findCDriver(false, nullptr, std::string(TargetTriple)));
                for (const auto& l : implicitLinkLibs(macros, usesEH, usesThreads))
                    implied.push_back(libcxx && l == "stdc++" ? "c++" : l);
                // emscripten catches exceptions only when linked to (its JavaScript runtime).
                if (usesEH && macros.count("__EMSCRIPTEN__")) extra.push_back("-sDISABLE_EXCEPTION_CATCHING=0");
                std::set<std::string> seen(libs.begin(), libs.end());
                for (const auto& l : implied)
                    if (seen.insert(l).second) extra.push_back("-l" + l);
            }
            // On macOS the object records a minimum OS version (the native triple carries the
            // host's); link for the same one, or ld warns that the object is newer than the
            // version the C driver picked from its SDK.
            if (!eirModule) {
                llvm::Triple t(codegen.getModule()->getTargetTriple());
                llvm::VersionTuple v;
                if (t.isMacOSX() && t.getMacOSXVersion(v) && v.getMajor() > 0)
                    extra.push_back("-mmacosx-version-min=" + v.getAsString());
            } else {
                std::string v = eir::eirLinkMacOSVersion(std::string(TargetTriple), OptLevel);
                if (!v.empty()) extra.push_back("-mmacosx-version-min=" + v);
            }
            // --subsystem=windows on a Windows target: the GUI subsystem, entered at main.
            if (Subsystem == "windows" && macros.count("_WIN32")) {
                std::string tt = std::string(TargetTriple);
                if (tt.find("msvc") != std::string::npos) {
                    extra.push_back("-Wl,/subsystem:windows");
                    extra.push_back("-Wl,/entry:mainCRTStartup");
                } else {
                    extra.push_back("-mwindows");
                }
            }
            // A build inside a package finds the native libraries it links in the archives
            // its packages declare for the target (or host) platform (pkg.cpp): -L, an rpath
            // for run time (Windows: the DLLs are copied next to the program).
            bool winTarget = macros.count("_WIN32") > 0;
            std::vector<std::string> nativeDirs, systemDirs;
            {
                std::vector<std::string> linked(LinkLibs.begin(), LinkLibs.end());
                if (!NoDefaultLibs) linked.insert(linked.end(), pragmaLibs.begin(), pragmaLibs.end());
                try {
                    nativeDirs = pkgNativeLibDirs(linked, &systemDirs);
                } catch (const std::exception& e) {
                    std::cerr << "error: " << e.what() << std::endl;
                    llvm::sys::fs::remove(tmpObjPath);
                    return 1;
                }
                bool rpath = !winTarget && !macros.count("__wasm__") && !macros.count("__ANDROID__") &&
                             !macros.count("TARGET_OS_IPHONE");
                for (const auto& d : nativeDirs) {
                    paths.push_back(d);
                    // An rpath to this machine's cache helps a program run here; an app for a
                    // phone carries its libraries in its package, so it gets none.
                    if (rpath) extra.push_back("-Wl,-rpath," + d);
                }
                // A system library in place of an archive (ESKIU_SYSTEM_LIBS, or an archive
                // that could not be downloaded): its directory, unless the linker and the
                // loader search it already.
                for (const auto& d : systemDirs) {
                    if (d == "/usr/lib" || d == "/lib" || d == "/usr/lib64" || d == "/lib64") continue;
                    paths.push_back(d);
                    if (rpath) extra.push_back("-Wl,-rpath," + d);
                }
            }
            bool ok = linkExecutable(tmpObjPath, outFile, libs, paths, extra, /*sanitized=*/Asan,
                                     std::string(TargetTriple));
            if (ok && winTarget) copyNativeDlls(nativeDirs, outFile);
            if (ok && macros.count("__EMSCRIPTEN__") && !g_runMode) pkgCopyWebAssets(outFile);
            llvm::sys::fs::remove(tmpObjPath);
            if (!ok) { if (g_runMode) llvm::sys::fs::remove(runExePath); return 1; }

            if (g_runMode) {
                int rc = runExecutable(runExePath, g_runArgs);
                llvm::sys::fs::remove(runExePath);
                return rc;
            }
            return 0;
        }

        if (!emitObject(outFile)) {
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
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
