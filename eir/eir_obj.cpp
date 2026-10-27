#include "eir_obj.h"
#include "eir_asm.h"
#include "eir_c.h"
#include "main_support.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/FileSystem.h"
#include <iostream>
#include <fstream>
#include <sstream>

namespace eir {

bool emitEirObject(const Module* mod, const std::string& outFile, const std::string& triple) {
    bool isArm64 = triple.find("aarch64") != std::string::npos ||
                   triple.find("arm64") != std::string::npos;
#if defined(__aarch64__) || defined(__arm64__)
    if (triple.empty()) isArm64 = true;
#endif

    // Find C/assembler driver
    std::vector<std::string> ccArgs;
    std::string driver = findCDriver(false, &ccArgs, triple);
    if (driver.empty()) {
        std::cerr << "error: no C compiler / assembler found (looked for $CC, cc, clang, gcc)\n";
        return false;
    }

    if (isArm64) {
        // 1. Emit native ARM64 assembly
        llvm::SmallString<128> tmpAsm;
        if (llvm::sys::fs::createTemporaryFile("eskiu_eir", "s", tmpAsm)) {
            std::cerr << "error: could not create temporary assembly file\n";
            return false;
        }
        std::string tmpAsmPath(tmpAsm.str());
        {
            std::ofstream out(tmpAsmPath);
            if (!out) {
                std::cerr << "error: cannot write to " << tmpAsmPath << "\n";
                llvm::sys::fs::remove(tmpAsmPath);
                return false;
            }
            auto asmEmitter = createArm64Emitter(triple);
            asmEmitter->emitModule(mod, out);
        }

        // 2. Assemble to object file
        std::vector<std::string> argv = {driver};
        argv.insert(argv.end(), ccArgs.begin(), ccArgs.end());
        argv.push_back("-c");
        argv.push_back(tmpAsmPath);
        argv.push_back("-o");
        argv.push_back(outFile);

        std::vector<llvm::StringRef> args(argv.begin(), argv.end());
        std::string errMsg;
        int rc = llvm::sys::ExecuteAndWait(driver, args, /*Env=*/std::nullopt,
                                           /*Redirects=*/{}, /*SecondsToWait=*/0,
                                           /*MemoryLimit=*/0, &errMsg);
        llvm::sys::fs::remove(tmpAsmPath);
        if (rc != 0) {
            std::cerr << "error: assembler failed";
            if (!errMsg.empty()) std::cerr << ": " << errMsg;
            std::cerr << std::endl;
            return false;
        }
        return true;
    } else {
        // Fallback for other architectures: emit portable C99 and compile with cc -c
        llvm::SmallString<128> tmpC;
        if (llvm::sys::fs::createTemporaryFile("eskiu_eir", "c", tmpC)) {
            std::cerr << "error: could not create temporary C file\n";
            return false;
        }
        std::string tmpCPath(tmpC.str());
        {
            std::ofstream out(tmpCPath);
            if (!out) {
                std::cerr << "error: cannot write to " << tmpCPath << "\n";
                llvm::sys::fs::remove(tmpCPath);
                return false;
            }
            auto cEmitter = createCEmitter();
            cEmitter->emitModule(mod, out);
        }

        std::vector<std::string> argv = {driver};
        argv.insert(argv.end(), ccArgs.begin(), ccArgs.end());
        argv.push_back("-std=c99");
        argv.push_back("-c");
        argv.push_back(tmpCPath);
        argv.push_back("-o");
        argv.push_back(outFile);

        std::vector<llvm::StringRef> args(argv.begin(), argv.end());
        std::string errMsg;
        int rc = llvm::sys::ExecuteAndWait(driver, args, /*Env=*/std::nullopt,
                                           /*Redirects=*/{}, /*SecondsToWait=*/0,
                                           /*MemoryLimit=*/0, &errMsg);
        llvm::sys::fs::remove(tmpCPath);
        if (rc != 0) {
            std::cerr << "error: C compiler failed";
            if (!errMsg.empty()) std::cerr << ": " << errMsg;
            std::cerr << std::endl;
            return false;
        }
        return true;
    }
}

} // namespace eir
