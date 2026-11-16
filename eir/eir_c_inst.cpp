#include "eir_c_int.h"

namespace eir {

void CEmitterImpl::emitInstructionBody(const Instruction* inst, std::ostream& os) {
    const auto& ops = inst->operands;
    std::string res = inst->resultVar.empty() ? "" : var(inst->resultVar);
    TypePtr rt = inst->type;
    switch (inst->op) {
        case Opcode::Alloca: case Opcode::Store: case Opcode::Load: case Opcode::FieldPtr:
        case Opcode::OffsetPtr: case Opcode::Gep:
            emitMemoryInst(inst, os, ops, res, rt);
            break;
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul: case Opcode::And: case Opcode::Or:
        case Opcode::Xor: case Opcode::Shl: case Opcode::LShr: case Opcode::AShr:
        case Opcode::SDiv: case Opcode::SRem: case Opcode::UDiv: case Opcode::URem:
        case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv:
        case Opcode::Neg: case Opcode::Not:
            emitArithInst(inst, os, ops, res, rt);
            break;
        case Opcode::ICmp: case Opcode::FCmp: case Opcode::Trunc: case Opcode::ZExt:
        case Opcode::SExt: case Opcode::FPTrunc: case Opcode::FPExt: case Opcode::SIToFP:
        case Opcode::FPToSI: case Opcode::Bitcast: case Opcode::PtrToInt: case Opcode::IntToPtr:
            emitCompareConvInst(inst, os, ops, res, rt);
            break;
        case Opcode::Br: case Opcode::BrCond: case Opcode::Switch: case Opcode::Call:
        case Opcode::CallIndirect: case Opcode::MakeClosure: case Opcode::MakeInterface:
        case Opcode::MakeSlice: case Opcode::ExtractFat: case Opcode::Ret:
        case Opcode::Unreachable: case Opcode::Throw: case Opcode::LandingPad:
        case Opcode::PushHandler: case Opcode::PopHandler: case Opcode::InlineAsm: default:
            emitControlInst(inst, os, ops, res, rt);
            break;
    }
}

void CEmitterImpl::emitMemoryInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt) {
    switch (inst->op) {
        // A stack slot starts zeroed each time its alloca runs, as in the EVM.
        case Opcode::Alloca: {
            std::string tag = res.substr(2);
            os << "    { size_t z; for (z = 0; z < sizeof s_" << tag << "; z++) ((unsigned char*)&s_" << tag << ")[z] = 0; }\n";
            break;
        }

        case Opcode::Store: {
            TypePtr valTy = ops[0].type;
            TypePtr dstTy = (ops[1].type && ops[1].type->pointee) ? ops[1].type->pointee : valTy;
            std::string dst = pval(ops[1]);
            if (isAggregate(dstTy) || (!dstTy && isAggregate(valTy))) {
                os << "    __eskc_copy(" << dst << ", " << pval(ops[0]) << ", " << sizeOf(dstTy ? dstTy : valTy) << ");\n";
            } else if (dstTy && dstTy->isInt()) {
                os << "    *(" << cType(dstTy) << "*)" << dst << " = (" << cType(dstTy) << ")" << ival(ops[0]) << ";\n";
            } else if (dstTy && dstTy->isFloat()) {
                os << "    *(" << cType(dstTy) << "*)" << dst << " = (" << cType(dstTy) << ")" << fval(ops[0]) << ";\n";
            } else {
                os << "    *(void**)" << dst << " = " << pval(ops[0]) << ";\n";
            }
            break;
        }

        case Opcode::Load:
            if (rt && rt->isVoid()) break;
            if (isAggregate(rt)) os << "    " << res << " = " << pval(ops[0]) << ";\n";
            else os << "    " << res << " = " << loadExpr(rt, pval(ops[0])) << ";\n";
            break;

        case Opcode::FieldPtr: {
            size_t off = currentMod->getFieldOffset(ops[0].text, ops[2].text);
            if (off == 0 && (ops[0].text.empty() || ops[0].text == "slice") && ops[2].text == "len") off = 8;
            os << "    " << res << " = (void*)((char*)" << pval(ops[1]) << " + " << off << ");\n";
            break;
        }

        case Opcode::OffsetPtr:
            os << "    " << res << " = (void*)((char*)" << pval(ops[0]) << " + (intptr_t)" << ival(ops[1]) << ");\n";
            break;

        case Opcode::Gep: {
            TypePtr elemType = rt->pointee ? rt->pointee : (rt->elemType ? rt->elemType : rt);
            os << "    " << res << " = (void*)((char*)" << pval(ops[0]) << " + (intptr_t)" << ival(ops[1])
               << " * " << sizeOf(elemType) << ");\n";
            break;
        }
        default:
            break;
    }
}

