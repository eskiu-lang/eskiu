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
        auto* mf = nodeCast<FunctionDecl>(m.get());
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
    inst->sourceFile = mf->sourceFile;
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
