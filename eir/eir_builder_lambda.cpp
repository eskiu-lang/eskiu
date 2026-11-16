// eir_builder_lambda.cpp: lambdas, asm, unions, sizeof, threads, await, throw, try and defer (split from eir_builder.cpp).
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

void EIRBuilder::visit(LambdaExpr* node) {
    std::string lambdaName = "__lambda_" + std::to_string(nextLambdaId++);

    std::vector<TypePtr> capTypes;
    std::vector<size_t> capOffsets;
    size_t envOffset = 0;
    size_t maxAlign = 1;
    for (const auto& cap : node->captures) {
        TypePtr ct = toEirType(cap.second);
        size_t al = getTypeAlign(ct);
        if (al > maxAlign) maxAlign = al;
        envOffset = (envOffset + al - 1) & ~(al - 1);
        capOffsets.push_back(envOffset);
        envOffset += getTypeSize(ct);
        capTypes.push_back(ct);
    }
    size_t totalEnvSize = (envOffset + maxAlign - 1) & ~(maxAlign - 1);
    if (totalEnvSize == 0) totalEnvSize = 8;

    Operand envPtr = Operand::constNull();
    if (!node->captures.empty()) {
        std::string envSlot = newVar() + ".env";
        if (node->escapes) {
            emit(Instruction{Opcode::Call, envSlot, Type::getPtr(), {
                Operand::global("malloc", Type::getPtr()), Operand::constInt(static_cast<int64_t>(totalEnvSize), Type::getI64())
            }});
        } else {
            emit(Instruction{Opcode::Alloca, envSlot, Type::getArray(Type::getU8(), totalEnvSize), {Operand::constInt(static_cast<int64_t>(maxAlign), Type::getI32())}});
        }
        for (size_t c = 0; c < node->captures.size(); ++c) {
            Operand* srcSlot = lookupVar(node->captures[c].first);
            if (srcSlot) {
                std::string valVar = newVar() + ".cap";
                emit(Instruction{Opcode::Load, valVar, capTypes[c], {*srcSlot}});
                std::string capFieldPtr = newVar() + ".cap_field";
                emit(Instruction{Opcode::OffsetPtr, capFieldPtr, Type::getPtr(capTypes[c]), {
                    Operand::var(envSlot, Type::getPtr()), Operand::constInt(static_cast<int64_t>(capOffsets[c]), Type::getI64())
                }});
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {
                    Operand::var(valVar, capTypes[c]), Operand::var(capFieldPtr, Type::getPtr(capTypes[c]))
                }});
            }
        }
        envPtr = Operand::var(envSlot, Type::getPtr());
    }

    std::string closureSlot = newVar() + ".closure";
    TypePtr lambdaRet = toEirType(node->returnType);
    emit(Instruction{Opcode::MakeClosure, closureSlot, Type::getClosure(lambdaRet), {
        Operand::global(lambdaName, Type::getPtr()), envPtr
    }});
    Operand callerClosure = Operand::var(closureSlot, Type::getClosure(lambdaRet));

    // Compile lambda function body
    Function* prevFn = currentFunction;
    BasicBlock* prevBB = currentBlock;
    int prevVarId = nextVarId;
    int prevBlockId = nextBlockId;
    auto prevBreakStack = breakStack;
    auto prevContinueStack = continueStack;
    auto prevCleanups = std::move(cleanupScopes);
    auto prevLoops = std::move(loopContextStack);
    auto prevUnwind = currentUnwindTarget;
    auto prevUnwindStack = std::move(unwindTargetStack);
    size_t prevHandlerDepth = currentHandlerDepth;
    cleanupScopes.clear();
    cleanupScopes.emplace_back(); // root lambda cleanup frame
    loopContextStack.clear();
    currentUnwindTarget = nullptr;
    unwindTargetStack.clear();
    currentHandlerDepth = 0;

    std::vector<std::pair<std::string, TypePtr>> fnParams;
    fnParams.emplace_back("env", Type::getPtr());
    for (const auto& p : node->params) {
        fnParams.emplace_back(p.second, toEirType(p.first));
    }
    Function* lambdaFn = module->createFunction(lambdaName, lambdaRet, fnParams);
    currentFunction = lambdaFn;
    nextVarId = 0;
    nextBlockId = 0;
    breakStack.clear();
    continueStack.clear();

    BasicBlock* entryBB = lambdaFn->createBlock("entry");
    setBlock(entryBB);
    pushScope();

    for (size_t i = 0; i < lambdaFn->params.size(); ++i) {
        const auto& p = lambdaFn->params[i];
        std::string slotName = "%" + p.first + ".addr";
        emit(Instruction{Opcode::Alloca, slotName, p.second, {Operand::constInt(p.second->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var("%" + p.first, p.second), Operand::var(slotName, Type::getPtr(p.second))}});
        declareVar(p.first, Operand::var(slotName, Type::getPtr(p.second)), p.second);
    }

    if (!node->captures.empty()) {
        for (size_t c = 0; c < node->captures.size(); ++c) {
            std::string cname = node->captures[c].first;
            TypePtr cty = capTypes[c];
            std::string capFieldPtr = newVar() + "." + cname + ".ptr";
            emit(Instruction{Opcode::OffsetPtr, capFieldPtr, Type::getPtr(cty), {
                Operand::var("%env", Type::getPtr()), Operand::constInt(static_cast<int64_t>(capOffsets[c]), Type::getI64())
            }});
            std::string loadedVal = newVar() + "." + cname + ".val";
            emit(Instruction{Opcode::Load, loadedVal, cty, {Operand::var(capFieldPtr, Type::getPtr(cty))}});
            std::string localSlot = "%" + cname + ".addr";
            emit(Instruction{Opcode::Alloca, localSlot, cty, {Operand::constInt(cty->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(loadedVal, cty), Operand::var(localSlot, Type::getPtr(cty))}});
            declareVar(cname, Operand::var(localSlot, Type::getPtr(cty)), cty);
        }
    }

    node->body->accept(this);

    if (!currentBlock->hasTerminator()) {
        if (lambdaFn->returnType->isVoid()) {
            emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
        } else {
            emit(Instruction{Opcode::Unreachable, "", Type::getVoid(), {}});
        }
    }

    popScope();

    cleanupScopes = std::move(prevCleanups);
    loopContextStack = std::move(prevLoops);
    currentUnwindTarget = prevUnwind;
    unwindTargetStack = std::move(prevUnwindStack);
    currentHandlerDepth = prevHandlerDepth;

    currentFunction = prevFn;
    setBlock(prevBB);
    nextVarId = prevVarId;
    nextBlockId = prevBlockId;
    breakStack = prevBreakStack;
    continueStack = prevContinueStack;

    lastValue = callerClosure;
    lastLValue = callerClosure;
}

void EIRBuilder::visit(AsmStmt* node) {
    std::vector<Operand> operands;
    std::string constraints;
    auto addConstraint = [&](const std::string& c) {
        if (!constraints.empty()) constraints += ",";
        constraints += c;
    };

    struct RegOut { Operand addr; TypePtr ty; bool vol; };
    std::vector<RegOut> regOuts;
    struct Tied { std::string constraint; Operand val; };
    std::vector<Tied> tied;

    for (size_t i = 0; i < node->outputs.size(); ++i) {
        const std::string& c = node->outputs[i].first;
        const ExprPtr& e = node->outputs[i].second;
        bool rw = !c.empty() && c[0] == '+';
        std::string body = c.substr(1);
        TypePtr ty = getExprEirType(e);
        Operand addr = evaluateLValue(e);
        bool vol = volatileRooted(e.get());
        if (body == "m") {
            addConstraint("=*m");
            operands.push_back(addr);
            if (rw) tied.push_back({"*m", addr});
        } else {
            addConstraint("=" + body);
            regOuts.push_back({addr, ty, vol});
            if (rw) {
                std::string cur = newVar();
                Instruction ld{Opcode::Load, cur, ty, {addr}};
                ld.isVolatile = vol;
                emit(ld);
                tied.push_back({std::to_string(i), Operand::var(cur, ty)});
            }
        }
    }

    for (auto& [constraint, expr] : node->inputs) {
        operands.push_back(evaluateExpr(expr));
        addConstraint(constraint);
    }
    for (auto& t : tied) {
        operands.push_back(t.val);
        addConstraint(t.constraint);
    }
    for (const auto& clob : node->clobbers) {
        addConstraint("~{" + clob + "}");
    }

    Instruction inst;
    inst.op = Opcode::InlineAsm;
    inst.asmString = node->asmString;
    inst.asmConstraints = constraints;
    inst.clobbers = node->clobbers;
    inst.operands = operands;

    if (regOuts.size() == 1) {
        inst.type = regOuts[0].ty;
        inst.resultVar = newVar();
        emit(inst);
        Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(inst.resultVar, inst.type), regOuts[0].addr}};
        st.isVolatile = regOuts[0].vol;
        emit(st);
    } else if (regOuts.size() > 1) {
        std::vector<TypePtr> retFields;
        for (auto& r : regOuts) retFields.push_back(r.ty);
        inst.type = Type::getStruct("asm_out", retFields);
        inst.resultVar = newVar();
        emit(inst);
        for (size_t i = 0; i < regOuts.size(); ++i) {
            std::string ev = newVar();
            emit(Instruction{Opcode::ExtractFat, ev, regOuts[i].ty, {Operand::var(inst.resultVar, inst.type), Operand::constInt((int64_t)i, Type::getI32())}});
            Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(ev, regOuts[i].ty), regOuts[i].addr}};
            st.isVolatile = regOuts[i].vol;
            emit(st);
        }
    } else {
        inst.type = Type::getVoid();
        emit(inst);
    }
}