void CEmitterImpl::emitArithInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt) {
    switch (inst->op) {
        case Opcode::Add: case Opcode::Sub: case Opcode::Mul: {
            const char* o = inst->op == Opcode::Add ? "+" : inst->op == Opcode::Sub ? "-" : "*";
            std::string e = "(int64_t)(" + uval(ops[0]) + " " + o + " " + uval(ops[1]) + ")";
            if (rt && rt->isInt() && rt->bitWidth == 32) e = narrow(e, rt);
            os << setInt(inst, e);
            break;
        }

        case Opcode::And: case Opcode::Or: case Opcode::Xor: {
            const char* o = inst->op == Opcode::And ? "&" : inst->op == Opcode::Or ? "|" : "^";
            std::string e = "(int64_t)(" + uval(ops[0]) + " " + o + " " + uval(ops[1]) + ")";
            if (rt && rt->isInt() && !rt->isSigned && rt->bitWidth > 1) e = narrow(e, rt);
            os << setInt(inst, e);
            break;
        }

        case Opcode::Shl: case Opcode::LShr: case Opcode::AShr: {
            unsigned bw = (rt && rt->isInt() && rt->bitWidth > 1) ? rt->bitWidth : 64;
            std::string cnt = "(" + uval(ops[1]) + " & " + std::to_string(bw - 1) + ")";
            std::string ut = bw == 8 ? "uint8_t" : bw == 16 ? "uint16_t" : bw == 32 ? "uint32_t" : "uint64_t";
            std::string st = bw == 8 ? "int8_t" : bw == 16 ? "int16_t" : bw == 32 ? "int32_t" : "int64_t";
            std::string e;
            if (inst->op == Opcode::Shl) {
                e = "(" + ut + ")((uint64_t)(" + ut + ")" + ival(ops[0]) + " << " + cnt + ")";
                if (bw == 32 && rt->isSigned) e = "(int32_t)" + e;
            } else if (inst->op == Opcode::LShr) {
                e = "((uint64_t)(" + ut + ")" + ival(ops[0]) + " >> " + cnt + ")";
            } else {
                e = "((int64_t)(" + st + ")" + ival(ops[0]) + " >> " + cnt + ")";
            }
            os << setInt(inst, "(int64_t)" + e);
            break;
        }

        case Opcode::SDiv: case Opcode::SRem: {
            bool rem = inst->op == Opcode::SRem;
            if (rt && rt->isInt() && rt->bitWidth == 32)
                os << "    " << res << " = __eskc_s" << (rem ? "rem" : "div") << "32((int32_t)" << ival(ops[0]) << ", (int32_t)" << ival(ops[1]) << ");\n";
            else
                os << "    " << res << " = __eskc_s" << (rem ? "rem" : "div") << "64(" << ival(ops[0]) << ", " << ival(ops[1]) << ");\n";
            break;
        }

        case Opcode::UDiv: case Opcode::URem:
            os << "    " << res << " = __eskc_u" << (inst->op == Opcode::URem ? "rem" : "div") << "64(" << uval(ops[0]) << ", " << uval(ops[1]) << ");\n";
            break;

        case Opcode::FAdd: case Opcode::FSub: case Opcode::FMul: case Opcode::FDiv: {
            const char* o = inst->op == Opcode::FAdd ? "+" : inst->op == Opcode::FSub ? "-" : inst->op == Opcode::FMul ? "*" : "/";
            std::string ft = cType(rt);
            os << "    " << res << " = (" << ft << ")((" << ft << ")" << fval(ops[0]) << " " << o << " (" << ft << ")" << fval(ops[1]) << ");\n";
            break;
        }

        case Opcode::Neg:
            if (rt && rt->isFloat()) os << "    " << res << " = (" << cType(rt) << ")-" << fval(ops[0]) << ";\n";
            else os << setInt(inst, "(int64_t)(UINT64_C(0) - " + uval(ops[0]) + ")");
            break;

        case Opcode::Not:
            os << setInt(inst, "(int64_t)~" + uval(ops[0]));
            break;
        default:
            break;
    }
}

