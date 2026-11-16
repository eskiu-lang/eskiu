// codegen_try.cpp: exception text, throw, try/catch, threads, await and inline asm (split from codegen_closure.cpp).
#include "codegen.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "llvm/IR/InlineAsm.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"
#include "codegen_eh.h"

static bool hasExceptionText(Expr* e);
static bool hasExceptionText(Stmt* s) {
    if (!s) return false;
    if (auto* b = nodeCast<BlockStmt>(s)) {
        for (auto& st : b->items) {
            if (std::holds_alternative<StmtPtr>(st)) {
                if (hasExceptionText(std::get<StmtPtr>(st).get())) return true;
            } else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(st).get())) {
                if (hasExceptionText(vd->initializer.get())) return true;
            }
        }
        return false;
    }
    if (auto* i = nodeCast<IfStmt>(s))
        return hasExceptionText(i->condition.get()) || hasExceptionText(i->thenBranch.get()) || hasExceptionText(i->elseBranch.get());
    if (auto* w = nodeCast<WhileStmt>(s))
        return hasExceptionText(w->condition.get()) || hasExceptionText(w->body.get());
    if (auto* dw = nodeCast<DoWhileStmt>(s))
        return hasExceptionText(dw->body.get()) || hasExceptionText(dw->condition.get());
    if (auto* f = nodeCast<ForStmt>(s))
        return hasExceptionText(f->init.get()) || hasExceptionText(f->condition.get()) || hasExceptionText(f->step.get()) || hasExceptionText(f->body.get());
    if (auto* fi = nodeCast<ForInStmt>(s))
        return hasExceptionText(fi->iterable.get()) || hasExceptionText(fi->body.get());
    if (auto* r = nodeCast<ReturnStmt>(s)) return hasExceptionText(r->value.get());
    if (auto* sw = nodeCast<SwitchStmt>(s)) {
        if (hasExceptionText(sw->subject.get())) return true;
        for (auto& c : sw->cases) {
            if (hasExceptionText(c.value.get())) return true;
            for (auto& it : c.stmts) {
                if (std::holds_alternative<StmtPtr>(it)) {
                    if (hasExceptionText(std::get<StmtPtr>(it).get())) return true;
                } else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) {
                    if (hasExceptionText(vd->initializer.get())) return true;
                }
            }
        }
        return false;
    }
    if (auto* m = nodeCast<MatchStmt>(s)) {
        if (hasExceptionText(m->subject.get())) return true;
        for (auto& a : m->arms) if (hasExceptionText(a.body.get())) return true;
        return false;
    }
    if (auto* th = nodeCast<ThrowStmt>(s)) return hasExceptionText(th->value.get());
    if (auto* tr = nodeCast<TryStmt>(s))
        return hasExceptionText(tr->body.get()) || hasExceptionText(tr->finally.get());
    if (auto* d = nodeCast<DeferStmt>(s)) return hasExceptionText(d->body.get());
    if (auto* tj = nodeCast<ThreadJoinStmt>(s)) return hasExceptionText(tj->tid.get());
    if (auto* a = nodeCast<AsmStmt>(s)) {
        for (auto& out : a->outputs) if (hasExceptionText(out.second.get())) return true;
        for (auto& in : a->inputs) if (hasExceptionText(in.second.get())) return true;
        return false;
    }
    if (auto* es = nodeCast<ExprStmt>(s)) return hasExceptionText(es->expr.get());
    return false;
}
static bool hasExceptionText(Expr* e) {
    if (!e || nodeCast<LambdaExpr>(e)) return false;
    if (auto* c = nodeCast<CallExpr>(e))
        if (auto* id = nodeCast<IdentExpr>(c->callee.get());
            id && ((id->name == "exception_text" && c->args.empty()) || (id->name == "__exception_text_into" && c->args.size() == 1)))
            return true;
    bool found = false;
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& ch) { if (!found && hasExceptionText(ch.get())) found = true; });
    return found;
}

void CodeGen::ensureEHRuntime() {
    ensureEHDecls(module.get(), *context, ehPersonalityName());
}

