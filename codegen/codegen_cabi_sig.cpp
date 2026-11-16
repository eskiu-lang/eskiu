// codegen_cabi_sig.cpp: C ABI signatures and attributes, extern calls, callback thunks, va_arg and C function pointers (split from codegen_cabi.cpp).
#include "codegen.h"
#include "../ast/type_qual.h"
#include "llvm/TargetParser/Triple.h"
#include <functional>
#include <set>
#include "codegen_cabi_util.h"

bool CodeGen::buildCAbiSig(llvm::FunctionType* logical, CAbiSig& sig) const {
    CAbiTarget tgt = cabiTarget();
    if (tgt == CAbiTarget::None) return false;
    bool any = isCAggregate(logical->getReturnType());
    for (llvm::Type* p : logical->params()) any = any || isCAggregate(p);
    if (!any) return false;
    sig.logical = logical;
    unsigned freeInt = 6, freeSSE = 8;
    llvm::Type* rt = logical->getReturnType();
    if (!rt->isVoidTy()) sig.ret = classifyCAbi(rt, true, tgt, freeInt, freeSSE);
    if (sig.ret.kind == CAbiArg::Sret && freeInt) --freeInt;
    for (llvm::Type* p : logical->params())
        sig.params.push_back(classifyCAbi(p, false, tgt, freeInt, freeSSE));
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    std::vector<llvm::Type*> irParams;
    if (sig.ret.kind == CAbiArg::Sret) irParams.push_back(ptrTy);
    for (size_t i = 0; i < sig.params.size(); ++i) {
        const CAbiArg& a = sig.params[i];
        switch (a.kind) {
        case CAbiArg::Direct: irParams.push_back(logical->getParamType((unsigned)i)); break;
        case CAbiArg::Coerce: irParams.push_back(a.ty); break;
        case CAbiArg::Expand:
            for (llvm::Type* e : llvm::cast<llvm::StructType>(a.ty)->elements()) irParams.push_back(e);
            break;
        default: irParams.push_back(ptrTy); break;
        }
    }
    llvm::Type* irRet = sig.ret.kind == CAbiArg::Sret ? llvm::Type::getVoidTy(*context)
                      : sig.ret.kind == CAbiArg::Coerce ? sig.ret.ty : rt;
    sig.lowered = llvm::FunctionType::get(irRet, irParams, logical->isVarArg());
    return true;
}

// The ABI attributes (sret / byval + alignment) at their lowered parameter indices.
void CodeGen::addCAbiAttrs(const CAbiSig& sig,
                           const std::function<void(unsigned, llvm::Attribute)>& add) const {
    llvm::LLVMContext& ctx = *context;
    unsigned idx = 0;
    if (sig.ret.kind == CAbiArg::Sret) {
        add(0, llvm::Attribute::getWithStructRetType(ctx, sig.ret.ty));
        add(0, llvm::Attribute::getWithAlignment(ctx, llvm::Align(sig.ret.align)));
        idx = 1;
    }
    for (const auto& a : sig.params) {
        if (a.stackAlign)
            add(idx, llvm::Attribute::getWithStackAlignment(ctx, llvm::Align(a.stackAlign)));
        if (a.kind == CAbiArg::ByVal) {
            add(idx, llvm::Attribute::getWithByValType(ctx, a.ty));
            add(idx, llvm::Attribute::getWithAlignment(ctx, llvm::Align(a.align)));
        }
        idx += a.kind == CAbiArg::Expand
             ? llvm::cast<llvm::StructType>(a.ty)->getNumElements() : 1;
    }
}

llvm::Function* CodeGen::declareCAbiExtern(const std::string& name, const CAbiSig& sig) {
    llvm::Function* fn = llvm::Function::Create(sig.lowered, llvm::Function::ExternalLinkage,
                                                name, module.get());
    addCAbiAttrs(sig, [&](unsigned i, llvm::Attribute a) { fn->addParamAttr(i, a); });
    externAbi[name] = sig;
    return fn;
}

// Reinterpret `v` as `to` through memory (clang's coerced load/store): store it into a
// slot big and aligned enough for both types, then load the other type back.
llvm::Value* CodeGen::cabiReinterpret(llvm::Value* v, llvm::Type* to) {
    if (v->getType() == to) return v;
    const llvm::DataLayout& DL = module->getDataLayout();
    llvm::Type* from = v->getType();
    llvm::Type* big = DL.getTypeAllocSize(from) >= DL.getTypeAllocSize(to) ? from : to;
    llvm::AllocaInst* slot = entryAlloca(big, nullptr, "cabi.coerce");
    slot->setAlignment(std::max(DL.getABITypeAlign(from), DL.getABITypeAlign(to)));
    if (DL.getTypeAllocSize(from) < DL.getTypeAllocSize(to))
        builder->CreateStore(llvm::Constant::getNullValue(to), slot);
    builder->CreateStore(v, slot);
    return builder->CreateLoad(to, slot);
}

