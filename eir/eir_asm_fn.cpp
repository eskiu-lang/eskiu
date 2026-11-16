#include "eir_asm_int.h"

namespace eir {

void Arm64Emitter::emitFunction(const Function* fn, int idx, std::ostream& os) {
    curFn = fn;
    fnTag = "F" + std::to_string(idx);
    slots.clear();
    addrVars.clear();
    resultTypes.clear();
    handlerIds.clear();

    fnHasSret = isAggregate(fn->returnType);
    fnSavesCaught = false;
    size_t pushCount = 0;
    size_t outgoing = 0;

    for (const auto& bb : fn->blocks) {
        for (const auto& inst : bb->instructions) {
            if (inst->op == Opcode::PushHandler) {
                pushCount++;
                if (!handlerIds.count(inst->operands[0].text)) {
                    int id = static_cast<int>(handlerIds.size());
                    handlerIds[inst->operands[0].text] = id;
                }
            }
            if (inst->op == Opcode::Call && inst->operands[0].text == "__eir_push_caught") fnSavesCaught = true;
            if ((inst->op == Opcode::Call || inst->op == Opcode::CallIndirect) && !isSpecialCall(inst->operands[0].text)) {
                size_t bytes = 0;
                CallShape cs = callShape(inst.get());
                if (isCRecordCall(inst.get(), cs)) classifyC(cs.argTypes, cs.fixedCount, cs.variadic, &bytes);
                else classify(cs.argTypes, cs.fixedCount, cs.variadic, &bytes);
                outgoing = std::max(outgoing, bytes);
            }
        }
    }

    size_t local = outgoing;
    auto alloc = [&](size_t sz, size_t al) {
        local = alignUp(local, std::max<size_t>(al, 8));
        size_t off = local;
        local += alignUp(std::max<size_t>(sz, 1), 8);
        return off;
    };

    std::vector<TypePtr> paramTypes;
    for (const auto& p : fn->params) {
        slots["%" + p.first] = alloc(8, 8);
        paramTypes.push_back(p.second);
    }
    std::vector<ArgLoc> paramLocs = classify(paramTypes, paramTypes.size(), false, &incomingStackBytes);
    incomingStackBytes = 0;
    fixedGr = fixedVr = 0;
    for (const auto& l : paramLocs) {
        if (l.reg < 0) incomingStackBytes = std::max(incomingStackBytes, alignUp(l.stackOff + l.size, 8));
        else if (l.fp) fixedVr++;
        else fixedGr++;
    }
    if (fnHasSret) sretSlot = alloc(8, 8);
    callScratch.clear();
    for (const auto& bb : fn->blocks) {
        for (const auto& inst : bb->instructions) {
            if (inst->op != Opcode::Call && inst->op != Opcode::CallIndirect) continue;
            if (isSpecialCall(inst->operands[0].text)) continue;
            CallShape cs = callShape(inst.get());
            if (!isCRecordCall(inst.get(), cs)) continue;
            size_t total = 0;
            for (const auto& t : cs.argTypes) if (isCAggregate(t)) total += alignUp(std::max<size_t>(sizeOf(t), 16), 16);
            callScratch[inst.get()] = alloc(std::max<size_t>(total, 16), 16);
        }
    }
    if (fn->isVariadic && !isDarwin) {
        grSaveSlot = alloc(64, 16);
        vrSaveSlot = alloc(128, 16);
    }
    if (pushCount > 0) {
        handlerCap = std::max<size_t>(16, pushCount * 4);
        handlerDepthSlot = alloc(8, 8);
        handlerStackSlot = alloc(handlerCap * 8, 8);
    }
    if (fnSavesCaught) {
        caughtSlot = alloc(8, 8);
        retSaveSlot = alloc(16, 8);
    }

    for (const auto& bb : fn->blocks) {
        for (const auto& inst : bb->instructions) {
            const std::string& rv = inst->resultVar;
            if (inst->op == Opcode::Alloca) {
                size_t al = inst->operands.empty() ? 8 : static_cast<size_t>(std::max<int64_t>(inst->operands[0].intVal, 1));
                if (!slots.count(rv)) slots[rv] = alloc(sizeOf(inst->type), std::min<size_t>(al, 16));
                addrVars.insert(rv);
            } else if (inst->op == Opcode::MakeClosure || inst->op == Opcode::MakeInterface || inst->op == Opcode::MakeSlice) {
                if (!slots.count(rv)) slots[rv] = alloc(16, 8);
                addrVars.insert(rv);
            } else if ((inst->op == Opcode::Call || inst->op == Opcode::CallIndirect) && !rv.empty() && isAggregate(inst->type)) {
                if (!slots.count(rv)) slots[rv] = alloc(sizeOf(inst->type), 8);
                addrVars.insert(rv);
            } else if (inst->op == Opcode::InlineAsm && !rv.empty() && inst->type && inst->type->isStruct()) {
                if (!slots.count(rv)) slots[rv] = alloc(8 * std::max<size_t>(inst->type->members.size(), 1), 8);
                addrVars.insert(rv);
            } else if (!rv.empty() && !slots.count(rv)) {
                slots[rv] = alloc(8, 8);
            }
            if (!rv.empty() && !addrVars.count(rv) && inst->type && inst->type->isInt() && inst->type->bitWidth < 64)
                resultTypes[rv] = inst->type;
        }
    }

    size_t frameSize = alignUp(local, 16);
    if (frameSize == 0) frameSize = 16;

    std::string sym = bodySym(fn->name);
    if (!isRuntimeName(fn->name) && !exportEntry(fn)) os << "    .globl " << sym << "\n";
    os << "    .p2align 2\n" << sym << ":\n";
    os << "    stp x29, x30, [sp, #-16]!\n";
    os << "    mov x29, sp\n";
    if (frameSize <= 4095) {
        os << "    sub sp, sp, #" << frameSize << "\n";
    } else {
        movImm("x16", static_cast<int64_t>(frameSize), os);
        os << "    sub sp, sp, x16\n";
    }

    if (fn->isVariadic && !isDarwin) {
        addImm("x16", "sp", grSaveSlot, os);
        for (int r = 0; r < 8; r += 2)
            os << "    stp x" << r << ", x" << r + 1 << ", [x16, #" << r * 8 << "]\n";
        addImm("x16", "sp", vrSaveSlot, os);
        for (int r = 0; r < 8; r += 2)
            os << "    stp q" << r << ", q" << r + 1 << ", [x16, #" << r * 16 << "]\n";
    }
    if (fnHasSret) memOp("str", "x8", "sp", sretSlot, 8, os);

    for (size_t i = 0; i < fn->params.size(); ++i) {
        const ArgLoc& l = paramLocs[i];
        TypePtr pty = fn->params[i].second;
        std::string var = "%" + fn->params[i].first;
        if (l.fp) {
            bool is32 = pty->bitWidth == 32;
            if (l.reg >= 0) {
                storeFloat(l.reg, is32, var, os);
            } else {
                memOp("ldr", is32 ? "s16" : "d16", "x29", 16 + l.stackOff, is32 ? 4 : 8, os);
                storeFloat(16, is32, var, os);
            }
            continue;
        }
        if (l.reg >= 0) {
            if (l.reg != 9) os << "    mov x9, x" << l.reg << "\n";
        } else {
            loadSized("x9", "x29", 16 + l.stackOff, isDarwin ? pty : Type::getI64(), os);
        }
        normalize(9, pty, os);
        storeX("x9", var, os);
    }

    if (pushCount > 0) memOp("str", "xzr", "sp", handlerDepthSlot, 8, os);
    if (fnSavesCaught) {
        os << "    bl " << mangleSym("__eir_caught_depth") << "\n";
        memOp("str", "x0", "sp", caughtSlot, 8, os);
    }

    std::string retLabel = localPrefix() + "ret_" + fnTag;
    std::string unwindLabel = localPrefix() + "unwind_" + fnTag;

    for (const auto& bb : fn->blocks) {
        os << blockLabel(bb->name) << ":\n";
        for (const auto& inst : bb->instructions) emitInstruction(inst.get(), retLabel, unwindLabel, os);
        if (!bb->hasTerminator()) {
            if (fn->name == "main") os << "    mov x0, #0\n";
            os << "    b " << retLabel << "\n";
        }
    }

    if (throws || pushCount > 0) {
        os << unwindLabel << ":\n";
        if (pushCount > 0) {
            std::string none = newLocal("nohandler");
            memOp("ldr", "x9", "sp", handlerDepthSlot, 8, os);
            os << "    cbz x9, " << none << "\n";
            os << "    sub x9, x9, #1\n";
            memOp("str", "x9", "sp", handlerDepthSlot, 8, os);
            addImm("x10", "sp", handlerStackSlot, os);
            os << "    ldr x9, [x10, x9, lsl #3]\n";
            std::vector<std::string> byId(handlerIds.size());
            for (const auto& kv : handlerIds) byId[kv.second] = kv.first;
            for (size_t id = 0; id < byId.size(); ++id) {
                movImm("x10", static_cast<int64_t>(id), os);
                os << "    cmp x9, x10\n";
                os << "    b.eq " << blockLabel(byId[id]) << "\n";
            }
            os << "    brk #1\n";
            os << none << ":\n";
        }
        if (fn->name == "main") os << "    bl " << mangleSym("__eir_uncaught") << "\n";
        os << "    mov x0, #0\n";
        os << "    b " << retLabel << "\n";
    }

    os << retLabel << ":\n";
    if (fnSavesCaught) {
        memOp("str", "x0", "sp", retSaveSlot, 8, os);
        memOp("str", "d0", "sp", retSaveSlot + 8, 8, os);
        memOp("ldr", "x0", "sp", caughtSlot, 8, os);
        os << "    bl " << mangleSym("__eir_set_caught_depth") << "\n";
        memOp("ldr", "x0", "sp", retSaveSlot, 8, os);
        memOp("ldr", "d0", "sp", retSaveSlot + 8, 8, os);
    }
    os << "    mov sp, x29\n";
    os << "    ldp x29, x30, [sp], #16\n";
    os << "    ret\n\n";
}

} // namespace eir
