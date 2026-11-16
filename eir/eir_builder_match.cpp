// eir_builder_match.cpp: switch, match, ?, struct and array literals, alloc_with and template calls (split from eir_builder.cpp).
#include "eir_builder.h"
#include "../sema/type_checker.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "../template_utils.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <functional>

namespace eir {

void EIRBuilder::visit(SwitchStmt* node) {
    Operand subj = evaluateExpr(node->subject);
    BasicBlock* endBB = createBlock("switch.end");

    std::vector<BasicBlock*> caseBBs;
    std::vector<int64_t> caseVals;
    BasicBlock* defaultBB = endBB;

    for (size_t i = 0; i < node->cases.size(); ++i) {
        std::string lbl = node->cases[i].value ? "switch.case." + std::to_string(i) : "switch.default";
        BasicBlock* bb = createBlock(lbl);
        caseBBs.push_back(bb);
        if (!node->cases[i].value) {
            defaultBB = bb;
            caseVals.push_back(0);
        } else {
            int64_t v = foldConstInt(node->cases[i].value);
            caseVals.push_back(v);
        }
    }

    Instruction swInst{Opcode::Switch, "", Type::getVoid(), {subj}};
    swInst.defaultLabel = defaultBB->name;
    for (size_t i = 0; i < node->cases.size(); ++i) {
        if (node->cases[i].value) {
            swInst.switchCases.emplace_back(caseVals[i], caseBBs[i]->name);
        }
    }
    emit(std::move(swInst));

    breakStack.push_back(endBB);
    loopContextStack.push_back({endBB, nullptr, cleanupScopes.size(), currentHandlerDepth});
    pushScope();

    for (size_t i = 0; i < node->cases.size(); ++i) {
        setBlock(caseBBs[i]);
        // A declaration after a case label is visible to the rest of the switch (C); the
        // case's defers run at its end.
        cleanupScopes.emplace_back();
        size_t caseDepth = cleanupScopes.size() - 1;
        for (auto& item : node->cases[i].stmts) {
            if (currentBlock->hasTerminator()) break;
            if (std::holds_alternative<DeclPtr>(item)) std::get<DeclPtr>(item)->accept(this);
            else if (std::holds_alternative<StmtPtr>(item)) std::get<StmtPtr>(item)->accept(this);
        }
        if (!currentBlock->hasTerminator()) runCleanupsToDepth(caseDepth, false);
        cleanupScopes.pop_back();
        if (!currentBlock->hasTerminator()) {
            BasicBlock* next = (i + 1 < caseBBs.size()) ? caseBBs[i + 1] : endBB;
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(next->name)}});
        }
    }

    popScope();
    loopContextStack.pop_back();
    breakStack.pop_back();

    setBlock(endBB);
}