void CodeGen::visit(ThrowStmt* node) {
    ensureEHDecls(module.get(), *context, ehPersonalityName());
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    llvm::Type* i64   = llvm::Type::getInt64Ty(*context);

    // EskiuEx: { int64 size, ptr type_name, payload } where the payload is the thrown
    // value stored by its own type at offset 16 (a double, an int64 or a whole struct
    // keep their bits; a catch loads the same type back) and `size` is the object's
    // byte size (a capturing catch copies that many bytes).
    llvm::Value* val = evaluateExpr(node->value);
    llvm::Value* exPtr = val;
    if (!node->rethrowCaptured) {
        llvm::Type* payTy = val->getType();
        uint64_t paySize = module->getDataLayout().getTypeAllocSize(payTy);
        llvm::Type* szTy = module->getDataLayout().getIntPtrType(*context);   // size_t
        llvm::Function* allocEx = getOrDeclareFunc("__cxa_allocate_exception",
            ptrTy, {szTy});
        exPtr = builder->CreateCall(allocEx,
            {llvm::ConstantInt::get(szTy, 16 + paySize)}, "ex.alloc");
        builder->CreateStore(llvm::ConstantInt::get(i64, 16 + paySize), exPtr);
        auto* paySlot = builder->CreateConstGEP1_64(
            llvm::Type::getInt8Ty(*context), exPtr, 16, "ex.pay.slot");
        builder->CreateStore(val, paySlot);
    }

    // The static type of the thrown value. The type checker stamps it on the node,
    // but a generic body is shared by its instances, so there it is derived per
    // instance from the value's type under the active substitutions.
    if (!node->rethrowCaptured) {
        std::string thrownType = node->valueType;
        if (thrownType.empty() || !typeParamOverride.empty()) {
            std::string d = getExprEskiuType(node->value);
            if (!typeParamOverride.empty()) d = substType(d, typeParamOverride);
            if (!d.empty() && d != "unknown") thrownType = d;
        }
        thrownType = exceptionTypeName(thrownType);

        // Store type name at offset 8
        auto* typeStr = builder->CreateGlobalString(thrownType, ".ex.tname");
        auto* typeSlot = builder->CreateConstGEP1_64(
            llvm::Type::getInt8Ty(*context), exPtr, 8, "ex.type.slot");
        builder->CreateStore(typeStr, typeSlot);
    }

    // __cxa_throw(ex, _ZTIPv, null)
    // Must be an invoke when inside a try body so the local landingpad fires.
    llvm::Function* cxaThrow = getOrDeclareFunc("__cxa_throw",
        llvm::Type::getVoidTy(*context), {ptrTy, ptrTy, ptrTy});
    // Do NOT mark noreturn — it prevents invoke from propagating the exception
    llvm::Value* typeInfo = module->getNamedGlobal("_ZTIPv");
    llvm::Value* nullPtr = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(ptrTy));
    std::vector<llvm::Value*> throwArgs = {exPtr, typeInfo, nullPtr};

    if (unwindTarget) {
        // Inside a try — use invoke so the landingpad catches it
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        auto* unreachBB = llvm::BasicBlock::Create(*context, "throw.unreach", fn);
        builder->CreateInvoke(cxaThrow->getFunctionType(), cxaThrow,
            unreachBB, unwindTarget, throwArgs);
        builder->SetInsertPoint(unreachBB);
    } else {
        builder->CreateCall(cxaThrow, throwArgs);
    }
    builder->CreateUnreachable();
}

