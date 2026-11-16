#include "eir_asm_int.h"

namespace eir {

void Arm64Emitter::emitInstruction(const Instruction* inst, const std::string& retLabel,
                     const std::string& unwindLabel, std::ostream& os) {
    const auto& ops = inst->operands;
    switch (inst->op) {
        case Opcode::Alloca: case Opcode::Store: case Opcode::Load: case Opcode::FieldPtr:
        case Opcode::OffsetPtr: case Opcode::Gep:
            emitMemoryInst(inst, os, ops);
            break;
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::And: case Opcode::Or:
        case Opcode::Xor: case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
        case Opcode::SDiv: case Opcode::UDiv: case Opcode::SRem: case Opcode::URem:
        case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
        case Opcode::Not: case Opcode::Neg:
            emitArithInst(inst, os, ops);
            break;
        case Opcode::ICmp: case Opcode::FCmp: case Opcode::SIToFP: case Opcode::FPToSI:
        case Opcode::FPTrunc: case Opcode::FPExt: case Opcode::Trunc: case Opcode::SExt:
        case Opcode::ZExt: case Opcode::Bitcast: case Opcode::PtrToInt: case Opcode::IntToPtr:
            emitCompareConvInst(inst, os, ops);
            break;
        case Opcode::Br: case Opcode::BrCond: case Opcode::Switch: case Opcode::Call:
        case Opcode::CallIndirect: case Opcode::MakeClosure: case Opcode::MakeInterface:
        case Opcode::MakeSlice: case Opcode::ExtractFat: case Opcode::Ret:
        case Opcode::Unreachable: case Opcode::Throw: case Opcode::LandingPad:
        case Opcode::PushHandler: case Opcode::PopHandler: case Opcode::InlineAsm: default:
            emitControlInst(inst, retLabel, unwindLabel, os, ops);
            break;
    }
}

void Arm64Emitter::emitMemoryInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops) {
    switch (inst->op) {
        // A stack slot starts zeroed each time its alloca runs, as in the EVM.
        case Opcode::Alloca: {
            size_t sz = alignUp(sizeOf(inst->type), 8);
            addImm("x10", "sp", slotOf(inst->resultVar), os);
            if (sz <= 128) {
                for (size_t b = 0; b < sz; b += 8) os << "    str xzr, [x10, #" << b << "]\n";
            } else {
                std::string loop = newLocal("zero");
                movImm("x12", static_cast<int64_t>(sz / 8), os);
                os << loop << ":\n    str xzr, [x10], #8\n    subs x12, x12, #1\n    b.ne " << loop << "\n";
            }
            break;
        }

        case Opcode::Store: {
            TypePtr valTy = ops[0].type;
            TypePtr dstTy = (ops[1].type && ops[1].type->pointee) ? ops[1].type->pointee : valTy;
            if (!dstTy) dstTy = valTy;
            if (isAggregate(dstTy) || (!dstTy && isAggregate(valTy))) {
                loadOperand(ops[0], 9, os);
                loadOperand(ops[1], 10, os);
                copyBytes(sizeOf(dstTy ? dstTy : valTy), os);
                break;
            }
            loadOperand(ops[1], 10, os);
            if (dstTy && dstTy->isFloat()) {
                bool is32 = dstTy->bitWidth == 32;
                loadFloat(ops[0], 16, is32, os);
                os << "    str " << (is32 ? "s16" : "d16") << ", [x10]\n";
                break;
            }
            bool valFloat = (valTy && valTy->isFloat()) || ops[0].kind == OperandKind::ConstantFloat;
            if (valFloat && dstTy && dstTy->isInt()) {
                loadFloat(ops[0], 16, false, os);
                os << "    fcvtzs x9, d16\n";
            } else {
                loadOperand(ops[0], 9, os);
            }
            size_t w = (dstTy && dstTy->isInt()) ? getTypeSize(dstTy) : 8;
            if (w == 1) os << "    strb w9, [x10]\n";
            else if (w == 2) os << "    strh w9, [x10]\n";
            else if (w == 4) os << "    str w9, [x10]\n";
            else os << "    str x9, [x10]\n";
            break;
        }

        case Opcode::Load: {
            loadOperand(ops[0], 10, os);
            TypePtr ty = inst->type;
            if (isAggregate(ty)) {
                storeX("x10", inst->resultVar, os);
            } else if (ty && ty->isFloat()) {
                bool is32 = ty->bitWidth == 32;
                os << "    ldr " << (is32 ? "s16" : "d16") << ", [x10]\n";
                storeFloat(16, is32, inst->resultVar, os);
            } else {
                loadSized("x9", "x10", 0, ty, os);
                storeX("x9", inst->resultVar, os);
            }
            break;
        }

        case Opcode::FieldPtr: {
            std::string structName = ops[0].text;
            loadOperand(ops[1], 10, os);
            std::string fieldName = ops[2].text;
            size_t off = currentMod->getFieldOffset(structName, fieldName);
            if (off == 0 && (structName.empty() || structName == "slice")) {
                if (fieldName == "len") off = 8;
            }
            addImm("x10", "x10", off, os);
            storeX("x10", inst->resultVar, os);
            break;
        }

        case Opcode::OffsetPtr: {
            loadOperand(ops[0], 9, os);
            loadOperand(ops[1], 10, os);
            os << "    add x9, x9, x10\n";
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::Gep: {
            loadOperand(ops[0], 9, os);
            loadOperand(ops[1], 10, os);
            TypePtr elemType = inst->type->pointee ? inst->type->pointee : (inst->type->elemType ? inst->type->elemType : inst->type);
            size_t elemSize = sizeOf(elemType);
            if (elemSize == 1) os << "    add x9, x9, x10\n";
            else if (elemSize == 2) os << "    add x9, x9, x10, lsl #1\n";
            else if (elemSize == 4) os << "    add x9, x9, x10, lsl #2\n";
            else if (elemSize == 8) os << "    add x9, x9, x10, lsl #3\n";
            else {
                movImm("x11", static_cast<int64_t>(elemSize), os);
                os << "    madd x9, x10, x11, x9\n";
            }
            storeX("x9", inst->resultVar, os);
            break;
        }
        default:
            break;
    }
}

void Arm64Emitter::emitArithInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops) {
    switch (inst->op) {
        // Integer values follow the EVM: 64-bit arithmetic, wrapped to 32 bits for a
        // 32-bit add/sub/mul, a narrow unsigned bitwise result masked to its width,
        // shifts done at the type's width with the count masked.
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul: {
            loadOperand(ops[0], 9, os);
            loadOperand(ops[1], 10, os);
            bool is32 = inst->type && inst->type->isInt() && inst->type->bitWidth == 32;
            const char* mn = inst->op == Opcode::Add ? "add" : inst->op == Opcode::Sub ? "sub" : "mul";
            if (is32) {
                os << "    " << mn << " w9, w9, w10\n";
                normalize(9, inst->type, os);
            } else {
                os << "    " << mn << " x9, x9, x10\n";
            }
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::And: case Opcode::Or: case Opcode::Xor: {
            loadOperand(ops[0], 9, os);
            loadOperand(ops[1], 10, os);
            const char* mn = inst->op == Opcode::And ? "and" : inst->op == Opcode::Or ? "orr" : "eor";
            os << "    " << mn << " x9, x9, x10\n";
            if (inst->type && inst->type->isInt() && !inst->type->isSigned && inst->type->bitWidth > 1 && inst->type->bitWidth < 64)
                normalize(9, inst->type, os);
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::Shl: case Opcode::LShr: case Opcode::AShr: {
            loadOperand(ops[0], 9, os);
            loadOperand(ops[1], 10, os);
            unsigned bw = (inst->type && inst->type->isInt() && inst->type->bitWidth > 1) ? inst->type->bitWidth : 64;
            if (bw < 32) {
                os << "    and x10, x10, #" << (bw - 1) << "\n";
                if (inst->op == Opcode::AShr) os << "    sbfx x9, x9, #0, #" << bw << "\n";
                else os << "    ubfx x9, x9, #0, #" << bw << "\n";
                const char* mn = inst->op == Opcode::Shl ? "lsl" : inst->op == Opcode::LShr ? "lsr" : "asr";
                os << "    " << mn << " x9, x9, x10\n";
                if (inst->op == Opcode::AShr) os << "    sbfx x9, x9, #0, #" << bw << "\n";
                else os << "    ubfx x9, x9, #0, #" << bw << "\n";
            } else if (bw == 32) {
                const char* mn = inst->op == Opcode::Shl ? "lsl" : inst->op == Opcode::LShr ? "lsr" : "asr";
                os << "    " << mn << " w9, w9, w10\n";
                if (inst->op == Opcode::LShr) os << "    mov w9, w9\n";
                else if (inst->op == Opcode::AShr) os << "    sxtw x9, w9\n";
                else normalize(9, inst->type, os);
            } else {
                const char* mn = inst->op == Opcode::Shl ? "lsl" : inst->op == Opcode::LShr ? "lsr" : "asr";
                os << "    " << mn << " x9, x9, x10\n";
            }
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::SDiv: case Opcode::UDiv: case Opcode::SRem: case Opcode::URem: {
            loadOperand(ops[0], 9, os);
            loadOperand(ops[1], 10, os);
            divCheck(ops[1], os);
            bool sgn = inst->op == Opcode::SDiv || inst->op == Opcode::SRem;
            bool is32 = sgn && inst->type && inst->type->isInt() && inst->type->bitWidth == 32;
            std::string r = is32 ? "w" : "x";
            os << "    " << (sgn ? "sdiv " : "udiv ") << r << "11, " << r << "9, " << r << "10\n";
            if (inst->op == Opcode::SRem || inst->op == Opcode::URem)
                os << "    msub " << r << "9, " << r << "11, " << r << "10, " << r << "9\n";
            else
                os << "    mov " << r << "9, " << r << "11\n";
            if (is32) os << "    sxtw x9, w9\n";
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv: {
            bool is32 = inst->type && inst->type->bitWidth == 32;
            loadFloat(ops[0], 16, is32, os);
            loadFloat(ops[1], 17, is32, os);
            std::string a = is32 ? "s16" : "d16", b = is32 ? "s17" : "d17";
            const char* mn = inst->op == Opcode::FAdd ? "fadd" : inst->op == Opcode::FSub ? "fsub"
                           : inst->op == Opcode::FMul ? "fmul" : "fdiv";
            os << "    " << mn << " " << a << ", " << a << ", " << b << "\n";
            storeFloat(16, is32, inst->resultVar, os);
            break;
        }

        case Opcode::Not: {
            loadOperand(ops[0], 9, os);
            os << "    mvn x9, x9\n";
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::Neg: {
            if (inst->type && inst->type->isFloat()) {
                bool is32 = inst->type->bitWidth == 32;
                loadFloat(ops[0], 16, is32, os);
                os << "    fneg " << (is32 ? "s16, s16" : "d16, d16") << "\n";
                storeFloat(16, is32, inst->resultVar, os);
            } else {
                loadOperand(ops[0], 9, os);
                os << "    neg x9, x9\n";
                storeX("x9", inst->resultVar, os);
            }
            break;
        }
        default:
            break;
    }
}

void Arm64Emitter::emitCompareConvInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops) {
    switch (inst->op) {
        case Opcode::ICmp: {
            loadOperand(ops[0], 9, os);
            loadOperand(ops[1], 10, os);
            if (ops[0].type && ops[0].type->isInterface()) os << "    cbz x9, " << "1f\n    ldr x9, [x9]\n1:\n";
            if (ops[1].type && ops[1].type->isInterface()) os << "    cbz x10, " << "1f\n    ldr x10, [x10]\n1:\n";
            os << "    cmp x9, x10\n";
            const char* c = "eq";
            switch (inst->icmpCond) {
                case ICmpCond::Eq: c = "eq"; break;
                case ICmpCond::Ne: c = "ne"; break;
                case ICmpCond::Slt: c = "lt"; break;
                case ICmpCond::Sle: c = "le"; break;
                case ICmpCond::Sgt: c = "gt"; break;
                case ICmpCond::Sge: c = "ge"; break;
                case ICmpCond::Ult: c = "lo"; break;
                case ICmpCond::Ule: c = "ls"; break;
                case ICmpCond::Ugt: c = "hi"; break;
                case ICmpCond::Uge: c = "hs"; break;
            }
            os << "    cset x9, " << c << "\n";
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::FCmp: {
            bool is32 = (ops[0].type && ops[0].type->isFloat() && ops[0].type->bitWidth == 32) &&
                        (!ops[1].type || !ops[1].type->isFloat() || ops[1].type->bitWidth == 32);
            loadFloat(ops[0], 16, is32, os);
            loadFloat(ops[1], 17, is32, os);
            os << "    fcmp " << (is32 ? "s16, s17" : "d16, d17") << "\n";
            const char* c = "eq";
            switch (inst->fcmpCond) {
                case FCmpCond::Oeq: c = "eq"; break;
                case FCmpCond::One: c = "ne"; break;
                case FCmpCond::Olt: c = "mi"; break;
                case FCmpCond::Ole: c = "ls"; break;
                case FCmpCond::Ogt: c = "gt"; break;
                case FCmpCond::Oge: c = "ge"; break;
            }
            os << "    cset x9, " << c << "\n";
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::SIToFP: {
            bool is32 = inst->type && inst->type->bitWidth == 32;
            loadOperand(ops[0], 9, os);
            bool uns = ops[0].type && ops[0].type->isInt() && !ops[0].type->isSigned;
            os << "    " << (uns ? "ucvtf " : "scvtf ") << (is32 ? "s16" : "d16") << ", x9\n";
            storeFloat(16, is32, inst->resultVar, os);
            break;
        }

        case Opcode::FPToSI: {
            bool src32 = ops[0].type && ops[0].type->isFloat() && ops[0].type->bitWidth == 32;
            loadFloat(ops[0], 16, src32, os);
            TypePtr dt = inst->type;
            bool uns = dt && dt->isInt() && !dt->isSigned;
            unsigned bw = dt && dt->isInt() ? dt->bitWidth : 32;
            std::string src = src32 ? "s16" : "d16";
            if (bw > 32) {
                os << "    " << (uns ? "fcvtzu" : "fcvtzs") << " x9, " << src << "\n";
            } else {
                os << "    " << (uns ? "fcvtzu" : "fcvtzs") << " w9, " << src << "\n";
                if (bw < 32 && bw > 1) {
                    int64_t hi = uns ? ((1LL << bw) - 1) : ((1LL << (bw - 1)) - 1);
                    movImm("w10", hi, os);
                    os << "    cmp w9, w10\n";
                    os << "    csel w9, w10, w9, " << (uns ? "hi" : "gt") << "\n";
                    if (!uns) {
                        movImm("w10", -(1LL << (bw - 1)), os);
                        os << "    cmp w9, w10\n";
                        os << "    csel w9, w10, w9, lt\n";
                    }
                }
            }
            normalize(9, dt, os);
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::FPTrunc: {
            loadFloat(ops[0], 16, true, os);
            storeFloat(16, true, inst->resultVar, os);
            break;
        }

        case Opcode::FPExt: {
            loadFloat(ops[0], 16, false, os);
            storeFloat(16, false, inst->resultVar, os);
            break;
        }

        case Opcode::Trunc: {
            loadOperand(ops[0], 9, os);
            normalize(9, inst->type, os);
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::SExt:
        case Opcode::ZExt: {
            loadOperand(ops[0], 9, os);
            unsigned bw = ops[0].type && ops[0].type->isInt() ? ops[0].type->bitWidth : 64;
            if (bw < 64) {
                if (inst->op == Opcode::SExt) os << "    sbfx x9, x9, #0, #" << bw << "\n";
                else os << "    ubfx x9, x9, #0, #" << bw << "\n";
            }
            normalize(9, inst->type, os);
            storeX("x9", inst->resultVar, os);
            break;
        }

        case Opcode::Bitcast:
        case Opcode::PtrToInt:
        case Opcode::IntToPtr: {
            bool srcF = ops[0].type && ops[0].type->isFloat();
            bool dstF = inst->type && inst->type->isFloat();
            if (srcF && dstF) {
                bool is32 = inst->type->bitWidth == 32;
                loadFloat(ops[0], 16, is32, os);
                storeFloat(16, is32, inst->resultVar, os);
            } else if (srcF) {
                bool is32 = ops[0].type->bitWidth == 32;
                loadFloat(ops[0], 16, is32, os);
                os << "    fmov " << (is32 ? "w9, s16" : "x9, d16") << "\n";
                normalize(9, inst->type, os);
                storeX("x9", inst->resultVar, os);
            } else if (dstF) {
                loadOperand(ops[0], 9, os);
                bool is32 = inst->type->bitWidth == 32;
                os << "    fmov " << (is32 ? "s16, w9" : "d16, x9") << "\n";
                storeFloat(16, is32, inst->resultVar, os);
            } else {
                loadOperand(ops[0], 9, os);
                normalize(9, inst->type, os);
                storeX("x9", inst->resultVar, os);
            }
            break;
        }
        default:
            break;
    }
}

void Arm64Emitter::emitControlInst(const Instruction* inst, const std::string& retLabel, const std::string& unwindLabel, std::ostream& os, const std::vector<Operand>& ops) {
    switch (inst->op) {
        case Opcode::Br:
            os << "    b " << blockLabel(ops[0].text) << "\n";
            break;

        case Opcode::BrCond:
            loadOperand(ops[0], 9, os);
            os << "    cbnz x9, " << blockLabel(ops[1].text) << "\n";
            os << "    b " << blockLabel(ops[2].text) << "\n";
            break;

        case Opcode::Switch: {
            loadOperand(ops[0], 9, os);
            for (const auto& sc : inst->switchCases) {
                movImm("x10", sc.first, os);
                os << "    cmp x9, x10\n";
                os << "    b.eq " << blockLabel(sc.second) << "\n";
            }
            if (!inst->defaultLabel.empty()) os << "    b " << blockLabel(inst->defaultLabel) << "\n";
            break;
        }

        case Opcode::Call:
        case Opcode::CallIndirect:
            emitCall(inst, unwindLabel, os);
            break;

        case Opcode::MakeClosure:
        case Opcode::MakeInterface:
        case Opcode::MakeSlice: {
            size_t slot = slotOf(inst->resultVar);
            loadOperand(ops[0], 9, os);
            memOp("str", "x9", "sp", slot, 8, os);
            loadOperand(ops[1], 9, os);
            memOp("str", "x9", "sp", slot + 8, 8, os);
            break;
        }

        case Opcode::ExtractFat: {
            loadOperand(ops[0], 10, os);
            int64_t idx = ops[1].kind == OperandKind::ConstantInt ? ops[1].intVal : 0;
            if (inst->type && inst->type->isFloat()) {
                bool is32 = inst->type->bitWidth == 32;
                memOp("ldr", is32 ? "s16" : "d16", "x10", static_cast<size_t>(idx) * 8, is32 ? 4 : 8, os);
                storeFloat(16, is32, inst->resultVar, os);
            } else {
                memOp("ldr", "x9", "x10", static_cast<size_t>(idx) * 8, 8, os);
                normalize(9, inst->type, os);
                storeX("x9", inst->resultVar, os);
            }
            break;
        }

        case Opcode::Ret: {
            bool hasVal = !ops.empty() && ops[0].type && !ops[0].type->isVoid();
            if (fnHasSret) {
                memOp("ldr", "x10", "sp", sretSlot, 8, os);
                if (hasVal) {
                    loadOperand(ops[0], 9, os);
                    copyBytes(sizeOf(curFn->returnType), os);
                }
                memOp("ldr", "x0", "sp", sretSlot, 8, os);
            } else if (hasVal && curFn->returnType && curFn->returnType->isFloat()) {
                loadFloat(ops[0], 0, curFn->returnType->bitWidth == 32, os);
            } else if (hasVal) {
                loadOperand(ops[0], 0, os);
            } else if (curFn->name == "main") {
                os << "    mov x0, #0\n";
            }
            os << "    b " << retLabel << "\n";
            break;
        }

        case Opcode::Unreachable:
            os << "    brk #1\n";
            break;

        case Opcode::Throw:
            loadOperand(ops[0], 0, os);
            os << "    bl " << mangleSym("__eir_throw") << "\n";
            os << "    b " << unwindLabel << "\n";
            break;

        case Opcode::LandingPad:
            os << "    bl " << mangleSym("__eir_take_exception") << "\n";
            if (!inst->resultVar.empty()) storeX("x0", inst->resultVar, os);
            break;

        case Opcode::PushHandler: {
            int id = handlerIds.at(ops[0].text);
            std::string ok = newLocal("hok");
            memOp("ldr", "x9", "sp", handlerDepthSlot, 8, os);
            movImm("x10", static_cast<int64_t>(handlerCap), os);
            os << "    cmp x9, x10\n";
            os << "    b.lo " << ok << "\n";
            os << "    brk #1\n";
            os << ok << ":\n";
            addImm("x10", "sp", handlerStackSlot, os);
            movImm("x11", id, os);
            os << "    str x11, [x10, x9, lsl #3]\n";
            os << "    add x9, x9, #1\n";
            memOp("str", "x9", "sp", handlerDepthSlot, 8, os);
            break;
        }

        case Opcode::PopHandler: {
            if (handlerCap == 0) break;
            memOp("ldr", "x9", "sp", handlerDepthSlot, 8, os);
            os << "    subs x9, x9, #1\n";
            os << "    csel x9, xzr, x9, lt\n";
            memOp("str", "x9", "sp", handlerDepthSlot, 8, os);
            break;
        }

        case Opcode::InlineAsm:
            emitInlineAsm(inst, os);
            break;

        default:
            throw std::runtime_error("EIR arm64: unsupported instruction " + inst->str());
    }
}

} // namespace eir