void CEmitterImpl::emitCompareConvInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt) {
    switch (inst->op) {
        case Opcode::ICmp: {
            const char* o = "==";
            bool sgn = false;
            switch (inst->icmpCond) {
                case ICmpCond::Eq: o = "=="; break;
                case ICmpCond::Ne: o = "!="; break;
                case ICmpCond::Slt: o = "<"; sgn = true; break;
                case ICmpCond::Sle: o = "<="; sgn = true; break;
                case ICmpCond::Sgt: o = ">"; sgn = true; break;
                case ICmpCond::Sge: o = ">="; sgn = true; break;
                case ICmpCond::Ult: o = "<"; break;
                case ICmpCond::Ule: o = "<="; break;
                case ICmpCond::Ugt: o = ">"; break;
                case ICmpCond::Uge: o = ">="; break;
            }
            auto side = [&](const Operand& op) {
                std::string v = (op.type && op.type->isInterface()) ? "__eskc_iface_data(" + pval(op) + ")" : ival(op);
                return sgn ? v : "(uint64_t)" + v;
            };
            os << "    " << res << " = (int64_t)(" << side(ops[0]) << " " << o << " " << side(ops[1]) << ");\n";
            break;
        }

        case Opcode::FCmp: {
            const char* o = "==";
            switch (inst->fcmpCond) {
                case FCmpCond::Oeq: o = "=="; break;
                case FCmpCond::One: o = "!="; break;
                case FCmpCond::Olt: o = "<"; break;
                case FCmpCond::Ole: o = "<="; break;
                case FCmpCond::Ogt: o = ">"; break;
                case FCmpCond::Oge: o = ">="; break;
            }
            os << "    " << res << " = (int64_t)(" << fval(ops[0]) << " " << o << " " << fval(ops[1]) << ");\n";
            break;
        }

        case Opcode::Trunc:
            if (rt && rt->isInt() && rt->bitWidth == 1) os << "    " << res << " = " << ival(ops[0]) << " & 1;\n";
            else os << "    " << res << " = " << narrow(ival(ops[0]), rt) << ";\n";
            break;

        case Opcode::ZExt:
        case Opcode::SExt: {
            TypePtr st = ops[0].type;
            unsigned bw = (st && st->isInt()) ? st->bitWidth : 64;
            std::string e;
            if (bw == 1) e = inst->op == Opcode::ZExt ? "(" + ival(ops[0]) + " & 1)" : "-(" + ival(ops[0]) + " & 1)";
            else if (bw >= 64) e = ival(ops[0]);
            else {
                std::string ct = inst->op == Opcode::ZExt ? (bw == 8 ? "uint8_t" : bw == 16 ? "uint16_t" : "uint32_t")
                                                          : (bw == 8 ? "int8_t" : bw == 16 ? "int16_t" : "int32_t");
                e = "(int64_t)(" + ct + ")" + ival(ops[0]);
            }
            os << setInt(inst, e);
            break;
        }

        case Opcode::FPTrunc:
        case Opcode::FPExt:
            os << "    " << res << " = (" << cType(rt) << ")" << fval(ops[0]) << ";\n";
            break;

        case Opcode::SIToFP:
            os << "    " << res << " = (" << cType(rt) << ")" << fval(ops[0]) << ";\n";
            break;

        case Opcode::FPToSI: {
            unsigned w = (rt && rt->isInt()) ? (rt->bitWidth <= 1 ? 8 : rt->bitWidth) : 64;
            bool uns = rt && rt->isInt() && !rt->isSigned;
            if (uns) {
                std::string hi = w >= 64 ? "UINT64_MAX" : "UINT64_C(" + std::to_string((1ULL << w) - 1) + ")";
                os << "    " << res << " = __eskc_f2u(" << fval(ops[0]) << ", " << hi << ");\n";
            } else {
                std::string lo = w >= 64 ? "INT64_MIN" : "(-INT64_C(" + std::to_string(1ULL << (w - 1)) + "))";
                std::string hi = w >= 64 ? "INT64_MAX" : "INT64_C(" + std::to_string((1ULL << (w - 1)) - 1) + ")";
                os << "    " << res << " = __eskc_f2s(" << fval(ops[0]) << ", " << lo << ", " << hi << ");\n";
            }
            break;
        }

        case Opcode::Bitcast:
        case Opcode::PtrToInt:
        case Opcode::IntToPtr: {
            char dk = varKind.count(inst->resultVar) ? varKind[inst->resultVar] : kindOf(rt);
            char sk = opKind(ops[0]);
            bool srcF = sk == 'f' || sk == 'd', dstF = dk == 'f' || dk == 'd';
            if (srcF != dstF) {
                std::string tmp = "t" + std::to_string(nextTmp++);
                if (srcF) {
                    std::string ft = kindType(sk), ut = sk == 'f' ? "uint32_t" : "uint64_t";
                    os << "    { " << ft << " " << tmp << " = " << var(ops[0].text) << "; " << ut << " u_" << tmp << "; __eskc_copy(&u_" << tmp
                       << ", &" << tmp << ", sizeof " << tmp << "); " << res << " = (int64_t)u_" << tmp << "; }\n";
                } else {
                    std::string ut = dk == 'f' ? "uint32_t" : "uint64_t";
                    os << "    { " << ut << " " << tmp << " = (" << ut << ")" << ival(ops[0]) << "; __eskc_copy(&" << res << ", &" << tmp
                       << ", sizeof " << res << "); }\n";
                }
            } else {
                os << "    " << res << " = " << asKind(ops[0], dk) << ";\n";
            }
            break;
        }
        default:
            break;
    }
}

