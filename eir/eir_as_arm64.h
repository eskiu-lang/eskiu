#ifndef EIR_AS_ARM64_H
#define EIR_AS_ARM64_H

#include <cstdint>
#include <string>

namespace eir {

// The object format and, for Mach-O, the LC_BUILD_VERSION the integrated assembler writes.
struct Arm64ObjTarget {
    bool macho = true;          // Mach-O (Apple targets), else ELF64 (aarch64 Linux)
    uint32_t platform = 1;      // LC_BUILD_VERSION platform: 1 macOS, 2 iOS, 7 iOS simulator
    uint32_t minOS = 0;         // X.Y.Z as X << 16 | Y << 8 | Z
};

// The integrated assembler: turns the ARM64 assembly the EIR emitter writes
// (eir_asm_arm64.cpp, Mach-O or ELF syntax by target) into a relocatable object, with no
// external assembler and no LLVM. It reads the subset the emitter produces (and the common
// instructions inline asm uses); any other statement is an error naming its line, never
// skipped. On failure `error` says why and `object` is unspecified.
bool assembleArm64(const std::string& text, const Arm64ObjTarget& target, std::string& object,
                   std::string& error);

} // namespace eir

#endif // EIR_AS_ARM64_H