void CodeGen::visit(TryStmt* node) {
    ensureEHDecls(module.get(), *context, ehPersonalityName());
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    llvm::Type* i64   = llvm::Type::getInt64Ty(*context);
    llvm::Type* i32   = llvm::Type::getInt32Ty(*context);

    llvm::Function* fn = builder->GetInsertBlock()->getParent();

    // Set personality on the enclosing function if not already set
    if (!fn->hasPersonalityFn()) {
        auto* pers = module->getFunction(ehPersonalityName());
        fn->setPersonalityFn(pers);
    }

    llvm::BasicBlock* lpadBB    = llvm::BasicBlock::Create(*context, "try.lpad",    fn);
    llvm::BasicBlock* dispatchBB = llvm::BasicBlock::Create(*context, "try.dispatch", fn);
    llvm::BasicBlock* finallyBB = llvm::BasicBlock::Create(*context, "try.finally", fn);
    llvm::BasicBlock* doneBB    = llvm::BasicBlock::Create(*context, "try.done",    fn);

    // ── try body — all calls become invokes ───────────────────────────────
    llvm::BasicBlock* savedUnwind = unwindTarget;
    unwindTarget = lpadBB;
    // Register `finally` as a cleanup for the body's duration, so an early exit
    // (return/break/continue/`?`) from inside the body runs it — the normal
    // fall-through and exception paths still emit it via finallyBB / the landingpad
    // below, so we pop this frame WITHOUT running it here.
    cleanupScopes.emplace_back();
    // (An async lowering wrapper's `finally` runs only on the exceptional path.)
    if (node->finally && !node->unwindOnly) cleanupScopes.back().push_back(makeCleanup(node->finally.get(), /*isErr=*/false));
    // A defer in the body joins the catch dispatch from its own landingpad after running
    // the body's pending defers (see emitDeferPad).
    tryStack.push_back({cleanupScopes.size(), dispatchBB, {}});
    if (node->body) node->body->accept(this);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming = std::move(tryStack.back().incoming);
    tryStack.pop_back();
    popCleanupFrame();
    unwindTarget = savedUnwind;
    if (!hasTerminator(builder->GetInsertBlock()))
        builder->CreateBr(finallyBB);

    // ── landingpad ────────────────────────────────────────────────────────
    builder->SetInsertPoint(lpadBB);
    auto* lp = builder->CreateLandingPad(
        llvm::StructType::get(*context, {ptrTy, i32}), 1, "lpad");
    // catch i8* null = catch-all
    lp->addClause(llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(ptrTy)));

    incoming.insert(incoming.begin(), {builder->CreateExtractValue(lp, {0}, "ex.ptr"), lpadBB});
    builder->CreateBr(dispatchBB);

    // ── catch dispatch (from the landingpad or a body defer's pad) ─────────
    builder->SetInsertPoint(dispatchBB);
    llvm::Value* exObjPtr = incoming.front().first;
    if (incoming.size() > 1) {
        llvm::PHINode* phi = builder->CreatePHI(ptrTy, incoming.size(), "ex.obj");
        for (auto& in : incoming) phi->addIncoming(in.first, in.second);
        exObjPtr = phi;
    }

    // __cxa_begin_catch(ex) → pointer to our EskiuEx
    llvm::Function* beginCatch = getOrDeclareFunc("__cxa_begin_catch",
        ptrTy, {ptrTy});
    llvm::Value* exData = builder->CreateCall(beginCatch, {exObjPtr}, "ex.data");
    // emscripten's runtime hands the thrown object itself to the landingpad, and its
    // __cxa_begin_catch returns that pointer adjusted for the `void*` typeinfo it was
    // thrown with (the first word of the object), so the object is the landingpad's.
    if (llvm::Triple(module->getTargetTriple()).isOSEmscripten()) exData = exObjPtr;

    // Read the type name (offset 8)
    auto* typeSlot = builder->CreateConstGEP1_64(
        llvm::Type::getInt8Ty(*context), exData, 8, "ex.tslot");
    llvm::Value* exType = builder->CreateLoad(ptrTy,
        builder->CreateBitCast(typeSlot, ptrTy), "ex.type");

    // ── catch clauses ─────────────────────────────────────────────────────
    llvm::Function* endCatch  = getOrDeclareFunc("__cxa_end_catch",
        llvm::Type::getVoidTy(*context), {});
    llvm::Function* strcmpFn  = getOrDeclareFunc("strcmp", i32, {ptrTy, ptrTy});

    for (auto& c : node->catches) {
        if (c.captureAll) {
            // Any exception: copy the object into a fresh, unthrown exception (its size
            // is at offset 0) for a later rethrow, and release the caught one.
            pushScope();
            llvm::Value* exSize = builder->CreateLoad(i64, exData, "ex.size");
            llvm::Type* szTy = module->getDataLayout().getIntPtrType(*context);   // size_t
            llvm::Function* allocEx = getOrDeclareFunc("__cxa_allocate_exception", ptrTy, {szTy});
            llvm::Value* copy = builder->CreateCall(allocEx, {builder->CreateZExtOrTrunc(exSize, szTy)}, "ex.copy");
            builder->CreateMemCpy(copy, llvm::MaybeAlign(8), exData, llvm::MaybeAlign(8), exSize);
            auto* slot = entryAlloca(ptrTy, nullptr, c.name);
            builder->CreateStore(copy, slot);
            defineSymbol(c.name, slot);
            defineVarType(c.name, c.type);
            builder->CreateCall(endCatch, {});
            if (c.body) c.body->accept(this);
            popScope();
            if (!hasTerminator(builder->GetInsertBlock()))
                builder->CreateBr(finallyBB);
            builder->SetInsertPoint(llvm::BasicBlock::Create(*context, "catch.after", fn));
            continue;
        }
        // A generic body's catch type names the type params: match per instance.
        const std::string cType = c.type.empty() ? "" : (typeParamOverride.empty() ? c.type : substType(c.type, typeParamOverride));
        auto* nextBB = llvm::BasicBlock::Create(*context, "catch.next", fn);
        if (!cType.empty()) {
            auto* cTypeStr  = builder->CreateGlobalString(exceptionTypeName(cType), ".catch.t");
            llvm::Value* cmp   = builder->CreateCall(strcmpFn, {exType, cTypeStr}, "tcmp");
            llvm::Value* match = builder->CreateICmpEQ(cmp,
                llvm::ConstantInt::get(i32, 0), "tmatch");

            auto* handlerBB = llvm::BasicBlock::Create(*context, "catch." + c.type, fn);
            builder->CreateCondBr(match, handlerBB, nextBB);
            builder->SetInsertPoint(handlerBB);
        }
        pushScope();

        if (!cType.empty()) {
            // Load the payload (offset 16) by the catch type, which the name match made
            // the thrown value's own type.
            llvm::Type*  catchTy = getTypeFromString(cType);
            auto* paySlot = builder->CreateConstGEP1_64(
                llvm::Type::getInt8Ty(*context), exData, 16, "ex.pay");
            llvm::Value* catchVal = builder->CreateLoad(catchTy, paySlot, "ex.val");
            auto* catchAlloca = entryAlloca(catchTy, nullptr, c.name);
            builder->CreateStore(catchVal, catchAlloca);
            defineSymbol(c.name, catchAlloca);
            defineVarType(c.name, cType);
        }
        bool hasExText = hasExceptionText(c.body.get());
        if (hasExText) {
            // The text lives in this handler's own buffer, valid until the catch block ends.
            auto* textBuf = entryAlloca(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), 512), nullptr, "ex.text.buf");
            llvm::Value* txt = builder->CreateCall(ensureExceptionTextFn(), {exData, textBuf}, "ex.text");
            auto* txtSlot = entryAlloca(ptrTy, nullptr, "ex.text.slot");
            builder->CreateStore(txt, txtSlot);
            catchTextSlots.push_back(txtSlot);
        }
        // The payload is copied out, so the exception object can be released before the
        // handler runs; then an early exit (return/break/continue) from the handler has
        // nothing left to end.
        builder->CreateCall(endCatch, {});

        // `finally` also runs when the handler leaves early (return/break/continue).
        // A throw out of the handler runs it too (then the new exception propagates): the
        // handler's calls unwind to a pad that runs the pending cleanups, as after a defer.
        cleanupScopes.emplace_back();
        if (node->finally && !node->unwindOnly) {
            cleanupScopes.back().push_back(makeCleanup(node->finally.get(), /*isErr=*/false));
            emitDeferPad();
        }
        if (c.body) c.body->accept(this);
        if (hasExText) catchTextSlots.pop_back();
        popCleanupFrame();
        popScope();

        if (!hasTerminator(builder->GetInsertBlock()))
            builder->CreateBr(finallyBB);

        builder->SetInsertPoint(nextBB);
    }

    // No catch clause matched (or there were none, e.g. a catch-less try/finally):
    // run the finally body on this exceptional path too, then re-raise the in-flight
    // exception with __cxa_rethrow. (end_catch + resume here double-freed the
    // exception and aborted; and the finally was skipped entirely.) The rethrow is an
    // invoke when an enclosing try can catch it, so its landingpad fires.
    if (!hasTerminator(builder->GetInsertBlock())) {
        if (node->finally) node->finally->accept(this);
        if (!hasTerminator(builder->GetInsertBlock())) {
            llvm::Function* rethrow = getOrDeclareFunc("__cxa_rethrow",
                llvm::Type::getVoidTy(*context), {});
            if (savedUnwind) {
                auto* rethrowUnreach = llvm::BasicBlock::Create(*context, "rethrow.unreach", fn);
                builder->CreateInvoke(rethrow->getFunctionType(), rethrow,
                                      rethrowUnreach, savedUnwind, {});
                builder->SetInsertPoint(rethrowUnreach);
            } else {
                builder->CreateCall(rethrow, {});
            }
            builder->CreateUnreachable();
        }
    }

    // ── finally (normal, non-exceptional path) ─────────────────────────────
    builder->SetInsertPoint(finallyBB);
    if (node->finally && !node->unwindOnly) node->finally->accept(this);
    if (!hasTerminator(builder->GetInsertBlock()))
        builder->CreateBr(doneBB);

    builder->SetInsertPoint(doneBB);
}

