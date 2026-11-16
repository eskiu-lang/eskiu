#include "codegen.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "llvm/IR/InlineAsm.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"
#include "codegen_eh.h"

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

// `ptr __eskiu_exception_text(ptr %obj, ptr %buf)`: formats a caught EskiuEx object into
// `buf` (512 bytes, the catch handler's own) as `emitUncaughtReporter` does (`"int 42"`,
// `"Oops"`), and returns `buf`.
llvm::Function* CodeGen::ensureExceptionTextFn() {
    const char* name = "__eskiu_exception_text";
    if (llvm::Function* f = module->getFunction(name)) return f;
    llvm::LLVMContext& c = *context;
    llvm::Type* ptrTy = llvm::PointerType::get(c, 0);
    llvm::Type* i8Ty  = llvm::Type::getInt8Ty(c);
    llvm::Type* i32   = llvm::Type::getInt32Ty(c);
    llvm::Type* i64   = llvm::Type::getInt64Ty(c);
    auto* fty = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
    auto* f = llvm::Function::Create(fty, llvm::Function::InternalLinkage, name, module.get());

    llvm::IRBuilder<> b(c);
    auto* entry = llvm::BasicBlock::Create(c, "entry", f);
    b.SetInsertPoint(entry);
    llvm::Value* obj = f->getArg(0);
    llvm::Value* buf = f->getArg(1);
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
