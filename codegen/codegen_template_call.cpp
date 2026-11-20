// codegen_template_call.cpp: function lookup, alloc_with, function values and template calls (split from codegen_call.cpp).
#include "codegen.h"
#include "../intrinsics.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"
#include "../sema/type.h"

llvm::Function* CodeGen::getOrDeclareFunc(const std::string& name, llvm::Type* retType,
                                           std::vector<llvm::Type*> paramTypes, bool isVarArg) {
    llvm::Function* f = module->getFunction(name);
    if (!f) {
        auto* ft = llvm::FunctionType::get(retType, paramTypes, isVarArg);
        f = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, name, module.get());
    }
    return f;
}

void CodeGen::visit(AllocWithExpr* node) {
    // Lower to: (*T) <AllocType>_alloc(allocator, count * sizeof(T)), then zero the
    // block when it is not null, as alloc<T> returns zeroed memory whatever the
    // allocator (a Bump after reset or a Pool block reused hands back old bytes).
    // The allocator is a struct providing `*void alloc(... size)`.
    llvm::Value* allocPtr = evaluateExpr(node->allocator);

    std::string at = getExprEskiuType(node->allocator);
    while (!at.empty() && at.front() == '*') at = at.substr(1);
    while (!at.empty() && at.back()  == '*') at.pop_back();
    if (at.rfind("struct:", 0) == 0) at = at.substr(7);

    std::string fnName = at + "_alloc";
    llvm::Function* af = module->getFunction(fnName);
    if (!af) af = instantiateGenericMethod(at, "alloc");   // an inline method of a generic struct
    if (!af)
        throw std::runtime_error("alloc_with: allocator type '" + at +
                                 "' has no alloc method (" + fnName + ")");

    llvm::Type* elemTy = getTypeFromString(node->elemType);
    uint64_t esz = module->getDataLayout().getTypeAllocSize(elemTy);
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    llvm::Value* n64 = builder->CreateIntCast(evaluateExpr(node->count), i64,
                            !eskiuUnsigned(getExprEskiuType(node->count)));
    // n * sizeof(T) as a signed 64-bit size: a count that overflows it, or is negative,
    // (or a size too wide for a narrower size parameter) yields null instead of a
    // wrapped, too-small allocation.
    llvm::Function* smul = llvm::Intrinsic::getOrInsertDeclaration(
        module.get(), llvm::Intrinsic::smul_with_overflow, {i64});
    llvm::Value* prod = builder->CreateCall(smul, {n64, llvm::ConstantInt::get(i64, esz)}, "allocw.mul");
    llvm::Value* total = builder->CreateExtractValue(prod, {0}, "allocw.size");
    llvm::Value* bad = builder->CreateOr(builder->CreateExtractValue(prod, {1}),
        builder->CreateICmpSLT(total, llvm::ConstantInt::get(i64, 0)), "allocw.bad");

    llvm::Value* total64 = total;
    // Coerce the size to the alloc method's second parameter type.
    llvm::FunctionType* fty = af->getFunctionType();
    if (fty->getNumParams() >= 2 && fty->getParamType(1) != total->getType()) {
        llvm::Type* pt = fty->getParamType(1);
        if (pt->isIntegerTy() && pt->getIntegerBitWidth() < 64) {
            uint64_t maxv = (uint64_t(1) << (pt->getIntegerBitWidth() - 1)) - 1;
            bad = builder->CreateOr(bad, builder->CreateICmpSGT(total, llvm::ConstantInt::get(i64, maxv)));
        }
        total = builder->CreateIntCast(total, pt, false);
    }

    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* doCall = llvm::BasicBlock::Create(*context, "allocw.call", fn);
    llvm::BasicBlock* fail   = llvm::BasicBlock::Create(*context, "allocw.fail", fn);
    llvm::BasicBlock* done   = llvm::BasicBlock::Create(*context, "allocw.done", fn);
    builder->CreateCondBr(bad, fail, doCall);
    builder->SetInsertPoint(doCall);
    // Returns *void; the cast to *T is a no-op under opaque pointers.
    llvm::Value* p = createMaybeInvoke(af->getFunctionType(), af, {allocPtr, total}, "allocw.ptr");
    llvm::BasicBlock* zero = llvm::BasicBlock::Create(*context, "allocw.zero", fn);
    builder->CreateCondBr(builder->CreateIsNull(p), done, zero);
    llvm::BasicBlock* callEnd = builder->GetInsertBlock();
    builder->SetInsertPoint(zero);
    emitZeroBytes(p, total64, llvm::Align(1));
    llvm::BasicBlock* zeroEnd = builder->GetInsertBlock();
    builder->CreateBr(done);
    builder->SetInsertPoint(fail);
    builder->CreateBr(done);
    builder->SetInsertPoint(done);
    llvm::PHINode* phi = builder->CreatePHI(p->getType(), 3, "allocw.res");
    phi->addIncoming(p, callEnd);
    phi->addIncoming(p, zeroEnd);
    phi->addIncoming(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(p->getType())), fail);
    exprValueStack.push(phi);
}