llvm::Value* CodeGen::cabiToCoerced(llvm::Value* v, const CAbiArg& a) {
    if (!a.offset) return cabiReinterpret(v, a.ty);
    llvm::Type* wrap = llvm::StructType::get(*context,
        {llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), a.offset), a.ty}, /*isPacked=*/true);
    return builder->CreateExtractValue(cabiReinterpret(v, wrap), {1});
}

llvm::Value* CodeGen::cabiFromCoerced(llvm::Value* c, const CAbiArg& a, llvm::Type* logical) {
    if (!a.offset) return cabiReinterpret(c, logical);
    llvm::Type* wrap = llvm::StructType::get(*context,
        {llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), a.offset), a.ty}, /*isPacked=*/true);
    return cabiReinterpret(builder->CreateInsertValue(llvm::Constant::getNullValue(wrap), c, {1}), logical);
}

llvm::Value* CodeGen::emitCAbiCall(llvm::Function* fn, const CAbiSig& sig,
                                   const std::vector<llvm::Value*>& args, bool allowInvoke) {
    std::vector<llvm::Value*> ir;
    llvm::AllocaInst* sretSlot = nullptr;
    if (sig.ret.kind == CAbiArg::Sret) {
        sretSlot = entryAlloca(sig.ret.ty, nullptr, "cabi.sret");
        ir.push_back(sretSlot);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        llvm::Value* v = args[i];
        if (i >= sig.params.size()) { ir.push_back(v); continue; }   // variadic tail
        const CAbiArg& a = sig.params[i];
        switch (a.kind) {
        case CAbiArg::Direct: ir.push_back(v); break;
        case CAbiArg::Coerce: ir.push_back(cabiToCoerced(v, a)); break;
        case CAbiArg::Expand: {
            llvm::Value* pair = cabiReinterpret(v, a.ty);
            for (unsigned e = 0; e < llvm::cast<llvm::StructType>(a.ty)->getNumElements(); ++e)
                ir.push_back(builder->CreateExtractValue(pair, {e}));
            break;
        }
        default: {                                   // a caller-owned copy in memory
            llvm::AllocaInst* copy = entryAlloca(v->getType(), nullptr, "cabi.arg");
            copy->setAlignment(std::max(copy->getAlign(), llvm::Align(a.align)));
            builder->CreateStore(v, copy);
            ir.push_back(copy);
            break;
        }
        }
    }
    llvm::Value* call = allowInvoke ? createMaybeInvoke(sig.lowered, fn, ir)
                                    : builder->CreateCall(sig.lowered, fn, ir);
    if (auto* cb = llvm::dyn_cast<llvm::CallBase>(call))
        addCAbiAttrs(sig, [&](unsigned i, llvm::Attribute a) { cb->addParamAttr(i, a); });
    llvm::Type* rt = sig.logical->getReturnType();
    if (sig.ret.kind == CAbiArg::Sret) return builder->CreateLoad(rt, sretSlot);
    if (sig.ret.kind == CAbiArg::Coerce) return cabiFromCoerced(call, sig.ret, rt);
    return call;
}

// `export T f(...)`: C calls f by its name with the C convention. When that differs from
// the Eskiu one (a struct or union by value, a large result, a va_list), the Eskiu body
// becomes the internal `f.esk` and `f` is the C-signature thunk that calls it; Eskiu
// callers keep calling the body (the rename follows every use).
void CodeGen::emitExportEntries() {
    for (const std::string& name : exportedFns) {
        llvm::Function* fn = module->getFunction(name);
        if (!fn || fn->isDeclaration()) continue;
        llvm::Function* thunk = cabiCallbackThunk(fn);
        if (thunk == fn) continue;
        fn->setName(name + ".esk");
        fn->setLinkage(llvm::GlobalValue::InternalLinkage);
        thunk->setName(name);
        thunk->setLinkage(llvm::GlobalValue::ExternalLinkage);
    }
}

