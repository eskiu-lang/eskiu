#include "codegen.h"
#include "../intrinsics.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"
#include "../sema/type.h"

// CodeGen — calls, template instantiation, intrinsics, function pointers,
// and explicit-allocator construction.
// Part of the codegen_expr.cpp split; all methods are CodeGen members (codegen.h).

void CodeGen::unifyTypeParam(std::string pattern, std::string concrete,
                             const std::set<std::string>& tps,
                             std::map<std::string, std::string>& subs) const {
    auto stripStruct = [](std::string s) {
        return s.rfind("struct:", 0) == 0 ? s.substr(7) : s;
    };
    auto canon = [](std::string t) {           // move trailing '*' to leading
        int stars = 0;
        while (!t.empty() && t.back()  == '*') { t.pop_back();    stars++; }
        while (!t.empty() && t.front() == '*') { t = t.substr(1); stars++; }
        return std::string(stars, '*') + t;
    };
    pattern  = canon(stripStruct(tyq::strip(pattern)));    // const does not change the shape
    concrete = canon(stripStruct(tyq::strip(concrete)));
    // strip matching leading '*' from both
    size_t pi = 0, ci = 0;
    while (pi < pattern.size() && pattern[pi] == '*' &&
           ci < concrete.size() && concrete[ci] == '*') { pi++; ci++; }
    pattern = pattern.substr(pi);
    concrete = stripStruct(concrete.substr(ci));
    if (pattern.empty() || concrete.empty()) return;

    if (tps.count(pattern)) {                   // bare type parameter
        if (!subs.count(pattern)) subs[pattern] = concrete;
        return;
    }
    auto isSlice = [](const std::string& t) {
        return t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0;
    };
    if (isSlice(pattern)) {                     // `T[]` against a slice binds the element
        if (isSlice(concrete))
            unifyTypeParam(pattern.substr(0, pattern.size() - 2),
                           concrete.substr(0, concrete.size() - 2), tps, subs);
        return;
    }
    size_t lt = pattern.find('<');              // Name<args> vs an instance
    if (lt == std::string::npos) return;
    auto [pbase, pargs] = splitTemplateType(pattern);
    std::string cbase; std::vector<std::string> cargs;
    if (concrete.find('<') != std::string::npos) {
        auto pr = splitTemplateType(concrete); cbase = pr.first; cargs = pr.second;
    } else {
        auto it = templateInstanceArgs.find(concrete);
        if (it != templateInstanceArgs.end()) { cbase = it->second.first; cargs = it->second.second; }
    }
    if (cbase != pbase) return;
    for (size_t i = 0; i < pargs.size() && i < cargs.size(); ++i)
        unifyTypeParam(pargs[i], cargs[i], tps, subs);
}

void CodeGen::ensureTemplateInstantiated(const std::string& mangled,
                                          const std::string& tname,
                                          const std::vector<std::string>& args) {
    templateInstanceArgs[mangled] = {tname, args};  // record for type-arg inference
    if (structTypes.count(mangled)) return;
    auto it = templateDecls.find(tname);
    if (it == templateDecls.end()) return;
    StructDecl* tmpl = it->second;

    auto& tp = tmpl->typeParams;
    std::map<std::string, std::string> subs;
    for (size_t i = 0; i < tp.size() && i < args.size(); ++i) subs[tp[i]] = args[i];

    std::vector<StructDecl::Field> fields;
    for (const auto& f : tmpl->fields)
        fields.push_back({substType(f.type, subs), f.name, f.bitWidth});
    layoutStruct(mangled, fields, tmpl->pads, tmpl->isPacked, tmpl->packAlign);
}

std::string CodeGen::instanceSpelling(const std::string& t) {
    ty::Type ty = ty::Type::parse(tyq::strip(t));
    auto inner = [&](std::shared_ptr<ty::Type>& p) {
        if (p) p = std::make_shared<ty::Type>(ty::Type::parse(instanceSpelling(p->str())));
    };
    switch (ty.kind) {
        case ty::Type::Kind::Pointer: inner(ty.pointee); return ty.str();
        case ty::Type::Kind::Array:
        case ty::Type::Kind::Slice:   inner(ty.elem);    return ty.str();
        case ty::Type::Kind::Fn:
            for (auto& p : ty.params) p = ty::Type::parse(instanceSpelling(p.str()));
            inner(ty.ret);
            return ty.str();
        case ty::Type::Kind::Template: {
            std::string s = ty.str();
            auto [tn, targs] = splitTemplateType(s);
            std::string mangled = mangleTemplate(s);
            ensureTemplateInstantiated(mangled, tn, targs);
            return mangled;
        }
        default: return tyq::strip(t);
    }
}

FunctionDecl* CodeGen::genericMethod(const std::string& instName, const std::string& method,
                                     std::map<std::string, std::string>* subsOut) const {
    auto ia = templateInstanceArgs.find(instName);
    if (ia == templateInstanceArgs.end()) return nullptr;
    auto td = templateDecls.find(ia->second.first);
    if (td == templateDecls.end()) return nullptr;
    for (const auto& m : td->second->methods) {
        auto* mf = dynamic_cast<FunctionDecl*>(m.get());
        if (!mf || mf->name != method) continue;
        if (subsOut) {
            const auto& tp = td->second->typeParams;
            for (size_t i = 0; i < tp.size() && i < ia->second.second.size(); ++i)
                (*subsOut)[tp[i]] = ia->second.second[i];
        }
        return mf;
    }
    return nullptr;
}