void EIRBuilder::visit(UnionDecl*) {}

void EIRBuilder::visit(SizeofExpr* node) {
    TypePtr ty = nullptr;
    if (!node->typeName.empty() && lookupVar(node->typeName)) {
        ty = lookupVarType(node->typeName);   // `sizeof(x)` of a variable the parser kept as a name
    } else if (!node->typeName.empty()) {
        std::string actualType = node->typeName;
        if (!typeParamOverride.empty()) {
            actualType = substType(actualType, typeParamOverride);
        }
        ty = toEirType(actualType);
    } else if (node->operand) {
        ty = getExprEirType(node->operand);
    }
    size_t sz = 0;
    if (ty) {
        if (ty->isStruct()) ensureStructLayout(ty->nominalName());
        sz = getTypeSize(ty, module.get());
        if (ty->isStruct() && module) {
            size_t ss = module->getStructSize(ty->nominalName());
            if (ss) sz = ss;
        }
    }
    if (sz == 0) sz = 8;
    lastValue = Operand::constInt(static_cast<int64_t>(sz), Type::getI64());
    lastLValue = lastValue;
}

void EIRBuilder::visit(ThreadCreateExpr* node) {
    Operand worker = evaluateExpr(node->worker);
    std::string handleVar = newVar() + ".thr_handle";
    bool owned = nodeCast<LambdaExpr>(node->worker.get()) != nullptr;
    emit(Instruction{Opcode::Call, handleVar, Type::getPtr(), {
        Operand::global("__eir_thread_create", Type::getPtr()),
        worker,
        Operand::constInt(owned ? 1 : 0, Type::getI32())
    }});
    lastValue = Operand::var(handleVar, Type::getPtr());
    lastLValue = lastValue;
}

