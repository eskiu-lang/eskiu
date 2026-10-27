#pragma once

#include "eir.h"
#include <string>
#include <ostream>
#include <memory>

namespace eir {

class AsmEmitter {
public:
    virtual ~AsmEmitter() = default;
    virtual void emitModule(const Module* mod, std::ostream& os) = 0;
};

// Create an ARM64 (AArch64) assembler emitter
std::unique_ptr<AsmEmitter> createArm64Emitter(const std::string& targetTriple = "");

} // namespace eir
