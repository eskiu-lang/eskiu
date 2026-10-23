#include "codegen.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "llvm/IR/InlineAsm.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

// CodeGen — closures/lambdas, exception handling (throw/try + invoke),
// threads, await, and inline asm.
// Part of the codegen_expr.cpp split; see codegen.h.

// Emit the lambda's underlying LLVM function (params: env* first, then the
// declared params) and compile its body. `envTy` is the capture-env struct type
// (null for a non-capturing lambda). Saves/restores the caller's insert point, so
// this is safe to call from a global (builder-less) context. Returns the function.
llvm::Function* CodeGen::emitLambdaFunction(LambdaExpr* node,
                                            const std::string& lambdaName,
                                            llvm::StructType* envTy) {
    bool hasCaptures = !node->captures.empty();
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);

    // ── Build lambda function: env* always first param ───────────────────
    std::vector<llvm::Type*> paramTypes = {ptrTy}; // env* (null if no captures)
    for (const auto& p : node->params)
        paramTypes.push_back(getTypeFromString(p.first));

    llvm::Type* retTy = getTypeFromString(node->returnType);
    llvm::FunctionType* fty = llvm::FunctionType::get(retTy, paramTypes, false);
    llvm::Function* func = llvm::Function::Create(
        fty, llvm::Function::InternalLinkage, lambdaName, module.get());
    // Its `return` converts to the declared type like a function's (an interface boxes).
    funcEskiuReturnType[lambdaName] = typeParamOverride.empty()
        ? node->returnType : substType(node->returnType, typeParamOverride);

    auto argIt = func->arg_begin();
    argIt->setName("env");
    llvm::Argument* envArg = &*argIt++;
    size_t i = 0;
    for (; argIt != func->arg_end(); ++argIt, ++i)
        argIt->setName(node->params[i].second);

    // ── Compile lambda body ───────────────────────────────────────────────
    llvm::Function* prevFunc     = currentFunction;
    llvm::Value*    prevSret     = currentSretParam;
    llvm::BasicBlock* prevInsert = builder->GetInsertBlock();

    llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entry);
    currentFunction  = func;
    currentSretParam = nullptr;
    // The lambda body is its own function: not the enclosing body's defers, loops or `try`.
    BodyContext bodyCtx(this);
    pushScope();

    // Expose captured variables by loading from env
    if (hasCaptures) {
        for (size_t ci = 0; ci < node->captures.size(); ++ci) {
            const auto& [capName, capType] = node->captures[ci];
            llvm::Type* capLLVMTy = getTypeFromString(capType);
            auto* capAlloca = entryAlloca(capLLVMTy, nullptr, capName);
            auto* gep = builder->CreateStructGEP(envTy, envArg, ci, capName + ".gep");
            auto* val = builder->CreateLoad(capLLVMTy, gep, capName + ".val");
            builder->CreateStore(val, capAlloca);
            defineSymbol(capName, capAlloca);
            // A capture of a generic instance's local has the source-form type
            // (`*__f_frame<T>`): its uses must see this instance's (`*__f_frame<P>`).
            defineVarType(capName, typeParamOverride.empty() ? capType
                                                             : substType(capType, typeParamOverride));
        }
    }

    // Define parameters
    i = 0;
    argIt = func->arg_begin();
    ++argIt; // skip env
    for (; argIt != func->arg_end(); ++argIt, ++i) {
        // Every parameter gets a stack slot, as in a named function: the body may
        // reassign it like a local, and a struct param has an address to GEP.
        auto* slot = entryAlloca(argIt->getType(), nullptr, node->params[i].second);
        builder->CreateStore(&*argIt, slot);
        defineSymbol(node->params[i].second, slot);
        defineVarType(node->params[i].second, node->params[i].first);
    }

    if (node->body) node->body->accept(this);
    if (!hasTerminator(builder->GetInsertBlock())) {
        if (retTy->isVoidTy()) builder->CreateRetVoid();
        else builder->CreateRet(llvm::Constant::getNullValue(retTy));
    }

    popScope();
    currentFunction  = prevFunc;
    currentSretParam = prevSret;
    if (prevInsert) builder->SetInsertPoint(prevInsert);
    return func;
}