void EIRBuilder::visit(FreeClosureExpr* node) {
    Operand closureOp = evaluateExpr(node->closure);
    std::string envPtr = newVar() + ".env";
    emit(Instruction{Opcode::ExtractFat, envPtr, Type::getPtr(), {closureOp, Operand::constInt(1, Type::getI32())}});
    emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("free", Type::getPtr()), Operand::var(envPtr, Type::getPtr())}});
    lastValue = Operand::var("", Type::getVoid());
}

void EIRBuilder::visit(AwaitExpr*) {}

void EIRBuilder::visit(ThreadJoinStmt* node) {
    Operand tid = evaluateExpr(node->tid);
    emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("__eir_thread_join", Type::getVoid()), tid}});
}
std::string EIRBuilder::exceptionTypeName(const std::string& raw) const {
    std::string t = tyq::strip(raw);
    if (!t.empty() && t[0] == '?') t = t.substr(1);
    for (const char* tag : {"struct:", "interface:"}) {
        size_t p;
        while ((p = t.find(tag)) != std::string::npos) t.erase(p, std::string(tag).size());
    }
    if (t.find('<') != std::string::npos) t = mangleTemplate(t);
    if (t == "int32") t = "int";
    if (t == "uint32") t = "uint";
    return t.empty() ? "unknown" : t;
}

