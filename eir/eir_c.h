#ifndef EIR_C_H
#define EIR_C_H

#include "eir.h"
#include <ostream>
#include <memory>

namespace eir {

class CEmitter {
public:
    virtual ~CEmitter() = default;
    virtual void emitModule(const Module* mod, std::ostream& os) = 0;
};

std::unique_ptr<CEmitter> createCEmitter();

} // namespace eir

#endif // EIR_C_H
