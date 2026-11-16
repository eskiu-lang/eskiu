#include "eir_asm_int.h"

namespace eir {

size_t Arm64Emitter::argSize(const TypePtr& t) const {
    if (!t || isAggregate(t)) return 8;
    if (t->isFloat()) return t->bitWidth == 32 ? 4 : 8;
    if (t->isInt()) return t->bitWidth <= 8 ? 1 : t->bitWidth <= 16 ? 2 : t->bitWidth <= 32 ? 4 : 8;
    return 8;
}

std::vector<ArgLoc> Arm64Emitter::classify(const std::vector<TypePtr>& tys, size_t fixedCount, bool variadic, size_t* stackBytes) const {
    std::vector<ArgLoc> locs;
    int ngr = 0, nvr = 0;
    size_t off = 0;
    for (size_t i = 0; i < tys.size(); ++i) {
        ArgLoc l;
        l.fp = tys[i] && tys[i]->isFloat();
        l.size = argSize(tys[i]);
        l.vararg = variadic && i >= fixedCount;
        if (isDarwin && l.vararg) {
            off = alignUp(off, 8);
            l.stackOff = off;
            l.size = 8;
            off += 8;
        } else if (l.fp && nvr < 8) {
            l.reg = nvr++;
        } else if (!l.fp && ngr < 8) {
            l.reg = ngr++;
        } else {
            size_t al = isDarwin ? l.size : 8;
            off = alignUp(off, al);
            l.stackOff = off;
            off += isDarwin ? l.size : 8;
        }
        locs.push_back(l);
    }
    if (stackBytes) *stackBytes = alignUp(off, 16);
    return locs;
}

Arm64Emitter::CallShape Arm64Emitter::callShape(const Instruction* inst) const {
    CallShape cs;
    const Function* callee = nullptr;
    if (inst->op == Opcode::Call) {
        callee = currentMod->getFunction(inst->operands[0].text);
        cs.external = !callee || callee->isExtern || callee->blocks.empty();
    } else {
        cs.indirectEnv = inst->operands[0].type && inst->operands[0].type->isClosure();
    }
    if (cs.indirectEnv) cs.argTypes.push_back(Type::getPtr());
    for (size_t i = 1; i < inst->operands.size(); ++i) {
        TypePtr t = inst->operands[i].type;
        if (callee && i - 1 < callee->params.size() && callee->params[i - 1].second) t = callee->params[i - 1].second;
        if (inst->operands[i].kind == OperandKind::ConstantFloat && !(t && t->isFloat())) t = Type::getF64();
        cs.argTypes.push_back(t);
    }
    cs.variadic = callee && callee->isVariadic;
    cs.fixedCount = callee ? callee->params.size() : cs.argTypes.size();
    return cs;
}

void Arm64Emitter::leaves(const TypePtr& t, std::vector<TypePtr>& out, int depth) {
    if (!t || depth > 8 || out.size() > 8) { out.push_back(Type::getI64()); return; }
    if (t->isArray()) {
        for (uint64_t i = 0; i < t->arraySize && out.size() <= 8; ++i) leaves(t->elemType, out, depth + 1);
        return;
    }
    if (!t->isStruct()) { out.push_back(t->isClosure() || t->isInterface() || t->isSlice() ? Type::getI64() : t); return; }
    auto it = currentMod->structLayouts.find(t->nominalName());
    if (it == currentMod->structLayouts.end()) { out.push_back(Type::getI64()); return; }
    const StructInfo& si = it->second;
    for (const auto& pd : si.pads) if (pd.width > 0) out.push_back(Type::getI64());   // a `: 0` is not data
    if (si.isUnion) {
        std::vector<TypePtr> widest;
        bool same = true;
        TypePtr base;
        for (const auto& f : si.fields) {
            std::vector<TypePtr> m;
            if (f.isBitfield) m.push_back(Type::getI64()); else leaves(f.type, m, depth + 1);
            for (const auto& l : m) {
                if (!base) base = l;
                else if (!(l->isFloat() && base->isFloat() && l->bitWidth == base->bitWidth)) same = false;
            }
            if (m.size() > widest.size()) widest = m;
        }
        if (!same) widest.push_back(Type::getI64());
        out.insert(out.end(), widest.begin(), widest.end());
        return;
    }
    for (const auto& f : si.fields) {
        if (f.isBitfield) out.push_back(Type::getI64());
        else leaves(f.type, out, depth + 1);
    }
}

int Arm64Emitter::hfaOf(const TypePtr& t, bool& isDouble) {
    std::vector<TypePtr> l;
    leaves(t, l);
    if (l.empty() || l.size() > 4) return 0;
    for (const auto& x : l)
        if (!x->isFloat() || x->bitWidth != l[0]->bitWidth) return 0;
    isDouble = l[0]->bitWidth == 64;
    return static_cast<int>(l.size());
}

std::vector<Arm64Emitter::CLoc> Arm64Emitter::classifyC(const std::vector<TypePtr>& tys, size_t fixedCount, bool variadic, size_t* stackBytes) {
    std::vector<CLoc> locs;
    int ngr = 0, nvr = 0;
    size_t off = 0;
    auto onStack = [&](CLoc& l, size_t size, size_t align) {
        off = alignUp(off, align);
        l.off = off;
        off += alignUp(size, isDarwin ? std::min<size_t>(align, 8) : 8);
    };
    for (size_t i = 0; i < tys.size(); ++i) {
        const TypePtr& t = tys[i];
        CLoc l;
        l.vararg = variadic && i >= fixedCount;
        if (isCAggregate(t)) {
            size_t size = sizeOf(t);
            bool dbl = false;
            int hfa = hfaOf(t, dbl);
            if (isDarwin && l.vararg) {
                l.kind = size > 16 ? CLoc::Indirect : CLoc::StackCopy;
                l.viaStack = true;
                l.size = size;
                onStack(l, size > 16 ? 8 : size, 8);
            } else if (hfa) {
                l.size = size;
                l.dbl = dbl;
                l.count = hfa;
                if (nvr + hfa <= 8) { l.kind = CLoc::Hfa; l.reg = nvr; nvr += hfa; }
                else { nvr = 8; l.kind = CLoc::StackCopy; onStack(l, size, dbl ? 8 : 4); }
            } else if (size <= 16) {
                int words = static_cast<int>((size + 7) / 8);
                l.size = size;
                l.count = words;
                if (ngr + words <= 8) { l.kind = CLoc::Words; l.reg = ngr; ngr += words; }
                else { ngr = 8; l.kind = CLoc::StackCopy; onStack(l, size, 8); }
            } else {
                l.kind = CLoc::Indirect;
                l.size = size;
                if (ngr < 8) l.reg = ngr++;
                else { l.viaStack = true; onStack(l, 8, 8); }
            }
        } else {
            bool fp = t && t->isFloat();
            size_t sz = argSize(t);
            l.size = sz;
            if (isDarwin && l.vararg) { l.kind = CLoc::Stack; onStack(l, 8, 8); }
            else if (fp && nvr < 8) { l.kind = CLoc::Fpr; l.reg = nvr++; }
            else if (!fp && ngr < 8) { l.kind = CLoc::Gpr; l.reg = ngr++; }
            else { l.kind = CLoc::Stack; onStack(l, sz, isDarwin ? sz : 8); }
            l.dbl = fp && t->bitWidth != 32;
        }
        locs.push_back(l);
    }
    if (stackBytes) *stackBytes = alignUp(off, 16);
    return locs;
}

bool Arm64Emitter::isCRecordCall(const Instruction* inst, const CallShape& cs) const {
    if (!cs.external || inst->op != Opcode::Call || isRuntimeName(inst->operands[0].text)) return false;
    if (isCAggregate(inst->type)) return true;
    for (const auto& t : cs.argTypes) if (isCAggregate(t)) return true;
    return false;
}

void Arm64Emitter::emitCRecordCall(const Instruction* inst, const CallShape& cs, std::ostream& os) {
    std::vector<CLoc> locs = classifyC(cs.argTypes, cs.fixedCount, cs.variadic, nullptr);
    std::vector<Operand> args(inst->operands.begin() + 1, inst->operands.end());
    size_t scratch = callScratch.at(inst);
    std::vector<size_t> copyAt(args.size(), 0);
    for (size_t i = 0; i < args.size(); ++i) {
        if (!isCAggregate(cs.argTypes[i])) continue;
        copyAt[i] = scratch;
        loadOperand(args[i], 9, os);
        addImm("x10", "sp", scratch, os);
        copyBytes(sizeOf(cs.argTypes[i]), os);
        scratch += alignUp(std::max<size_t>(sizeOf(cs.argTypes[i]), 16), 16);
    }
    // Stack placements.
    for (size_t i = 0; i < args.size(); ++i) {
        const CLoc& l = locs[i];
        if (l.kind == CLoc::StackCopy) {
            addImm("x9", "sp", copyAt[i], os);
            addImm("x10", "sp", l.off, os);
            copyBytes(l.size, os);
        } else if (l.kind == CLoc::Indirect && l.viaStack) {
            addImm("x9", "sp", copyAt[i], os);
            memOp("str", "x9", "sp", l.off, 8, os);
        } else if (l.kind == CLoc::Stack) {
            TypePtr t = cs.argTypes[i];
            if (t && t->isFloat()) {
                bool is32 = !l.vararg && t->bitWidth == 32;
                loadFloat(args[i], 16, is32, os);
                memOp("str", is32 ? "s16" : "d16", "sp", l.off, is32 ? 4 : 8, os);
            } else {
                loadArgInt(args[i], t, true, 9, os);
                size_t w = (isDarwin && !l.vararg) ? l.size : 8;
                if (w == 1) memOp("strb", "w9", "sp", l.off, 1, os);
                else if (w == 2) memOp("strh", "w9", "sp", l.off, 2, os);
                else if (w == 4) memOp("str", "w9", "sp", l.off, 4, os);
                else memOp("str", "x9", "sp", l.off, 8, os);
            }
        }
    }
    // Register placements.
    for (size_t i = 0; i < args.size(); ++i) {
        const CLoc& l = locs[i];
        TypePtr t = cs.argTypes[i];
        switch (l.kind) {
            case CLoc::Gpr: loadArgInt(args[i], t, true, l.reg, os); break;
            case CLoc::Fpr: loadFloat(args[i], l.reg, !l.vararg && t && t->bitWidth == 32, os); break;
            case CLoc::Hfa:
                for (int k = 0; k < l.count; ++k)
                    memOp("ldr", (l.dbl ? "d" : "s") + std::to_string(l.reg + k), "sp", copyAt[i] + k * (l.dbl ? 8 : 4), l.dbl ? 8 : 4, os);
                break;
            case CLoc::Words:
                for (int k = 0; k < l.count; ++k) memOp("ldr", "x" + std::to_string(l.reg + k), "sp", copyAt[i] + k * 8, 8, os);
                break;
            case CLoc::Indirect:
                if (!l.viaStack) addImm("x" + std::to_string(l.reg), "sp", copyAt[i], os);
                break;
            default: break;
        }
    }
    bool recRet = isCAggregate(inst->type);
    if (recRet && !inst->resultVar.empty() && sizeOf(inst->type) > 16 && !hfaRet(inst->type))
        addImm("x8", "sp", slotOf(inst->resultVar), os);
    os << "    bl " << bodySym(inst->operands[0].text) << "\n";
    if (inst->resultVar.empty()) return;
    if (recRet) {
        bool dbl = false;
        int hfa = hfaOf(inst->type, dbl);
        size_t at = slotOf(inst->resultVar);
        if (hfa) {
            for (int k = 0; k < hfa; ++k)
                memOp("str", (dbl ? "d" : "s") + std::to_string(k), "sp", at + k * (dbl ? 8 : 4), dbl ? 8 : 4, os);
        } else if (sizeOf(inst->type) <= 16) {
            memOp("str", "x0", "sp", at, 8, os);
            if (sizeOf(inst->type) > 8) memOp("str", "x1", "sp", at + 8, 8, os);
        }
    } else if (inst->type && inst->type->isFloat()) {
        storeFloat(0, inst->type->bitWidth == 32, inst->resultVar, os);
    } else {
        normalize(0, inst->type, os);
        storeX("x0", inst->resultVar, os);
    }
}

bool Arm64Emitter::hfaRet(const TypePtr& t) {
    bool dbl = false;
    return hfaOf(t, dbl) != 0;
}

void Arm64Emitter::findCabiFns(const Module* mod) {
    cabiFns.clear();
    cabiOrder.clear();
    for (const auto& fn : mod->functions) {
        for (const auto& bb : fn->blocks) {
            for (const auto& inst : bb->instructions) {
                for (size_t i = 0; i < inst->operands.size(); ++i) {
                    const Operand& op = inst->operands[i];
                    if (op.kind != OperandKind::GlobalRef) continue;
                    if (i == 0 && (inst->op == Opcode::Call || inst->op == Opcode::MakeClosure)) continue;
                    const Function* f = mod->getFunction(op.text);
                    if (!f || f->isExtern || f->blocks.empty()) continue;
                    bool rec = isCAggregate(f->returnType);
                    for (const auto& p : f->params) rec = rec || isCAggregate(p.second) || isVaListType(p.second);
                    if (rec && cabiFns.insert(f->name).second) cabiOrder.push_back(f->name);
                }
            }
        }
    }
    for (const auto& g : mod->globals) {
        for (const auto& e : g.elements) {
            if (e.kind != OperandKind::GlobalRef) continue;
            const Function* f = mod->getFunction(e.text);
            if (!f || f->isExtern || f->blocks.empty()) continue;
            bool rec = isCAggregate(f->returnType);
            for (const auto& p : f->params) rec = rec || isCAggregate(p.second) || isVaListType(p.second);
            if (rec && cabiFns.insert(f->name).second) cabiOrder.push_back(f->name);
        }
    }
}

void Arm64Emitter::emitCabiThunk(const Function* fn, std::ostream& os) {
    std::vector<TypePtr> ctys;
    for (const auto& p : fn->params) ctys.push_back(p.second);
    std::vector<CLoc> in = classifyC(ctys, ctys.size(), false, nullptr);
    size_t outgoingBytes = 0;
    std::vector<ArgLoc> out = classify(ctys, ctys.size(), false, &outgoingBytes);
    size_t local = outgoingBytes;
    auto alloc = [&](size_t sz) { local = alignUp(local, 16); size_t o = local; local += alignUp(std::max<size_t>(sz, 8), 16); return o; };
    std::vector<size_t> slot(ctys.size());
    for (size_t i = 0; i < ctys.size(); ++i)
        slot[i] = alloc(isVaListType(ctys[i]) ? 32 : isCAggregate(ctys[i]) ? sizeOf(ctys[i]) : 8);
    bool recRet = isCAggregate(fn->returnType);
    size_t retBuf = recRet ? alloc(sizeOf(fn->returnType)) : 0;
    size_t x8Slot = alloc(8);
    size_t frame = alignUp(local, 16);

    std::string sym = cabiSym(fn->name);
    if (exportEntry(fn)) os << "    .globl " << sym << "\n";
    os << "    .p2align 2\n" << sym << ":\n";
    os << "    stp x29, x30, [sp, #-16]!\n    mov x29, sp\n";
    movImm("x16", static_cast<int64_t>(frame), os);
    os << "    sub sp, sp, x16\n";
    memOp("str", "x8", "sp", x8Slot, 8, os);
    for (size_t i = 0; i < ctys.size(); ++i) {
        const CLoc& l = in[i];
        const TypePtr& t = ctys[i];
        switch (l.kind) {
            case CLoc::Gpr: memOp("str", "x" + std::to_string(l.reg), "sp", slot[i], 8, os); break;
            case CLoc::Fpr: memOp("str", (l.dbl ? "d" : "s") + std::to_string(l.reg), "sp", slot[i], l.dbl ? 8 : 4, os); break;
            case CLoc::Stack:
                if (t && t->isFloat()) {
                    memOp("ldr", l.dbl ? "d16" : "s16", "x29", 16 + l.off, l.dbl ? 8 : 4, os);
                    memOp("str", l.dbl ? "d16" : "s16", "sp", slot[i], l.dbl ? 8 : 4, os);
                } else {
                    loadSized("x9", "x29", 16 + l.off, isDarwin ? t : Type::getI64(), os);
                    memOp("str", "x9", "sp", slot[i], 8, os);
                }
                break;
            case CLoc::Hfa:
                for (int k = 0; k < l.count; ++k)
                    memOp("str", (l.dbl ? "d" : "s") + std::to_string(l.reg + k), "sp", slot[i] + k * (l.dbl ? 8 : 4), l.dbl ? 8 : 4, os);
                break;
            case CLoc::Words:
                for (int k = 0; k < l.count; ++k) memOp("str", "x" + std::to_string(l.reg + k), "sp", slot[i] + k * 8, 8, os);
                break;
            case CLoc::StackCopy:
                addImm("x9", "x29", 16 + l.off, os);
                addImm("x10", "sp", slot[i], os);
                copyBytes(l.size, os);
                break;
            case CLoc::Indirect:
                if (l.viaStack) memOp("ldr", "x9", "x29", 16 + l.off, 8, os);
                else os << "    mov x9, x" << l.reg << "\n";
                addImm("x10", "sp", slot[i], os);
                copyBytes(l.size, os);
                break;
        }
        // A va_list: on Darwin its char* arrived (stored above); elsewhere a pointer to it.
        if (isVaListType(t) && !isDarwin) {
            memOp("ldr", "x9", "sp", slot[i], 8, os);
            addImm("x10", "sp", slot[i], os);
            copyBytes(32, os);
        }
    }
    // The Eskiu call: stack arguments first, then registers.
    auto argInto = [&](size_t i, int reg, bool toStack, size_t off) {
        const TypePtr& t = ctys[i];
        std::string r = toStack ? "x9" : "x" + std::to_string(reg);
        if (isCAggregate(t) || isVaListType(t)) addImm(r, "sp", slot[i], os);
        else memOp("ldr", r, "sp", slot[i], 8, os);
        if (toStack) memOp("str", "x9", "sp", off, 8, os);
    };
    for (size_t i = 0; i < ctys.size(); ++i) {
        const ArgLoc& l = out[i];
        if (l.reg >= 0) continue;
        if (l.fp) {
            bool is32 = ctys[i]->bitWidth == 32;
            memOp("ldr", is32 ? "s16" : "d16", "sp", slot[i], is32 ? 4 : 8, os);
            memOp("str", is32 ? "s16" : "d16", "sp", l.stackOff, is32 ? 4 : 8, os);
        } else {
            argInto(i, 9, true, l.stackOff);
        }
    }
    for (size_t i = 0; i < ctys.size(); ++i) {
        const ArgLoc& l = out[i];
        if (l.reg < 0) continue;
        if (l.fp) {
            bool is32 = ctys[i]->bitWidth == 32;
            memOp("ldr", (is32 ? "s" : "d") + std::to_string(l.reg), "sp", slot[i], is32 ? 4 : 8, os);
        } else {
            argInto(i, l.reg, false, 0);
        }
    }
    bool bigRet = recRet && sizeOf(fn->returnType) > 16 && !hfaRet(fn->returnType);
    if (recRet) {
        if (bigRet) memOp("ldr", "x8", "sp", x8Slot, 8, os);
        else addImm("x8", "sp", retBuf, os);
    }
    os << "    bl " << bodySym(fn->name) << "\n";
    if (recRet && !bigRet) {
        bool dbl = false;
        int hfa = hfaOf(fn->returnType, dbl);
        if (hfa) {
            for (int k = 0; k < hfa; ++k)
                memOp("ldr", (dbl ? "d" : "s") + std::to_string(k), "sp", retBuf + k * (dbl ? 8 : 4), dbl ? 8 : 4, os);
        } else {
            memOp("ldr", "x0", "sp", retBuf, 8, os);
            memOp("ldr", "x1", "sp", retBuf + 8, 8, os);
        }
    } else if (bigRet) {
        memOp("ldr", "x0", "sp", x8Slot, 8, os);
    }
    os << "    mov sp, x29\n    ldp x29, x30, [sp], #16\n    ret\n\n";
}

} // namespace eir