void CodeGen::visit(LambdaExpr* node) {
    std::string lambdaName = "__lambda" + std::to_string(lambdaSeq++);
    bool hasCaptures = !node->captures.empty();

    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);

    // ── Build env struct type (one field per captured variable) ──────────
    llvm::StructType* envTy = nullptr;
    llvm::Value*      envAlloca = nullptr;
    if (hasCaptures) {
        std::vector<llvm::Type*> envFields;
        for (const auto& [name, type] : node->captures)
            envFields.push_back(getTypeFromString(type));
        envTy = llvm::StructType::create(*context, envFields,
                                         lambdaName + ".env");
        if (node->escapes) {
            // Escaping closure (returned, stored, or passed to an `escaping`
            // parameter): heap-allocate the env so it outlives this function.
            // Freed via free_closure (the async transform emits it at the owner
            // boundary; otherwise the owner frees it explicitly).
            uint64_t envSize = module->getDataLayout().getTypeAllocSize(envTy);
            // Under --freestanding there is no libc: use the program's esk_alloc, the
            // same allocator <mem> uses there.
            // malloc takes a size_t; esk_alloc an int64 (its Eskiu contract).
            llvm::Type* szTy = freestanding ? llvm::Type::getInt64Ty(*context)
                                            : module->getDataLayout().getIntPtrType(*context);
            llvm::Function* mallocFn = getOrDeclareFunc(
                freestanding ? "esk_alloc" : "malloc", ptrTy, {szTy}, false);
            envAlloca = builder->CreateCall(mallocFn,
                {llvm::ConstantInt::get(szTy, envSize)},
                lambdaName + ".env.heap");
        } else {
            // Non-escaping closure: the env dies with this frame — stack-allocate
            // it. Zero cost, no leak. (The common map/filter/apply case.)
            envAlloca = entryAlloca(envTy, nullptr, lambdaName + ".env");
        }
        for (size_t ci = 0; ci < node->captures.size(); ++ci) {
            llvm::Value* capturedVal = nullptr;
            llvm::Value* sym = lookupSymbol(node->captures[ci].first);
            if (sym) {
                // Load the current value from the outer alloca/variable
                if (llvm::isa<llvm::AllocaInst>(sym)) {
                    auto* alloca = llvm::cast<llvm::AllocaInst>(sym);
                    capturedVal = builder->CreateLoad(
                        alloca->getAllocatedType(), sym, node->captures[ci].first);
                } else {
                    capturedVal = sym;
                }
            }
            if (capturedVal) {
                auto* gep = builder->CreateStructGEP(envTy, envAlloca, ci);
                builder->CreateStore(capturedVal, gep);
            }
        }
    }

    llvm::Function* func = emitLambdaFunction(node, lambdaName, envTy);

    // ── Build fat pointer {fn_ptr, env_ptr} ──────────────────────────────
    llvm::StructType* fatTy = llvm::cast<llvm::StructType>(
        getTypeFromString("fn()->void")); // any fn type gives {ptr,ptr}
    llvm::Value* fatAlloca = entryAlloca(fatTy, nullptr, lambdaName + ".fat");
    auto* fnSlot  = builder->CreateStructGEP(fatTy, fatAlloca, 0);
    auto* envSlot = builder->CreateStructGEP(fatTy, fatAlloca, 1);
    builder->CreateStore(func, fnSlot);
    builder->CreateStore(
        hasCaptures ? (llvm::Value*)envAlloca
                    : llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)),
        envSlot);
    exprValueStack.push(builder->CreateLoad(fatTy, fatAlloca, lambdaName + ".fat.val"));
}

// ── Exception helpers (invoke/landingpad) ─────────────────────────────────