llvm::Value* CodeGen::makeFunctionPointer(llvm::Function* target) {
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    std::string wname = "__fnptr_" + target->getName().str();
    llvm::Function* wrapper = module->getFunction(wname);
    if (!wrapper) {
        // Thunk: (env*, params...) -> ret  that ignores env and calls target. It
        // presents the Eskiu-level signature an indirect call uses: a C-ABI-lowered
        // extern is wrapped at its logical signature, and an sret function returns
        // its struct by value (the thunk passes the hidden sret slot itself).
        auto abiIt = externAbi.find(target->getName().str());
        auto sretIt = funcSretTypes.find(target->getName().str());
        bool viaSret = abiIt == externAbi.end() && sretIt != funcSretTypes.end();
        llvm::FunctionType* tfty = abiIt != externAbi.end() ? abiIt->second.logical
                                                            : target->getFunctionType();
        std::vector<llvm::Type*> wparams;
        wparams.push_back(ptrTy);  // env (unused)
        auto tparams = tfty->params();
        for (size_t i = viaSret ? 1 : 0; i < tparams.size(); ++i) wparams.push_back(tparams[i]);
        llvm::Type* wret = viaSret ? sretIt->second : tfty->getReturnType();
        llvm::FunctionType* wfty = llvm::FunctionType::get(wret, wparams, tfty->isVarArg());
        wrapper = llvm::Function::Create(wfty, llvm::Function::InternalLinkage,
                                         wname, module.get());

        llvm::BasicBlock* prev = builder->GetInsertBlock();
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(*context, "entry", wrapper);
        builder->SetInsertPoint(entry);
        std::vector<llvm::Value*> callArgs;
        llvm::Value* sretSlot = nullptr;
        if (viaSret) {
            sretSlot = builder->CreateAlloca(sretIt->second, nullptr, "sret.tmp");
            callArgs.push_back(sretSlot);
        }
        auto ai = wrapper->arg_begin(); ++ai;  // skip env
        for (; ai != wrapper->arg_end(); ++ai) callArgs.push_back(&*ai);
        llvm::Value* r = abiIt != externAbi.end()
            ? emitCAbiCall(target, abiIt->second, callArgs, /*allowInvoke=*/false)
            : builder->CreateCall(target, callArgs);
        if (viaSret) builder->CreateRet(builder->CreateLoad(sretIt->second, sretSlot));
        else if (tfty->getReturnType()->isVoidTy()) builder->CreateRetVoid();
        else builder->CreateRet(r);
        if (prev) builder->SetInsertPoint(prev);
    }

    // The fat pointer {wrapper, null}, the shape lambdas produce. It is a constant, so a
    // global initializer can hold a function name (`Op g = add;`), as in C.
    llvm::StructType* fatTy = llvm::cast<llvm::StructType>(getTypeFromString("fn()->void"));
    return llvm::ConstantStruct::get(fatTy,
        {wrapper, llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy))});
}

llvm::Function* CodeGen::instantiateFnTemplate(FunctionDecl* fd, const std::string& mangledName,
                                               const std::map<std::string, std::string>& subs) {
    // Instantiate if not already in module.
    // Save/restore the insert point — we may be inside another function's body.
    if (!module->getFunction(mangledName)) {
        llvm::BasicBlock*          savedBB         = builder->GetInsertBlock();
        llvm::BasicBlock::iterator savedPoint      = builder->GetInsertPoint();
        llvm::Function*            savedFunc       = currentFunction;
        llvm::Value*               savedSretParam  = currentSretParam;
        // Restore (not clear) the override: this call may be nested inside another
        // template body whose substitutions must survive the inner instantiation.
        auto                       savedOverride   = typeParamOverride;

        typeParamOverride = subs;
        auto inst = std::make_shared<FunctionDecl>(mangledName, fd->returnType, fd->params, fd->body);
        inst->sourceFile = fd->sourceFile;
        inst->accept(this);
        typeParamOverride = savedOverride;

        // Restore caller's context
        currentFunction  = savedFunc;
        currentSretParam = savedSretParam;
        if (savedBB) builder->SetInsertPoint(savedBB, savedPoint);
    }
    return module->getFunction(mangledName);
}