void EIRBuilder::visit(MatchStmt* node) {
    std::string enumName = node->enumName;
    if (enumName.empty()) {
        TypePtr subjTy = getExprEirType(node->subject);
        if (subjTy) enumName = subjTy->nominalName();
    }

    if (plainEnums.count(enumName)) {
        EnumDecl* ped = plainEnums[enumName];
        auto valueOf = [&](const std::string& v) -> int64_t {
            auto it = enumConstants.find(v);
            if (it != enumConstants.end()) return it->second;
            for (const auto& m : ped->members) {
                if (m.first == v) return m.second;
            }
            return 0;
        };

        Operand subj = evaluateExpr(node->subject);
        BasicBlock* endBB = createBlock("match.end");
        std::vector<BasicBlock*> armBBs(node->arms.size());
        BasicBlock* defaultBB = endBB;
        for (size_t i = 0; i < node->arms.size(); ++i) {
            armBBs[i] = createBlock("match.arm");
            if (node->arms[i].variant.empty() || node->arms[i].variant == "_") {
                defaultBB = armBBs[i];
            }
        }

        Instruction swInst{Opcode::Switch, "", Type::getVoid(), {subj}};
        swInst.defaultLabel = defaultBB->name;
        for (size_t i = 0; i < node->arms.size(); ++i) {
            if (!node->arms[i].variant.empty() && node->arms[i].variant != "_") {
                swInst.switchCases.emplace_back(valueOf(node->arms[i].variant), armBBs[i]->name);
            }
        }
        emit(std::move(swInst));

        for (size_t i = 0; i < node->arms.size(); ++i) {
            setBlock(armBBs[i]);
            pushScope();
            visitBody(node->arms[i].body);
            popScope();
            if (!currentBlock->hasTerminator()) {
                emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(endBB->name)}});
            }
        }
        setBlock(endBB);
        return;
    }

    // ADT Enum Match
    Operand subjVal = evaluateExpr(node->subject);
    Operand subjAddr = subjVal;
    TypePtr enumTy = Type::getStruct(enumName, {});
    if (!subjVal.type || !subjVal.type->isPointer()) {
        std::string tempSubj = newVar() + ".match_subj";
        emit(Instruction{Opcode::Alloca, tempSubj, enumTy, {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {subjVal, Operand::var(tempSubj, Type::getPtr(enumTy))}});
        subjAddr = Operand::var(tempSubj, Type::getPtr(enumTy));
    }

    // Load tag from offset 0
    std::string tagVal = newVar() + ".tag";
    emit(Instruction{Opcode::Load, tagVal, Type::getI32(), {subjAddr}});
    Operand tagOp = Operand::var(tagVal, Type::getI32());

    BasicBlock* endBB = createBlock("match.end");
    std::vector<BasicBlock*> armBBs(node->arms.size());
    BasicBlock* defaultBB = endBB;
    for (size_t i = 0; i < node->arms.size(); ++i) {
        armBBs[i] = createBlock("match.arm");
        if (node->arms[i].variant.empty() || node->arms[i].variant == "_") {
            defaultBB = armBBs[i];
        }
    }

    Instruction swInst{Opcode::Switch, "", Type::getVoid(), {tagOp}};
    swInst.defaultLabel = defaultBB->name;
    std::string instName;
    if (TypePtr st = getExprEirType(node->subject)) {
        instName = st->nominalName();
        if (!templateInstanceArgs.count(instName)) instName.clear();
    }
    for (size_t i = 0; i < node->arms.size(); ++i) {
        if (!node->arms[i].variant.empty() && node->arms[i].variant != "_") {
            int vidx = 0;
            if (const AdtVariantInfo* v = variantFor(node->arms[i].variant, instName)) vidx = v->variantIndex;
            swInst.switchCases.emplace_back(vidx, armBBs[i]->name);
        }
    }
    emit(std::move(swInst));

    for (size_t i = 0; i < node->arms.size(); ++i) {
        auto& arm = node->arms[i];
        setBlock(armBBs[i]);
        pushScope();

        if (!arm.variant.empty() && arm.variant != "_" && !arm.bindings.empty()) {
            const AdtVariantInfo* vp = variantFor(arm.variant, instName);
            if (vp) {
                std::vector<TypePtr> actualPayloadTypes = vp->payloadTypes;
                size_t payOffset = 8;
                for (size_t b = 0; b < arm.bindings.size() && b < actualPayloadTypes.size(); ++b) {
                    TypePtr bTy = actualPayloadTypes[b];
                    size_t bAlign = getTypeAlign(bTy, module.get());
                    payOffset = (payOffset + bAlign - 1) & ~(bAlign - 1);
                    std::string fPtr = newVar() + "." + arm.bindings[b] + ".ptr";
                    emit(Instruction{Opcode::OffsetPtr, fPtr, Type::getPtr(bTy), {
                        subjAddr, Operand::constInt(static_cast<int64_t>(payOffset), Type::getI64())
                    }});
                    std::string loadedVal = newVar() + "." + arm.bindings[b];
                    emit(Instruction{Opcode::Load, loadedVal, bTy, {Operand::var(fPtr, Type::getPtr(bTy))}});

                    std::string slot = "%" + arm.bindings[b] + ".addr";
                    emit(Instruction{Opcode::Alloca, slot, bTy, {Operand::constInt(static_cast<int64_t>(getTypeAlign(bTy)), Type::getI32())}});
                    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(loadedVal, bTy), Operand::var(slot, Type::getPtr(bTy))}});
                    declareVar(arm.bindings[b], Operand::var(slot, Type::getPtr(bTy)), bTy);

                    payOffset += getTypeSize(bTy, module.get());
                }
            }
        }

        visitBody(arm.body);
        popScope();

        if (!currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(endBB->name)}});
        }
    }

    setBlock(endBB);
}