// A shared library that exports functions offers only those: every other definition
// with external linkage gets hidden visibility (ELF, Mach-O), or on COFF the exports are
// marked dllexport, which stops the linker exporting every symbol.
void CodeGen::hideNonExports(const llvm::Triple& triple) {
    std::set<std::string> exported(exportedFns.begin(), exportedFns.end());
    auto apply = [&](llvm::GlobalValue& gv) {
        if (gv.isDeclaration() || gv.hasLocalLinkage() || gv.hasAppendingLinkage()) return;
        if (gv.getName().starts_with("llvm.")) return;
        bool isExport = exported.count(gv.getName().str()) > 0;
        if (triple.isOSBinFormatCOFF()) {
            if (isExport) gv.setDLLStorageClass(llvm::GlobalValue::DLLExportStorageClass);
        } else if (!isExport) {
            gv.setVisibility(llvm::GlobalValue::HiddenVisibility);
        }
    };
    for (llvm::Function& fn : *module) apply(fn);
    for (llvm::GlobalVariable& gv : module->globals()) apply(gv);
}

// C calls an Eskiu function through a raw pointer (`(*void)f`): the thunk takes the
// lowered C signature, rebuilds the Eskiu-level values and calls `target` with the
// Eskiu convention (first-class aggregates, hidden result pointer for a large struct),
// then hands the result back the C way.
llvm::Function* CodeGen::cabiCallbackThunk(llvm::Function* target) {
    std::string name = target->getName().str();
    if (externAbi.count(name) || target->isVarArg()) return target;
    auto sretIt = funcSretTypes.find(name);
    bool eskSret = sretIt != funcSretTypes.end();
    llvm::FunctionType* fty = target->getFunctionType();
    std::vector<llvm::Type*> lps(fty->param_begin() + (eskSret ? 1 : 0), fty->param_end());
    llvm::Type* lret = eskSret ? sretIt->second : fty->getReturnType();
    // A `va_list` parameter comes from C the way C passes its `va_list` (see evalCVaList).
    std::vector<llvm::Type*> cps = lps;
    bool anyVa = false;
    for (llvm::Type*& p : cps) {
        auto* st = llvm::dyn_cast<llvm::StructType>(p);
        if (st && !st->isLiteral() && st->getName() == "__va_list") {
            p = llvm::PointerType::get(*context, 0);
            anyVa = true;
        }
    }
    llvm::FunctionType* cfty = llvm::FunctionType::get(lret, cps, false);
    CAbiSig sig;
    if (!buildCAbiSig(cfty, sig)) {
        if (!anyVa || cabiTarget() == CAbiTarget::None) return target;
        sig.logical = cfty;
        sig.lowered = cfty;
        sig.params.assign(cps.size(), CAbiArg{});
    }
    std::string tname = "__cabi_" + name;
    if (llvm::Function* have = module->getFunction(tname)) return have;
    llvm::Function* thunk = llvm::Function::Create(sig.lowered, llvm::Function::InternalLinkage,
                                                   tname, module.get());
    addCAbiAttrs(sig, [&](unsigned i, llvm::Attribute a) { thunk->addParamAttr(i, a); });
    if (sig.ret.kind == CAbiArg::Direct && !lret->isVoidTy() && funcEskiuReturnType.count(name)) {
        llvm::Attribute::AttrKind ext = cabiExtAttr(funcEskiuReturnType[name], lret);
        if (ext != llvm::Attribute::None) thunk->addRetAttr(ext);
    }

    llvm::IRBuilderBase::InsertPointGuard guard(*builder);
    builder->SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", thunk));
    auto ai = thunk->arg_begin();
    llvm::Value* sretOut = nullptr;
    if (sig.ret.kind == CAbiArg::Sret) sretOut = &*ai++;
    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < sig.params.size(); ++i) {
        const CAbiArg& a = sig.params[i];
        switch (a.kind) {
        case CAbiArg::Direct:
            args.push_back(cps[i] != lps[i] ? cVaListToEskiu(&*ai++, lps[i]) : &*ai++);
            break;
        case CAbiArg::Coerce: args.push_back(cabiFromCoerced(&*ai++, a, lps[i])); break;
        case CAbiArg::Expand: {
            auto* st = llvm::cast<llvm::StructType>(a.ty);
            llvm::Value* pair = llvm::UndefValue::get(st);
            for (unsigned e = 0; e < st->getNumElements(); ++e)
                pair = builder->CreateInsertValue(pair, &*ai++, {e});
            args.push_back(cabiReinterpret(pair, lps[i]));
            break;
        }
        default: args.push_back(builder->CreateLoad(lps[i], &*ai++)); break;
        }
    }
    llvm::Value* r = nullptr;
    if (eskSret) {
        llvm::AllocaInst* tmp = entryAlloca(lret, nullptr, "cabi.res");
        args.insert(args.begin(), tmp);
        builder->CreateCall(target, args);
        r = builder->CreateLoad(lret, tmp);
    } else {
        r = builder->CreateCall(target, args);
    }
    if (sig.ret.kind == CAbiArg::Sret) {
        builder->CreateStore(r, sretOut);
        builder->CreateRetVoid();
    } else if (lret->isVoidTy()) {
        builder->CreateRetVoid();
    } else {
        builder->CreateRet(sig.ret.kind == CAbiArg::Coerce ? cabiToCoerced(r, sig.ret) : r);
    }
    return thunk;
}