FunctionDecl* CodeGen::genericFreeMethod(const std::string& instName, const std::string& method) const {
    auto ia = templateInstanceArgs.find(instName);
    if (ia == templateInstanceArgs.end()) return nullptr;
    auto ft = funcTemplateDecls.find(ia->second.first + "_" + method);
    if (ft == funcTemplateDecls.end() || ft->second->params.empty()) return nullptr;
    return ft->second;
}

std::map<std::string, std::string> CodeGen::genericFreeMethodSubs(FunctionDecl* fd, const std::string& recvType,
                                                                  const std::vector<ExprPtr>& args) const {
    const std::string& selfT = fd->params[0].first;
    bool recvPtr = tyq::isPtr(recvType), selfPtr = tyq::isPtr(selfT);
    std::string recvAsSelf = recvType;
    if (selfPtr && !recvPtr) recvAsSelf = "*" + recvType;
    else if (!selfPtr && recvPtr) recvAsSelf = tyq::pointee(recvType);
    std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
    std::map<std::string, std::string> subs;
    unifyTypeParam(selfT, recvAsSelf, tps, subs);
    for (size_t j = 1; j < fd->params.size() && j - 1 < args.size(); ++j)
        unifyTypeParam(fd->params[j].first, getExprEskiuType(args[j - 1]), tps, subs);
    return subs;
}

llvm::Function* CodeGen::instantiateGenericMethod(const std::string& instName, const std::string& method) {
    std::string mangled = instName + "_" + method;
    if (llvm::Function* f = module->getFunction(mangled)) return f;
    std::map<std::string, std::string> subs;
    FunctionDecl* mf = genericMethod(instName, method, &subs);
    if (!mf || !mf->body) return nullptr;
    std::vector<std::pair<std::string, std::string>> params{{"*" + instName, "self"}};
    for (const auto& p : mf->params) params.push_back({substType(p.first, subs), p.second});

    // Same context save/restore as a generic function instantiated mid-body.
    llvm::BasicBlock*          savedBB        = builder->GetInsertBlock();
    llvm::BasicBlock::iterator savedPoint     = builder->GetInsertPoint();
    llvm::Function*            savedFunc      = currentFunction;
    llvm::Value*               savedSretParam = currentSretParam;
    auto                       savedOverride  = typeParamOverride;
    typeParamOverride = subs;
    auto inst = std::make_shared<FunctionDecl>(mangled, substType(mf->returnType, subs), params, mf->body);
    inst->accept(this);
    typeParamOverride = savedOverride;
    currentFunction  = savedFunc;
    currentSretParam = savedSretParam;
    if (savedBB) builder->SetInsertPoint(savedBB, savedPoint);
    return module->getFunction(mangled);
}

// Lower a call to an `intrinsic`-declared function to inline IR. The registry of
// supported intrinsics lives here; their signatures are declared in stdlib (e.g.
// stdlib/atomic.esk) and the orderings/semantics are fixed (docs/dev/async-design.md §3).
llvm::Value* CodeGen::lowerIntrinsicCall(const std::string& fn, CallExpr* node) {
    AtomicIntrinsic a;
    if (!parseAtomicIntrinsic(fn, a))
        throw std::runtime_error("intrinsic '" + fn + "' is declared but has no codegen lowering");
    if (a.op == "fence") {
        builder->CreateFence(llvm::AtomicOrdering::SequentiallyConsistent);
        return llvm::UndefValue::get(llvm::Type::getVoidTy(*context));
    }
    llvm::Type* ty = a.kind == AtomicKind::I64 ? llvm::Type::getInt64Ty(*context)
                   : a.kind == AtomicKind::Ptr ? (llvm::Type*)llvm::PointerType::get(*context, 0)
                   : llvm::Type::getInt32Ty(*context);
    llvm::Align al = module->getDataLayout().getABITypeAlign(ty);
    llvm::Value* cell = evaluateExpr(node->args[0]);
    auto operand = [&](size_t i) { return coerceValue(evaluateExpr(node->args[i]), ty, false); };
    auto order = [&](llvm::AtomicOrdering o) { return a.relaxed ? llvm::AtomicOrdering::Monotonic : o; };
    if (a.op == "load") {
        auto* ld = builder->CreateLoad(ty, cell, "atm.load");
        ld->setAtomic(order(llvm::AtomicOrdering::Acquire));
        ld->setAlignment(al);
        return ld;
    }
    if (a.op == "store") {
        auto* st = builder->CreateStore(operand(1), cell);
        st->setAtomic(order(llvm::AtomicOrdering::Release));
        st->setAlignment(al);
        return llvm::UndefValue::get(llvm::Type::getVoidTy(*context));
    }
    if (a.op == "cas") {
        llvm::Value* expected = operand(1);
        llvm::Value* desired = operand(2);
        llvm::Value* cx = builder->CreateAtomicCmpXchg(cell, expected, desired, al,
            order(llvm::AtomicOrdering::AcquireRelease), order(llvm::AtomicOrdering::Acquire));
        return builder->CreateExtractValue(cx, 1, "atm.cas.ok");   // success bit (i1)
    }
    llvm::AtomicRMWInst::BinOp op = a.op == "swap" ? llvm::AtomicRMWInst::Xchg
        : a.op == "fetch_add" ? llvm::AtomicRMWInst::Add : a.op == "fetch_sub" ? llvm::AtomicRMWInst::Sub
        : a.op == "fetch_and" ? llvm::AtomicRMWInst::And : a.op == "fetch_or" ? llvm::AtomicRMWInst::Or
        : llvm::AtomicRMWInst::Xor;
    return builder->CreateAtomicRMW(op, cell, operand(1), al, order(llvm::AtomicOrdering::AcquireRelease));
}

