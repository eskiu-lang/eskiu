#include "eir_asm_int.h"

namespace eir {

void Arm64Emitter::movImm(const std::string& reg, int64_t v, std::ostream& os) {
    uint64_t uv = static_cast<uint64_t>(v);
    if (v >= 0 && v <= 65535) { os << "    mov " << reg << ", #" << v << "\n"; return; }
    if (v < 0 && v >= -65536) { os << "    mov " << reg << ", #" << v << "\n"; return; }
    os << "    movz " << reg << ", #" << (uv & 0xffff) << "\n";
    for (int sh = 16; sh < 64; sh += 16) {
        uint64_t part = (uv >> sh) & 0xffff;
        if (part) os << "    movk " << reg << ", #" << part << ", lsl #" << sh << "\n";
    }
}

void Arm64Emitter::addImm(const std::string& rd, const std::string& rn, size_t imm, std::ostream& os) {
    if (imm <= 4095) {
        if (imm == 0 && rd == rn) return;
        os << "    add " << rd << ", " << rn << ", #" << imm << "\n";
    } else {
        movImm("x16", static_cast<int64_t>(imm), os);
        os << "    add " << rd << ", " << rn << ", x16\n";
    }
}

void Arm64Emitter::memOp(const std::string& op, const std::string& reg, const std::string& base, size_t off,
           size_t scale, std::ostream& os) {
    if (off % scale == 0 && off / scale <= 4095) {
        os << "    " << op << " " << reg << ", [" << base << ", #" << off << "]\n";
    } else {
        movImm("x16", static_cast<int64_t>(off), os);
        os << "    " << op << " " << reg << ", [" << base << ", x16]\n";
    }
}

size_t Arm64Emitter::slotOf(const std::string& v) const {
    auto it = slots.find(v);
    if (it == slots.end()) throw std::runtime_error("EIR arm64: no slot for " + v + " in " + curFn->name);
    return it->second;
}

void Arm64Emitter::storeX(const std::string& reg, const std::string& var, std::ostream& os) {
    auto it = resultTypes.find(var);
    if (it != resultTypes.end() && reg.size() > 1 && reg[0] == 'x' && isdigit(static_cast<unsigned char>(reg[1])))
        normalize(std::stoi(reg.substr(1)), it->second, os);
    memOp("str", reg, "sp", slotOf(var), 8, os);
}

void Arm64Emitter::storeFloat(int fr, bool is32, const std::string& var, std::ostream& os) {
    if (is32) memOp("str", "s" + std::to_string(fr), "sp", slotOf(var), 4, os);
    else memOp("str", "d" + std::to_string(fr), "sp", slotOf(var), 8, os);
}

void Arm64Emitter::normalize(int r, const TypePtr& t, std::ostream& os) {
    if (!t || !t->isInt()) return;
    std::string x = "x" + std::to_string(r), w = "w" + std::to_string(r);
    switch (t->bitWidth) {
        case 1: os << "    and " << x << ", " << x << ", #1\n"; break;
        case 8: os << (t->isSigned ? "    sxtb " + x + ", " + w + "\n" : "    and " + x + ", " + x + ", #0xff\n"); break;
        case 16: os << (t->isSigned ? "    sxth " + x + ", " + w + "\n" : "    and " + x + ", " + x + ", #0xffff\n"); break;
        case 32: os << (t->isSigned ? "    sxtw " + x + ", " + w + "\n" : "    mov " + w + ", " + w + "\n"); break;
        default: break;
    }
}

void Arm64Emitter::loadAddrOfSym(const std::string& reg, const std::string& name, std::ostream& os) {
    std::string sym = cabiFns.count(name) ? cabiSym(name) : bodySym(name);
    bool local = definedSyms.count(name) > 0;
    if (isDarwin) {
        if (local) {
            os << "    adrp " << reg << ", " << sym << "@PAGE\n";
            os << "    add " << reg << ", " << reg << ", " << sym << "@PAGEOFF\n";
        } else {
            os << "    adrp " << reg << ", " << sym << "@GOTPAGE\n";
            os << "    ldr " << reg << ", [" << reg << ", " << sym << "@GOTPAGEOFF]\n";
        }
    } else {
        if (local) {
            os << "    adrp " << reg << ", " << sym << "\n";
            os << "    add " << reg << ", " << reg << ", :lo12:" << sym << "\n";
        } else {
            os << "    adrp " << reg << ", :got:" << sym << "\n";
            os << "    ldr " << reg << ", [" << reg << ", :got_lo12:" << sym << "]\n";
        }
    }
}

void Arm64Emitter::loadOperand(const Operand& op, int regIdx, std::ostream& os) {
    std::string reg = "x" + std::to_string(regIdx);
    switch (op.kind) {
        case OperandKind::ConstantInt:
            movImm(reg, truncConst(op.intVal, op.type), os);
            break;
        case OperandKind::ConstantNull:
            os << "    mov " << reg << ", #0\n";
            break;
        case OperandKind::ConstantFloat: {
            if (op.type && op.type->bitWidth == 32) {
                float f = static_cast<float>(op.floatVal);
                uint32_t bits;
                std::memcpy(&bits, &f, sizeof bits);
                movImm(reg, bits, os);
            } else {
                uint64_t bits;
                std::memcpy(&bits, &op.floatVal, sizeof bits);
                movImm(reg, static_cast<int64_t>(bits), os);
            }
            break;
        }
        case OperandKind::ConstantString: {
            const std::string& lbl = stringTable[op.text];
            if (isDarwin) {
                os << "    adrp " << reg << ", " << lbl << "@PAGE\n";
                os << "    add " << reg << ", " << reg << ", " << lbl << "@PAGEOFF\n";
            } else {
                os << "    adrp " << reg << ", " << lbl << "\n";
                os << "    add " << reg << ", " << reg << ", :lo12:" << lbl << "\n";
            }
            break;
        }
        case OperandKind::GlobalRef:
            loadAddrOfSym(reg, op.text, os);
            break;
        case OperandKind::Variable: {
            // A value nothing defined (a void result) reads as 0, as in the EVM.
            if (!slots.count(op.text)) os << "    mov " << reg << ", #0\n";
            else if (addrVars.count(op.text)) addImm(reg, "sp", slotOf(op.text), os);
            else memOp("ldr", reg, "sp", slotOf(op.text), 8, os);
            break;
        }
        default:
            throw std::runtime_error("EIR arm64: unsupported operand " + op.str());
    }
}

void Arm64Emitter::loadFloat(const Operand& op, int r, bool want32, std::ostream& os) {
    std::string s = "s" + std::to_string(r), d = "d" + std::to_string(r);
    std::string dst = want32 ? s : d;
    if (op.kind == OperandKind::ConstantFloat) {
        if (want32) {
            float f = static_cast<float>(op.floatVal);
            uint32_t bits;
            std::memcpy(&bits, &f, sizeof bits);
            movImm("w16", bits, os);
            os << "    fmov " << s << ", w16\n";
        } else {
            uint64_t bits;
            std::memcpy(&bits, &op.floatVal, sizeof bits);
            movImm("x16", static_cast<int64_t>(bits), os);
            os << "    fmov " << d << ", x16\n";
        }
        return;
    }
    if (op.kind == OperandKind::Variable && op.type && op.type->isFloat() && slots.count(op.text)) {
        bool src32 = op.type->bitWidth == 32;
        if (src32) memOp("ldr", s, "sp", slotOf(op.text), 4, os);
        else memOp("ldr", d, "sp", slotOf(op.text), 8, os);
        if (src32 && !want32) os << "    fcvt " << d << ", " << s << "\n";
        if (!src32 && want32) os << "    fcvt " << s << ", " << d << "\n";
        return;
    }
    loadOperand(op, 16, os);
    bool uns = op.type && op.type->isInt() && !op.type->isSigned;
    os << "    " << (uns ? "ucvtf " : "scvtf ") << dst << ", x16\n";
}

void Arm64Emitter::loadSized(const std::string& xreg, const std::string& base, size_t off, const TypePtr& t, std::ostream& os) {
    std::string w = "w" + xreg.substr(1);
    if (!t || !t->isInt() || t->bitWidth > 32) { memOp("ldr", xreg, base, off, 8, os); return; }
    if (t->bitWidth <= 8) memOp(t->isSigned && t->bitWidth > 1 ? "ldrsb" : "ldrb", t->isSigned && t->bitWidth > 1 ? xreg : w, base, off, 1, os);
    else if (t->bitWidth <= 16) memOp(t->isSigned ? "ldrsh" : "ldrh", t->isSigned ? xreg : w, base, off, 2, os);
    else memOp(t->isSigned ? "ldrsw" : "ldr", t->isSigned ? xreg : w, base, off, 4, os);
}

void Arm64Emitter::copyBytes(size_t size, std::ostream& os) {
    if (size <= 128) {
        size_t b = 0;
        for (; b + 8 <= size; b += 8) {
            os << "    ldr x11, [x9, #" << b << "]\n";
            os << "    str x11, [x10, #" << b << "]\n";
        }
        for (; b < size; ++b) {
            os << "    ldrb w11, [x9, #" << b << "]\n";
            os << "    strb w11, [x10, #" << b << "]\n";
        }
        return;
    }
    std::string loop = newLocal("copy");
    movImm("x12", static_cast<int64_t>(size / 8), os);
    os << loop << ":\n";
    os << "    ldr x11, [x9], #8\n";
    os << "    str x11, [x10], #8\n";
    os << "    subs x12, x12, #1\n";
    os << "    b.ne " << loop << "\n";
    for (size_t b = 0; b < size % 8; ++b) {
        os << "    ldrb w11, [x9], #1\n";
        os << "    strb w11, [x10], #1\n";
    }
}

void Arm64Emitter::divCheck(const Operand& divisor, std::ostream& os) {
    if (divisor.kind == OperandKind::ConstantInt && truncConst(divisor.intVal, divisor.type) != 0) return;
    std::string ok = newLocal("divok");
    os << "    cbnz x10, " << ok << "\n";
    os << "    brk #1\n";
    os << ok << ":\n";
}

void Arm64Emitter::exceptionCheck(std::ostream& os, const std::string& unwindLabel) {
    if (!throws) return;
    os << "    bl " << mangleSym("__eir_exc_pending") << "\n";
    os << "    cbnz x0, " << unwindLabel << "\n";
}

} // namespace eir
