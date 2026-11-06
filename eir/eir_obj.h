#ifndef EIR_OBJ_H
#define EIR_OBJ_H

#include "eir.h"
#include <string>

namespace eir {

// Whether the EIR backend builds `triple` (or the host, for an empty one) through ARM64
// assembly rather than C.
bool eirUsesAsm(const std::string& triple, unsigned optLevel);
bool emitEirObject(const Module* mod, const std::string& outFile, const std::string& triple,
                   unsigned optLevel = 0);
// The minimum macOS version the objects of an EIR build record and the link must name
// (-mmacosx-version-min); empty when no integrated-assembler Mach-O object for macOS is built.
std::string eirLinkMacOSVersion(const std::string& triple, unsigned optLevel);
// Assemble ARM64 assembly as --emit-asm writes it into an object with the integrated
// assembler (the hidden --eir-assemble mode, for testing it alone).
bool assembleEirFile(const std::string& inFile, const std::string& outFile, const std::string& triple);

} // namespace eir

#endif // EIR_OBJ_H