// A `va_list` argument of a C function (vprintf, vsnprintf, ...), passed the way C passes
// its `va_list`: on x86-64 System V and AArch64 outside Darwin (and Windows) it is an
// array/struct type that goes by address; elsewhere (Darwin AArch64, Windows x64, 32-bit
// ARM) it is a `char*`, the pointer `llvm.va_start` stores at offset 0 of the storage.
llvm::Value* CodeGen::evalCVaList(const ExprPtr& arg) {
    llvm::Triple t(module->getTargetTriple());
    bool byAddr = !t.isOSWindows() && (t.getArch() == llvm::Triple::x86_64
                                      || (t.isAArch64() && !t.isOSDarwin()));
    llvm::Type* vaTy = getTypeFromString("va_list");
    if (byAddr) {
        if (auto* id = nodeCast<IdentExpr>(arg.get()))
            if (lookupSymbol(id->name)) return evaluateLValue(arg);
        llvm::AllocaInst* copy = entryAlloca(vaTy, nullptr, "va.arg");
        builder->CreateStore(evaluateExpr(arg), copy);
        return copy;
    }
    if (auto* id = nodeCast<IdentExpr>(arg.get()))
        if (lookupSymbol(id->name))
            return builder->CreateLoad(llvm::PointerType::get(*context, 0), evaluateLValue(arg));
    return builder->CreateExtractValue(evaluateExpr(arg), {0});
}

// The C integer types an extern spells by name (<ctypes>): size_t, ssize_t and c_long
// are int64 in Eskiu, and in the extern's declaration the target's C types, 32 bits
// wide on 32-bit targets (and c_long on Windows). 'z', 's', 'l', or 0 for anything else.
char CodeGen::cIntKind(const std::string& spelling) {
    std::string t = spelling;
    if (t.rfind("const ", 0) == 0) t = t.substr(6);
    if (t == "size_t") return 'z';
    if (t == "ssize_t") return 's';
    if (t == "c_long") return 'l';
    return 0;
}

llvm::Type* CodeGen::cIntType(char kind) {
    llvm::Triple t(module->getTargetTriple());
    unsigned ptrBits = module->getDataLayout().getPointerSizeInBits();
    unsigned bits = kind == 'l' && t.isOSWindows() ? 32 : ptrBits;
    return llvm::Type::getIntNTy(*context, bits);
}

// The int64 Eskiu sees for the result of an extern whose C result is narrower.
llvm::Value* CodeGen::widenCResult(const std::string& name, llvm::Value* v) {
    auto it = externRetExt.find(name);
    if (it == externRetExt.end() || !v->getType()->isIntegerTy() || v->getType()->isIntegerTy(64))
        return v;
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    return it->second == 'z' ? builder->CreateZExt(v, i64) : builder->CreateSExt(v, i64);
}

// The Eskiu `va_list` value of a `va_list` C passed in: on x86-64 System V and AArch64
// outside Darwin and Windows the C `va_list` (24 or 32 bytes) at that address, copied
// into the front of the Eskiu storage; elsewhere the `char*` itself, at offset 0.
llvm::Value* CodeGen::cVaListToEskiu(llvm::Value* c, llvm::Type* vaTy) {
    llvm::Triple t(module->getTargetTriple());
    bool x86 = t.getArch() == llvm::Triple::x86_64;
    bool byAddr = !t.isOSWindows() && (x86 || (t.isAArch64() && !t.isOSDarwin()));
    if (!byAddr) return builder->CreateInsertValue(llvm::Constant::getNullValue(vaTy), c, {0});
    llvm::AllocaInst* slot = entryAlloca(vaTy, nullptr, "va.in");
    builder->CreateStore(llvm::Constant::getNullValue(vaTy), slot);
    builder->CreateMemCpy(slot, llvm::MaybeAlign(8), c, llvm::MaybeAlign(8), x86 ? 24 : 32);
    return builder->CreateLoad(vaTy, slot);
}

