#ifndef EIR_OBJ_H
#define EIR_OBJ_H

#include "eir.h"
#include <string>

namespace eir {

// Whether the EIR backend builds `triple` (or the host, for an empty one) through ARM64
// assembly rather than C.
bool eirUsesAsm(const std::string& triple);
bool emitEirObject(const Module* mod, const std::string& outFile, const std::string& triple,
                   unsigned optLevel = 0);

} // namespace eir

#endif // EIR_OBJ_H
