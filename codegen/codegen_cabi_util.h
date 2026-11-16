// codegen_cabi_util.h: a static helper shared by codegen_cabi.cpp and codegen_cabi_sig.cpp (internal).
#pragma once
#include "codegen.h"

// Only named struct types are C aggregates here: user structs and unions (and ADT
// enums). Literal structs are Eskiu fat values (closures, slices) passed as-is, and
// the Eskiu `va_list` keeps its first-class lowering.
static bool isCAggregate(llvm::Type* ty) {
    auto* st = llvm::dyn_cast<llvm::StructType>(ty);
    return st && !st->isLiteral() && st->getName() != "__va_list";
}