void CEmitterImpl::emitControlInst(const Instruction* inst, std::ostream& os, const std::vector<Operand>& ops, const std::string& res, const TypePtr& rt) {
    switch (inst->op) {
        case Opcode::Br:
            os << "    goto " << label(ops[0].text) << ";\n";
            break;

        case Opcode::BrCond:
            os << "    if (" << ival(ops[0]) << ") goto " << label(ops[1].text) << "; else goto " << label(ops[2].text) << ";\n";
            break;

        case Opcode::Switch: {
            os << "    switch (" << ival(ops[0]) << ") {\n";
            std::unordered_set<int64_t> used;
            for (const auto& sc : inst->switchCases) {
                if (!used.insert(sc.first).second) continue;
                os << "    case " << i64Const(sc.first) << ": goto " << label(sc.second) << ";\n";
            }
            os << "    default: " << (inst->defaultLabel.empty() ? "break;" : "goto " + label(inst->defaultLabel) + ";") << "\n    }\n";
            break;
        }

        case Opcode::Call:
        case Opcode::CallIndirect:
            emitCall(inst, os);
            break;

        case Opcode::MakeClosure:
        case Opcode::MakeInterface:
        case Opcode::MakeSlice:
            os << "    ((void**)" << res << ")[0] = " << pval(ops[0]) << ";\n";
            if (opKind(ops[1]) == 'i' && ops[1].kind != OperandKind::ConstantNull)
                os << "    *(int64_t*)((char*)" << res << " + 8) = " << ival(ops[1]) << ";\n";
            else
                os << "    ((void**)" << res << ")[1] = " << pval(ops[1]) << ";\n";
            break;

        case Opcode::ExtractFat: {
            int64_t idx = ops[1].kind == OperandKind::ConstantInt ? ops[1].intVal : 0;
            std::string addr = "((char*)" + pval(ops[0]) + " + " + std::to_string(idx * 8) + ")";
            if (rt && rt->isInt() && rt->bitWidth < 64 && !(ops[0].type && ops[0].type->isStruct()))
                os << "    " << res << " = *(int64_t*)" << addr << ";\n";
            else
                os << "    " << res << " = " << loadExpr(rt, addr) << ";\n";
            break;
        }

        case Opcode::Ret: {
            bool hasVal = !ops.empty() && ops[0].type && !ops[0].type->isVoid();
            emitReturn(hasVal ? &ops[0] : nullptr, os);
            break;
        }

        case Opcode::Unreachable:
            os << "    __eskc_trap();\n";
            break;

        case Opcode::Throw:
            os << "    __eir_throw(" << pval(ops[0]) << ");\n    goto __eskc_unwind;\n";
            break;

        case Opcode::LandingPad:
            if (!res.empty()) os << "    " << res << " = __eir_take_exception();\n";
            else os << "    (void)__eir_take_exception();\n";
            break;

        case Opcode::PushHandler:
            os << "    if (__eskc_hd >= " << handlerCap << ") __eskc_trap();\n";
            os << "    __eskc_hs[__eskc_hd++] = " << handlerIds.at(ops[0].text) << ";\n";
            break;

        case Opcode::PopHandler:
            if (handlerCap) os << "    if (__eskc_hd > 0) __eskc_hd--;\n";
            break;

        case Opcode::InlineAsm:
            emitInlineAsm(inst, os);
            break;

        default:
            throw std::runtime_error("EIR C: unsupported instruction " + inst->str());
    }
}

} // namespace eir