void CodeGen::visit(AwaitExpr* node) {
    // The async transform rewrites `async fn`/`await` into a state machine before
    // codegen; reaching here means the transform has not run on this code.
    (void)node;
    throw std::runtime_error("internal error: an `await` survived to codegen — "
                             "the async state-machine transform did not run");
}

void CodeGen::visit(FreeClosureExpr* node) {
    // A closure is a fat pointer {fn_ptr, env_ptr}. Free its heap env (slot 1).
    // A non-capturing closure has a null env; free(null) is a safe no-op.
    llvm::Value* fat = evaluateExpr(node->closure);
    llvm::Value* env = builder->CreateExtractValue(fat, 1, "clos.env");
    llvm::Function* freeFn = getOrDeclareFunc(
        freestanding ? "esk_free" : "free", llvm::Type::getVoidTy(*context),
        {llvm::PointerType::get(*context, 0)}, false);
    builder->CreateCall(freeFn, {env});
    exprValueStack.push(llvm::UndefValue::get(llvm::Type::getVoidTy(*context)));
}

// `ptr __eskiu_thread_owned(ptr pack)`: the start routine of a thread that owns its
// closure. `pack` is a malloc'd {fn, env}; run fn(env), then free env and pack.
llvm::Function* CodeGen::ownedThreadTrampoline() {
    const char* name = "__eskiu_thread_owned";
    if (llvm::Function* f = module->getFunction(name)) return f;
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    auto* fty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* f = llvm::Function::Create(fty, llvm::Function::InternalLinkage, name, module.get());
    llvm::BasicBlock* savedBB = builder->GetInsertBlock();
    llvm::BasicBlock::iterator savedPt = builder->GetInsertPoint();
    builder->SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", f));
    llvm::StructType* packTy = llvm::StructType::get(*context, {ptrTy, ptrTy});
    llvm::Value* pack = f->getArg(0);
    llvm::Value* fn  = builder->CreateLoad(ptrTy, builder->CreateStructGEP(packTy, pack, 0), "fn");
    llvm::Value* env = builder->CreateLoad(ptrTy, builder->CreateStructGEP(packTy, pack, 1), "env");
    llvm::Function* freeFn = getOrDeclareFunc("free", llvm::Type::getVoidTy(*context), {ptrTy}, false);
    builder->CreateCall(freeFn, {pack});
    builder->CreateCall(llvm::FunctionType::get(llvm::Type::getVoidTy(*context), {ptrTy}, false), fn, {env});
    builder->CreateCall(freeFn, {env});
    builder->CreateRet(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)));
    if (savedBB) builder->SetInsertPoint(savedBB, savedPt);
    return f;
}