void CodeGen::visit(TemplateCallExpr* node) {
    // Variadic access: va_arg<T>(ap) -> next argument of type T.
    if (node->templateName == "va_arg" && node->args.size() == 1 && node->typeArgs.size() == 1) {
        llvm::Value* ap = evaluateLValue(node->args[0]);
        std::string t = typeParamOverride.empty() ? node->typeArgs[0]
                                                   : substType(node->typeArgs[0], typeParamOverride);
        exprValueStack.push(emitVaArg(ap, getTypeFromString(t)));
        return;
    }
    // Generic algebraic-variant construction: `Some<int>(5)`, `Left<A,B>(x)`. Type
    // args resolve through the enclosing template's substitutions (so `Left<A,B>`
    // inside select2<A,B> becomes Left<int,int> at instantiation).
    if (genericVariants.count(node->templateName)) {
        auto& gi = genericVariants[node->templateName];      // (genericName, tag)
        std::vector<std::string> args;
        for (const auto& t : node->typeArgs)
            args.push_back(typeParamOverride.empty() ? t : substType(t, typeParamOverride));
        std::string mangled = ensureEnumInst(gi.first, args);
        EnumDecl* ge = genericEnumDecls[gi.first];
        std::map<std::string, std::string> subs;
        for (size_t i = 0; i < ge->typeParams.size() && i < args.size(); ++i)
            subs[ge->typeParams[i]] = args[i];
        std::vector<std::string> fts;
        for (const auto& ft : ge->payloads[gi.second]) fts.push_back(substType(ft, subs));
        exprValueStack.push(buildEnumValue(structTypes[mangled], gi.second, fts, node->args));
        return;
    }
    auto templ = funcTemplateDecls.find(node->templateName);
    if (templ == funcTemplateDecls.end())
        failAt(node, "unknown generic function '" + node->templateName + "'");

    FunctionDecl* fd = templ->second;
    auto& tp = fd->typeParams;
    // Resolve each explicit type argument through the enclosing template's active
    // substitutions. When this call appears inside another template body (e.g.
    // `mk<T>(n)` inside `esz<T>`), node->typeArgs holds the literal param name
    // "T"; without this it would instantiate `mk_T` (T unresolved → i32). We must
    // not mutate node->typeArgs — the same node is re-visited per instantiation.
    auto resolveArg = [&](const std::string& t) {
        return typeParamOverride.empty() ? t : substType(t, typeParamOverride);
    };
    std::map<std::string, std::string> subs;
    for (size_t i = 0; i < tp.size() && i < node->typeArgs.size(); ++i)
        subs[tp[i]] = resolveArg(node->typeArgs[i]);

    // Mangle the instantiated function name
    std::string mangledName = node->templateName;
    for (const auto& t : node->typeArgs) mangledName += "_" + mangleTemplate(resolveArg(t));

    llvm::Function* func = instantiateFnTemplate(fd, mangledName, subs);
    if (!func) failAt(node, "could not instantiate '" + mangledName + "'");

    // An argument bound to an interface parameter (`Sh y`, or `T x` with T = Sh) is
    // boxed to the interface, as for a direct call.
    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < node->args.size(); ++i) {
        bool fixed = i < fd->params.size() && fd->params[i].first != "...";
        args.push_back(fixed ? evalForType(node->args[i], substType(fd->params[i].first, subs))
                             : evaluateExpr(node->args[i]));
    }

    // Coerce arguments to the instantiated function's parameter types — e.g. a
    // `double` literal passed where T=float substituted the parameter to `float`.
    llvm::FunctionType* fty = func->getFunctionType();
    unsigned pbase = funcSretTypes.count(mangledName) ? 1u : 0u; // skip hidden sret ptr
    for (size_t i = 0; i < args.size(); ++i) {
        unsigned pidx = pbase + (unsigned)i;
        if (pidx >= fty->getNumParams()) break;
        llvm::Type* pt = fty->getParamType(pidx);
        llvm::Value* v = args[i];
        if (!v || v->getType() == pt) continue;
        args[i] = coerceValue(v, pt, i < node->args.size() && eskiuUnsigned(getExprEskiuType(node->args[i])));
    }

    auto sretIt = funcSretTypes.find(mangledName);
    if (sretIt != funcSretTypes.end()) {
        llvm::Value* sretAlloca = entryAlloca(sretIt->second, nullptr, "sret.tmp");
        markFreshTemp(sretAlloca);
        args.insert(args.begin(), sretAlloca);
        createMaybeInvoke(func->getFunctionType(), func, args);
        exprValueStack.push(builder->CreateLoad(sretIt->second, sretAlloca));
    } else {
        exprValueStack.push(createMaybeInvoke(func->getFunctionType(), func, args));
    }
}