void CodeGen::dbgFormat(const std::string& t0, llvm::Value* addr, std::string& fmt,
                        std::vector<llvm::Value*>& args, int depth) {
    std::string t = tyq::strip(expandAlias(t0));
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    llvm::Type* ty = getTypeFromString(t);
    auto lit = [&](const std::string& s) { for (char c : s) fmt += c == '%' ? std::string("%%") : std::string(1, c); };
    static const std::set<std::string> sints = {"int", "int8", "int16", "int32", "int64"};
    static const std::set<std::string> uints = {"uint", "uint8", "uint16", "uint32", "uint64"};
    ty::Type pt = ty::Type::parse(t);
    if (sints.count(t) || uints.count(t)) {
        llvm::Value* v = builder->CreateLoad(ty, addr);
        args.push_back(sints.count(t) ? builder->CreateSExtOrTrunc(v, i64) : builder->CreateZExtOrTrunc(v, i64));
        fmt += sints.count(t) ? "%lld" : "%llu";
    } else if (t == "bool") {
        llvm::Value* v = builder->CreateLoad(ty, addr);
        args.push_back(builder->CreateSelect(v, builder->CreateGlobalString("true"), builder->CreateGlobalString("false")));
        fmt += "%s";
    } else if (t == "char") {
        args.push_back(builder->CreateZExt(builder->CreateLoad(ty, addr), llvm::Type::getInt32Ty(*context)));
        fmt += "'%c'";
    } else if (t == "float" || t == "double") {
        llvm::Value* v = builder->CreateLoad(ty, addr);
        args.push_back(t == "float" ? builder->CreateFPExt(v, builder->getDoubleTy()) : v);
        fmt += "%g";
    } else if (t == "string") {
        llvm::Value* v = builder->CreateLoad(ty, addr);
        llvm::Value* isNull = builder->CreateIsNull(v);
        llvm::Value* q = builder->CreateSelect(isNull, builder->CreateGlobalString(""), builder->CreateGlobalString("\""));
        args.push_back(q);
        args.push_back(builder->CreateSelect(isNull, builder->CreateGlobalString("null"), v));
        args.push_back(q);
        fmt += "%s%s%s";
    } else if (pt.kind == ty::Type::Kind::Array && pt.elem) {
        auto* at = llvm::dyn_cast<llvm::ArrayType>(ty);
        uint64_t n = at ? at->getNumElements() : 0;
        fmt += "[";
        for (uint64_t i = 0; i < n && i < 16; ++i) {
            if (i) fmt += ", ";
            llvm::Value* ep = builder->CreateConstGEP2_64(ty, addr, 0, i);
            if (depth < 4) dbgFormat(pt.elem->str(), ep, fmt, args, depth + 1); else fmt += "...";
        }
        if (n > 16) fmt += ", ...";
        fmt += "]";
    } else if (ty->isStructTy() && structFields.count(stripToStructKey(t)) && !unionFields.count(stripToStructKey(t)) &&
               !structLayout.count(stripToStructKey(t)) && !isPointerType(t)) {
        std::string sname = stripToStructKey(t);
        const auto& fields = structFields[sname];
        lit(sname); fmt += "{";
        for (size_t i = 0; i < fields.size(); ++i) {
            if (i) fmt += ", ";
            lit(fields[i].name); fmt += ": ";
            llvm::Value* fp = builder->CreateStructGEP(structTypes[sname], addr, (unsigned)i);
            if (depth < 4) dbgFormat(fields[i].type, fp, fmt, args, depth + 1); else fmt += "...";
        }
        fmt += "}";
    } else if (ty->isPointerTy()) {
        args.push_back(builder->CreateLoad(ty, addr));
        fmt += "%p";
    } else if (ty->isIntegerTy(32)) {
        lit(stripToStructKey(t)); fmt += "(%lld)";
        args.push_back(builder->CreateSExt(builder->CreateLoad(ty, addr), i64));
    } else {
        fmt += "<"; lit(stripToStructKey(t)); fmt += ">";
    }
}