void EIRBuilder::emitDeferPad(Stmt* body) {
    if (!currentFunction || !currentBlock) return;
    BasicBlock* savedBB = currentBlock;
    BasicBlock* pad = createBlock("defer.lpad");
    setBlock(pad);

    std::string exVar = newVar();
    emit(Instruction{Opcode::LandingPad, exVar, Type::getPtr(), {}});
    Operand exObj = Operand::var(exVar, Type::getPtr());

    if (body) {
        body->accept(this);
    }

    if (currentBlock && !currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Throw, "", Type::getVoid(), {exObj}});
    }

    setBlock(savedBB);
    currentUnwindTarget = pad;
    currentHandlerDepth++;
    emit(Instruction{Opcode::PushHandler, "", Type::getVoid(), {Operand::block(pad->name)}});
}

void EIRBuilder::visit(ThrowStmt* node) {
    if (!node->value) return;

    Operand val = evaluateExpr(node->value);
    Operand exPtr;

    if (node->rethrowCaptured) {
        exPtr = val;
    } else {
        // In a generic instance the shared AST's stamp is some instance's: the thrown type is
        // this instance's.
        std::string thrownType = typeParamOverride.empty() ? node->valueType : "";
        if (thrownType.empty()) thrownType = eskiuSpelling(val.type ? val.type : getExprEirType(node->value));
        thrownType = exceptionTypeName(thrownType);

        size_t paySize = 8;
        if (val.type) {
            if (val.type->isStruct() && module) {
                paySize = module->getStructSize(val.type->nominalName());
            } else {
                paySize = getTypeSize(val.type, module.get());
            }
        }
        if (paySize == 0) paySize = 8;

        Operand typeStr = Operand::constString(thrownType);
        std::string exVar = newVar();
        emit(Instruction{Opcode::Call, exVar, Type::getPtr(), {
            Operand::global("__eir_alloc_exception", Type::getPtr()),
            Operand::constInt(static_cast<int64_t>(16 + paySize), Type::getI64()),
            typeStr
        }});
        exPtr = Operand::var(exVar, Type::getPtr());

        std::string slotVar = newVar();
        emit(Instruction{Opcode::OffsetPtr, slotVar, Type::getPtr(), {
            exPtr,
            Operand::constInt(16, Type::getI64())
        }});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {
            val,
            Operand::var(slotVar, Type::getPtr())
        }});
    }

    emit(Instruction{Opcode::Throw, "", Type::getVoid(), {exPtr}});
}

