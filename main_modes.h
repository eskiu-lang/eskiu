// main_modes.h: the inputs, the --test-* modes, the EIR modes and --emit-header (internal to main.cpp, which includes it once, in order).
#pragma once
#include "llvm/Support/CommandLine.h"
#include "sema/type_checker.h"
#include "codegen/codegen.h"
#include "main_support.h"

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
        if (!g_jsonDiagnostics) std::cerr << "Parse failed!" << std::endl;
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

// Does the program have an async function for the async lowering to rewrite? The
// lowering replaces each one with ordinary code (a frame struct, a resume function, a
// constructor) that the type checker must check again before codegen reads its types.
// Without one the lowering leaves the program as it was, so the first check's results
// stand and the second run is skipped (docs/dev/compile-speed.md).
static bool programHasAsync(const Program* program) {
    for (const auto& decl : program->declarations)
        if (auto* fd = nodeCast<const FunctionDecl>(decl.get()); fd && fd->isAsync) return true;
    return false;
}

// Test codegen: tokenize, parse, generate LLVM IR, and print it
static int testCodegen(const std::string& filename) {
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) {
        if (!g_jsonDiagnostics) std::cerr << "Parse failed!" << std::endl;
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
            if (!g_jsonDiagnostics) std::cerr << "Type checking failed!" << std::endl;
            return 1;
        }
        // Single resolver: re-resolve the post-transform AST; codegen consumes it.
        std::unique_ptr<TypeChecker> postOwn;
        TypeChecker* post = &tc;
        if (programHasAsync(program.get())) {
            AsyncTransform(&tc.expressionTypeMap(), &tc.instanceArgsMap(), &tc.instanceExprTypeMap()).run(program.get());
            postOwn = std::make_unique<TypeChecker>();
            TypeChecker& postTc = *postOwn;
            postTc.targetTriple = std::string(TargetTriple); postTc.warnDeprecated = false; postTc.warnDefaults = false; postTc.sourceFile = filename;
            if (!postTc.check(program.get())) {
                std::cerr << "error: internal: the async lowering produced a program that does not type-check" << std::endl;
                return 1;
            }
            post = &postTc;
        }
        // Codegen
        CodeGen codegen;
        codegen.resolvedExprTypes = &post->expressionTypeMap();
        codegen.semaInstanceArgs = &post->instanceArgsMap();
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
        if (!g_jsonDiagnostics) std::cerr << "error: parse failed" << std::endl;
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
    // -O reaches the C compiler (an optimized build writes C on arm64 too); the ARM64
    // assembly --emit-asm prints has no levels.
    if (OptLevel && EmitAsm)
        bad.push_back("-O" + std::to_string(OptLevel));
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

// --emit-header FILE: the C declarations of the program's `export` functions. The include
// guard comes from the file's name (`mylib.h` -> ESKIU_MYLIB_H).
static int writeCHeader(TypeChecker& tc, Program* program) {
    std::string path = EmitHeader;
    std::string base = path.substr(path.find_last_of("/\\") == std::string::npos ? 0 : path.find_last_of("/\\") + 1);
    std::string guard = "ESKIU_";
    for (char c : base) guard += std::isalnum((unsigned char)c) ? (char)std::toupper((unsigned char)c) : '_';
    std::string src = InputFilename;
    src = src.substr(src.find_last_of("/\\") == std::string::npos ? 0 : src.find_last_of("/\\") + 1);
    std::string err;
    std::string text = tc.cHeader(program, src, guard, err);
    if (!err.empty()) {
        std::cerr << "error: --emit-header: " << err << std::endl;
        return 1;
    }
    std::ofstream out(path, std::ios::binary);
    out << text;
    if (!out) {
        std::cerr << "error: cannot write " << path << std::endl;
        return 1;
    }
    return 0;
}

// Test parser: tokenize, parse, and print AST
static int testParser(const std::string& filename) {
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding);
    if (!program) {
        if (!g_jsonDiagnostics) std::cerr << "Parse failed!" << std::endl;
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