void CodeGen::visit(ThreadCreateExpr* node) {
    // Evaluate the closure — a fat pointer {fn_ptr, env_ptr}
    llvm::Value* fatPtr = evaluateExpr(node->worker);

    // Extract fn_ptr and env_ptr
    llvm::Value* fnPtr  = builder->CreateExtractValue(fatPtr, {0}, "thr.fn");
    llvm::Value* envPtr = builder->CreateExtractValue(fatPtr, {1}, "thr.env");

    // pthread_t is typically *void; alloca space for the tid
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    llvm::Value* tidAlloca = entryAlloca(ptrTy, nullptr, "thr.tid");

    // pthread_create(pthread_t* tid, null, fn_ptr, env_ptr)
    llvm::Function* pthreadCreate = getOrDeclareFunc("pthread_create",
        llvm::Type::getInt32Ty(*context),
        {ptrTy, ptrTy, ptrTy, ptrTy});

    // A lambda written in the call (`thread_create(void() { ... })`) has no other owner,
    // so the thread owns it: start it through a trampoline that frees its env once the
    // body returns. A closure value passed in stays its owner's (free_closure after
    // thread_join), since the same closure may start several threads.
    if (nodeCast<LambdaExpr>(node->worker.get())) {
        llvm::Type* szTy = module->getDataLayout().getIntPtrType(*context);
        llvm::Function* mallocFn = getOrDeclareFunc("malloc", ptrTy, {szTy}, false);
        llvm::Value* pack = builder->CreateCall(mallocFn,
            {llvm::ConstantInt::get(szTy, 2 * module->getDataLayout().getPointerSize())},
            "thr.pack");
        llvm::StructType* packTy = llvm::StructType::get(*context, {ptrTy, ptrTy});
        builder->CreateStore(fnPtr, builder->CreateStructGEP(packTy, pack, 0));
        builder->CreateStore(envPtr, builder->CreateStructGEP(packTy, pack, 1));
        fnPtr = ownedThreadTrampoline();
        envPtr = pack;
    }

    builder->CreateCall(pthreadCreate, {
        tidAlloca,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
        fnPtr,
        envPtr
    });

    // Return the thread handle (tid value)
    exprValueStack.push(builder->CreateLoad(ptrTy, tidAlloca, "thr.handle"));
}