std::string CodeGen::ehPersonalityName() const {
    const std::string& tt = targetTriple;
    bool win;
    if (tt.empty()) {
        // Native build (no --target): follow the host, as main.cpp does for the OS macro.
#if defined(_WIN32)
        win = true;
#else
        win = false;
#endif
    } else {
        win = tt.find("windows") != std::string::npos ||
              tt.find("win32")   != std::string::npos ||
              tt.find("mingw")   != std::string::npos;
    }
    return win ? "__gxx_personality_seh0" : "__gxx_personality_v0";
}

// Ensure personality function and _ZTIPv type_info are declared in the module.
static void ensureEHDecls(llvm::Module* mod, llvm::LLVMContext& ctx,
                          const std::string& personalityName) {
    if (mod->getFunction(personalityName)) return;
    llvm::Type* i32  = llvm::Type::getInt32Ty(ctx);
    llvm::Type* ptrTy = llvm::PointerType::get(ctx, 0);
    // Personality function
    llvm::FunctionType* persType = llvm::FunctionType::get(i32, true);
    llvm::Function::Create(persType, llvm::Function::ExternalLinkage,
        personalityName, mod);
    // _ZTIPv — void* type_info (from libc++)
    if (!mod->getNamedGlobal("_ZTIPv"))
        new llvm::GlobalVariable(*mod, ptrTy, true,
            llvm::GlobalValue::ExternalLinkage, nullptr, "_ZTIPv");
}

llvm::Value* CodeGen::createMaybeInvoke(
    llvm::FunctionType* fty, llvm::Value* callee,
    llvm::ArrayRef<llvm::Value*> args, const llvm::Twine& name) {

    if (!unwindTarget)
        return builder->CreateCall(fty, callee, args, name);

    // Create a "normal" continuation block
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    auto* contBB = llvm::BasicBlock::Create(*context, "invoke.cont", fn);
    auto* inv = builder->CreateInvoke(fty, callee, contBB, unwindTarget, args, name);
    builder->SetInsertPoint(contBB);
    return inv;
}

