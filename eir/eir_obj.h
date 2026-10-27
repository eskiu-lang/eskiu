#ifndef EIR_OBJ_H
#define EIR_OBJ_H

#include "eir.h"
#include <string>

namespace eir {

bool emitEirObject(const Module* mod, const std::string& outFile, const std::string& triple);

} // namespace eir

#endif // EIR_OBJ_H