void CodeGen::visit(ThreadJoinStmt* node) {
    llvm::Value* tid = evaluateExpr(node->tid);
    llvm::Type*  ptrTy = llvm::PointerType::get(*context, 0);
    llvm::Function* pthreadJoin = getOrDeclareFunc("pthread_join",
        llvm::Type::getInt32Ty(*context), {ptrTy, ptrTy});
    builder->CreateCall(pthreadJoin, {
        tid,
        llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy))
    });
}

// On x86 a GCC single-register constraint letter names one register; LLVM wants it
// spelled out, as clang emits it: `a` is `{ax}`, `Nd` is `N{dx}`. Modifiers, other
// letters, digits and `{...}` (also a clobber's `~{...}`) stay as written.
static std::string x86AsmConstraint(const std::string& c) {
    std::string out;
    for (size_t i = 0; i < c.size(); ++i) {
        char ch = c[i];
        if (ch == '{') {
            size_t e = c.find('}', i);
            if (e == std::string::npos) e = c.size() - 1;
            out += c.substr(i, e - i + 1);
            i = e;
            continue;
        }
        switch (ch) {
            case 'a': out += "{ax}"; break;
            case 'b': out += "{bx}"; break;
            case 'c': out += "{cx}"; break;
            case 'd': out += "{dx}"; break;
            case 'S': out += "{si}"; break;
            case 'D': out += "{di}"; break;
            default: out += ch;
        }
    }
    return out;
}

