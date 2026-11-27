// main_compile.h: the build (parse, check, generate, write the object, link, run);
// internal to main.cpp, which includes it once, in order.
#pragma once
#include <functional>
#include "llvm/Support/CommandLine.h"
#include "sema/type_checker.h"
#include "codegen/codegen.h"
#include "main_support.h"
#include "build_cache.h"

// Link the program into an executable (and run it for `eskiuc run`): the object written
// to a temporary file, the libraries given and implied, the platform's link flags.
static int linkProgram(const std::function<bool(const std::string&)>& emitObject,
                       const objcache::Meta& facts, const std::vector<std::string>& pragmaLibs,
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
        bool usesEH = facts.usesEH, usesThreads = facts.usesThreads;
        std::vector<std::string> implied = pragmaLibs;
        bool libcxx = usesEH && macros.count("_WIN32") &&
                      mingwLibcxxDriver(findCDriver(false, nullptr, std::string(TargetTriple)));
        for (const auto& l : implicitLinkLibs(macros, usesEH, usesThreads))
            implied.push_back(libcxx && l == "stdc++" ? "c++" : l);
        // emscripten catches exceptions only when linked to (its JavaScript runtime), and
        // links the C++ runtime they need (__cxa_begin_catch, ...) only in C++ mode, which
        // newer emsdks no longer default to when the driver is emcc.
        if (usesEH && macros.count("__EMSCRIPTEN__")) {
            extra.push_back("-sDISABLE_EXCEPTION_CATCHING=0");
            extra.push_back("-sDEFAULT_TO_CXX=1");
        }
        std::set<std::string> seen(libs.begin(), libs.end());
        for (const auto& l : implied)
            if (seen.insert(l).second) extra.push_back("-l" + l);
    }
    // On macOS the object records a minimum OS version (the native triple carries the
    // host's); link for the same one, or ld warns that the object is newer than the
    // version the C driver picked from its SDK.
    if (!facts.macosMin.empty()) extra.push_back("-mmacosx-version-min=" + facts.macosMin);
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
    phaseMark("link");
    if (ok && winTarget) copyNativeDlls(nativeDirs, outFile);
    if (ok && macros.count("__EMSCRIPTEN__") && !g_runMode) pkgCopyWebAssets(outFile);
    llvm::sys::fs::remove(tmpObjPath);
    if (!ok) { if (g_runMode) llvm::sys::fs::remove(runExePath); return 1; }

    if (g_runMode) {
        phaseTotal();
        int rc = runExecutable(runExePath, g_runArgs);
        llvm::sys::fs::remove(runExePath);
        return rc;
    }
    return 0;
}

// What the link needs to know of the program besides its object: whether it throws or
// catches (the C++ runtime), whether it starts threads, and the macOS version its object
// targets. Kept with a cached object (build_cache.h).
static objcache::Meta linkFacts(const std::unique_ptr<eir::Module>& eirModule, CodeGen& codegen) {
    objcache::Meta f;
    if (eirModule) {
        f.usesThreads = eir::moduleUsesThreads(eirModule.get());
        f.macosMin = eir::eirLinkMacOSVersion(std::string(TargetTriple), OptLevel);
        return f;
    }
    llvm::Module* mod = codegen.getModule();
    f.usesEH = mod->getFunction("__cxa_throw") || mod->getFunction("__cxa_rethrow") ||
               mod->getFunction("__gxx_personality_v0") || mod->getFunction("__gxx_personality_seh0");
    f.usesThreads = mod->getFunction("pthread_create") != nullptr;
    llvm::Triple t(mod->getTargetTriple());
    llvm::VersionTuple v;
    if (t.isMacOSX() && t.getMacOSXVersion(v) && v.getMajor() > 0) f.macosMin = v.getAsString();
    return f;
}

// Whether the output is a static library (.a, .lib): the object in an archive, for a
// host build system (an iOS xcframework, an Android or emscripten build) to link.
static bool outputIsStaticLibrary() {
    std::string o = std::string(OutputFilename);
    return !g_runMode && !CompileOnly && (endsWith(o, ".a") || endsWith(o, ".lib"));
}