// The name a thrown value's type is matched by (a catch clause strcmp's it): the
// written spelling with const, aliases and the `struct:` tag removed, a template
// instance mangled (`Box<int>` -> `Box_int`), and `int32` spelled `int`.
std::string CodeGen::exceptionTypeName(const std::string& raw) const {
    std::string t = expandAlias(tyq::strip(raw));
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

// A program that throws reports an exception nobody catches (in main, a thread, or
// through a C frame) by type and value, as the REPL does, instead of the C++ runtime's
// "uncaught exception of type void*": a constructor installs a std::terminate handler
// that rethrows the current exception into a catch-all, prints it to stderr and aborts.
void CodeGen::emitUncaughtReporter() {
    if (freestanding || !module->getFunction("__cxa_throw")) return;
    llvm::Function* mainFn = module->getFunction("main");
    if (!mainFn || mainFn->isDeclaration()) return;
    // The C functions it calls must be the C library's, not the program's own.
    for (const char* f : {"strcmp", "snprintf", "fflush", "write", "abort", "__cxa_rethrow", "__cxa_begin_catch"})
        if (llvm::Function* g = module->getFunction(f); g && !g->isDeclaration()) return;
    llvm::LLVMContext& c = *context;
    llvm::Type* ptrTy = llvm::PointerType::get(c, 0);
    llvm::Type* i32 = llvm::Type::getInt32Ty(c);
    llvm::Type* i64 = llvm::Type::getInt64Ty(c);
    llvm::Type* voidTy = llvm::Type::getVoidTy(c);
    ensureEHDecls(module.get(), c, ehPersonalityName());
    llvm::IRBuilder<> b(c);

    llvm::Function* handler = llvm::Function::Create(llvm::FunctionType::get(voidTy, false),
        llvm::Function::InternalLinkage, "__eskiu_terminate", module.get());
    handler->setPersonalityFn(module->getFunction(ehPersonalityName()));
    auto* entry = llvm::BasicBlock::Create(c, "entry", handler);
    auto* never = llvm::BasicBlock::Create(c, "never", handler);
    auto* lp = llvm::BasicBlock::Create(c, "lp", handler);
    b.SetInsertPoint(entry);
    llvm::FunctionCallee rethrow = module->getOrInsertFunction("__cxa_rethrow", voidTy);
    b.CreateInvoke(rethrow, never, lp);
    b.SetInsertPoint(never);
    b.CreateUnreachable();
    b.SetInsertPoint(lp);
    auto* pad = b.CreateLandingPad(llvm::StructType::get(c, {ptrTy, i32}), 1);
    pad->addClause(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)));
    llvm::Value* exc = b.CreateExtractValue(pad, {0});
    llvm::FunctionCallee beginCatch = module->getOrInsertFunction("__cxa_begin_catch", ptrTy, ptrTy);
    llvm::Value* obj = b.CreateCall(beginCatch, {exc});
    if (llvm::Triple(module->getTargetTriple()).isOSEmscripten()) obj = exc;   // see visit(TryStmt)
    llvm::Value* tname = b.CreateLoad(ptrTy, b.CreateConstGEP1_64(b.getInt8Ty(), obj, 8));
    llvm::Value* pay = b.CreateConstGEP1_64(b.getInt8Ty(), obj, 16);
    llvm::FunctionCallee strcmpFn = module->getOrInsertFunction("strcmp", i32, ptrTy, ptrTy);
    llvm::Type* szTy = module->getDataLayout().getIntPtrType(c);   // size_t
    llvm::FunctionCallee snprintfFn = module->getOrInsertFunction("snprintf",
        llvm::FunctionType::get(i32, {ptrTy, szTy, ptrTy}, true));
    llvm::Value* buf = b.CreateAlloca(llvm::ArrayType::get(b.getInt8Ty(), 512));
    auto isType = [&](const char* name) {
        return b.CreateICmpEQ(b.CreateCall(strcmpFn, {tname, b.CreateGlobalString(name)}), b.getInt32(0));
    };
    // One entry per primitive type name an exception can carry (exceptionTypeName): the
    // payload is read with that type only after its name matched (a smaller payload has
    // fewer bytes), widened, and formatted; any other type prints its name alone.
    struct Entry { const char* name; char kind; unsigned bits; };
    static const Entry entries[] = {
        {"int", 'i', 32}, {"int8", 'i', 8}, {"int16", 'i', 16}, {"int64", 'i', 64},
        {"uint", 'u', 32}, {"uint8", 'u', 8}, {"uint16", 'u', 16}, {"uint64", 'u', 64},
        {"double", 'f', 64}, {"float", 'f', 32}, {"bool", 'b', 8}, {"char", 'c', 8}, {"string", 's', 0}};
    const std::string head = "error: uncaught exception: %s";
    auto* bOut = llvm::BasicBlock::Create(c, "out", handler);
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> lens;
    for (const Entry& e : entries) {
        auto* yes = llvm::BasicBlock::Create(c, std::string("is.") + e.name, handler);
        auto* next = llvm::BasicBlock::Create(c, std::string("not.") + e.name, handler);
        b.CreateCondBr(isType(e.name), yes, next);
        b.SetInsertPoint(yes);
        llvm::Value* fmt = nullptr;
        llvm::Value* arg = nullptr;
        if (e.kind == 'i' || e.kind == 'u') {
            llvm::Value* v = b.CreateLoad(b.getIntNTy(e.bits), pay);
            arg = e.kind == 'i' ? b.CreateSExt(v, i64) : b.CreateZExt(v, i64);
            if (e.bits == 64) arg = v;
            fmt = b.CreateGlobalString(head + (e.kind == 'i' ? " %lld\n" : " %llu\n"));
        } else if (e.kind == 'f') {
            llvm::Value* v = b.CreateLoad(e.bits == 32 ? b.getFloatTy() : b.getDoubleTy(), pay);
            arg = e.bits == 32 ? b.CreateFPExt(v, b.getDoubleTy()) : v;
            fmt = b.CreateGlobalString(head + " %g\n");
        } else if (e.kind == 'b') {
            llvm::Value* v = b.CreateICmpNE(b.CreateLoad(b.getInt8Ty(), pay), b.getInt8(0));
            arg = b.CreateSelect(v, b.CreateGlobalString("true"), b.CreateGlobalString("false"));
            fmt = b.CreateGlobalString(head + " %s\n");
        } else if (e.kind == 'c') {
            arg = b.CreateZExt(b.CreateLoad(b.getInt8Ty(), pay), i32);
            fmt = b.CreateGlobalString(head + " '%c'\n");
        } else {
            arg = b.CreateLoad(ptrTy, pay);
            fmt = b.CreateSelect(b.CreateIsNull(arg), b.CreateGlobalString(head + "\n"),
                                 b.CreateGlobalString(head + " \"%s\"\n"));
        }
        lens.push_back({b.CreateCall(snprintfFn, {buf, llvm::ConstantInt::get(szTy, 512), fmt, tname, arg}), b.GetInsertBlock()});
        b.CreateBr(bOut);
        b.SetInsertPoint(next);
    }
    lens.push_back({b.CreateCall(snprintfFn, {buf, llvm::ConstantInt::get(szTy, 512), b.CreateGlobalString(head + "\n"), tname}),
                    b.GetInsertBlock()});
    b.CreateBr(bOut);
    b.SetInsertPoint(bOut);
    llvm::PHINode* n = b.CreatePHI(i32, (unsigned)lens.size());
    for (auto& [v, bb] : lens) n->addIncoming(v, bb);
    llvm::Value* len = b.CreateSelect(b.CreateICmpSGT(n, b.getInt32(511)), b.getInt32(511), n);
    llvm::FunctionCallee fflushFn = module->getOrInsertFunction("fflush", i32, ptrTy);
    b.CreateCall(fflushFn, {llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy))});
    llvm::FunctionCallee writeFn = module->getOrInsertFunction("write", szTy, i32, ptrTy, szTy);
    b.CreateCall(writeFn, {b.getInt32(2), buf, b.CreateSExtOrTrunc(len, szTy)});
    llvm::FunctionCallee abortFn = module->getOrInsertFunction("abort", voidTy);
    b.CreateCall(abortFn);
    b.CreateUnreachable();

    llvm::Function* install = llvm::Function::Create(llvm::FunctionType::get(voidTy, false),
        llvm::Function::InternalLinkage, "__eskiu_install_terminate", module.get());
    b.SetInsertPoint(llvm::BasicBlock::Create(c, "entry", install));
    llvm::FunctionCallee setTerm = module->getOrInsertFunction("_ZSt13set_terminatePFvvE", ptrTy, ptrTy);
    b.CreateCall(setTerm, {handler});
    b.CreateRetVoid();
    llvm::appendToGlobalCtors(*module, install, 65535);
}