// Extended asm (GCC syntax) as LLVM inline asm, numbered like clang: the outputs first
// (`$0`...), then the inputs. A register output (`=r`, `=&r`) is a result of the call,
// stored into its lvalue afterwards (several make a struct result); a memory output
// (`=m`) is an indirect `=*m` operand taking the lvalue's address. A read-write `+r`
// also feeds the lvalue's value in through an input tied to the output (`"0"`), and
// `+m` passes the address again as a `*m` input; those follow the explicit inputs.
void CodeGen::visit(AsmStmt* node) {
    std::vector<llvm::Value*> argVals;
    std::vector<llvm::Type*> elemTypes;       // per argument: its elementtype (indirect), or null
    std::string constraints;
    CAbiTarget tgt = cabiTarget();
    bool x86 = tgt == CAbiTarget::SysV || tgt == CAbiTarget::Win64 || tgt == CAbiTarget::X86;
    auto addConstraint = [&](const std::string& c) {
        if (!constraints.empty()) constraints += ",";
        constraints += x86 ? x86AsmConstraint(c) : c;
    };
    struct RegOut { llvm::Value* addr; llvm::Type* ty; bool vol; };
    std::vector<RegOut> regOuts;
    struct Tied { std::string constraint; llvm::Value* val; llvm::Type* elem; };
    std::vector<Tied> tied;

    for (size_t i = 0; i < node->outputs.size(); ++i) {
        const std::string& c = node->outputs[i].first;
        const ExprPtr& e = node->outputs[i].second;
        bool rw = !c.empty() && c[0] == '+';
        std::string body = c.substr(1);
        llvm::Type* ty = getTypeFromString(getExprEskiuType(e));
        llvm::Value* addr = evaluateLValue(e);
        bool vol = volatileRooted(e.get());
        if (body == "m") {
            addConstraint("=*m");
            argVals.push_back(addr); elemTypes.push_back(ty);
            if (rw) tied.push_back({"*m", addr, ty});
        } else {
            addConstraint("=" + body);
            regOuts.push_back({addr, ty, vol});
            if (rw) {
                auto* cur = builder->CreateLoad(ty, addr);
                cur->setVolatile(vol);
                tied.push_back({std::to_string(i), cur, nullptr});
            }
        }
    }
    for (auto& [constraint, expr] : node->inputs) {
        argVals.push_back(evaluateExpr(expr)); elemTypes.push_back(nullptr);
        addConstraint(constraint);
    }
    for (auto& t : tied) {
        argVals.push_back(t.val); elemTypes.push_back(t.elem);
        addConstraint(t.constraint);
    }
    for (const auto& clob : node->clobbers) addConstraint("~{" + clob + "}");
    // sideeffect + alignstack are standard for kernel inline asm
    if (!constraints.empty()) constraints += ",~{dirflag},~{fpsr},~{flags}";

    std::vector<llvm::Type*> argTypes;
    for (auto* v : argVals) argTypes.push_back(v->getType());
    llvm::Type* retTy = llvm::Type::getVoidTy(*context);
    if (regOuts.size() == 1) retTy = regOuts[0].ty;
    else if (regOuts.size() > 1) {
        std::vector<llvm::Type*> fields;
        for (auto& r : regOuts) fields.push_back(r.ty);
        retTy = llvm::StructType::get(*context, fields);
    }

    auto* fty = llvm::FunctionType::get(retTy, argTypes, false);
    auto* iasm = llvm::InlineAsm::get(
        fty, node->asmString, constraints,
        /*hasSideEffects=*/true, /*isAlignStack=*/false,
        llvm::InlineAsm::AD_ATT);

    auto* call = builder->CreateCall(iasm, argVals);
    for (size_t i = 0; i < elemTypes.size(); ++i)
        if (elemTypes[i])
            call->addParamAttr((unsigned)i, llvm::Attribute::get(*context, llvm::Attribute::ElementType, elemTypes[i]));
    for (size_t i = 0; i < regOuts.size(); ++i) {
        llvm::Value* v = regOuts.size() == 1 ? (llvm::Value*)call : builder->CreateExtractValue(call, {(unsigned)i});
        builder->CreateStore(v, regOuts[i].addr)->setVolatile(regOuts[i].vol);
    }
}
