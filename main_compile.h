// main_compile.h: the build (parse, check, generate, write the object, link, run);
// internal to main.cpp, which includes it once, in order.
#pragma once
#include <functional>
#include "llvm/Support/CommandLine.h"
#include "sema/type_checker.h"
#include "codegen/codegen.h"
#include "main_support.h"

// Link the program into an executable (and run it for `eskiuc run`): the object written
// to a temporary file, the libraries given and implied, the platform's link flags.
static int linkProgram(const std::function<bool(const std::string&)>& emitObject,
                       const std::unique_ptr<eir::Module>& eirModule, CodeGen& codegen,
                       const std::vector<std::string>& pragmaLibs,
                       const std::map<std::string, Macro>& macros, const std::string& outFile,
                       const std::string& runExePath) {
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

// Full compilation pipeline — parse every input file and merge their
// top-level declarations into a single program (`eskiuc a.esk b.esk ...`).
static int compileProgram(unsigned safeChecks, bool useEir) {
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

    if (linkExe) return linkProgram(emitObject, eirModule, codegen, pragmaLibs, macros, outFile, runExePath);

    if (!emitObject(outFile)) {
        return 1;
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
}
}