// `ptr __eskiu_exception_text(ptr %obj)`: formats a caught EskiuEx object into one of 8
// rotating 512-byte static buffers as `emitUncaughtReporter` does (`"int 42"`, `"Oops"`).
llvm::Function* CodeGen::ensureExceptionTextFn() {
    const char* name = "__eskiu_exception_text";
    if (llvm::Function* f = module->getFunction(name)) return f;
    llvm::LLVMContext& c = *context;
    llvm::Type* ptrTy = llvm::PointerType::get(c, 0);
    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(c);
    llvm::Type* i32   = llvm::Type::getInt32Ty(c);
    llvm::Type* i64   = llvm::Type::getInt64Ty(c);
    auto* fty = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
    auto* f = llvm::Function::Create(fty, llvm::Function::InternalLinkage, name, module.get());

    llvm::ArrayType* slotTy = llvm::ArrayType::get(i8Ty, 512);
    llvm::ArrayType* ringTy = llvm::ArrayType::get(slotTy, 8);
    auto* ringGV = new llvm::GlobalVariable(*module, ringTy, false,
        llvm::GlobalValue::InternalLinkage, llvm::ConstantAggregateZero::get(ringTy), "__eskiu_ex_buf");
    auto* idxGV = new llvm::GlobalVariable(*module, i32, false,
        llvm::GlobalValue::InternalLinkage, llvm::ConstantInt::get(i32, 0), "__eskiu_ex_idx");

    llvm::IRBuilder<> b(c);
    auto* entry = llvm::BasicBlock::Create(c, "entry", f);
    b.SetInsertPoint(entry);
    llvm::Value* obj = f->getArg(0);
    llvm::Value* curIdx = b.CreateLoad(i32, idxGV, "idx");
    llvm::Value* nextIdx = b.CreateAnd(b.CreateAdd(curIdx, b.getInt32(1)), b.getInt32(7), "next");
    b.CreateStore(nextIdx, idxGV);
    llvm::Value* buf = b.CreateInBoundsGEP(ringTy, ringGV, {b.getInt32(0), curIdx, b.getInt32(0)}, "buf");
    llvm::Value* tname = b.CreateLoad(ptrTy, b.CreateConstGEP1_64(i8Ty, obj, 8), "tn");
    llvm::Value* pay = b.CreateConstGEP1_64(i8Ty, obj, 16, "pay");
    llvm::FunctionCallee strcmpFn = module->getOrInsertFunction("strcmp", i32, ptrTy, ptrTy);
    llvm::Type* szTy = module->getDataLayout().getIntPtrType(c);
    llvm::FunctionCallee snprintfFn = module->getOrInsertFunction("snprintf",
        llvm::FunctionType::get(i32, {ptrTy, szTy, ptrTy}, true));
    auto isType = [&](const char* nm) {
        return b.CreateICmpEQ(b.CreateCall(strcmpFn, {tname, b.CreateGlobalString(nm)}), b.getInt32(0));
    };
    struct Entry { const char* name; char kind; unsigned bits; };
    static const Entry entries[] = {
        {"int", 'i', 32}, {"int8", 'i', 8}, {"int16", 'i', 16}, {"int64", 'i', 64},
        {"uint", 'u', 32}, {"uint8", 'u', 8}, {"uint16", 'u', 16}, {"uint64", 'u', 64},
        {"double", 'f', 64}, {"float", 'f', 32}, {"bool", 'b', 8}, {"char", 'c', 8}, {"string", 's', 0}};
    const std::string head = "%s";
    auto* bOut = llvm::BasicBlock::Create(c, "out", f);
    for (const Entry& e : entries) {
        auto* yes = llvm::BasicBlock::Create(c, std::string("is.") + e.name, f);
        auto* next = llvm::BasicBlock::Create(c, std::string("not.") + e.name, f);
        b.CreateCondBr(isType(e.name), yes, next);
        b.SetInsertPoint(yes);
        llvm::Value* fmt = nullptr;
        llvm::Value* arg = nullptr;
        if (e.kind == 'i' || e.kind == 'u') {
            llvm::Value* v = b.CreateLoad(b.getIntNTy(e.bits), pay);
            arg = e.kind == 'i' ? b.CreateSExt(v, i64) : b.CreateZExt(v, i64);
            if (e.bits == 64) arg = v;
            fmt = b.CreateGlobalString(head + (e.kind == 'i' ? " %lld" : " %llu"));
        } else if (e.kind == 'f') {
            llvm::Value* v = b.CreateLoad(e.bits == 32 ? b.getFloatTy() : b.getDoubleTy(), pay);
            arg = e.bits == 32 ? b.CreateFPExt(v, b.getDoubleTy()) : v;
            fmt = b.CreateGlobalString(head + " %g");
        } else if (e.kind == 'b') {
            llvm::Value* v = b.CreateICmpNE(b.CreateLoad(i8Ty, pay), b.getInt8(0));
            arg = b.CreateSelect(v, b.CreateGlobalString("true"), b.CreateGlobalString("false"));
            fmt = b.CreateGlobalString(head + " %s");
        } else if (e.kind == 'c') {
            arg = b.CreateZExt(b.CreateLoad(i8Ty, pay), i32);
            fmt = b.CreateGlobalString(head + " '%c'");
        } else {
            arg = b.CreateLoad(ptrTy, pay);
            fmt = b.CreateSelect(b.CreateIsNull(arg), b.CreateGlobalString(head),
                                 b.CreateGlobalString(head + " \"%s\""));
        }
        b.CreateCall(snprintfFn, {buf, llvm::ConstantInt::get(szTy, 512), fmt, tname, arg});
        b.CreateBr(bOut);
        b.SetInsertPoint(next);
    }
    b.CreateCall(snprintfFn, {buf, llvm::ConstantInt::get(szTy, 512), b.CreateGlobalString(head), tname});
    b.CreateBr(bOut);
    b.SetInsertPoint(bOut);
    b.CreateRet(buf);
    return f;
}

