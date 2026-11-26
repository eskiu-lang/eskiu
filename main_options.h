// main_options.h: the command-line options, --safe and the warning flags (internal to main.cpp, which includes it once, in order).
#pragma once
#include "llvm/Support/CommandLine.h"
#include "sema/type_checker.h"
#include "codegen/codegen.h"
#include "main_support.h"

// Command line options. All of them live in one category so `--help` lists only
// Eskiu's options (not the ~200 LLVM internals linked in with the backend).
static llvm::cl::OptionCategory EskiuCat("Eskiu options");

static const char* OVERVIEW =
    "Eskiu Language Compiler\n\n"
    "  eskiuc file.esk [more.esk ...] -o prog   compile and link an executable\n"
    "  eskiuc file.esk -c -o file.o             compile to an object file\n"
    "  eskiuc run [flags] file.esk [--] [args]  compile to a temp executable and run it\n"
    "  eskiuc fmt [--check] [--width N] file.esk ...  reindent files in place\n"
    "  eskiuc repl [file.esk ...]               interactive session (JIT)\n"
    "  eskiuc file.esk --dev -o prog            development build: run-time checks (--safe) and -Wall\n";
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

static llvm::cl::opt<std::string> EmitHeader("emit-header",
                                   llvm::cl::desc("Write the C header of the `export` functions to <file>"),
                                   llvm::cl::value_desc("file"), llvm::cl::cat(EskiuCat));

static llvm::cl::opt<bool> EmitObj("emit-obj",
                                   llvm::cl::desc("Emit native object file (.o) via EIR without LLVM"),
    llvm::cl::cat(EskiuCat));

// The integrated ARM64 assembler alone: the input is assembly as --emit-asm writes it.
static llvm::cl::opt<bool> EirAssemble("eir-assemble",
                                       llvm::cl::desc("Assemble the input (--emit-asm output) into the -o object"),
    llvm::cl::Hidden, llvm::cl::cat(EskiuCat));

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

static llvm::cl::opt<std::string> Diagnostics("diagnostics",
    llvm::cl::desc("How to print errors and warnings: text (default) or json (one JSON object per line)"),
    llvm::cl::value_desc("text|json"), llvm::cl::init("text"), llvm::cl::cat(EskiuCat));

static llvm::cl::opt<std::string> Safe("safe",
    llvm::cl::desc("Insert run-time checks that report the source location and abort on a violation: "
                   "all, or --safe=LIST of bounds,overflow,null"),
    llvm::cl::value_desc("list"), llvm::cl::ValueOptional, llvm::cl::cat(EskiuCat));

// --dev: the profile to develop with, every --safe check and the -Wall lints (at the
// default -O0). An explicit --safe=LIST or -O<n> still applies.
static llvm::cl::opt<bool> Dev("dev",
    llvm::cl::desc("Development profile: --safe (every check) and -Wall; --safe=LIST and -O<n> still apply"),
    llvm::cl::cat(EskiuCat));

// The CodeGen::Safe* bits named by --safe[=LIST]: none without the flag (all three
// under --dev), all three for a bare --safe, else the comma-separated subset. An
// unknown name is an error.
static bool safeChecksFromFlag(unsigned& mask) {
    mask = 0;
    if (Safe.getNumOccurrences() == 0) {
        if (Dev) mask = CodeGen::SafeAll;
        return true;
    }
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
    tc.lintRoot = TypeChecker::lintRootFor(tc.sourceFile);
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

const char* VERSION = "0.16.3";

// `eskiuc run`: set when argv[1] == "run". The program is compiled to a
// temporary executable, run with g_runArgs, then deleted (see main()).
static bool g_runMode = false;
static std::vector<std::string> g_runArgs;
static bool sawSeparator = false;   // a `--` after the script separates program args
