// codegen_call_expr.cpp: the call expression visitor, visit(CallExpr) (split from codegen_call.cpp).
#include "codegen.h"
#include "../intrinsics.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"
#include "../sema/type.h"

void CodeGen::visit(CallExpr* node) {
    if (node->dbgBuiltin && node->args.size() == 1) { exprValueStack.push(emitDbg(node)); return; }
    // Variadic access: va_start(ap) / va_end(ap) — `ap` is a local va_list, whose
    // alloca is the pointer the intrinsics need.
    if (auto* bid = nodeCast<IdentExpr>(node->callee.get())) {
        if ((bid->name == "va_start" || bid->name == "va_end") && node->args.size() == 1) {
            llvm::Value* ap = evaluateLValue(node->args[0]);
            llvm::Function* fn = getOrDeclareFunc(
                bid->name == "va_start" ? "llvm.va_start.p0" : "llvm.va_end.p0",
                llvm::Type::getVoidTy(*context), {llvm::PointerType::get(*context, 0)}, false);
            exprValueStack.push(builder->CreateCall(fn, {ap}));
            return;
        }
        // The builtin, unless the program defines a function or variable of that name.
        if (bid->name == "exception_text" && node->args.empty() && !catchTextSlots.empty() &&
            !lookupSymbol("exception_text") && !funcEskiuReturnType.count("exception_text")) {
            exprValueStack.push(builder->CreateLoad(llvm::PointerType::get(*context, 0), catchTextSlots.back(), "ex.text"));
            return;
        }
        // `__exception_text_into(buf)`, emitted by the async lowering only: copies the text
        // (512 bytes with its NUL) into a frame field that outlives the handler's stack.
        if (bid->name == "__exception_text_into" && node->args.size() == 1 && !catchTextSlots.empty()) {
            llvm::Value* dst = evaluateExpr(node->args[0]);
            llvm::Value* src = builder->CreateLoad(llvm::PointerType::get(*context, 0), catchTextSlots.back(), "ex.text");
            exprValueStack.push(builder->CreateMemCpy(dst, llvm::MaybeAlign(1), src, llvm::MaybeAlign(1), 512));
            return;
        }
    }
    // Algebraic variant construction: `Circle(2.0)`, `Some(x)`.
    if (auto* vid = nodeCast<IdentExpr>(node->callee.get())) {
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
    if (auto* aid = nodeCast<IdentExpr>(node->callee.get())) {
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
    if (auto* id = nodeCast<IdentExpr>(node->callee.get())) {
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
    if (auto member = nodeCast<MemberExpr>(node->callee.get())) {
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
            failAt(node, "undefined method '" + member->member + "' on type '" + baseType + "'");
    }

    // A bare name that resolves to a function (and is not shadowed by a local
    // fn-pointer variable) is a direct call — use the function itself, not the
    // decayed closure fat pointer that evaluating it as a value would produce.
    llvm::Value* calleeVal = nullptr;
    if (auto ident = nodeCast<IdentExpr>(node->callee.get())) {
        if (!lookupSymbol(ident->name))
            calleeVal = module->getFunction(ident->name);
    }
    if (!calleeVal) calleeVal = evaluateExpr(node->callee);
    if (!calleeVal) failAt(node, "the called expression has no value");

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
        if (auto* ident = nodeCast<IdentExpr>(node->callee.get())) {
            std::string shadow = module->getFunction(ident->name) ? " (a variable named '" + ident->name +
                                                                        "' hides the function of that name)"
                                                                  : "";
            failAt(node->callee.get(), "'" + ident->name + "' is a variable of type '" + eskiuType +
                                           "', not a function" + shadow);
        }
        failAt(node->callee.get(), "the called expression has type '" + eskiuType + "', not a function type");
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
