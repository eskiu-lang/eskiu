#include "eir.h"
#include <iomanip>

namespace eir {

// ============================================================================
// Type Implementations
// ============================================================================

std::string Type::str() const {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Int: {
            if (bitWidth == 1) return "i1";
            return (isSigned ? "i" : "u") + std::to_string(bitWidth);
        }
        case TypeKind::Float: {
            return (bitWidth == 32) ? "f32" : "f64";
        }
        case TypeKind::Pointer: return "ptr";
        case TypeKind::Struct: return "struct @" + name;
        case TypeKind::Array:
            return "[" + std::to_string(arraySize) + " x " + (elemType ? elemType->str() : "void") + "]";
        case TypeKind::Function: {
            std::string s = "fn(";
            for (size_t i = 0; i < members.size(); ++i) {
                if (i > 0) s += ", ";
                s += members[i]->str();
            }
            s += ") -> " + (returnType ? returnType->str() : "void");
            return s;
        }
        case TypeKind::Interface: return "{ ptr, ptr }";
        case TypeKind::Closure: return "{ ptr, ptr }";
        case TypeKind::Slice: return "{ ptr, i64 }";
    }
    return "unknown";
}

TypePtr Type::getVoid() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Void, "void", 0, false, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getI1() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "i1", 1, false, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getI8() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "i8", 8, true, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getI16() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "i16", 16, true, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getI32() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "i32", 32, true, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getI64() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "i64", 64, true, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getU8() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "u8", 8, false, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getU16() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "u16", 16, false, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getU32() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "u32", 32, false, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getU64() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Int, "u64", 64, false, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getF32() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Float, "f32", 32, true, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getF64() {
    static auto t = std::make_shared<Type>(Type{TypeKind::Float, "f64", 64, true, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getPtr(TypePtr pointee) {
    auto t = std::make_shared<Type>(Type{TypeKind::Pointer, "ptr", 64, false, pointee, pointee, 0, {}, nullptr});
    return t;
}

TypePtr Type::getStruct(const std::string& name, const std::vector<TypePtr>& fields) {
    auto t = std::make_shared<Type>(Type{TypeKind::Struct, name, 0, false, nullptr, nullptr, 0, fields, nullptr});
    return t;
}

TypePtr Type::getArray(TypePtr elem, uint64_t size) {
    auto t = std::make_shared<Type>(Type{TypeKind::Array, "array", 0, false, nullptr, elem, size, {}, nullptr});
    return t;
}

TypePtr Type::getSlice(TypePtr elem) {
    auto t = std::make_shared<Type>(Type{TypeKind::Slice, "slice", 0, false, nullptr, elem, 0, {}, nullptr});
    return t;
}

TypePtr Type::getInterface(const std::string& name) {
    auto t = std::make_shared<Type>(Type{TypeKind::Interface, name, 0, false, nullptr, nullptr, 0, {}, nullptr});
    return t;
}

TypePtr Type::getClosure(TypePtr ret) {
    if (!ret) {
        static auto t = std::make_shared<Type>(Type{TypeKind::Closure, "closure", 0, false, nullptr, nullptr, 0, {}, nullptr});
        return t;
    }
    return std::make_shared<Type>(Type{TypeKind::Closure, "closure", 0, false, nullptr, nullptr, 0, {}, ret});
}

TypePtr Type::getFunction(TypePtr ret, const std::vector<TypePtr>& params) {
    auto t = std::make_shared<Type>(Type{TypeKind::Function, "fn", 0, false, nullptr, nullptr, 0, params, ret});
    return t;
}

// ============================================================================
// Operand Implementations
// ============================================================================

std::string Operand::str() const {
    switch (kind) {
        case OperandKind::Variable: return text;
        case OperandKind::ConstantInt: {
            if (type && type->bitWidth == 1) return intVal ? "true" : "false";
            return std::to_string(intVal);
        }
        case OperandKind::ConstantFloat: {
            std::ostringstream ss;
            ss << floatVal;
            if (ss.str().find('.') == std::string::npos && ss.str().find('e') == std::string::npos) {
                ss << ".0";
            }
            return ss.str();
        }
        case OperandKind::ConstantString: {
            std::ostringstream ss;
            ss << "\"";
            for (char c : text) {
                if (c == '\n') ss << "\\n";
                else if (c == '\r') ss << "\\r";
                else if (c == '\t') ss << "\\t";
                else if (c == '\"') ss << "\\\"";
                else if (c == '\\') ss << "\\\\";
                else if (c == '\0') ss << "\\0";
                else ss << c;
            }
            ss << "\"";
            return ss.str();
        }
        case OperandKind::ConstantNull: return "null";
        case OperandKind::GlobalRef: return "@" + text;
        case OperandKind::BlockRef: return "$" + text;
    }
    return "<operand>";
}

Operand Operand::var(const std::string& name, TypePtr type) {
    return Operand{OperandKind::Variable, type, name, 0, 0.0};
}

Operand Operand::constInt(int64_t val, TypePtr type) {
    return Operand{OperandKind::ConstantInt, type, "", val, 0.0};
}

Operand Operand::constFloat(double val, TypePtr type) {
    return Operand{OperandKind::ConstantFloat, type, "", 0, val};
}

Operand Operand::constString(const std::string& val) {
    return Operand{OperandKind::ConstantString, Type::getPtr(Type::getU8()), val, 0, 0.0};
}

Operand Operand::constNull() {
    return Operand{OperandKind::ConstantNull, Type::getPtr(), "null", 0, 0.0};
}

Operand Operand::global(const std::string& name, TypePtr type) {
    return Operand{OperandKind::GlobalRef, type, name, 0, 0.0};
}

Operand Operand::block(const std::string& label) {
    return Operand{OperandKind::BlockRef, nullptr, label, 0, 0.0};
}

// ============================================================================
// Instruction Implementations
// ============================================================================

std::string icmpCondToStr(ICmpCond cond) {
    switch (cond) {
        case ICmpCond::Eq:  return "eq";
        case ICmpCond::Ne:  return "ne";
        case ICmpCond::Slt: return "slt";
        case ICmpCond::Sle: return "sle";
        case ICmpCond::Sgt: return "sgt";
        case ICmpCond::Sge: return "sge";
        case ICmpCond::Ult: return "ult";
        case ICmpCond::Ule: return "ule";
        case ICmpCond::Ugt: return "ugt";
        case ICmpCond::Uge: return "uge";
    }
    return "eq";
}

std::string fcmpCondToStr(FCmpCond cond) {
    switch (cond) {
        case FCmpCond::Oeq: return "oeq";
        case FCmpCond::One: return "one";
        case FCmpCond::Olt: return "olt";
        case FCmpCond::Ole: return "ole";
        case FCmpCond::Ogt: return "ogt";
        case FCmpCond::Oge: return "oge";
    }
    return "oeq";
}

bool Instruction::isTerminator() const {
    switch (op) {
        case Opcode::Ret:
        case Opcode::Br:
        case Opcode::BrCond:
        case Opcode::Switch:
        case Opcode::Unreachable:
        case Opcode::Throw:
            return true;
        default:
            return false;
    }
}

std::string Instruction::str() const {
    std::ostringstream ss;
    if (!resultVar.empty()) {
        ss << resultVar << " = ";
    }

    switch (op) {
        case Opcode::Alloca:
            ss << "alloca " << (type ? type->str() : "void");
            if (!operands.empty()) ss << ", align " << operands[0].str();
            break;
        case Opcode::Load:
            ss << (isVolatile ? "load volatile " : "load ") << (type ? type->str() : "void") << ", ptr " << operands[0].str();
            break;
        case Opcode::Store:
            ss << (isVolatile ? "store volatile " : "store ") << operands[0].type->str() << " " << operands[0].str() << ", ptr " << operands[1].str();
            break;
        case Opcode::Gep:
            ss << "gep " << operands[0].type->str() << ", ptr " << operands[0].str() << ", "
               << operands[1].type->str() << " " << operands[1].str();
            break;
        case Opcode::FieldPtr:
            ss << "field_ptr @" << operands[0].text << ", ptr " << operands[1].str() << ", " << operands[2].str();
            break;
        case Opcode::OffsetPtr:
            ss << "offset_ptr ptr " << operands[0].str() << ", i64 " << operands[1].str();
            break;
        case Opcode::Add:  ss << "add "  << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::Sub:  ss << "sub "  << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::Mul:  ss << "mul "  << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::SDiv: ss << "sdiv " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::UDiv: ss << "udiv " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::SRem: ss << "srem " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::URem: ss << "urem " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::FAdd: ss << "fadd " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::FSub: ss << "fsub " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::FMul: ss << "fmul " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::FDiv: ss << "fdiv " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::Neg:  ss << "neg "  << type->str() << " " << operands[0].str(); break;
        case Opcode::And:  ss << "and "  << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::Or:   ss << "or "   << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::Xor:  ss << "xor "  << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::Shl:  ss << "shl "  << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::LShr: ss << "lshr " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::AShr: ss << "ashr " << type->str() << " " << operands[0].str() << ", " << operands[1].str(); break;
        case Opcode::Not:  ss << "not "  << type->str() << " " << operands[0].str(); break;

        case Opcode::ICmp:
            ss << "icmp " << icmpCondToStr(icmpCond) << " " << operands[0].type->str() << " "
               << operands[0].str() << ", " << operands[1].str();
            break;
        case Opcode::FCmp:
            ss << "fcmp " << fcmpCondToStr(fcmpCond) << " " << operands[0].type->str() << " "
               << operands[0].str() << ", " << operands[1].str();
            break;

        case Opcode::Trunc:   ss << "trunc "   << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::ZExt:    ss << "zext "    << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::SExt:    ss << "sext "    << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::FPTrunc: ss << "fptrunc " << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::FPExt:   ss << "fpext "   << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::FPToSI:  ss << "fptosi "  << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::SIToFP:  ss << "sitofp "  << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::Bitcast: ss << "bitcast " << operands[0].type->str() << " " << operands[0].str() << " to " << type->str(); break;
        case Opcode::PtrToInt:ss << "ptrtoint ptr " << operands[0].str() << " to " << type->str(); break;
        case Opcode::IntToPtr:ss << "inttoptr " << operands[0].type->str() << " " << operands[0].str() << " to ptr"; break;

        case Opcode::Br:
            ss << "br " << operands[0].str();
            break;
        case Opcode::BrCond:
            ss << "br_cond i1 " << operands[0].str() << ", " << operands[1].str() << ", " << operands[2].str();
            break;
        case Opcode::Switch: {
            ss << "switch " << operands[0].type->str() << " " << operands[0].str() << ", $" << defaultLabel << " [";
            for (size_t i = 0; i < switchCases.size(); ++i) {
                if (i > 0) ss << ", ";
                ss << switchCases[i].first << ": $" << switchCases[i].second;
            }
            ss << "]";
            break;
        }
        case Opcode::Ret:
            if (operands.empty() || operands[0].type->isVoid()) {
                ss << "ret void";
            } else {
                ss << "ret " << operands[0].type->str() << " " << operands[0].str();
            }
            break;
        case Opcode::Unreachable:
            ss << "unreachable";
            break;

        case Opcode::Call: {
            ss << "call " << (type ? type->str() : "void") << " " << operands[0].str() << "(";
            for (size_t i = 1; i < operands.size(); ++i) {
                if (i > 1) ss << ", ";
                ss << operands[i].type->str() << " " << operands[i].str();
            }
            ss << ")";
            break;
        }
        case Opcode::CallIndirect: {
            ss << "call_indirect " << (type ? type->str() : "void") << " " << operands[0].str() << "(";
            for (size_t i = 1; i < operands.size(); ++i) {
                if (i > 1) ss << ", ";
                ss << operands[i].type->str() << " " << operands[i].str();
            }
            ss << ")";
            break;
        }
        case Opcode::MakeInterface:
            ss << "make_interface ptr " << operands[0].str() << ", ptr " << operands[1].str();
            break;
        case Opcode::MakeClosure:
            ss << "make_closure ptr " << operands[0].str() << ", ptr " << operands[1].str();
            break;
        case Opcode::MakeSlice:
            ss << "make_slice ptr " << operands[0].str() << ", i64 " << operands[1].str();
            break;
        case Opcode::ExtractFat:
            ss << "extract_fat " << operands[0].type->str() << " " << operands[0].str() << ", " << operands[1].str();
            break;

        case Opcode::Throw:
            ss << "throw ptr " << operands[0].str();
            break;
        case Opcode::Invoke:
            ss << "invoke " << operands[0].str() << " to " << operands[1].str() << " unwind " << operands[2].str();
            break;
        case Opcode::LandingPad:
            ss << "landingpad";
            break;
        case Opcode::PushHandler:
            ss << "push_handler " << operands[0].str();
            break;
        case Opcode::PopHandler:
            ss << "pop_handler";
            break;
        case Opcode::InlineAsm: {
            ss << "inline_asm \"" << asmString << "\", \"" << asmConstraints << "\"";
            for (size_t i = 0; i < operands.size(); ++i) {
                ss << ", " << operands[i].type->str() << " " << operands[i].str();
            }
            if (!clobbers.empty()) {
                ss << " ~[";
                for (size_t i = 0; i < clobbers.size(); ++i) {
                    if (i > 0) ss << ", ";
                    ss << "\"" << clobbers[i] << "\"";
                }
                ss << "]";
            }
            break;
        }
    }
    return ss.str();
}

// ============================================================================
// BasicBlock Implementations
// ============================================================================

bool BasicBlock::hasTerminator() const {
    return !instructions.empty() && instructions.back()->isTerminator();
}

void BasicBlock::dump(std::ostream& os) const {
    os << name << ":\n";
    for (const auto& inst : instructions) {
        os << "    " << inst->str() << "\n";
    }
}

// ============================================================================
// Function Implementations
// ============================================================================

BasicBlock* Function::createBlock(const std::string& name) {
    std::string bname = name.empty() ? ("bb." + std::to_string(blocks.size())) : name;
    blocks.push_back(std::make_unique<BasicBlock>(bname));
    return blocks.back().get();
}

void Function::dump(std::ostream& os) const {
    if (isExtern) {
        os << "extern fn @" << name << "(";
        for (size_t i = 0; i < params.size(); ++i) {
            if (i > 0) os << ", ";
            os << params[i].second->str();
        }
        if (isVariadic) {
            if (!params.empty()) os << ", ";
            os << "...";
        }
        os << ") -> " << (returnType ? returnType->str() : "void") << "\n\n";
        return;
    }

    os << "fn @" << name << "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) os << ", ";
        os << params[i].second->str() << " %" << params[i].first;
    }
    os << ") -> " << (returnType ? returnType->str() : "void") << " {\n";
    for (const auto& bb : blocks) {
        bb->dump(os);
    }
    os << "}\n\n";
}

// ============================================================================
// Module Implementations
// ============================================================================

Function* Module::getFunction(const std::string& name) const {
    for (const auto& f : functions) {
        if (f->name == name) return f.get();
    }
    return nullptr;
}

Function* Module::createFunction(const std::string& name, TypePtr ret, const std::vector<std::pair<std::string, TypePtr>>& params) {
    auto fn = std::make_unique<Function>();
    fn->name = name;
    fn->returnType = ret;
    fn->params = params;
    functions.push_back(std::move(fn));
    return functions.back().get();
}

void Module::dump(std::ostream& os) const {
    os << ";; EIR Module: " << (name.empty() ? "eskiu" : name) << "\n";
    if (!targetTriple.empty()) {
        os << ";; Target: " << targetTriple << "\n";
    }
    os << "\n";

    if (!structTypes.empty()) {
        os << ";; Type definitions\n";
        for (const auto& st : structTypes) {
            os << "type @" << st->name << " = { ";
            for (size_t i = 0; i < st->members.size(); ++i) {
                if (i > 0) os << ", ";
                os << st->members[i]->str();
            }
            os << " }\n";
        }
        os << "\n";
    }

    if (!globals.empty()) {
        os << ";; Globals\n";
        for (const auto& g : globals) {
            os << "global @" << g.name << " : " << g.type->str();
            if (!g.elements.empty()) {
                os << " = [";
                for (size_t i = 0; i < g.elements.size(); ++i) os << (i ? ", " : "") << g.elements[i].str();
                os << "]";
            } else if (g.init.kind != OperandKind::Variable) {
                os << " = " << g.init.str();
            }
            os << "\n";
        }
        os << "\n";
    }

    for (const auto& fn : functions) {
        fn->dump(os);
    }
}

std::string Module::str() const {
    std::ostringstream ss;
    dump(ss);
    return ss.str();
}

size_t getTypeSize(TypePtr t, const Module* mod) {
    if (!t) return 0;
    switch (t->kind) {
        case TypeKind::Void: return 0;
        case TypeKind::Int: {
            if (t->bitWidth == 1) return 1;
            return (t->bitWidth + 7) / 8;
        }
        case TypeKind::Float: return (t->bitWidth == 32) ? 4 : 8;
        case TypeKind::Pointer: return 8;
        case TypeKind::Array: return t->arraySize * getTypeSize(t->elemType, mod);
        case TypeKind::Slice:
        case TypeKind::Interface:
        case TypeKind::Closure: return 16;
        case TypeKind::Struct: {
            if (mod) {
                size_t sz = mod->getStructSize(t->nominalName());
                if (sz > 0) return sz;
            }
            size_t total = 0;
            for (const auto& m : t->members) {
                size_t al = getTypeAlign(m, mod);
                total = (total + al - 1) & ~(al - 1);
                total += getTypeSize(m, mod);
            }
            size_t maxAl = getTypeAlign(t, mod);
            return (total + maxAl - 1) & ~(maxAl - 1);
        }
        case TypeKind::Function: return 8;
    }
    return 8;
}

size_t getTypeAlign(TypePtr t, const Module* mod) {
    if (!t) return 1;
    switch (t->kind) {
        case TypeKind::Void: return 1;
        case TypeKind::Int: {
            size_t sz = getTypeSize(t, mod);
            return sz > 8 ? 8 : (sz == 0 ? 1 : sz);
        }
        case TypeKind::Float: return (t->bitWidth == 32) ? 4 : 8;
        case TypeKind::Pointer: return 8;
        case TypeKind::Array: return getTypeAlign(t->elemType, mod);
        case TypeKind::Slice:
        case TypeKind::Interface:
        case TypeKind::Closure: return 8;
        case TypeKind::Struct: {
            if (mod) {
                auto it = mod->structLayouts.find(t->nominalName());
                if (it != mod->structLayouts.end() && it->second.alignment > 0) {
                    return it->second.alignment;
                }
            }
            size_t maxAl = 1;
            for (const auto& m : t->members) {
                size_t al = getTypeAlign(m, mod);
                if (al > maxAl) maxAl = al;
            }
            return maxAl;
        }
        case TypeKind::Function: return 8;
    }
    return 8;
}

} // namespace eir