// Make the output from the object `emitObject` writes: link the executable (and run it
// for `eskiuc run`), archive a static library, or write the object file.
static int finishBuild(const std::function<bool(const std::string&)>& emitObject,
                       const objcache::Meta& facts, const std::vector<std::string>& pragmaLibs,
                       const std::map<std::string, Macro>& macros) {
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

    if (outputIsStaticLibrary()) {
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

    if (linkExe) return linkProgram(emitObject, facts, pragmaLibs, macros, outFile, runExePath);

    if (!emitObject(outFile)) {
        return 1;
    }
    return 0;
}

// A build whose object is in the cache: print the warnings its compile printed and make
// the output from the cached object. False on a miss (`rc` untouched).
static bool buildFromCache(const std::string& key, const std::vector<std::string>& pragmaLibs,
                           const std::map<std::string, Macro>& macros, int& rc) {
    llvm::SmallString<128> tmp;
    if (llvm::sys::fs::createTemporaryFile("eskiu-cached", "o", tmp)) return false;
    std::string cached(tmp.str());
    objcache::Meta meta;
    if (!objcache::lookup(key, cached, meta)) {
        llvm::sys::fs::remove(cached);
        phaseNote("cache", ("miss " + key.substr(0, 16)).c_str());
        return false;
    }
    phaseNote("cache", ("hit " + key.substr(0, 16)).c_str());
    std::cerr << meta.diagnostics << std::flush;
    auto emitObject = [&](const std::string& path) {
        bool ok = !llvm::sys::fs::copy_file(cached, path);
        if (!ok) std::cerr << "error: could not write " << path << std::endl;
        phaseMark("cache-copy");
        return ok;
    };
    rc = finishBuild(emitObject, meta, pragmaLibs, macros);
    llvm::sys::fs::remove(cached);
    return true;
}

// Full compilation pipeline — parse every input file and merge their
// top-level declarations into a single program (`eskiuc a.esk b.esk ...`).
static int compileProgram(unsigned safeChecks, bool useEir) {
try {
    std::map<std::string, Macro> macros;
    bool lexFailed = false;
    // The object cache: the loader records the files it reads, for the key.
    bool useCache = !useEir && EmitHeader.empty() && !outputIsStaticLibrary() &&
                    objcache::wanted(UseCache, NoCache);
    objcache::setRecording(useCache);
    if (useCache) objcache::setProject(std::string(InputFilename));
    phaseMark("start");
    auto program = loadProgram(allInputs(), std::string(TargetTriple), Freestanding,
                               &macros, &lexFailed);
    phaseMark("load+parse");
    objcache::setRecording(false);
    if (!program) {
        if (!lexFailed && !g_jsonDiagnostics) std::cerr << "error: parse failed" << std::endl;
        return 1;
    }
    const std::vector<std::string>& pragmaLibs = program->linkLibs;

    std::string cacheKey;
    if (useCache) {
        cacheKey = objectCacheKey();
        phaseMark("cache-key");
        int rc = 1;
        if (!cacheKey.empty() && buildFromCache(cacheKey, pragmaLibs, macros, rc)) return rc;
    }
    // A compile that may be stored keeps what it prints (its warnings) with the object.
    std::unique_ptr<objcache::CerrCapture> capture;
    if (!cacheKey.empty()) capture = std::make_unique<objcache::CerrCapture>();

    TypeChecker typeChecker; typeChecker.targetTriple = std::string(TargetTriple);
    typeChecker.sourceFile = std::string(InputFilename);
    typeChecker.warnAll = Wall || Dev;
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
    objcache::Meta facts = linkFacts(eirModule, codegen);
    auto emitObject = [&](const std::string& path) {
        bool ok = eirModule ? eir::emitEirObject(eirModule.get(), path, std::string(TargetTriple), OptLevel)
                            : codegen.emitObjectFile(path);
        phaseMark("emit-object");
        if (ok && capture) {
            facts.diagnostics = capture->text();
            capture.reset();
            objcache::store(cacheKey, path, facts);
            phaseMark("cache-store");
        }
        return ok;
    };
    return finishBuild(emitObject, facts, pragmaLibs, macros);
} catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
}
}
