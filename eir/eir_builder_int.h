// eir_builder_int.h: intSpelling, shared by eir_builder_stmt.cpp and eir_builder_ops.cpp (internal).
#pragma once
#include "eir_builder.h"

namespace eir {

// The Eskiu spelling of an integer EIR type.
static std::string intSpelling(const TypePtr& t) {
    if (!t || !t->isInt()) return "";
    switch (t->bitWidth) {
        case 1: return "bool";
        case 8: return t->isSigned ? "int8" : "uint8";
        case 16: return t->isSigned ? "int16" : "uint16";
        case 64: return t->isSigned ? "int64" : "uint64";
        default: return t->isSigned ? "int" : "uint";
    }
}

} // namespace eir
