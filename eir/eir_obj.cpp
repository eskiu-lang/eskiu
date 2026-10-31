#include "eir_obj.h"
#include "eir_asm.h"
#include "eir_c.h"
#include "eir_runtime.h"
#include "main_support.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include <iostream>
#include <fstream>
#include <functional>

namespace eir {

namespace {

// A temporary file holding what `write` produces; removed when the guard goes.
struct TempFile {
    std::string path;
    ~TempFile() { if (!path.empty()) llvm::sys::fs::remove(path); }
};

bool makeTemp(const char* suffix, TempFile& tf) {
    llvm::SmallString<128> p;
    if (llvm::sys::fs::createTemporaryFile("eskiu_eir", suffix, p)) {
        std::cerr << "error: could not create a temporary ." << suffix << " file" << std::endl;
        return false;
    }
    tf.path = std::string(p.str());
    return true;
}

bool writeTemp(const char* suffix, TempFile& tf, const std::function<void(std::ostream&)>& write) {
    if (!makeTemp(suffix, tf)) return false;
    std::ofstream out(tf.path);
    if (!out) {
        std::cerr << "error: cannot write to " << tf.path << std::endl;
        return false;
    }
    write(out);
    return static_cast<bool>(out);
}

bool runDriver(const std::string& driver, const std::vector<std::string>& ccArgs,
               const std::vector<std::string>& args, const char* what) {
    std::vector<std::string> argv = {driver};
    argv.insert(argv.end(), ccArgs.begin(), ccArgs.end());
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<llvm::StringRef> refs(argv.begin(), argv.end());
    std::string errMsg;
    int rc = llvm::sys::ExecuteAndWait(driver, refs, std::nullopt, {}, 0, 0, &errMsg);
    if (rc != 0) {
        std::cerr << "error: " << what << " failed";
        if (!errMsg.empty()) std::cerr << ": " << errMsg;
        std::cerr << std::endl;
        return false;
    }
    return true;
}

} // namespace

// The system C driver turns the backend's output (ARM64 assembly, or C99 for any other
// target) into an object. The assembly path compiles the runtime as a second object and
// joins the two with a relocatable link, so the result is still one object file. The C is
// compiled at `optLevel` (eskiuc's -O) with strict aliasing off: Eskiu defines a pointer
// cast that reads one type through another.
bool eirUsesAsm(const std::string& triple) {
    bool isArm64 = triple.find("aarch64") != std::string::npos ||
                   triple.find("arm64") != std::string::npos;
#if defined(__aarch64__) || defined(__arm64__)
    if (triple.empty()) isArm64 = true;
#endif
    bool windows = triple.find("windows") != std::string::npos || triple.find("mingw") != std::string::npos;
#ifdef _WIN32
    if (triple.empty()) windows = true;
#endif
    return isArm64 && !windows;
}

bool emitEirObject(const Module* mod, const std::string& outFile, const std::string& triple,
                   unsigned optLevel) {
    bool isArm64 = eirUsesAsm(triple);

    std::vector<std::string> ccArgs;
    std::string driver = findCDriver(false, &ccArgs, triple);
    if (driver.empty()) {
        std::cerr << "error: no C compiler / assembler found (looked for $CC, cc, clang, gcc)" << std::endl;
        return false;
    }

    if (!isArm64) {
        TempFile src;
        if (!writeTemp("c", src, [&](std::ostream& os) { createCEmitter()->emitModule(mod, os); })) return false;
        return runDriver(driver, ccArgs, {"-std=c99", "-w", "-fno-strict-aliasing",
                                          "-O" + std::to_string(optLevel), "-c", src.path,
                                          "-o", outFile}, "C compiler");
    }

    TempFile s;
    if (!writeTemp("s", s, [&](std::ostream& os) { createArm64Emitter(triple)->emitModule(mod, os); })) return false;
    if (!moduleNeedsRuntime(mod)) {
        return runDriver(driver, ccArgs, {"-c", s.path, "-o", outFile}, "assembler");
    }

    TempFile codeObj, rtSrc, rtObj;
    if (!makeTemp("o", codeObj) || !makeTemp("o", rtObj)) return false;
    if (!writeTemp("c", rtSrc, [](std::ostream& os) { os << runtimeStandaloneSource(); })) return false;
    return runDriver(driver, ccArgs, {"-c", s.path, "-o", codeObj.path}, "assembler") &&
           runDriver(driver, ccArgs, {"-O2", "-w", "-c", rtSrc.path, "-o", rtObj.path}, "runtime compile") &&
           runDriver(driver, ccArgs, {"-r", "-nostdlib", codeObj.path, rtObj.path, "-o", outFile}, "relocatable link");
}

} // namespace eir