llvm::Value* CodeGen::emitDbg(CallExpr* node) {
    std::string t = getExprEskiuType(node->args[0]);
    llvm::Value* v = evalForType(node->args[0], t);
    llvm::Value* slot = entryAlloca(v->getType(), nullptr, "dbg.val");
    builder->CreateStore(v, slot);
    std::string fmt;
    for (char c : "[dbg " + std::to_string(node->callee->line) + ":" + std::to_string(node->callee->col) + "] " + node->dbgText + " = ")
        fmt += c == '%' ? std::string("%%") : std::string(1, c);
    std::vector<llvm::Value*> args;
    dbgFormat(t, slot, fmt, args, 0);
    fmt += "\n";
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
    const llvm::Triple& tt = module->getTargetTriple();
    llvm::Value* err;
    if (tt.isOSWindows()) {
        llvm::FunctionCallee iob = module->getOrInsertFunction("__acrt_iob_func", ptrTy, i32);
        err = builder->CreateCall(iob, {builder->getInt32(2)});
    } else {
        const char* name = tt.isOSDarwin() ? "__stderrp" : "stderr";
        llvm::GlobalVariable* gv = module->getNamedGlobal(name);
        if (!gv) gv = new llvm::GlobalVariable(*module, ptrTy, false, llvm::GlobalValue::ExternalLinkage, nullptr, name);
        err = builder->CreateLoad(ptrTy, gv);
    }
    llvm::FunctionCallee fprintfFn = module->getOrInsertFunction("fprintf", llvm::FunctionType::get(i32, {ptrTy, ptrTy}, true));
    std::vector<llvm::Value*> callArgs{err, builder->CreateGlobalString(fmt)};
    callArgs.insert(callArgs.end(), args.begin(), args.end());
    createMaybeInvoke(fprintfFn.getFunctionType(), fprintfFn.getCallee(), callArgs, "");
    return builder->CreateLoad(v->getType(), slot);
}