void EIRBuilder::visit(QuestionExpr* node) {
    Operand resVal = evaluateExpr(node->operand);
    TypePtr st = resVal.type;
    std::string sname = st ? st->nominalName() : "";
    std::string tempSlot = newVar() + ".q_tmp";
    emit(Instruction{Opcode::Alloca, tempSlot, st, {Operand::constInt(8, Type::getI32())}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {resVal, Operand::var(tempSlot, Type::getPtr(st))}});

    std::string okPtr = newVar() + ".q_ok_ptr";
    emit(Instruction{Opcode::FieldPtr, okPtr, Type::getPtr(Type::getI1()), {Operand::var(sname, nullptr), Operand::var(tempSlot, Type::getPtr(st)), Operand::constString("ok")}});
    std::string okVal = newVar() + ".q_ok";
    emit(Instruction{Opcode::Load, okVal, Type::getI1(), {Operand::var(okPtr, Type::getPtr(Type::getI1()))}});

    std::string isErr = newVar() + ".q_iserr";
    emit(Instruction{Opcode::ICmp, isErr, Type::getI1(), {Operand::var(okVal, Type::getI1()), Operand::constInt(0, Type::getI1())}, ICmpCond::Eq});

    BasicBlock* errBB = createBlock("q.err");
    BasicBlock* contBB = createBlock("q.cont");
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(isErr, Type::getI1()), Operand::block(errBB->name), Operand::block(contBB->name)}});

    // Error path: return the error from the enclosing function. Its Result type may differ
    // in the success type only: then a fresh one (zeroed) gets the `ok` flag and the error.
    // This is an early exit, so the pending defers and errdefers run first.
    setBlock(errBB);
    Operand whole = Operand::var(tempSlot, st);
    TypePtr retTy = currentFunction ? currentFunction->returnType : nullptr;
    if (retTy && retTy->isStruct() && retTy->nominalName() != sname) {
        std::string rname = retTy->nominalName();
        ensureStructLayout(rname);
        std::string out = newVar() + ".q_out";
        emit(Instruction{Opcode::Alloca, out, retTy, {Operand::constInt(8, Type::getI32())}});
        for (const char* fname : {"ok", "error"}) {
            const StructFieldInfo* from = module->structLayouts.count(sname) ? module->structLayouts[sname].getFieldInfo(fname) : nullptr;
            const StructFieldInfo* to = module->structLayouts.count(rname) ? module->structLayouts[rname].getFieldInfo(fname) : nullptr;
            if (!from || !to) continue;
            std::string fp = newVar(), tp = newVar(), v = newVar();
            emit(Instruction{Opcode::FieldPtr, fp, Type::getPtr(from->type), {Operand::var(sname, nullptr), Operand::var(tempSlot, Type::getPtr(st)), Operand::constString(fname)}});
            emit(Instruction{Opcode::Load, v, from->type, {Operand::var(fp, Type::getPtr(from->type))}});
            emit(Instruction{Opcode::FieldPtr, tp, Type::getPtr(to->type), {Operand::var(rname, nullptr), Operand::var(out, Type::getPtr(retTy)), Operand::constString(fname)}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(v, from->type), Operand::var(tp, Type::getPtr(to->type))}});
        }
        whole = Operand::var(out, retTy);
    }
    runCleanupsToDepth(0, true);
    if (currentBlock && !currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Ret, "", whole.type, {whole}});
    }

    setBlock(contBB);
    TypePtr valTy = getExprEirType(node);
    std::string valPtr = newVar() + ".q_val_ptr";
    emit(Instruction{Opcode::FieldPtr, valPtr, Type::getPtr(valTy), {Operand::var(sname, nullptr), Operand::var(tempSlot, Type::getPtr(st)), Operand::constString("value")}});
    std::string unwrapped = newVar() + ".q_unwrapped";
    emit(Instruction{Opcode::Load, unwrapped, valTy, {Operand::var(valPtr, Type::getPtr(valTy))}});
    lastValue = Operand::var(unwrapped, valTy);
}