void EIRBuilder::visit(TryStmt* node) {
    if (!currentFunction) return;

    BasicBlock* lpadBB     = createBlock("try.lpad");
    BasicBlock* dispatchBB = createBlock("try.dispatch");
    BasicBlock* finallyBB  = createBlock("try.finally");
    BasicBlock* doneBB     = createBlock("try.done");

    BasicBlock* savedUnwind = currentUnwindTarget;
    currentUnwindTarget = lpadBB;
    size_t savedHandlerDepth = currentHandlerDepth;

    // Track finally in cleanupScopes for early return/break
    cleanupScopes.emplace_back();
    if (node->finally && !node->unwindOnly) {
        cleanupScopes.back().push_back({node->finally, false});
    }

    // Push try handler
    currentHandlerDepth++;
    emit(Instruction{Opcode::PushHandler, "", Type::getVoid(), {Operand::block(lpadBB->name)}});

    if (node->body) {
        node->body->accept(this);
    }

    // Normal exit of try body
    if (currentBlock && !currentBlock->hasTerminator()) {
        while (currentHandlerDepth > savedHandlerDepth) {
            emit(Instruction{Opcode::PopHandler, "", Type::getVoid(), {}});
            currentHandlerDepth--;
        }
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(finallyBB->name)}});
    }

    cleanupScopes.pop_back();
    currentUnwindTarget = savedUnwind;
    currentHandlerDepth = savedHandlerDepth;

    // --- Landing Pad ---
    setBlock(lpadBB);
    std::string exVar = newVar();
    emit(Instruction{Opcode::LandingPad, exVar, Type::getPtr(), {}});
    Operand exObj = Operand::var(exVar, Type::getPtr());
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(dispatchBB->name)}});

    // --- Catch Dispatch ---
    setBlock(dispatchBB);

    std::string typeSlotVar = newVar();
    emit(Instruction{Opcode::OffsetPtr, typeSlotVar, Type::getPtr(), {exObj, Operand::constInt(8, Type::getI64())}});
    std::string exTypeVar = newVar();
    emit(Instruction{Opcode::Load, exTypeVar, Type::getPtr(Type::getU8()), {Operand::var(typeSlotVar, Type::getPtr())}});
    Operand exType = Operand::var(exTypeVar, Type::getPtr(Type::getU8()));

    for (size_t i = 0; i < node->catches.size(); ++i) {
        auto& c = node->catches[i];
        bool isCatchAll = c.captureAll || c.type.empty() || c.type == "_" || c.name == "_";

        BasicBlock* handlerBB = createBlock("catch." + (c.type.empty() ? "any" : c.type));
        BasicBlock* nextClauseBB = isCatchAll ? nullptr : createBlock("catch.next");

        if (isCatchAll) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(handlerBB->name)}});
        } else {
            std::string cTypeName = exceptionTypeName(typeParamOverride.empty() ? c.type : substType(c.type, typeParamOverride));
            Operand cTypeStr = Operand::constString(cTypeName);
            std::string cmpVar = newVar();
            emit(Instruction{Opcode::Call, cmpVar, Type::getI32(), {
                Operand::global("strcmp", Type::getI32()),
                exType,
                cTypeStr
            }});
            std::string matchVar = newVar();
            Instruction cmpInst{Opcode::ICmp, matchVar, Type::getI1(), {
                Operand::var(cmpVar, Type::getI32()),
                Operand::constInt(0, Type::getI32())
            }};
            cmpInst.icmpCond = ICmpCond::Eq;
            emit(cmpInst);
            emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {
                Operand::var(matchVar, Type::getI1()),
                Operand::block(handlerBB->name),
                Operand::block(nextClauseBB->name)
            }});
        }

        // --- Handler Block ---
        setBlock(handlerBB);
        pushScope();

        // Register exception as active caught exception for exception_text()
        emit(Instruction{Opcode::Call, "", Type::getVoid(), {
            Operand::global("__eir_push_caught", Type::getVoid()),
            exObj
        }});
        std::string textSlot = newVar() + ".exc_text";
        entryAlloca(textSlot, Type::getArray(Type::getU8(), 512), 8);
        catchTextSlots.push_back(textSlot);
        emit(Instruction{Opcode::Call, "", Type::getVoid(), {
            Operand::global("__eir_clear_exception", Type::getVoid())
        }});

        if (c.captureAll && !c.name.empty()) {
            // The async lowering's capturing catch-all names the exception object itself,
            // which a later rethrow throws again or __cxa_free_exception releases.
            std::string localSlot = newVar();
            emit(Instruction{Opcode::Alloca, localSlot, Type::getPtr(), {}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {exObj, Operand::var(localSlot, Type::getPtr(Type::getPtr()))}});
            declareVar(c.name, Operand::var(localSlot, Type::getPtr(Type::getPtr())), Type::getPtr());
        }
        if (!isCatchAll && !c.name.empty()) {
            TypePtr catchEirTy = toEirType(typeParamOverride.empty() ? c.type : substType(c.type, typeParamOverride));
            std::string paySlotVar = newVar();
            emit(Instruction{Opcode::OffsetPtr, paySlotVar, Type::getPtr(), {exObj, Operand::constInt(16, Type::getI64())}});
            std::string payValVar = newVar();
            emit(Instruction{Opcode::Load, payValVar, catchEirTy, {Operand::var(paySlotVar, Type::getPtr())}});

            std::string localSlot = newVar();
            emit(Instruction{Opcode::Alloca, localSlot, catchEirTy, {}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {
                Operand::var(payValVar, catchEirTy),
                Operand::var(localSlot, Type::getPtr(catchEirTy))
            }});
            declareVar(c.name, Operand::var(localSlot, Type::getPtr(catchEirTy)), catchEirTy);
        }

        // Finally also runs on early return/break from catch body
        cleanupScopes.emplace_back();
        size_t catchStartHandler = currentHandlerDepth;
        BasicBlock* savedCatchUnwind = currentUnwindTarget;
        if (node->finally && !node->unwindOnly) {
            cleanupScopes.back().push_back({node->finally, false});
            emitDeferPad(node->finally.get());
        }

        if (c.body) {
            c.body->accept(this);
        }

        if (currentBlock && !currentBlock->hasTerminator()) {
            while (currentHandlerDepth > catchStartHandler) {
                emit(Instruction{Opcode::PopHandler, "", Type::getVoid(), {}});
                currentHandlerDepth--;
            }
            emit(Instruction{Opcode::Call, "", Type::getVoid(), {
                Operand::global("__eir_pop_caught", Type::getVoid())
            }});
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(finallyBB->name)}});
        }

        currentUnwindTarget = savedCatchUnwind;
        catchTextSlots.pop_back();
        cleanupScopes.pop_back();
        popScope();

        if (nextClauseBB) {
            setBlock(nextClauseBB);
        }
    }

    // --- Unhandled path (no catch matched, or no catch clauses) ---
    if (currentBlock && !currentBlock->hasTerminator()) {
        if (node->finally) {
            node->finally->accept(this);
        }
        if (currentBlock && !currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Throw, "", Type::getVoid(), {exObj}});
        }
    }

    // --- Finally Block (normal execution path) ---
    setBlock(finallyBB);
    if (node->finally && !node->unwindOnly) {
        node->finally->accept(this);
    }
    if (currentBlock && !currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(doneBB->name)}});
    }

    currentUnwindTarget = savedUnwind;
    currentHandlerDepth = savedHandlerDepth;

    setBlock(doneBB);
}

void EIRBuilder::visit(DeferStmt* node) {
    if (node->body && !cleanupScopes.empty()) {
        // The body names the variables visible here, even when it runs where a later
        // declaration shadows one of them.
        cleanupScopes.back().push_back({node->body, node->isErr, scopeStack, varTypeStack});
        if (!node->isErr && currentFunction) {
            emitDeferPad(node->body.get());
        }
    }
}

} // namespace eir