void CodeGen::visit(CallExpr* node) {
    if (node->dbgBuiltin && node->args.size() == 1) { exprValueStack.push(emitDbg(node)); return; }
    // Variadic access: va_start(ap) / va_end(ap) — `ap` is a local va_list, whose
    // alloca is the pointer the intrinsics need.
    if (auto* bid = dynamic_cast<IdentExpr*>(node->callee.get())) {
        if ((bid->name == "va_start" || bid->name == "va_end") && node->args.size() == 1) {
            llvm::Value* ap = evaluateLValue(node->args[0]);
            llvm::Function* fn = getOrDeclareFunc(
                bid->name == "va_start" ? "llvm.va_start.p0" : "llvm.va_end.p0",
                llvm::Type::getVoidTy(*context), {llvm::PointerType::get(*context, 0)}, false);
            exprValueStack.push(builder->CreateCall(fn, {ap}));
            return;
        }
        if (bid->name == "exception_text" && node->args.empty() && !catchTextSlots.empty() && !lookupSymbol("exception_text")) {
            exprValueStack.push(builder->CreateLoad(llvm::PointerType::get(*context, 0), catchTextSlots.back(), "ex.text"));
            return;
        }
    }
    // Algebraic variant construction: `Circle(2.0)`, `Some(x)`.
    if (auto* vid = dynamic_cast<IdentExpr*>(node->callee.get())) {
        if (!lookupSymbol(vid->name) && adtVariants.count(vid->name)) {
            exprValueStack.push(buildVariant(vid->name, node->args));
            return;
        }
        // Bare generic-variant with inference: infer each type param from the arg
        // whose payload slot IS that param (the type checker has verified all are
        // determined), then build the instance.
        if (!lookupSymbol(vid->name) && genericVariants.count(vid->name)) {
            auto& gi = genericVariants[vid->name];
            EnumDecl* ge = genericEnumDecls[gi.first];
            const auto& payload = ge->payloads[gi.second];
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < payload.size() && i < node->args.size(); ++i)
                for (const auto& tp : ge->typeParams)
                    if (payload[i] == tp && !subs.count(tp)) {
                        std::string at = getExprEskiuType(node->args[i]);
                        if (!typeParamOverride.empty()) at = substType(at, typeParamOverride);
                        subs[tp] = at;
                    }
            std::vector<std::string> targs;
            for (const auto& tp : ge->typeParams) targs.push_back(subs.count(tp) ? subs[tp] : "int");
            std::string mangled = ensureEnumInst(gi.first, targs);
            std::map<std::string, std::string> sub2;
            for (size_t i = 0; i < ge->typeParams.size() && i < targs.size(); ++i) sub2[ge->typeParams[i]] = targs[i];
            std::vector<std::string> fts;
            for (const auto& ft : payload) fts.push_back(substType(ft, sub2));
            exprValueStack.push(buildEnumValue(structTypes[mangled], gi.second, fts, node->args));
            return;
        }
    }
    // Intrinsics: a call to an `intrinsic`-declared name lowers to inline IR
    // instead of a call. Gated on intrinsicNames (only populated when the
    // declaring module is imported), so a user function of the same name that
    // was *not* imported as an intrinsic is unaffected.
    if (auto* aid = dynamic_cast<IdentExpr*>(node->callee.get())) {
        const std::string& fn = aid->name;
        if (intrinsicNames.count(fn) && !lookupSymbol(fn)) {
            exprValueStack.push(lowerIntrinsicCall(fn, node));
            return;
        }
    }

    // Template function called without explicit type arguments: infer each type
    // parameter by structurally unifying every parameter type against the concrete
    // argument type. Covers bare params (T max<T>(T a, T b) → max(3,5)) and
    // composite ones (T List_get<T>(List<T>* self, int i) → List_get(&nums, i)).
    if (auto* id = dynamic_cast<IdentExpr*>(node->callee.get())) {
        auto tIt = funcTemplateDecls.find(id->name);
        if (tIt != funcTemplateDecls.end()) {
            FunctionDecl* fd = tIt->second;
            std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
            std::map<std::string, std::string> subs;
            // Structural parameters bind first; by-value `T a` deductions of one parameter
            // meet at their common integer type (as the type checker decides).
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j)
                if (!tps.count(tyq::strip(fd->params[j].first)))
                    unifyTypeParam(fd->params[j].first, getExprEskiuType(node->args[j]), tps, subs);
            std::map<std::string, std::string> byValue;
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                std::string tp = tyq::strip(fd->params[j].first);
                if (!tps.count(tp) || subs.count(tp)) continue;
                std::string at = getExprEskiuType(node->args[j]);
                if (at.empty() || at == "null") continue;
                std::map<std::string, std::string> one;
                unifyTypeParam(fd->params[j].first, at, tps, one);
                if (!one.count(tp)) continue;
                auto bv = byValue.find(tp);
                if (bv == byValue.end()) { byValue[tp] = one[tp]; continue; }
                std::string common = ty::rangeVarType(expandAlias(bv->second), expandAlias(one[tp]));
                if (!common.empty() && expandAlias(bv->second) != expandAlias(one[tp])) bv->second = common;
            }
            for (const auto& kv : byValue) subs[kv.first] = kv.second;
            std::vector<std::string> typeArgs;
            for (const auto& tpName : fd->typeParams) {
                auto sIt = subs.find(tpName);
                if (sIt == subs.end()) break;
                typeArgs.push_back(sIt->second);
            }
            if (typeArgs.size() == fd->typeParams.size()) {
                TemplateCallExpr tc(id->name, typeArgs, node->args);
                visit(&tc);
                return;
            }
        }
    }

    // Unified method/interface call: callee is MemberExpr
    if (auto member = dynamic_cast<MemberExpr*>(node->callee.get())) {
        std::string baseType = getExprEskiuType(member->base);
        // Pointer-vs-value base: a pointer-to-struct base supplies the receiver
        // by its *value* (the pointer — load it), a value struct by its *address*.
        // (Mirrors the field-access logic; robust to parameters now living in a
        // stack slot.)
        bool baseIsPtr = (!baseType.empty() && (baseType.front() == '*' || baseType.back() == '*'));
        baseType = stripToStructKey(baseType);

        // Interface vtable dispatch
        auto ifIt = ifaceMethodOrder.find(baseType);
        if (ifIt != ifaceMethodOrder.end()) {
            // An interface value IS the fat {data, vtable} struct; through a `*I` it is
            // loaded first (`pi.f()` calls `(*pi).f()`).
            llvm::Value* fat     = evaluateExpr(member->base);
            if (baseIsPtr) fat = builder->CreateLoad(ifaceFatPtrTypes[baseType], fat, "iface.val");
            llvm::Value* dataPtr = builder->CreateExtractValue(fat, {0}, "iface.data");
            llvm::Value* vtPtr   = builder->CreateExtractValue(fat, {1}, "iface.vt");
            const auto& order = ifIt->second;
            size_t idx = 0;
            for (; idx < order.size(); ++idx) if (order[idx] == member->member) break;
            if (idx == order.size())
                throw std::runtime_error("Interface '" + baseType + "' has no method '" + member->member + "'");
            llvm::StructType* vtType = ifaceVtableTypes[baseType];
            llvm::Value* fnGEP = builder->CreateStructGEP(vtType, vtPtr, idx);
            llvm::Value* fnPtr = builder->CreateLoad(llvm::PointerType::get(*context, 0), fnGEP);
            // Build the correct function type from stored method signature
            llvm::Type* retType = llvm::Type::getVoidTy(*context);
            auto& retTypes  = ifaceMethodReturnTypes[baseType];
            auto& paramLists = ifaceMethodParamEskiuTypes[baseType];

            // Evaluate args, coercing each to the method's declared param type — a vtable
            // call needs the same widening as a direct call, else the value (e.g. an i32)
            // won't match the vtable slot's signature (e.g. i64) and LLVM rejects the call.
            const std::vector<std::string>* iParams =
                (idx < paramLists.size()) ? &paramLists[idx] : nullptr;
            std::vector<llvm::Value*> iargs = {dataPtr};
            for (size_t ai = 0; ai < node->args.size(); ++ai) {
                llvm::Value* av = (iParams && ai < iParams->size())
                    ? evalForType(node->args[ai], (*iParams)[ai]) : evaluateExpr(node->args[ai]);
                if (iParams && ai < iParams->size())
                    av = coerceValue(av, getTypeFromString((*iParams)[ai]),
                                     eskiuUnsigned(getExprEskiuType(node->args[ai])));
                iargs.push_back(av);
            }
            if (idx < retTypes.size()) retType = getTypeFromString(retTypes[idx]);

            std::vector<llvm::Type*> paramLLVM = {llvm::PointerType::get(*context, 0)}; // self
            if (idx < paramLists.size())
                for (const auto& pt : paramLists[idx])
                    paramLLVM.push_back(getTypeFromString(pt));
            // Pad remaining args as ptr if count doesn't match (variadic safety)
            while (paramLLVM.size() < iargs.size())
                paramLLVM.push_back(llvm::PointerType::get(*context, 0));

            // Use sret if return type is a large aggregate
            bool iSret = needsSret(retType);
            llvm::Value* sretBuf = nullptr;
            if (iSret) {
                sretBuf = entryAlloca(retType, nullptr, "iface.sret");
                iargs.insert(iargs.begin(), sretBuf); // sret pointer goes first
                paramLLVM.insert(paramLLVM.begin(), llvm::PointerType::get(*context, 0));
                retType = llvm::Type::getVoidTy(*context);
            }

            auto* ftype = llvm::FunctionType::get(retType, paramLLVM, false);
            llvm::Value* call = createMaybeInvoke(ftype, fnPtr, iargs);
            if (iSret)
                exprValueStack.push(builder->CreateLoad(
                    llvm::cast<llvm::StructType>(getTypeFromString(retTypes[idx])), sretBuf));
            else
                exprValueStack.push(call);
            return;
        }

        // Struct method call
        std::string mangled = baseType + "_" + member->member;
        llvm::Function* mfunc = module->getFunction(mangled);
        if (!mfunc) mfunc = instantiateGenericMethod(baseType, member->member);
        if (mfunc) {
            // self: a value-struct receiver passes its address; a pointer receiver
            // passes the pointer it holds (loaded), not the address of its slot.
            llvm::Value* self = baseIsPtr ? evaluateExpr(member->base)
                                          : evaluateAddress(member->base);
            std::vector<llvm::Value*> margs = {self};
            // The method's param types; [0] is self.
            auto mpt = funcEskiuParamTypes.find(mangled);
            for (size_t ai = 0; ai < node->args.size(); ++ai) {
                bool has = mpt != funcEskiuParamTypes.end() && ai + 1 < mpt->second.size();
                margs.push_back(has ? evalForType(node->args[ai], mpt->second[ai + 1])
                                    : evaluateExpr(node->args[ai]));
            }
            // Widen/narrow each argument to the method's declared parameter type, like a
            // direct call does (`r.set(5)` against an int64 parameter passes an i32 otherwise).
            llvm::FunctionType* mfty = mfunc->getFunctionType();
            unsigned mbase = funcSretTypes.count(mangled) ? 1u : 0u;
            for (size_t ai = 0; ai < node->args.size(); ++ai) {
                unsigned pi = mbase + 1 + (unsigned)ai;
                if (pi >= mfty->getNumParams()) break;
                llvm::Type* pt = mfty->getParamType(pi);
                if (margs[ai + 1]->getType() != pt)
                    margs[ai + 1] = coerceValue(margs[ai + 1], pt,
                                                eskiuUnsigned(getExprEskiuType(node->args[ai])));
            }
            // A large struct return goes through the hidden sret pointer, and inside a
            // `try` the call must be an invoke, exactly as for a direct call.
            auto msret = funcSretTypes.find(mangled);
            if (msret != funcSretTypes.end()) {
                llvm::Value* sretAlloca = entryAlloca(msret->second, nullptr, "sret.tmp");
                margs.insert(margs.begin(), sretAlloca);
                createMaybeInvoke(mfty, mfunc, margs);
                exprValueStack.push(builder->CreateLoad(msret->second, sretAlloca));
            } else {
                exprValueStack.push(createMaybeInvoke(mfty, mfunc, margs));
            }
            return;
        }
        // A generic free function `S_m<T..>` on a generic struct instance (the sema
        // side is checkGenericMethodCall): lower `x.m(args)` as the call `S_m(&x, args)`
        // (`S_m(x, args)` for a pointer receiver), whose type arguments are inferred.
        if (FunctionDecl* gf = genericFreeMethod(baseType, member->member)) {
            bool selfPtr = tyq::isPtr(gf->params[0].first);
            ExprPtr recv = member->base;
            if (selfPtr && !baseIsPtr) {
                // `&x`, or for an rvalue receiver (`mk().get(0)`) the address of a
                // temporary holding it, passed through a hidden local.
                std::string recvTy = getExprEskiuType(member->base);
                llvm::Value* addr = evaluateAddress(member->base);
                std::string tmpName = "__recv." + std::to_string(recvTmpCount++);
                llvm::AllocaInst* slot = entryAlloca(llvm::PointerType::get(*context, 0), nullptr, tmpName);
                builder->CreateStore(addr, slot);
                defineSymbol(tmpName, slot);
                defineVarType(tmpName, "*" + recvTy);
                recv = std::make_shared<IdentExpr>(tmpName);
            }
            else if (!selfPtr && baseIsPtr) recv = std::make_shared<UnaryExpr>("*", member->base);
            recv->line = member->line; recv->col = member->col;
            std::vector<ExprPtr> cargs{recv};
            cargs.insert(cargs.end(), node->args.begin(), node->args.end());
            CallExpr call(std::make_shared<IdentExpr>(gf->name), cargs);
            call.line = node->line; call.col = node->col;
            visit(&call);
            return;
        }
        // Free-function constraint satisfaction: `t.m(x)` on a PRIMITIVE receiver
        // (a bounded type param T:Iface satisfied by a free fn, e.g. `int cmp(int,int)`
        // satisfies `Ord` for int) lowers to `m(t, x)` — the receiver is the first
        // argument, passed by value. Gated to scalar primitives so a mistyped struct
        // method still errors instead of silently hitting a same-named global fn.
        static const std::set<std::string> kScalarPrims = {
            "int","int8","int16","int32","int64","uint","uint8","uint16","uint32",
            "uint64","char","bool","float","double"};
        if (kScalarPrims.count(baseType)) {
            if (llvm::Function* ffunc = module->getFunction(member->member)) {
                std::vector<llvm::Value*> fargs = {evaluateExpr(member->base)};
                for (auto& arg : node->args) fargs.push_back(evaluateExpr(arg));
                llvm::FunctionType* fty = ffunc->getFunctionType();
                for (size_t i = 0; i < fargs.size() && i < fty->getNumParams(); ++i) {
                    llvm::Type* pt = fty->getParamType(i);
                    if (fargs[i]->getType() != pt) {
                        std::string srcTy = (i == 0) ? baseType
                                                     : getExprEskiuType(node->args[i - 1]);
                        fargs[i] = coerceValue(fargs[i], pt, eskiuUnsigned(srcTy));
                    }
                }
                exprValueStack.push(createMaybeInvoke(ffunc->getFunctionType(), ffunc, fargs));
                return;
            }
        }
        // Not a method: if o.member is a fn-pointer field, fall through to the
        // general indirect-call path (evaluateExpr(callee) yields the fat ptr).
        std::string ft = getExprEskiuType(node->callee);
        if (!(ft.size() > 3 && ft.substr(0, 3) == "fn("))
            throw std::runtime_error("Undefined method: " + baseType + "::" + member->member);
    }

    // A bare name that resolves to a function (and is not shadowed by a local
    // fn-pointer variable) is a direct call — use the function itself, not the
    // decayed closure fat pointer that evaluating it as a value would produce.
    llvm::Value* calleeVal = nullptr;
    if (auto ident = dynamic_cast<IdentExpr*>(node->callee.get())) {
        if (!lookupSymbol(ident->name))
            calleeVal = module->getFunction(ident->name);
    }
    if (!calleeVal) calleeVal = evaluateExpr(node->callee);
    if (!calleeVal) throw std::runtime_error("Call target is null");

    // Indirect call through a fat-pointer closure {fn_ptr, env_ptr}
    if (!llvm::isa<llvm::Function>(calleeVal)) {
        std::string eskiuType = getExprEskiuType(node->callee);
        ty::Type fnTy = ty::Type::parse(expandAlias(eskiuType));
        if (fnTy.isFn()) {
            // The signature comes from the parsed fn type, so nested fn types and
            // template args with commas (`fn(fn(int)->int, Pair<int,double>)->int`)
            // split correctly.
            std::vector<std::string> paramStrs;
            for (const auto& p : fnTy.params) paramStrs.push_back(p.str());
            std::vector<llvm::Type*> pts;
            llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
            pts.push_back(ptrTy); // env* always first
            for (const auto& ps : paramStrs) pts.push_back(getTypeFromString(ps));
            llvm::Type* retTy = getTypeFromString(fnTy.ret->str());
            llvm::FunctionType* fty = llvm::FunctionType::get(retTy, pts, false);

            // Extract fn_ptr and env_ptr from the fat pointer struct
            llvm::Value* fnPtr  = builder->CreateExtractValue(calleeVal, {0}, "fn.ptr");
            llvm::Value* envPtr = builder->CreateExtractValue(calleeVal, {1}, "env.ptr");

            // Each argument converts to its parameter type like a direct call's: an
            // interface param boxes, and numbers get full coercion (int -> float,
            // float <-> double), not just integer widening.
            std::vector<llvm::Value*> iargs = {envPtr};
            for (size_t i = 0; i < node->args.size(); ++i) {
                size_t pidx = i + 1;   // env pointer is param 0
                llvm::Value* av = i < paramStrs.size() ? evalForType(node->args[i], paramStrs[i])
                                                       : evaluateExpr(node->args[i]);
                if (pidx < pts.size() && av->getType() != pts[pidx])
                    av = coerceValue(av, pts[pidx],
                                     eskiuUnsigned(getExprEskiuType(node->args[i])));
                iargs.push_back(av);
            }
            // A void-returning call must not be given a name (LLVM forbids it).
            exprValueStack.push(createMaybeInvoke(
                fty, fnPtr, iargs, retTy->isVoidTy() ? "" : "fn.call"));
            return;
        }
        throw std::runtime_error("Call target is not a function");
    }
    llvm::Function* func = llvm::cast<llvm::Function>(calleeVal);

    // An extern with by-value aggregates: coerce the args to its Eskiu-level signature
    // (variadic extras get the C default promotions), then lower to the C ABI.
    // A C function pointer parameter takes the C address of a named function.
    auto fpIt = externFnPtrParams.find(func->getName().str());
    auto isCFnParam = [&](size_t i) {
        return fpIt != externFnPtrParams.end() && i < fpIt->second.size() && fpIt->second[i];
    };
    auto vaIt = externVaListParams.find(func->getName().str());
    auto isCVaParam = [&](size_t i) {
        return vaIt != externVaListParams.end() && i < vaIt->second.size() && vaIt->second[i];
    };
    auto abiIt = externAbi.find(func->getName().str());
    if (abiIt != externAbi.end()) {
        auto lparams = abiIt->second.logical->params();
        std::vector<llvm::Value*> cargs;
        for (size_t i = 0; i < node->args.size(); ++i) {
            llvm::Value* v = isCFnParam(i) ? evalCFnPointer(node->args[i])
                           : isCVaParam(i) ? evalCVaList(node->args[i]) : evaluateExpr(node->args[i]);
            bool uns = eskiuUnsigned(getExprEskiuType(node->args[i]));
            if (i < lparams.size()) {
                if (v->getType() != lparams[i]) v = coerceValue(v, lparams[i], uns);
            } else if (v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() < 32) {
                llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
                v = (uns || v->getType()->isIntegerTy(1)) ? builder->CreateZExt(v, i32)
                                                           : builder->CreateSExt(v, i32);
            } else if (v->getType()->isFloatTy()) {
                v = builder->CreateFPExt(v, llvm::Type::getDoubleTy(*context));
            }
            cargs.push_back(v);
        }
        exprValueStack.push(widenCResult(func->getName().str(), emitCAbiCall(func, abiIt->second, cargs)));
        return;
    }

    // Evaluate args, boxing structs as interfaces where the param type demands it
    std::vector<llvm::Value*> args;
    auto ptIt = funcEskiuParamTypes.find(func->getName().str());
    for (size_t i = 0; i < node->args.size(); ++i) {
        // A param that expects an interface boxes a struct pointer argument.
        if (isCFnParam(i))
            args.push_back(evalCFnPointer(node->args[i]));
        else if (isCVaParam(i))
            args.push_back(evalCVaList(node->args[i]));
        else if (ptIt != funcEskiuParamTypes.end() && i < ptIt->second.size())
            args.push_back(evalForType(node->args[i], ptIt->second[i]));
        else
            args.push_back(evaluateExpr(node->args[i]));
    }

    // Widen/truncate integer arguments to match function parameter types. If the
    // callee returns via sret, its real params start at index 1 (param 0 is the
    // hidden sret pointer, prepended to `args` only later), so align with that
    // offset — otherwise the first scalar arg is matched against the sret pointer
    // and an int literal is left unwidened (caught by the IR verifier).
    {
        auto fparams = func->getFunctionType()->params();
        unsigned pbase = funcSretTypes.count(func->getName().str()) ? 1u : 0u;
        for (size_t i = 0; i < args.size() && i + pbase < fparams.size(); ++i) {
            llvm::Type* pt = fparams[i + pbase];
            if (args[i]->getType() != pt) {
                // Full numeric coercion (int/float widen or narrow), not just int:
                // a `double` literal passed to a `float` parameter must narrow, else
                // the argument type mismatches the signature and the IR verifier
                // rejects the call. Non-numeric values pass through unchanged.
                bool uns = i < node->args.size() && eskiuUnsigned(getExprEskiuType(node->args[i]));
                args[i] = coerceValue(args[i], pt, uns);
            }
        }
    }

    // C default argument promotions for the variadic ("...") arguments: an
    // integer narrower than int widens to i32 (sign/zero per its signedness),
    // and a float widens to double. Without this, printf("%d", anInt8) reads a
    // full int from a byte-sized argument slot.
    if (func->getFunctionType()->isVarArg()) {
        auto argUnsigned = [&](size_t i) -> bool {
            if (i >= node->args.size()) return false;
            return eskiuUnsigned(getExprEskiuType(node->args[i]));
        };
        unsigned fixed = func->getFunctionType()->getNumParams();
        llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
        for (size_t i = fixed; i < args.size(); ++i) {
            llvm::Type* at = args[i]->getType();
            if (at->isIntegerTy() && at->getIntegerBitWidth() < 32) {
                // i1 (a bool / comparison result) is 0/1 — always zero-extend.
                bool uns = argUnsigned(i) || at->getIntegerBitWidth() == 1;
                args[i] = uns ? builder->CreateZExt(args[i], i32)
                              : builder->CreateSExt(args[i], i32);
            } else if (at->isFloatTy()) {
                args[i] = builder->CreateFPExt(args[i], llvm::Type::getDoubleTy(*context));
            }
        }
    }

    // sret: alloca for large struct return, pass as hidden arg 0, load result
    auto sretIt = funcSretTypes.find(func->getName().str());
    if (sretIt != funcSretTypes.end()) {
        llvm::Value* sretAlloca = entryAlloca(sretIt->second, nullptr, "sret.tmp");
        args.insert(args.begin(), sretAlloca);
        createMaybeInvoke(func->getFunctionType(), func, args);
        exprValueStack.push(builder->CreateLoad(sretIt->second, sretAlloca));
    } else {
        exprValueStack.push(widenCResult(func->getName().str(),
            createMaybeInvoke(func->getFunctionType(), func, args)));
    }
}

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
        throw std::runtime_error("Unknown template function: " + node->templateName);

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
    if (!func) throw std::runtime_error("Template instantiation failed: " + mangledName);

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
        args.insert(args.begin(), sretAlloca);
        createMaybeInvoke(func->getFunctionType(), func, args);
        exprValueStack.push(builder->CreateLoad(sretIt->second, sretAlloca));
    } else {
        exprValueStack.push(createMaybeInvoke(func->getFunctionType(), func, args));
    }
}
