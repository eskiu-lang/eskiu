#include "eir_obj.h"
#include "eir_as_arm64.h"
#include "eir_asm.h"
#include "eir_c.h"
#include "eir_runtime.h"
#include "main_support.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

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

// Whether `triple` (the host, when empty) is an Apple target, as the ARM64 emitter decides
// (Mach-O syntax and objects).
bool isAppleTarget(const std::string& triple) {
    if (triple.empty()) {
#ifdef __APPLE__
        return true;
#else
        return false;
#endif
    }
    for (const char* k : {"darwin", "apple", "macos", "ios"})
        if (triple.find(k) != std::string::npos) return true;
    return false;
}

// The object the integrated assembler writes for `triple`: Mach-O with the LC_BUILD_VERSION
// the LLVM path writes (the host's macOS product version for a native build, as
// `nativeTriple` in codegen_module.cpp; else the triple's own version), or ELF.
Arm64ObjTarget objTarget(const std::string& triple) {
    Arm64ObjTarget t;
    t.macho = isAppleTarget(triple);
    if (!t.macho) return t;
    unsigned major = 11, minor = 0, patch = 0;
    if (triple.empty()) {
#ifdef __APPLE__
        char buf[64] = {0};
        size_t len = sizeof(buf) - 1;
        if (sysctlbyname("kern.osproductversion", buf, &len, nullptr, 0) == 0 && buf[0]) {
            unsigned v[3] = {0, 0, 0};
            int k = 0;
            for (const char* p = buf; *p && k < 3; ++p) {
                if (*p == '.') ++k;
                else if (*p >= '0' && *p <= '9') v[k] = v[k] * 10 + static_cast<unsigned>(*p - '0');
            }
            if (v[0] > 0) { major = v[0]; minor = v[1]; patch = 0; }
        }
#endif
    } else {
        llvm::Triple tt(triple);
        llvm::VersionTuple v;
        if (tt.isiOS()) {
            t.platform = tt.isSimulatorEnvironment() ? 7 : 2;
            major = 14;
            if (tt.getOSVersion().getMajor() > 0) v = tt.getiOSVersion();
        } else if (tt.getOSVersion().getMajor() > 0) {
            tt.getMacOSXVersion(v);
        }
        if (v.getMajor() > 0) {
            major = v.getMajor();
            minor = v.getMinor().value_or(0);
            patch = v.getSubminor().value_or(0);
        }
    }
    t.minOS = (major << 16) | (minor << 8) | patch;
    return t;
}

// ESKIU_EIR_EXTERNAL_AS=1 assembles with the system C driver instead (for comparison).
bool externalAssemblerRequested() {
    const char* e = std::getenv("ESKIU_EIR_EXTERNAL_AS");
    return e && *e && std::strcmp(e, "0") != 0;
}

bool moduleHasInlineAsm(const Module* mod) {
    for (const auto& fn : mod->functions)
        for (const auto& bb : fn->blocks)
            for (const auto& inst : bb->instructions)
                if (inst->op == Opcode::InlineAsm) return true;
    return false;
}

bool writeBinary(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        std::cerr << "error: cannot write to " << path << std::endl;
        return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

} // namespace

// The minimum macOS a build through the integrated assembler links for: the version its
// objects record (empty when another path or another OS builds the object).
std::string eirLinkMacOSVersion(const std::string& triple, unsigned optLevel) {
    if (!eirUsesAsm(triple, optLevel) || externalAssemblerRequested()) return "";
    Arm64ObjTarget t = objTarget(triple);
    if (!t.macho || t.platform != 1) return "";
    std::string v = std::to_string(t.minOS >> 16) + "." + std::to_string((t.minOS >> 8) & 0xff);
    if (t.minOS & 0xff) v += "." + std::to_string(t.minOS & 0xff);
    return v;
}

bool assembleEirFile(const std::string& inFile, const std::string& outFile, const std::string& triple) {
    std::ifstream in(inFile, std::ios::binary);
    if (!in) {
        std::cerr << "error: cannot read " << inFile << std::endl;
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string object, err;
    if (!assembleArm64(ss.str(), objTarget(triple), object, err)) {
        std::cerr << "error: " << inFile << ": " << err << std::endl;
        return false;
    }
    return writeBinary(outFile, object);
}

// The backend's output becomes an object: ARM64 assembly at -O0 on an arm64 target outside
// Windows, else C99, which the system C driver compiles (at `optLevel`, eskiuc's -O, with
// strict aliasing off: Eskiu defines a pointer cast that reads one type through another).
// The assembly goes through the integrated assembler (eir_as_arm64.cpp), so no external tool
// runs, unless the module needs the C runtime: that is compiled by the C driver and joined to
// the code with a relocatable link, so the result is still one object file. A module whose
// inline asm the integrated assembler does not take is assembled by the system C driver, as
// is every module under ESKIU_EIR_EXTERNAL_AS=1.
bool eirUsesAsm(const std::string& triple, unsigned optLevel) {
    if (optLevel > 0) return false;
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
    bool isArm64 = eirUsesAsm(triple, optLevel);

    std::string text, object;
    bool integrated = false;
    if (isArm64) {
        std::ostringstream ss;
        createArm64Emitter(triple)->emitModule(mod, ss);
        text = ss.str();
        if (!externalAssemblerRequested()) {
            std::string err;
            integrated = assembleArm64(text, objTarget(triple), object, err);
            if (!integrated && !moduleHasInlineAsm(mod)) {
                std::cerr << "error: " << err << std::endl;
                return false;
            }
        }
        if (integrated && !moduleNeedsRuntime(mod)) return writeBinary(outFile, object);
    }

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

    // The runtime and the relocatable link record the minimum macOS the code object does.
    std::vector<std::string> minOS;
    std::string v = integrated ? eirLinkMacOSVersion(triple, optLevel) : "";
    if (!v.empty()) minOS.push_back("-mmacosx-version-min=" + v);
    auto withMin = [&](std::vector<std::string> args) {
        args.insert(args.begin(), minOS.begin(), minOS.end());
        return args;
    };

    TempFile s, codeObj, rtSrc, rtObj;
    bool needRt = moduleNeedsRuntime(mod);
    if (needRt && !makeTemp("o", codeObj)) return false;
    std::string codePath = needRt ? codeObj.path : outFile;
    if (integrated) {
        if (!writeBinary(codePath, object)) return false;
    } else {
        if (!writeTemp("s", s, [&](std::ostream& os) { os << text; })) return false;
        if (!runDriver(driver, ccArgs, {"-c", s.path, "-o", codePath}, "assembler")) return false;
    }
    if (!needRt) return true;
    if (!makeTemp("o", rtObj)) return false;
    if (!writeTemp("c", rtSrc, [](std::ostream& os) { os << runtimeStandaloneSource(); })) return false;
    return runDriver(driver, ccArgs, withMin({"-O2", "-w", "-c", rtSrc.path, "-o", rtObj.path}), "runtime compile") &&
           runDriver(driver, ccArgs, withMin({"-r", "-nostdlib", codeObj.path, rtObj.path, "-o", outFile}), "relocatable link");
}

} // namespace eir
