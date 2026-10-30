#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <climits>
#include <set>
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
#include "eir/eir_builder.h"
#include "eir/eir_vm.h"
#include "eir/eir_asm.h"
#include "eir/eir_c.h"
#include "eir/eir_obj.h"
#include "eir/eir_runtime.h"
#include "main_support.h"
#include "repl.h"
#include "pkg.h"

// Command line options. All of them live in one category so `--help` lists only
// Eskiu's options (not the ~200 LLVM internals linked in with the backend).
static llvm::cl::OptionCategory EskiuCat("Eskiu options");

static const char* OVERVIEW =
    "Eskiu Language Compiler\n\n"
    "  eskiuc file.esk [more.esk ...] -o prog   compile and link an executable\n"
    "  eskiuc file.esk -c -o file.o             compile to an object file\n"
    "  eskiuc run [flags] file.esk [--] [args]  compile to a temp executable and run it\n"
    "  eskiuc fmt [--check] file.esk ...        reindent files in place\n"
    "  eskiuc repl [file.esk ...]               interactive session (JIT)\n";
static llvm::cl::opt<std::string> InputFilename(llvm::cl::Positional,
                                                 llvm::cl::desc("<input .esk file>"),
    llvm::cl::cat(EskiuCat));

// Additional .esk files: `eskiuc a.esk b.esk -o prog` compiles them together.
static llvm::cl::list<std::string> ExtraInputs(llvm::cl::Positional,
                                               llvm::cl::desc("[additional .esk files]"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> OutputFilename("o",
                                                  llvm::cl::desc("Output filename"),
                                                  llvm::cl::value_desc("filename"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> TestLexer("test-lexer",
                                     llvm::cl::desc("Tokenize input and print token stream"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> TestParser("test-parser",
                                      llvm::cl::desc("Parse input and print AST"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> TestCodegen("test-codegen",
                                       llvm::cl::desc("Generate LLVM IR and print it"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> TestEir("test-eir",
                                   llvm::cl::desc("Generate EIR (Eskiu Intermediate Representation) and print it"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> RunEir("run-eir",
                                  llvm::cl::desc("Execute program using the EIR Virtual Machine"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> EmitAsm("emit-asm",
                                   llvm::cl::desc("Emit native assembly (.s) via EIR without LLVM"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> EmitC("emit-c",
                                 llvm::cl::desc("Emit portable C99 code (.c) via EIR without LLVM"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> EmitObj("emit-obj",
                                   llvm::cl::desc("Emit native object file (.o) via EIR without LLVM"),
    llvm::cl::cat(EskiuCat));

enum class BackendKind {
    LLVM,
    EIR
};
static llvm::cl::opt<BackendKind> Backend("backend",
    llvm::cl::desc("Choose compiler backend (llvm or eir)"),
    llvm::cl::values(
        clEnumValN(BackendKind::LLVM, "llvm", "Use LLVM backend (default)"),
        clEnumValN(BackendKind::EIR, "eir", "Use EIR backend (alternative to LLVM)")
    ),
    llvm::cl::init(BackendKind::LLVM),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> TestTypeChecker("test-typechecker",
                                           llvm::cl::desc("Type check input and report errors"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> TargetTriple("target",
    llvm::cl::desc("Override target triple (e.g. x86_64-pc-none, aarch64-unknown-none)"),
    llvm::cl::value_desc("triple"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> TargetCPU("mcpu",
    llvm::cl::desc("Override target CPU (e.g. mpcore for the 3DS ARM11)"),
    llvm::cl::value_desc("cpu"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> TargetFeatures("mattr",
    llvm::cl::desc("Target feature string, LLVM -mattr syntax (e.g. +vfp2)"),
    llvm::cl::value_desc("features"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> RelocModel("reloc",
    llvm::cl::desc("Relocation model: pic (default), static, dynamic-no-pic. "
                   "3DS .3dsx targets need 'static'."),
    llvm::cl::value_desc("model"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> Subsystem("subsystem",
    llvm::cl::desc("Windows subsystem of the linked program: console (default) or windows, "
                   "for a program that opens its own window and no console"),
    llvm::cl::value_desc("console|windows"),
    llvm::cl::cat(EskiuCat));

// -ffp-contract=fast: let the backend fuse a multiply and an add into one instruction
// (FMA), rounding once instead of twice. Off by default, so floating-point results are the
// same on every target.
static llvm::cl::opt<std::string> FpContract("ffp-contract",
    llvm::cl::desc("Floating-point contraction: off (default) or fast (fuse a*b+c into an FMA)"),
    llvm::cl::value_desc("off|fast"),
    llvm::cl::init("off"),
    llvm::cl::cat(EskiuCat));

// -fstrict-aliasing: C's aliasing rule for the optimizer (scalar type-based alias
// information). Off by default: a pointer cast that reads one type through another is
// defined.
static llvm::cl::opt<bool> StrictAliasing("fstrict-aliasing",
    llvm::cl::desc("Assume an access of one scalar type does not alias another (C's rule)"),
    llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> NoStrictAliasing("fno-strict-aliasing",
    llvm::cl::desc("Allow reading one type through a pointer to another (the default)"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> Freestanding("freestanding",
    llvm::cl::desc("Compile without libc — alloc/free use esk_alloc/esk_free"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> Safe("safe",
    llvm::cl::desc("Insert run-time checks that trap on violation: all, or --safe=LIST of bounds,overflow,null"),
    llvm::cl::value_desc("list"), llvm::cl::ValueOptional, llvm::cl::cat(EskiuCat));

// The CodeGen::Safe* bits named by --safe[=LIST]: none without the flag, all three
// for a bare --safe, else the comma-separated subset. An unknown name is an error.
static bool safeChecksFromFlag(unsigned& mask) {
    mask = 0;
    if (Safe.getNumOccurrences() == 0) return true;
    std::string list = Safe;
    if (list.empty()) { mask = CodeGen::SafeAll; return true; }
    size_t start = 0;
    while (true) {
        size_t comma = list.find(',', start);
        std::string name = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (name == "bounds") mask |= CodeGen::SafeBounds;
        else if (name == "overflow") mask |= CodeGen::SafeOverflow;
        else if (name == "null") mask |= CodeGen::SafeNull;
        else {
            std::cerr << "error: unknown --safe check '" << name << "' (valid: bounds, overflow, null)" << std::endl;
            return false;
        }
        if (comma == std::string::npos) return true;
        start = comma + 1;
    }
}

static llvm::cl::opt<bool> WnoDeprecated("Wno-deprecated",
    llvm::cl::desc("Do not warn when a deprecated function is used"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> WnoStringCompare("Wno-string-compare",
    llvm::cl::desc("Do not warn on == / != against a string literal"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> WnoPackedAddress("Wno-address-of-packed-member",
    llvm::cl::desc("Do not warn on the address of a packed struct member"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> WnoDangling("Wno-dangling-pointer",
    llvm::cl::desc("Do not warn when a local's address escapes"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> WnoFrame("Wno-frame-larger-than",
    llvm::cl::desc("Do not warn on a local larger than 1 MiB"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> Werror("Werror",
    llvm::cl::desc("Report every warning as an error"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> Wnone("w",
    llvm::cl::desc("Print no warnings"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> WnoUnusedResult("Wno-unused-result",
    llvm::cl::desc("Do not warn when a call's Result value is discarded"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> WnoAwaitLock("Wno-await-holding-lock",
    llvm::cl::desc("Do not warn on an await while a Mutex taken in the same block is locked"), llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> WnoFormat("Wno-format",
    llvm::cl::desc("Do not check printf-style format strings against their arguments"), llvm::cl::cat(EskiuCat));

// The -Wno-<name> flags of the warnings that are on by default.
static void applyWarningFlags(TypeChecker& tc) {
    if (WnoStringCompare) tc.disabledWarnings.insert("string-compare");
    if (WnoPackedAddress) tc.disabledWarnings.insert("address-of-packed-member");
    if (WnoDangling) tc.disabledWarnings.insert("dangling-pointer");
    if (WnoFrame) tc.disabledWarnings.insert("frame-larger-than");
    if (WnoFormat) tc.disabledWarnings.insert("format");
    if (WnoUnusedResult) tc.disabledWarnings.insert("unused-result");
    if (WnoAwaitLock) tc.disabledWarnings.insert("await-holding-lock");
    tc.warningsAsErrors = Werror;
    tc.noWarnings = Wnone;
}

static llvm::cl::opt<bool> Wall("Wall",
    llvm::cl::desc("Enable lint-style warnings: unused variables, parameters, "
                   "and functions, assignment used as a condition, and locals "
                   "that may be used uninitialized"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> Wextra("Wextra",
    llvm::cl::desc("Extra warnings: signed/unsigned comparison mismatches"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> HoverAt("hover-at",
    llvm::cl::desc("Print the Eskiu type at LINE:COL (e.g. --hover-at 8:12)"),
    llvm::cl::value_desc("LINE:COL"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> DefinitionAt("definition-at",
    llvm::cl::desc("Print the definition location of the symbol at LINE:COL"),
    llvm::cl::value_desc("LINE:COL"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> ReferencesAt("references-at",
    llvm::cl::desc("Print every reference to the symbol at LINE:COL, one per line: FILE:LINE:COL:WIDTH"),
    llvm::cl::value_desc("LINE:COL"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> CompleteAt("complete-at",
    llvm::cl::desc("Print the completions at LINE:COL, one per line: kind, name, detail (tab-separated)"),
    llvm::cl::value_desc("LINE:COL"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> Symbols("symbols",
    llvm::cl::desc("Print the input's declarations, one per line: kind, name, LINE:COL, container, detail"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> CompileOnly("c",
    llvm::cl::desc("Compile to an object file only; do not link"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::list<std::string> LinkLibs("l", llvm::cl::Prefix,
    llvm::cl::desc("Link against a library, e.g. -lpthread (passed to the linker)"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> NoDefaultLibs("no-default-libs",
    llvm::cl::desc("Do not add the libraries the program implies (#pragma link, the C++ "
                   "exception runtime, pthread); link only what -l names"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::list<std::string> LinkPaths("L", llvm::cl::Prefix,
    llvm::cl::desc("Add a library search path (passed to the linker)"),
    llvm::cl::cat(EskiuCat));

static llvm::cl::list<std::string> LinkArgs("link-arg",
    llvm::cl::desc("Pass an extra argument to the linker (repeatable)"),
    llvm::cl::value_desc("arg"),
    llvm::cl::cat(EskiuCat));

// Sanitizers: instrument the module (real LLVM passes) and link the runtime.
static llvm::cl::opt<bool> Asan("asan",
    llvm::cl::desc("Instrument with AddressSanitizer (detects memory errors)"),
    llvm::cl::cat(EskiuCat));
static llvm::cl::opt<bool> Ubsan("ubsan",
    llvm::cl::desc("Instrument with bounds checking (traps on out-of-bounds access)"),
    llvm::cl::cat(EskiuCat));

// Optimization level: -O0 (default, naive IR straight to the backend), -O1/-O2/-O3
// run the LLVM middle-end (mem2reg/SROA/instcombine/inlining/GVN/...) before codegen.
static llvm::cl::opt<unsigned> OptLevel("O", llvm::cl::Prefix,
    llvm::cl::desc("Optimization level: -O0 (default), -O1, -O2, -O3"),
    llvm::cl::init(0),
    llvm::cl::cat(EskiuCat));

const char* VERSION = "0.14.1";

// `eskiuc run`: set when argv[1] == "run". The program is compiled to a
// temporary executable, run with g_runArgs, then deleted (see main()).
static bool g_runMode = false;
static std::vector<std::string> g_runArgs;
static bool sawSeparator = false;   // a `--` after the script separates program args

// Every input file: the first positional plus the extra ones (`eskiuc a.esk b.esk`).
// The build and the --test-parser/typechecker/codegen modes all merge them.
static std::vector<std::string> allInputs() {
    std::vector<std::string> ins = { std::string(InputFilename) };
    for (const auto& f : ExtraInputs) ins.push_back(f);
    return ins;
}

// Test lexer: tokenize and print all tokens
static int testLexer(const std::string& filename) {
    std::string source = readFile(filename);
    std::map<std::string, Macro> macros;
    seedPredefinedMacros(macros, std::string(TargetTriple), Freestanding);
    Lexer lexer(source, &macros, filename);

    std::cout << "Tokenizing: " << filename << std::endl;
    std::cout << "========================================================" << std::endl;

    Token tok = lexer.next_token();
    int tokenCount = 0;

    while (tok.type != TokenType::EOF_TOKEN) {
        std::string typeStr = tokenTypeToString(tok.type);
        // Right-align like printf("%3d") / "%15s": pad short fields, never truncate
        // (a column past 999 must not underflow the pad count).
        auto pad = [](const std::string& v, size_t w) { return std::string(v.size() < w ? w - v.size() : 0, ' ') + v; };
        std::cout << "  Line " << pad(std::to_string(tok.line), 3)
                  << ", Col " << pad(std::to_string(tok.column), 3)
                  << "  " << pad(typeStr, 15)
                  << "  '" << tok.value << "'" << std::endl;
        tok = lexer.next_token();
        tokenCount++;
    }

    std::cout << "========================================================" << std::endl;
    std::cout << "Total tokens: " << tokenCount << std::endl;
    return lexer.hadError ? 1 : 0;
}

// Test type checker: tokenize, parse, type check, and report errors
static int testTypeChecker(const std::string& filename) {
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) {
        std::cerr << "Parse failed!" << std::endl;
        return 1;
    }

    std::cout << "Type checking: " << filename << std::endl;
    std::cout << "========================================================" << std::endl;

    try {
        // Type check
        TypeChecker typeChecker; typeChecker.targetTriple = std::string(TargetTriple);
        typeChecker.sourceFile = filename;
        typeChecker.warnAll = Wall;
        typeChecker.warnDeprecated = !WnoDeprecated;
        typeChecker.warnExtra = Wextra;
        applyWarningFlags(typeChecker);
        bool success = typeChecker.check(program.get());

        std::cout << "========================================================" << std::endl;
        if (success) {
            std::cout << "Type checking succeeded!" << std::endl;
            return 0;
        } else {
            std::cout << "Type checking failed!" << std::endl;
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}

// Test codegen: tokenize, parse, generate LLVM IR, and print it
static int testCodegen(const std::string& filename) {
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) {
        std::cerr << "Parse failed!" << std::endl;
        return 1;
    }

    std::cout << "Generating LLVM IR: " << filename << std::endl;
    std::cout << "========================================================" << std::endl;

    try {
        // Type-check first: the async transform relies on resolved await types,
        // and codegen on the type checker's struct/enum registration.
        TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = !WnoDeprecated;
        tc.sourceFile = filename; applyWarningFlags(tc);
        if (!tc.check(program.get())) {
            std::cerr << "Type checking failed!" << std::endl;
            return 1;
        }
        AsyncTransform(&tc.expressionTypeMap(), &tc.instanceArgsMap(), &tc.instanceExprTypeMap()).run(program.get());
        // Single resolver: re-resolve the post-transform AST; codegen consumes it.
        TypeChecker postTc; postTc.targetTriple = std::string(TargetTriple); postTc.warnDeprecated = false; postTc.warnDefaults = false; postTc.sourceFile = filename;
        if (!postTc.check(program.get())) {
            std::cerr << "error: internal: the async lowering produced a program that does not type-check" << std::endl;
            return 1;
        }
        // Codegen
        CodeGen codegen;
        codegen.resolvedExprTypes = &postTc.expressionTypeMap();
        codegen.semaInstanceArgs = &postTc.instanceArgsMap();
        if (!TargetTriple.empty()) codegen.targetTriple = std::string(TargetTriple);
        if (!TargetCPU.empty()) codegen.targetCPU = std::string(TargetCPU);
        if (!TargetFeatures.empty()) codegen.targetFeatures = std::string(TargetFeatures);
        if (!RelocModel.empty()) codegen.relocModel = std::string(RelocModel);
        codegen.freestanding = Freestanding;
        { unsigned m = 0; safeChecksFromFlag(m); codegen.safeChecks = m; }
        codegen.optLevel = OptLevel;
        codegen.fpContract = FpContract == "fast";
        codegen.strictAliasing = StrictAliasing && !NoStrictAliasing;
        llvm::Module* module = codegen.generateCode(program);

        if (!module) {
            std::cerr << "Code generation failed!" << std::endl;
            return 1;
        }

        if (OptLevel) codegen.optimizeModule();

        llvm::raw_os_ostream out(std::cout);
        module->print(out, nullptr);
        out.flush();

        std::cout << "========================================================" << std::endl;
        std::cout << "Code generation succeeded!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}

// Test EIR: tokenize, parse, typecheck, async-transform, and emit EIR
// The EIR modes share the front end: parse, check, async-lower, check again, then lower
// the checked program to EIR.
static std::unique_ptr<eir::Module> lowerToEir(Program* program, TypeChecker& postTc) {
    eir::EIRBuilder builder;
    builder.resolvedExprTypes = &postTc.expressionTypeMap();
    builder.semaInstanceArgs = &postTc.instanceArgsMap();
    builder.instanceExprTypes = &postTc.instanceExprTypeMap();
    if (!TargetTriple.empty()) builder.targetTriple = std::string(TargetTriple);
    auto mod = builder.build(program);
    if (!mod) std::cerr << "error: EIR generation failed" << std::endl;
    return mod;
}

static std::unique_ptr<eir::Module> buildEir(const std::string& filename) {
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) {
        std::cerr << "error: parse failed" << std::endl;
        return nullptr;
    }
    TypeChecker tc; tc.targetTriple = std::string(TargetTriple); tc.warnDeprecated = !WnoDeprecated;
    tc.sourceFile = filename; applyWarningFlags(tc);
    if (!tc.check(program.get())) return nullptr;
    AsyncTransform(&tc.expressionTypeMap(), &tc.instanceArgsMap(), &tc.instanceExprTypeMap()).run(program.get());
    TypeChecker postTc; postTc.targetTriple = std::string(TargetTriple); postTc.warnDeprecated = false; postTc.warnDefaults = false; postTc.sourceFile = filename;
    postTc.recordInstanceTypes = true;
    if (!postTc.check(program.get())) {
        std::cerr << "error: internal: the async lowering produced a program that does not type-check" << std::endl;
        return nullptr;
    }
    return lowerToEir(program.get(), postTc);
}

// Write an EIR emitter's text to -o, or to stdout without one.
template <typename Emit>
static int writeEirText(Emit emit) {
    if (OutputFilename.empty() || OutputFilename == "-") {
        emit(std::cout);
        return 0;
    }
    std::ofstream out(OutputFilename);
    if (!out) {
        std::cerr << "error: cannot open output file '" << OutputFilename << "'" << std::endl;
        return 1;
    }
    emit(out);
    return 0;
}

// --test-eir, --run-eir, --emit-asm and --emit-c.
static int eirMode(const std::string& filename) {
    try {
        auto mod = buildEir(filename);
        if (!mod) return 1;
        if (TestEir) {
            mod->dump(std::cout);
            return 0;
        }
        if (RunEir) {
            eir::VM vm;
            return vm.run(mod.get(), "main", {filename});
        }
        if (EmitAsm) {
            auto emitter = eir::createArm64Emitter(std::string(TargetTriple));
            return writeEirText([&](std::ostream& os) { emitter->emitModule(mod.get(), os); });
        }
        auto emitter = eir::createCEmitter();
        return writeEirText([&](std::ostream& os) { emitter->emitModule(mod.get(), os); });
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << std::endl;
        return 1;
    }
}

// The EIR backend emits the program as written, so an option that changes how LLVM
// generates code is an error under it rather than silently ignored.
static bool checkEirOptions(unsigned safeChecks) {
    std::vector<std::string> bad;
    if (OptLevel) bad.push_back("-O" + std::to_string(OptLevel));
    if (safeChecks) bad.push_back("--safe");
    if (Freestanding) bad.push_back("--freestanding");
    if (Asan) bad.push_back("--asan");
    if (Ubsan) bad.push_back("--ubsan");
    if (!TargetCPU.empty()) bad.push_back("--mcpu");
    if (!TargetFeatures.empty()) bad.push_back("--mattr");
    if (!RelocModel.empty()) bad.push_back("--reloc");
    for (const auto& b : bad)
        std::cerr << "error: " << b << " is not supported by the EIR backend" << std::endl;
    return bad.empty();
}

// Test parser: tokenize, parse, and print AST
static int testParser(const std::string& filename) {
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) {
        std::cerr << "Parse failed!" << std::endl;
        return 1;
    }

    std::cout << "Parsing: " << filename << std::endl;
    std::cout << "========================================================" << std::endl;

    ASTPrinter printer;
    printer.print(program);

    std::cout << "========================================================" << std::endl;
    std::cout << "Parse succeeded!" << std::endl;
    return 0;
}

static int compilerMain(int argc, char** argv) {
    llvm::InitLLVM X(argc, argv);

    // Set version string for LLVM's built-in --version
    llvm::cl::SetVersionPrinter([](llvm::raw_ostream& os) {
        os << "Eskiu " << VERSION << " (LLVM " << LLVM_VERSION_MAJOR << "."
           << LLVM_VERSION_MINOR << "." << LLVM_VERSION_PATCH << ")\n";
    });

    // Package commands (new, add, build, fetch, test, and run inside a package): pkg.cpp.
    if (int rc = 0; pkgCommand(argc, argv, rc)) return rc;

    // `eskiuc fmt [--check] file.esk …` — reformat files in place. Handled before
    // option parsing; it does not use the compiler pipeline.
    if (argc >= 2 && std::string(argv[1]) == "fmt") {
        bool check = false;
        std::vector<std::string> files;
        for (int i = 2; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "--check") check = true;
            else files.push_back(a);
        }
        return runFmt(files, check);
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
            if (auto* f = dynamic_cast<FunctionDecl*>(d.get())) {
                emit("function", f->name, pos(f->line, f->col), "", sig(f, f->name));
            } else if (auto* s = dynamic_cast<StructDecl*>(d.get())) {
                emit("struct", s->name, pos(s->line, s->col), "", "struct " + s->name);
                for (const auto& fl : s->fields)
                    emit("field", fl.name, pos(fl.line, fl.col), s->name, fl.type);
                for (const auto& m : s->methods)
                    if (auto* f = dynamic_cast<FunctionDecl*>(m.get()))
                        emit("method", f->name, pos(f->line, f->col), s->name, sig(f, f->name));
            } else if (auto* u = dynamic_cast<UnionDecl*>(d.get())) {
                emit("struct", u->name, pos(u->line, u->col), "", "union " + u->name);
                for (const auto& fl : u->fields)
                    emit("field", fl.name, pos(fl.line, fl.col), u->name, fl.type);
            } else if (auto* e = dynamic_cast<EnumDecl*>(d.get())) {
                emit("enum", e->name, pos(e->line, e->col), "", "enum " + e->name);
            } else if (auto* i = dynamic_cast<InterfaceDecl*>(d.get())) {
                emit("interface", i->name, pos(i->line, i->col), "", "interface " + i->name);
            } else if (auto* a = dynamic_cast<TypeAliasDecl*>(d.get())) {
                emit("type", a->name, pos(a->line, a->col), "", "type " + a->name + " = " + a->aliased);
            } else if (auto* v = dynamic_cast<VarDecl*>(d.get())) {
                emit(v->isConst ? "constant" : "variable", v->name, pos(v->line, v->col), "", v->type);
            }
        }
        return 0;
    }

    // Handle --test-codegen
    if (TestCodegen) {
        return testCodegen(InputFilename);
    }

    // --test-eir, --run-eir, --emit-asm, --emit-c
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
        auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding,
                                   &macros, &lexFailed);
        if (!program) {
            if (!lexFailed) std::cerr << "error: parse failed" << std::endl;
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

        AsyncTransform(&typeChecker.expressionTypeMap(), &typeChecker.instanceArgsMap(), &typeChecker.instanceExprTypeMap()).run(program.get());
        // Single resolver: re-resolve the post-transform AST; codegen consumes it.
        TypeChecker postTc; postTc.targetTriple = std::string(TargetTriple); postTc.warnDeprecated = false; postTc.warnDefaults = false; postTc.sourceFile = std::string(InputFilename);
        postTc.recordInstanceTypes = useEir;
        if (!postTc.check(program.get())) {
            std::cerr << "error: internal: the async lowering produced a program that does not type-check" << std::endl;
            return 1;
        }
        CodeGen codegen;
        std::unique_ptr<eir::Module> eirModule;
        if (useEir) {
            eirModule = lowerToEir(program.get(), postTc);
            if (!eirModule) return 1;
        } else {
            codegen.resolvedExprTypes = &postTc.expressionTypeMap();
            codegen.semaInstanceArgs = &postTc.instanceArgsMap();
            if (!TargetTriple.empty()) codegen.targetTriple = std::string(TargetTriple);
            if (!TargetCPU.empty()) codegen.targetCPU = std::string(TargetCPU);
            if (!TargetFeatures.empty()) codegen.targetFeatures = std::string(TargetFeatures);
            if (!RelocModel.empty()) codegen.relocModel = std::string(RelocModel);
            codegen.freestanding = Freestanding;
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
        }
        auto emitObject = [&](const std::string& path) {
            if (eirModule) return eir::emitEirObject(eirModule.get(), path, std::string(TargetTriple), OptLevel);
            return codegen.emitObjectFile(path);
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
                for (const auto& l : implicitLinkLibs(macros, usesEH, usesThreads)) implied.push_back(l);
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
            std::vector<std::string> nativeDirs;
            {
                std::vector<std::string> linked(LinkLibs.begin(), LinkLibs.end());
                if (!NoDefaultLibs) linked.insert(linked.end(), pragmaLibs.begin(), pragmaLibs.end());
                try {
                    nativeDirs = pkgNativeLibDirs(linked);
                } catch (const std::exception& e) {
                    std::cerr << "error: " << e.what() << std::endl;
                    llvm::sys::fs::remove(tmpObjPath);
                    return 1;
                }
                for (const auto& d : nativeDirs) {
                    paths.push_back(d);
                    // An rpath to this machine's cache helps a program run here; an app for a
                    // phone carries its libraries in its package, so it gets none.
                    if (!winTarget && !macros.count("__wasm__") && !macros.count("__ANDROID__") &&
                        !macros.count("TARGET_OS_IPHONE"))
                        extra.push_back("-Wl,-rpath," + d);
                }
            }
            bool ok = linkExecutable(tmpObjPath, outFile, libs, paths, extra, /*sanitized=*/Asan,
                                     std::string(TargetTriple));
            if (ok && winTarget) copyNativeDlls(nativeDirs, outFile);
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
            return args.rc;
        }
    }
    std::cerr << "warning: could not allocate dedicated stack thread; falling back to caller stack\n";
    return compilerMain(argc, argv);
#endif
}