// `va_arg<T>(ap)`: the next variadic argument, read from the `va_list` storage at `ap`.
// On AArch64 outside Darwin and Windows the `va_list` is the AAPCS64 struct
// {__stack, __gr_top, __vr_top, __gr_offs, __vr_offs}, which LLVM's `va_arg` does not
// expand, so the read is spelled out as clang does (AArch64ABIInfo::EmitAAPCSVAArg): an
// integer or pointer comes from the general-register save area and a float or double from
// the FP/SIMD one while the offset is negative, then from the stack in 8-byte slots.
// Other targets keep the `va_arg` instruction (a `char*` list, or x86-64 System V, which
// LLVM expands for the scalars `va_arg` accepts).
llvm::Value* CodeGen::emitVaArg(llvm::Value* ap, llvm::Type* ty) {
    llvm::Triple t(module->getTargetTriple());
    bool isFp = ty->isFloatingPointTy();
    if (!t.isAArch64() || t.isOSDarwin() || t.isOSWindows() || (!isFp && !ty->isIntOrPtrTy()))
        return builder->CreateVAArg(ap, ty, "va.arg");
    const llvm::DataLayout& dl = module->getDataLayout();
    uint64_t size = dl.getTypeAllocSize(ty);
    bool be = dl.isBigEndian();
    llvm::Type* i8 = llvm::Type::getInt8Ty(*context);
    llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
    llvm::Type* ptr = llvm::PointerType::get(*context, 0);
    llvm::Type* vaTy = getTypeFromString("va_list");
    unsigned offsField = isFp ? 4 : 3, topField = isFp ? 2 : 1;
    int slot = isFp ? 16 : 8;
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* maybeReg = llvm::BasicBlock::Create(*context, "va.maybe_reg", fn);
    llvm::BasicBlock* inReg = llvm::BasicBlock::Create(*context, "va.in_reg", fn);
    llvm::BasicBlock* onStack = llvm::BasicBlock::Create(*context, "va.on_stack", fn);
    llvm::BasicBlock* end = llvm::BasicBlock::Create(*context, "va.end", fn);
    llvm::Value* offsP = builder->CreateStructGEP(vaTy, ap, offsField, "va.offs_p");
    llvm::Value* offs = builder->CreateLoad(i32, offsP, "va.offs");
    builder->CreateCondBr(builder->CreateICmpSGE(offs, llvm::ConstantInt::get(i32, 0)), onStack, maybeReg);
    builder->SetInsertPoint(maybeReg);
    llvm::Value* next = builder->CreateAdd(offs, llvm::ConstantInt::get(i32, slot), "va.next");
    builder->CreateStore(next, offsP);
    builder->CreateCondBr(builder->CreateICmpSLE(next, llvm::ConstantInt::get(i32, 0)), inReg, onStack);
    builder->SetInsertPoint(inReg);
    llvm::Value* top = builder->CreateLoad(ptr, builder->CreateStructGEP(vaTy, ap, topField), "va.top");
    llvm::Value* regAddr = builder->CreateGEP(i8, top, builder->CreateSExt(offs, llvm::Type::getInt64Ty(*context)), "va.reg_addr");
    if (be && size < (uint64_t)slot)
        regAddr = builder->CreateConstGEP1_64(i8, regAddr, slot - size);
    builder->CreateBr(end);
    builder->SetInsertPoint(onStack);
    llvm::Value* stackP = builder->CreateStructGEP(vaTy, ap, 0, "va.stack_p");
    llvm::Value* stack = builder->CreateLoad(ptr, stackP, "va.stack");
    builder->CreateStore(builder->CreateConstGEP1_64(i8, stack, 8), stackP);
    llvm::Value* stackAddr = stack;
    if (be && size < 8) stackAddr = builder->CreateConstGEP1_64(i8, stack, 8 - size);
    builder->CreateBr(end);
    builder->SetInsertPoint(end);
    llvm::PHINode* addr = builder->CreatePHI(ptr, 2, "va.addr");
    addr->addIncoming(regAddr, inReg);
    addr->addIncoming(stackAddr, onStack);
    return builder->CreateLoad(ty, addr, "va.arg");
}

// An argument for a C function pointer parameter (an extern's fn-typed parameter): the
// C address of the named top-level function (through its C-ABI thunk when needed), or
// null. The type checker rejects anything else (a closure value has an environment).
llvm::Value* CodeGen::evalCFnPointer(const ExprPtr& arg) {
    if (auto* id = nodeCast<IdentExpr>(arg.get()))
        if (!lookupSymbol(id->name))
            if (llvm::Function* fn = module->getFunction(id->name)) return cabiCallbackThunk(fn);
    llvm::Value* v = evaluateExpr(arg);
    if (!v->getType()->isPointerTy())
        throw std::runtime_error("a C function pointer argument must name a top-level function");
    return v;
}
