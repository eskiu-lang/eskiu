#include "eir_asm_int.h"

namespace eir {

void Arm64Emitter::emitVaStart(const Operand& ap, std::ostream& os) {
    loadOperand(ap, 10, os);
    addImm("x9", "x29", 16 + incomingStackBytes, os);
    os << "    str x9, [x10]\n";
    if (isDarwin) return;
    addImm("x9", "sp", grSaveSlot + 64, os);
    os << "    str x9, [x10, #8]\n";
    addImm("x9", "sp", vrSaveSlot + 128, os);
    os << "    str x9, [x10, #16]\n";
    movImm("w9", -(8 - fixedGr) * 8, os);
    os << "    str w9, [x10, #24]\n";
    movImm("w9", -(8 - fixedVr) * 16, os);
    os << "    str w9, [x10, #28]\n";
}

void Arm64Emitter::emitVaArgAddr(const TypePtr& t, std::ostream& os) {
    bool fp = t && t->isFloat();
    if (isDarwin) {
        os << "    ldr x9, [x10]\n";
        os << "    add x11, x9, #8\n";
        os << "    str x11, [x10]\n";
        return;
    }
    std::string stack = newLocal("vastack"), done = newLocal("vadone");
    size_t offsField = fp ? 28 : 24, topField = fp ? 16 : 8, step = fp ? 16 : 8;
    os << "    ldrsw x11, [x10, #" << offsField << "]\n";
    os << "    tbz x11, #63, " << stack << "\n";
    os << "    add x12, x11, #" << step << "\n";
    os << "    str w12, [x10, #" << offsField << "]\n";
    os << "    cmp x12, #0\n";
    os << "    b.gt " << stack << "\n";
    os << "    ldr x9, [x10, #" << topField << "]\n";
    os << "    add x9, x9, x11\n";
    os << "    b " << done << "\n";
    os << stack << ":\n";
    os << "    ldr x9, [x10]\n";
    os << "    add x11, x9, #8\n";
    os << "    str x11, [x10]\n";
    os << done << ":\n";
}

bool Arm64Emitter::emitSpecialCall(const Instruction* inst, std::ostream& os) {
    const std::string& n = inst->operands[0].text;
    if (currentMod->getFunction(n) && !currentMod->getFunction(n)->isExtern) return false;
    if (n == "va_start" || n == "llvm.va_start.p0") {
        if (!curFn->isVariadic) throw std::runtime_error("EIR arm64: va_start outside a variadic function");
        emitVaStart(inst->operands[1], os);
        return true;
    }
    if (n == "va_end" || n == "llvm.va_end.p0") return true;
    if (n == "va_arg") {
        TypePtr t = inst->type;
        if (isAggregate(t)) throw std::runtime_error("EIR arm64: va_arg of an aggregate is not supported");
        loadOperand(inst->operands[1], 10, os);
        emitVaArgAddr(t, os);
        if (inst->resultVar.empty()) return true;
        if (t && t->isFloat()) {
            os << "    ldr d16, [x9]\n";
            if (t->bitWidth == 32) os << "    fcvt s16, d16\n";
            storeFloat(16, t->bitWidth == 32, inst->resultVar, os);
        } else {
            os << "    ldr x9, [x9]\n";
            normalize(9, t, os);
            storeX("x9", inst->resultVar, os);
        }
        return true;
    }
    return false;
}

bool Arm64Emitter::emitAtomic(const Instruction* inst, std::ostream& os) {
    const std::string& n = inst->operands[0].text;
    AtomicIntrinsic a;
    if (!parseAtomicIntrinsic(n, a) || currentMod->getFunction(n)) return false;
    if (a.op == "fence") {
        os << "    dmb ish\n";
        return true;
    }
    std::string r = a.kind == AtomicKind::I32 ? "w" : "x";
    loadOperand(inst->operands[1], 9, os);
    if (inst->operands.size() > 2) loadOperand(inst->operands[2], 10, os);
    if (inst->operands.size() > 3) loadOperand(inst->operands[3], 13, os);
    std::string ld = a.relaxed ? "ldxr" : "ldaxr", st = a.relaxed ? "stxr" : "stlxr";
    if (a.op == "load") {
        os << "    " << (a.relaxed ? "ldr " : "ldar ") << r << "11, [x9]\n";
    } else if (a.op == "store") {
        os << "    " << (a.relaxed ? "str " : "stlr ") << r << "10, [x9]\n";
    } else if (a.op == "cas") {
        std::string loop = newLocal("cas"), fail = newLocal("casfail"), done = newLocal("casdone");
        os << loop << ":\n";
        os << "    " << ld << " " << r << "11, [x9]\n";
        os << "    cmp " << r << "11, " << r << "10\n";
        os << "    b.ne " << fail << "\n";
        os << "    " << st << " w12, " << r << "13, [x9]\n";
        os << "    cbnz w12, " << loop << "\n";
        os << "    mov x11, #1\n";
        os << "    b " << done << "\n";
        os << fail << ":\n";
        os << "    clrex\n";
        os << "    mov x11, #0\n";
        os << done << ":\n";
    } else {
        std::string loop = newLocal("rmw");
        const char* opc = a.op == "fetch_add" ? "add" : a.op == "fetch_sub" ? "sub" : a.op == "fetch_and" ? "and"
                        : a.op == "fetch_or" ? "orr" : a.op == "fetch_xor" ? "eor" : nullptr;
        os << loop << ":\n";
        os << "    " << ld << " " << r << "11, [x9]\n";
        if (opc) os << "    " << opc << " " << r << "13, " << r << "11, " << r << "10\n";
        else os << "    mov " << r << "13, " << r << "10\n";
        os << "    " << st << " w12, " << r << "13, [x9]\n";
        os << "    cbnz w12, " << loop << "\n";
    }
    if (!inst->resultVar.empty()) {
        if (a.kind == AtomicKind::I32 && a.op != "cas") os << "    sxtw x11, w11\n";
        storeX("x11", inst->resultVar, os);
    }
    return true;
}

void Arm64Emitter::emitCall(const Instruction* inst, const std::string& unwindLabel, std::ostream& os) {
    if (inst->op == Opcode::Call && isSpecialCall(inst->operands[0].text) && emitSpecialCall(inst, os)) return;
    if (inst->op == Opcode::Call && emitAtomic(inst, os)) return;

    CallShape cs = callShape(inst);
    const std::string& target = inst->operands[0].text;
    if (isCRecordCall(inst, cs)) {
        emitCRecordCall(inst, cs, os);
        return;
    }

    std::vector<ArgLoc> locs = classify(cs.argTypes, cs.fixedCount, cs.variadic, nullptr);
    std::vector<Operand> args;
    if (cs.indirectEnv) args.push_back(Operand::constNull());
    for (size_t i = 1; i < inst->operands.size(); ++i) args.push_back(inst->operands[i]);

    // Stack arguments first: their loads use only x9/x16/v16.
    for (size_t i = 0; i < args.size(); ++i) {
        const ArgLoc& l = locs[i];
        if (l.reg >= 0) continue;
        if (cs.indirectEnv && i == 0) continue;
        TypePtr t = cs.argTypes[i];
        if (l.fp || (l.vararg && args[i].kind == OperandKind::ConstantFloat)) {
            bool is32 = !l.vararg && t && t->bitWidth == 32;
            loadFloat(args[i], 16, is32, os);
            memOp("str", is32 ? "s16" : "d16", "sp", l.stackOff, is32 ? 4 : 8, os);
            continue;
        }
        loadArgInt(args[i], t, cs.external, 9, os);
        size_t w = (isDarwin && !l.vararg) ? l.size : 8;
        if (w == 1) memOp("strb", "w9", "sp", l.stackOff, 1, os);
        else if (w == 2) memOp("strh", "w9", "sp", l.stackOff, 2, os);
        else if (w == 4) memOp("str", "w9", "sp", l.stackOff, 4, os);
        else memOp("str", "x9", "sp", l.stackOff, 8, os);
    }

    for (size_t i = 0; i < args.size(); ++i) {
        const ArgLoc& l = locs[i];
        if (l.reg < 0 || (cs.indirectEnv && i == 0)) continue;
        if (l.fp) {
            loadFloat(args[i], l.reg, !l.vararg && cs.argTypes[i] && cs.argTypes[i]->bitWidth == 32, os);
        } else {
            loadArgInt(args[i], cs.argTypes[i], cs.external, l.reg, os);
        }
    }

    if (inst->op == Opcode::CallIndirect) {
        loadOperand(inst->operands[0], 17, os);
        if (cs.indirectEnv) {
            os << "    ldr x0, [x17, #8]\n";
            os << "    ldr x17, [x17]\n";
        }
    }
    if (isAggregate(inst->type) && !inst->resultVar.empty()) addImm("x8", "sp", slotOf(inst->resultVar), os);

    if (inst->op == Opcode::CallIndirect) os << "    blr x17\n";
    else os << "    bl " << bodySym(target) << "\n";

    if (!inst->resultVar.empty() && !isAggregate(inst->type)) {
        if (inst->type && inst->type->isFloat()) {
            storeFloat(0, inst->type->bitWidth == 32, inst->resultVar, os);
        } else {
            normalize(0, inst->type, os);
            storeX("x0", inst->resultVar, os);
        }
    }

    bool mayThrow = inst->op == Opcode::CallIndirect || (!cs.external && !isRuntimeName(target));
    if (mayThrow) exceptionCheck(os, unwindLabel);
}

void Arm64Emitter::loadArgInt(const Operand& op, const TypePtr& t, bool external, int reg, std::ostream& os) {
    if (external && isVaListType(t)) {
        loadOperand(op, reg, os);
        if (isDarwin) os << "    ldr x" << reg << ", [x" << reg << "]\n";
        return;
    }
    loadOperand(op, reg, os);
}

void Arm64Emitter::emitInlineAsm(const Instruction* inst, std::ostream& os) {
    std::vector<std::string> cons;
    {
        std::string cur;
        int depth = 0;
        for (char c : inst->asmConstraints) {
            if (c == '{') depth++;
            if (c == '}') depth--;
            if (c == ',' && depth == 0) { cons.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        if (!cur.empty()) cons.push_back(cur);
    }
    struct Bound { std::string text; int reg = -1; bool regOut = false; TypePtr ty; };
    std::vector<Bound> bound;
    std::vector<int> outRegs;
    std::vector<TypePtr> outTypes;
    std::vector<TypePtr> resultTypes;
    if (inst->type && inst->type->isStruct()) resultTypes = inst->type->members;
    else if (inst->type && !inst->type->isVoid()) resultTypes.push_back(inst->type);

    // Operand registers, skipping any the asm clobbers.
    static const int poolRegs[] = {9, 10, 11, 12, 13, 14, 15, 0, 1, 2, 3, 4, 5, 6, 7};
    std::vector<int> pool;
    for (int r : poolRegs) {
        bool clobbered = false;
        for (const auto& c : inst->clobbers)
            if (c == "x" + std::to_string(r) || c == "w" + std::to_string(r)) clobbered = true;
        if (!clobbered) pool.push_back(r);
    }
    size_t nextReg = 0, opIdx = 0, outIdx = 0;
    auto take = [&]() {
        if (nextReg >= pool.size()) throw std::runtime_error("EIR arm64: too many asm operands");
        return pool[nextReg++];
    };
    auto regName = [](int r, const TypePtr& t) {
        bool narrow = t && t->isInt() && t->bitWidth <= 32;
        return std::string(narrow ? "w" : "x") + std::to_string(r);
    };
    std::ostringstream loads;
    for (const auto& c : cons) {
        if (c.rfind("~{", 0) == 0) continue;
        Bound b;
        if (c == "=*m" || c == "*m" || c == "m") {
            if (opIdx >= inst->operands.size()) throw std::runtime_error("EIR arm64: asm operand mismatch");
            b.reg = take();
            loadOperand(inst->operands[opIdx++], b.reg, loads);
            b.text = "[x" + std::to_string(b.reg) + "]";
        } else if (!c.empty() && c[0] == '=') {
            b.reg = take();
            b.regOut = true;
            b.ty = outIdx < resultTypes.size() ? resultTypes[outIdx] : Type::getI64();
            outIdx++;
            outRegs.push_back(b.reg);
            outTypes.push_back(b.ty);
            b.text = regName(b.reg, b.ty);
        } else if (!c.empty() && isdigit(static_cast<unsigned char>(c[0]))) {
            size_t tiedTo = static_cast<size_t>(std::stoi(c));
            if (tiedTo >= bound.size() || opIdx >= inst->operands.size()) throw std::runtime_error("EIR arm64: asm tie mismatch");
            b = bound[tiedTo];
            b.regOut = false;
            loadOperand(inst->operands[opIdx++], b.reg, loads);
        } else if (c == "i" || c == "n") {
            const Operand& o = inst->operands[opIdx++];
            b.text = "#" + std::to_string(o.intVal);
        } else {
            if (opIdx >= inst->operands.size()) throw std::runtime_error("EIR arm64: asm operand mismatch");
            const Operand& o = inst->operands[opIdx++];
            b.reg = take();
            b.ty = o.type;
            loadOperand(o, b.reg, loads);
            b.text = regName(b.reg, o.type);
        }
        bound.push_back(b);
    }
    os << loads.str();

    std::string text;
    const std::string& a = inst->asmString;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i] == '$' && i + 1 < a.size() && a[i + 1] == '$') { text += '$'; ++i; continue; }
        if (a[i] == '$' && i + 1 < a.size() && (isdigit(static_cast<unsigned char>(a[i + 1])) || a[i + 1] == '{')) {
            size_t j = i + 1;
            bool brace = a[j] == '{';
            if (brace) ++j;
            size_t start = j;
            while (j < a.size() && isdigit(static_cast<unsigned char>(a[j]))) ++j;
            size_t n = static_cast<size_t>(std::stoul(a.substr(start, j - start)));
            std::string mod;
            if (brace) {
                if (j < a.size() && a[j] == ':') {
                    size_t e = a.find('}', j);
                    mod = a.substr(j + 1, e - j - 1);
                    j = e;
                }
                ++j;
            }
            if (n >= bound.size()) throw std::runtime_error("EIR arm64: asm operand $" + std::to_string(n) + " out of range");
            const Bound& b = bound[n];
            if (!mod.empty() && b.reg >= 0 && (mod == "w" || mod == "x")) text += mod + std::to_string(b.reg);
            else text += b.text;
            i = j - 1;
            continue;
        }
        text += a[i];
    }
    os << "    " << text << "\n";

    if (outRegs.size() == 1 && !inst->resultVar.empty() && !addrVars.count(inst->resultVar)) {
        if (outRegs[0] != 9) os << "    mov x9, x" << outRegs[0] << "\n";
        normalize(9, outTypes[0], os);
        storeX("x9", inst->resultVar, os);
    } else if (!outRegs.empty() && !inst->resultVar.empty()) {
        size_t base = slotOf(inst->resultVar);
        for (size_t k = 0; k < outRegs.size(); ++k) {
            normalize(outRegs[k], outTypes[k], os);
            memOp("str", "x" + std::to_string(outRegs[k]), "sp", base + k * 8, 8, os);
        }
    }
}

} // namespace eir