static bool hasExceptionText(Expr* e);
static bool hasExceptionText(Stmt* s) {
    if (!s) return false;
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& st : b->items) {
            if (std::holds_alternative<StmtPtr>(st)) {
                if (hasExceptionText(std::get<StmtPtr>(st).get())) return true;
            } else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(st).get())) {
                if (hasExceptionText(vd->initializer.get())) return true;
            }
        }
        return false;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s))
        return hasExceptionText(i->condition.get()) || hasExceptionText(i->thenBranch.get()) || hasExceptionText(i->elseBranch.get());
    if (auto* w = dynamic_cast<WhileStmt*>(s))
        return hasExceptionText(w->condition.get()) || hasExceptionText(w->body.get());
    if (auto* dw = dynamic_cast<DoWhileStmt*>(s))
        return hasExceptionText(dw->body.get()) || hasExceptionText(dw->condition.get());
    if (auto* f = dynamic_cast<ForStmt*>(s))
        return hasExceptionText(f->init.get()) || hasExceptionText(f->condition.get()) || hasExceptionText(f->step.get()) || hasExceptionText(f->body.get());
    if (auto* fi = dynamic_cast<ForInStmt*>(s))
        return hasExceptionText(fi->iterable.get()) || hasExceptionText(fi->body.get());
    if (auto* r = dynamic_cast<ReturnStmt*>(s)) return hasExceptionText(r->value.get());
    if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        if (hasExceptionText(sw->subject.get())) return true;
        for (auto& c : sw->cases) {
            if (hasExceptionText(c.value.get())) return true;
            for (auto& it : c.stmts) {
                if (std::holds_alternative<StmtPtr>(it)) {
                    if (hasExceptionText(std::get<StmtPtr>(it).get())) return true;
                } else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) {
                    if (hasExceptionText(vd->initializer.get())) return true;
                }
            }
        }
        return false;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(s)) {
        if (hasExceptionText(m->subject.get())) return true;
        for (auto& a : m->arms) if (hasExceptionText(a.body.get())) return true;
        return false;
    }
    if (auto* th = dynamic_cast<ThrowStmt*>(s)) return hasExceptionText(th->value.get());
    if (auto* tr = dynamic_cast<TryStmt*>(s))
        return hasExceptionText(tr->body.get()) || hasExceptionText(tr->finally.get());
    if (auto* d = dynamic_cast<DeferStmt*>(s)) return hasExceptionText(d->body.get());
    if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s)) return hasExceptionText(tj->tid.get());
    if (auto* a = dynamic_cast<AsmStmt*>(s)) {
        for (auto& out : a->outputs) if (hasExceptionText(out.second.get())) return true;
        for (auto& in : a->inputs) if (hasExceptionText(in.second.get())) return true;
        return false;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(s)) return hasExceptionText(es->expr.get());
    return false;
}
static bool hasExceptionText(Expr* e) {
    if (!e || dynamic_cast<LambdaExpr*>(e)) return false;
    if (auto* c = dynamic_cast<CallExpr*>(e))
        if (auto* id = dynamic_cast<IdentExpr*>(c->callee.get()); id && id->name == "exception_text" && c->args.empty())
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
            llvm::Value* txt = builder->CreateCall(ensureExceptionTextFn(), {exData}, "ex.text");
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
    if (dynamic_cast<LambdaExpr*>(node->worker.get())) {
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