void EIRBuilder::visit(StructInitExpr* node0) {
    std::shared_ptr<StructInitExpr> full = withDefaults(node0);
    StructInitExpr* node = full ? full.get() : node0;
    std::string sname = node->structName;
    if (sname.find('<') != std::string::npos) {
        sname = mangleTemplate(typeParamOverride.empty() ? sname : substType(sname, typeParamOverride));
    }
    TypePtr stype = toEirType(sname);
    size_t al = getTypeAlign(stype);
    if (al < 1) al = 1;
    std::string tempSlot = newVar();
    emit(Instruction{Opcode::Alloca, tempSlot, stype, {Operand::constInt(static_cast<int64_t>(al), Type::getI32())}});

    for (size_t i = 0; i < node->fieldInits.size(); ++i) {
        std::string fname = node->fieldInits[i].first;
        ExprPtr fval = node->fieldInits[i].second;
        if (!fval) continue;

        const StructFieldInfo* fi = nullptr;
        TypePtr ftype = nullptr;
        if (module) {
            auto it = module->structLayouts.find(sname);
            if (it != module->structLayouts.end()) {
                if (fname.empty() && i < it->second.fields.size()) {
                    fname = it->second.fields[i].name;
                }
                fi = it->second.getFieldInfo(fname);
                if (fi) ftype = fi->type;
            }
        }
        if (!ftype) ftype = getExprEirType(fval);

        if (fi && fi->isBitfield) {
            Operand val = evaluateExpr(fval);
            std::string wordPtr = newVar();
            emit(Instruction{Opcode::OffsetPtr, wordPtr, Type::getPtr(fi->storageType),
                             {Operand::var(tempSlot, Type::getPtr(stype)), Operand::constInt(static_cast<int64_t>(fi->storageOffset), Type::getI64())}});
            std::string wordVal = newVar();
            emit(Instruction{Opcode::Load, wordVal, fi->storageType, {Operand::var(wordPtr, Type::getPtr(fi->storageType))}});

            uint64_t mask = (fi->bitWidth >= 64) ? ~0ULL : ((1ULL << fi->bitWidth) - 1);
            uint64_t fieldMask = mask << fi->bitOffset;
            std::string cleared = newVar();
            emit(Instruction{Opcode::And, cleared, fi->storageType, {Operand::var(wordVal, fi->storageType), Operand::constInt(static_cast<int64_t>(~fieldMask), fi->storageType)}});

            Operand valToStore = val;
            if (val.type->bitWidth != fi->storageType->bitWidth) {
                std::string conv = newVar();
                if (val.type->bitWidth > fi->storageType->bitWidth) {
                    emit(Instruction{Opcode::Trunc, conv, fi->storageType, {val}});
                } else {
                    emit(Instruction{fi->isSigned ? Opcode::SExt : Opcode::ZExt, conv, fi->storageType, {val}});
                }
                valToStore = Operand::var(conv, fi->storageType);
            }

            std::string vMasked = newVar();
            emit(Instruction{Opcode::And, vMasked, fi->storageType, {valToStore, Operand::constInt(static_cast<int64_t>(mask), fi->storageType)}});
            Operand vShifted = Operand::var(vMasked, fi->storageType);
            if (fi->bitOffset > 0) {
                std::string s = newVar();
                emit(Instruction{Opcode::Shl, s, fi->storageType, {vShifted, Operand::constInt(static_cast<int64_t>(fi->bitOffset), fi->storageType)}});
                vShifted = Operand::var(s, fi->storageType);
            }

            std::string newWord = newVar();
            emit(Instruction{Opcode::Or, newWord, fi->storageType, {Operand::var(cleared, fi->storageType), vShifted}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(newWord, fi->storageType), Operand::var(wordPtr, Type::getPtr(fi->storageType))}});
            continue;
        }

        std::string fptr = newVar();
        emit(Instruction{Opcode::FieldPtr, fptr, Type::getPtr(ftype), {
            Operand::var(sname, nullptr),
            Operand::var(tempSlot, Type::getPtr(stype)),
            Operand::constString(fname)
        }});

        if (auto* arrLit = nodeCast<ArrayLitExpr>(fval.get()); arrLit && ftype && ftype->isArray()) {
            emitArrayInitInto(Operand::var(fptr, Type::getPtr(ftype)), arrLit, ftype);
            continue;
        }
        Operand val = evalForType(fval, ftype);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {val, Operand::var(fptr, Type::getPtr(ftype))}});
    }

    // An aggregate value is the address of its bytes, typed as the aggregate so a store
    // copies all of it.
    lastLValue = Operand::var(tempSlot, Type::getPtr(stype));
    lastValue = Operand::var(tempSlot, stype);
}

void EIRBuilder::visit(ArrayLitExpr* node) {
    TypePtr arrTy = getExprEirType(node);
    size_t al = getTypeAlign(arrTy);
    if (al < 1) al = 1;
    std::string tempSlot = newVar() + ".arrlit";
    emit(Instruction{Opcode::Alloca, tempSlot, arrTy, {Operand::constInt(static_cast<int64_t>(al), Type::getI32())}});
    emitArrayInitInto(Operand::var(tempSlot, Type::getPtr(arrTy)), node, arrTy);
    lastLValue = Operand::var(tempSlot, Type::getPtr(arrTy));
    lastValue = Operand::var(tempSlot, arrTy);
}

// `alloc_with(&a, T, n)` is `(*T)A_alloc(&a, n * sizeof(T))` (A the allocator's struct,
// or the inline `alloc` of a generic instance), null when the size overflows or is
// negative (or does not fit the size parameter), and the block zeroed when it is not
// null, as alloc<T> returns zeroed memory whatever the allocator.
void EIRBuilder::visit(AllocWithExpr* node) {
    Operand allocPtr = evaluateExpr(node->allocator);
    TypePtr at = getExprEirType(node->allocator);
    std::string an = at ? at->nominalName() : "";
    std::string fnName = an + "_alloc";
    Function* af = module->getFunction(fnName);
    if (!af) af = instantiateGenericMethod(an, "alloc");
    if (!af) throw std::runtime_error("EIR: alloc_with: allocator type '" + an + "' has no alloc method (" + fnName + ")");
    fnName = af->name;

    std::string et = typeParamOverride.empty() ? node->elemType : substType(node->elemType, typeParamOverride);
    TypePtr elemTy = toEirType(et);
    size_t esz = getTypeSize(elemTy, module.get());
    if (elemTy && elemTy->isStruct()) {
        size_t ssz = module->getStructSize(elemTy->nominalName());
        if (ssz) esz = ssz;
    }
    if (esz == 0) esz = 1;
    TypePtr i64 = Type::getI64();
    Operand n = coerceNumeric(evaluateExpr(node->count), i64);

    // bad = n < 0 || n > INT64_MAX / esz (|| the size does not fit a narrower parameter)
    int64_t maxN = INT64_MAX / static_cast<int64_t>(esz);
    TypePtr sizeTy = af->params.size() >= 2 ? af->params[1].second : i64;
    if (sizeTy && sizeTy->isInt() && sizeTy->bitWidth < 64) {
        int64_t maxv = static_cast<int64_t>((uint64_t(1) << (sizeTy->bitWidth - 1)) - 1);
        maxN = std::min<int64_t>(maxN, maxv / static_cast<int64_t>(esz));
    }
    std::string neg = newVar(), big = newVar(), bad = newVar();
    emit(Instruction{Opcode::ICmp, neg, Type::getI1(), {n, Operand::constInt(0, i64)}, ICmpCond::Slt});
    emit(Instruction{Opcode::ICmp, big, Type::getI1(), {n, Operand::constInt(maxN, i64)}, ICmpCond::Sgt});
    emit(Instruction{Opcode::Or, bad, Type::getI1(), {Operand::var(neg, Type::getI1()), Operand::var(big, Type::getI1())}});

    TypePtr resTy = Type::getPtr(elemTy);
    std::string slot = newVar() + ".allocw";
    emit(Instruction{Opcode::Alloca, slot, Type::getPtr(), {Operand::constInt(8, Type::getI32())}});
    BasicBlock* callBB = createBlock("allocw.call");
    BasicBlock* zeroBB = createBlock("allocw.zero");
    BasicBlock* loopBB = createBlock("allocw.loop");
    BasicBlock* bodyBB = createBlock("allocw.body");
    BasicBlock* doneBB = createBlock("allocw.done");
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(bad, Type::getI1()), Operand::block(doneBB->name), Operand::block(callBB->name)}});

    setBlock(callBB);
    std::string total = newVar();
    emit(Instruction{Opcode::Mul, total, i64, {n, Operand::constInt(static_cast<int64_t>(esz), i64)}});
    Operand sizeArg = coerceNumeric(Operand::var(total, i64), sizeTy);
    std::string p = newVar();
    emit(Instruction{Opcode::Call, p, Type::getPtr(), {Operand::global(fnName, Type::getPtr()), allocPtr, sizeArg}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(p, Type::getPtr()), Operand::var(slot, Type::getPtr(Type::getPtr()))}});
    std::string isNull = newVar();
    emit(Instruction{Opcode::ICmp, isNull, Type::getI1(), {Operand::var(p, Type::getPtr()), Operand::constNull()}, ICmpCond::Eq});
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(isNull, Type::getI1()), Operand::block(doneBB->name), Operand::block(zeroBB->name)}});

    // Zero the block a byte at a time.
    setBlock(zeroBB);
    std::string idxSlot = newVar() + ".i";
    emit(Instruction{Opcode::Alloca, idxSlot, i64, {Operand::constInt(8, Type::getI32())}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});
    setBlock(loopBB);
    std::string iv = newVar(), more = newVar();
    emit(Instruction{Opcode::Load, iv, i64, {Operand::var(idxSlot, Type::getPtr(i64))}});
    emit(Instruction{Opcode::ICmp, more, Type::getI1(), {Operand::var(iv, i64), Operand::var(total, i64)}, ICmpCond::Slt});
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(more, Type::getI1()), Operand::block(bodyBB->name), Operand::block(doneBB->name)}});
    setBlock(bodyBB);
    std::string bp = newVar(), next = newVar();
    emit(Instruction{Opcode::OffsetPtr, bp, Type::getPtr(Type::getU8()), {Operand::var(p, Type::getPtr()), Operand::var(iv, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getU8()), Operand::var(bp, Type::getPtr(Type::getU8()))}});
    emit(Instruction{Opcode::Add, next, i64, {Operand::var(iv, i64), Operand::constInt(1, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(next, i64), Operand::var(idxSlot, Type::getPtr(i64))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});

    setBlock(doneBB);
    std::string res = newVar();
    emit(Instruction{Opcode::Load, res, resTy, {Operand::var(slot, Type::getPtr(Type::getPtr()))}});
    lastValue = Operand::var(res, resTy);
}

void EIRBuilder::visit(TemplateCallExpr* node) {
    if (node->templateName == "va_arg" && node->args.size() == 1 && node->typeArgs.size() == 1) {
        Operand ap = evaluateLValue(node->args[0]);
        std::string t = typeParamOverride.empty() ? node->typeArgs[0] : substType(node->typeArgs[0], typeParamOverride);
        TypePtr resTy = toEirType(t);
        std::string dst = newVar();
        emit(Instruction{Opcode::Call, dst, resTy, {
            Operand::global("va_arg", Type::getPtr()),
            ap
        }});
        lastValue = Operand::var(dst, resTy);
        return;
    }

    std::string fnName = node->templateName;
    std::vector<std::string> substitutedArgs;
    for (const auto& a : node->typeArgs) {
        substitutedArgs.push_back(typeParamOverride.empty() ? a : substType(a, typeParamOverride));
    }

    auto templ = funcTemplateDecls.find(node->templateName);
    if (templ != funcTemplateDecls.end()) {
        FunctionDecl* fd = templ->second;
        std::map<std::string, std::string> subs;
        for (size_t i = 0; i < fd->typeParams.size() && i < substitutedArgs.size(); ++i) {
            subs[fd->typeParams[i]] = substitutedArgs[i];
        }
        std::string mangledName = node->templateName;
        for (const auto& t : substitutedArgs) mangledName += "_" + mangleTemplate(t);
        Function* func = instantiateFnTemplate(fd, mangledName, subs);
        if (func) fnName = func->name;
    } else if (module && !module->getFunction(fnName)) {
        std::string mangled = node->templateName;
        for (const auto& t : substitutedArgs) {
            mangled += "_" + mangleTemplate(t);
        }
        if (module->getFunction(mangled)) {
            fnName = mangled;
        }
    }

    // Check if callee is an ADT variant constructor
    const AdtVariantInfo* vptr = adtVariants.count(node->templateName)
        ? variantFor(node->templateName, genericVariantInstance(node->templateName, substitutedArgs)) : nullptr;
    if (vptr) {
        const AdtVariantInfo& vinfo = *vptr;
        TypePtr enumTy = Type::getStruct(vinfo.enumName, {});
        std::string slot = newVar() + ".adt";
        emit(Instruction{Opcode::Alloca, slot, enumTy, {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {
            Operand::constInt(vinfo.variantIndex, Type::getI32()),
            Operand::var(slot, Type::getPtr(Type::getI32()))
        }});

        size_t payOffset = 8;
        for (size_t p = 0; p < node->args.size() && p < vinfo.payloadTypes.size(); ++p) {
            TypePtr pt = vinfo.payloadTypes[p];
            size_t pAlign = getTypeAlign(pt, module.get());
            payOffset = (payOffset + pAlign - 1) & ~(pAlign - 1);
            std::string payPtr = newVar() + ".pay";
            emit(Instruction{Opcode::OffsetPtr, payPtr, Type::getPtr(pt), {
                Operand::var(slot, Type::getPtr()), Operand::constInt(static_cast<int64_t>(payOffset), Type::getI64())
            }});
            Operand argVal = evalForType(node->args[p], pt);
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {argVal, Operand::var(payPtr, Type::getPtr(pt))}});
            payOffset += getTypeSize(pt, module.get());
        }
        lastLValue = Operand::var(slot, Type::getPtr(enumTy));
        lastValue = Operand::var(slot, enumTy);
        return;
    }

    std::vector<Operand> args;
    args.push_back(Operand::global(fnName, Type::getPtr()));
    Function* targetFn = module ? module->getFunction(fnName) : nullptr;
    for (size_t i = 0; i < node->args.size(); ++i) {
        TypePtr paramType = (targetFn && i < targetFn->params.size()) ? targetFn->params[i].second : nullptr;
        args.push_back(evalForType(node->args[i], paramType));
    }

    TypePtr retTy = nullptr;
    if (module && !fnName.empty()) {
        std::string lookupName = fnName;
        if (lookupName[0] == '@') lookupName = lookupName.substr(1);
        if (Function* fn = module->getFunction(lookupName)) {
            retTy = fn->returnType;
        }
    }
    if (!retTy) retTy = getExprEirType(node);
    std::string dst = retTy->isVoid() ? "" : newVar();

    emit(Instruction{Opcode::Call, dst, retTy, args});
    lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
    lastLValue = (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isInterface() || retTy->isClosure() || retTy->isSlice())) ? Operand::var(dst, Type::getPtr(retTy)) : lastValue;
}

} // namespace eir
