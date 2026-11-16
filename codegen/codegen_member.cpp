// codegen_member.cpp: members, bitfields, casts, literals, identifiers, sizeof and lvalues (split from codegen_expr.cpp).
#include "codegen.h"
#include "../sema/type.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

std::string CodeGen::stripToStructKey(std::string baseType) {
    // The pointer decoration comes first in a `*struct:T` spelling, so strip it before
    // the `struct:` tag.
    if (!baseType.empty() && baseType.front() == '?') baseType = baseType.substr(1);
    while (!baseType.empty() && baseType.front() == '*') baseType = baseType.substr(1);
    if (baseType.size() > 7 && baseType.substr(0, 7) == "struct:") baseType = baseType.substr(7);
    while (!baseType.empty() && baseType.back()  == '*') baseType.pop_back();
    if (typeAliases.count(baseType)) return stripToStructKey(expandAlias(baseType));
    if (baseType.find('<') != std::string::npos) {
        auto [tn, args] = splitTemplateType(baseType);
        ensureTemplateInstantiated(mangleTemplate(baseType), tn, args);
        baseType = mangleTemplate(baseType);
    } else if (!structTypes.count(baseType)) {
        // A generic instance named only by its mangled form (the type of a call result
        // the type checker instantiated): build it now.
        auto ia = templateInstanceArgs.find(baseType);
        if (ia != templateInstanceArgs.end()) {
            auto inst = ia->second;
            if (templateDecls.count(inst.first)) ensureTemplateInstantiated(baseType, inst.first, inst.second);
            else if (genericEnumDecls.count(inst.first)) ensureEnumInst(inst.first, inst.second);
        }
    }
    return baseType;
}

std::string CodeGen::structBaseTypeOf(const ExprPtr& base) {
    return stripToStructKey(getExprEskiuType(base));
}

void CodeGen::visit(MemberExpr* node) {
    // Slice `.len`: read the fat pointer's length field (i64).
    if (node->member == "len" && ty::Type::parse(getExprEskiuType(node->base)).kind == ty::Type::Kind::Slice) {
        exprValueStack.push(builder->CreateExtractValue(evaluateExpr(node->base), {1}));
        return;
    }

    std::string baseType = structBaseTypeOf(node->base);

    // A pointer-to-struct base is dereferenced via its value; a value-struct
    // base via its address (see the matching logic in evaluateLValue).
    std::string rawBaseTy = getExprEskiuType(node->base);
    bool baseIsPtr = (!rawBaseTy.empty() && (rawBaseTy.front() == '*' || rawBaseTy.back() == '*'));
    // A struct-valued rvalue base (a call result, `a + b`, a struct literal, a field of
    // one) has no address: evaluateAddress materializes it into a temporary to GEP.
    auto baseAddr = [&]() -> llvm::Value* {
        if (baseIsPtr) return safeNonNull(evaluateExpr(node->base));
        return evaluateAddress(node->base);
    };

    // Bitfield-layout struct: physical slot map (handles bitfields and the
    // non-bitfield fields whose physical index differs from the logical one).
    auto lit = structLayout.find(baseType);
    if (lit != structLayout.end()) {
        auto sit = lit->second.find(node->member);
        if (sit == lit->second.end())
            throw std::runtime_error("Struct '" + baseType + "' has no field '" + node->member + "'");
        const BitfieldSlot& slot = sit->second;
        llvm::Value* basePtr = baseAddr();
        llvm::Value* gep = layoutFieldAddr(baseType, basePtr, slot, node->member);
        if (!slot.isBitfield) {
            exprValueStack.push(volLoad(builder->CreateLoad(slot.storageType, gep, node->member), node));
            return;
        }
        exprValueStack.push(bitfieldReadValue(loadBitfieldFrom(gep, slot, volatileRooted(node)), baseType, node->member));
        return;
    }

    auto fit = structFields.find(baseType);
    if (fit == structFields.end())
        throw std::runtime_error("Unknown struct type in member access: '" + baseType + "'");

    const auto& fields = fit->second;
    bool isUnion = unionFields.count(baseType) > 0;
    if (long fi = fieldIndexOf(baseType, fields, node->member); fi >= 0) {
        size_t i = (size_t)fi;
        {
            llvm::Value* basePtr = baseAddr();
            llvm::Type*  fieldTy = getTypeFromString(fields[i].type);
            llvm::Value* ptr;
            if (isUnion) {
                // Union: all fields at offset 0 — base ptr is the field ptr
                ptr = unionMemberPtr(basePtr);
            } else {
                ptr = builder->CreateStructGEP(structTypes[baseType], basePtr, i, node->member);
            }
            exprValueStack.push(volLoad(builder->CreateLoad(fieldTy, ptr, node->member), node));
            return;
        }
    }
    throw std::runtime_error("Struct/union '" + baseType + "' has no field '" + node->member + "'");
}

// The storage word is read and written as `accessType` (the declared type unless a
// packed struct's field spans an odd byte range); the value is in the declared type.
// A `volatile` root (`vol`) makes the word's load (and store) volatile.
static llvm::Value* loadWord(llvm::IRBuilder<>& b, llvm::Type* at, llvm::Value* p, unsigned align,
                             bool vol) {
    llvm::LoadInst* ld = align ? b.CreateAlignedLoad(at, p, llvm::MaybeAlign(align)) : b.CreateLoad(at, p);
    ld->setVolatile(vol);
    return ld;
}

llvm::Value* CodeGen::loadBitfieldFrom(llvm::Value* wordPtr, const BitfieldSlot& slot, bool vol) {
    llvm::Type* sty = slot.storageType;
    llvm::Type* aty = slot.accessType ? slot.accessType : sty;
    llvm::Value* word = loadWord(*builder, aty, wordPtr, slot.accessAlign, vol);
    llvm::Value* shifted = slot.bitOffset
        ? builder->CreateLShr(word, llvm::ConstantInt::get(aty, slot.bitOffset)) : word;
    if (aty != sty) shifted = builder->CreateZExtOrTrunc(shifted, sty);
    uint64_t mask = (slot.bitWidth >= 64) ? ~0ULL : ((1ULL << slot.bitWidth) - 1);
    llvm::Value* masked = builder->CreateAnd(shifted, llvm::ConstantInt::get(sty, mask));
    if (slot.isSigned && slot.bitWidth < sty->getIntegerBitWidth()) {
        unsigned sh = sty->getIntegerBitWidth() - slot.bitWidth;
        masked = builder->CreateAShr(builder->CreateShl(masked, sh), sh);
    }
    return masked;
}

void CodeGen::storeBitfieldInto(llvm::Value* wordPtr, const BitfieldSlot& slot,
                                llvm::Value* val, bool unsignedSrc, bool vol) {
    llvm::Type* sty = slot.storageType;  // integer storage word
    if (val->getType() != sty) {
        if (val->getType()->isIntegerTy())
            val = val->getType()->getIntegerBitWidth() > sty->getIntegerBitWidth()
                ? builder->CreateTrunc(val, sty)
                : unsignedSrc ? builder->CreateZExt(val, sty) : builder->CreateSExt(val, sty);
        else if (val->getType()->isFloatingPointTy())
            val = emitFloatToInt(val, sty, false);
    }
    llvm::Type* aty = slot.accessType ? slot.accessType : sty;
    llvm::Value* word = loadWord(*builder, aty, wordPtr, slot.accessAlign, vol);
    uint64_t mask = (slot.bitWidth >= 64) ? ~0ULL : ((1ULL << slot.bitWidth) - 1);
    llvm::Value* fieldMask = builder->CreateShl(llvm::ConstantInt::get(aty, mask), slot.bitOffset);
    llvm::Value* cleared  = builder->CreateAnd(word, builder->CreateNot(fieldMask));
    llvm::Value* vMasked  = builder->CreateAnd(val, llvm::ConstantInt::get(sty, mask));
    if (aty != sty) vMasked = builder->CreateZExtOrTrunc(vMasked, aty);
    llvm::Value* vShifted = slot.bitOffset
        ? builder->CreateShl(vMasked, llvm::ConstantInt::get(aty, slot.bitOffset)) : vMasked;
    llvm::StoreInst* st = builder->CreateStore(builder->CreateOr(cleared, vShifted), wordPtr);
    if (slot.accessAlign) st->setAlignment(llvm::Align(slot.accessAlign));
    st->setVolatile(vol);
}

void CodeGen::storeBitfield(MemberExpr* m, llvm::Value* val) {
    const BitfieldSlot* slot = nullptr;
    llvm::Value* gep = bitfieldWordPtr(m, slot);
    storeBitfieldInto(gep, *slot, val, false, volatileRooted(m));
}

llvm::Value* CodeGen::bitfieldWordPtr(MemberExpr* m, const BitfieldSlot*& slotOut) {
    std::string baseType = structBaseTypeOf(m->base);
    const BitfieldSlot& slot = structLayout[baseType][m->member];
    slotOut = &slot;
    // A pointer-to-struct base's address is the pointer's VALUE (evaluateExpr), not the
    // lvalue slot holding the pointer — mirrors the read path's baseAddr. The old code
    // used evaluateLValue for both, so a `*Struct` bitfield write hit the pointer's own
    // stack slot instead of the pointee.
    std::string rawBaseTy = getExprEskiuType(m->base);
    bool baseIsPtr = (!rawBaseTy.empty() && (rawBaseTy.front() == '*' || rawBaseTy.back() == '*'));
    llvm::Value* basePtr = baseIsPtr ? safeNonNull(evaluateExpr(m->base)) : evaluateLValue(m->base);
    return layoutFieldAddr(baseType, basePtr, slot);
}

void CodeGen::visit(CastExpr* node) {
    // `(void)e` evaluates `e` for its side effects and discards the value (sema only allows
    // it where no value is needed).
    if (node->targetType == "void") {
        evaluateExpr(node->expr);
        exprValueStack.push(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), 0));
        return;
    }
    llvm::Type* targetType = getTypeFromString(node->targetType);

    // Casting a top-level function name to a pointer type yields its RAW C
    // function pointer — the bare symbol address, not the {fn, env} closure fat
    // pointer the name would otherwise decay to. This is how an Eskiu function is
    // handed to a C API as a callback (e.g. OpenSSL's ALPN select callback); one taking
    // or returning a struct by value is reached through its C-ABI thunk.
    if (targetType->isPointerTy()) {
        if (auto* id = nodeCast<IdentExpr>(node->expr.get())) {
            if (!lookupSymbol(id->name)) {               // not shadowed by a variable
                if (llvm::Function* fn = module->getFunction(id->name)) {
                    exprValueStack.push(cabiCallbackThunk(fn));
                    return;
                }
            }
        }
    }

    llvm::Value* val = evaluateExpr(node->expr);

    llvm::Value* result = nullptr;

    if (val->getType() == targetType) {
        result = val;
    } else if (targetType->isIntegerTy(1)) {
        // Conversion to bool is `!= 0` (C `_Bool`), never a truncation: (bool)2,
        // (bool)256 and (bool)0.5 are all true.
        if (val->getType()->isFloatingPointTy() || val->getType()->isPointerTy() ||
            val->getType()->isIntegerTy())
            result = emitTruthy(val);
        else
            throw std::runtime_error("Cannot cast between these types");
    } else if (val->getType()->isIntegerTy() && targetType->isIntegerTy()) {
        // Integer to integer
        unsigned srcWidth = llvm::cast<llvm::IntegerType>(val->getType())->getBitWidth();
        unsigned dstWidth = llvm::cast<llvm::IntegerType>(targetType)->getBitWidth();
        if (srcWidth < dstWidth) {
            // Widen per the SOURCE's signedness: an unsigned source (uint*/char/bool)
            // zero-extends — e.g. (int)(uint8)255 is 255, not -1.
            bool uns = eskiuUnsigned(getExprEskiuType(node->expr));
            result = uns ? builder->CreateZExt(val, targetType)
                         : builder->CreateSExt(val, targetType);
        } else {
            result = builder->CreateTrunc(val, targetType);
        }
    } else if (val->getType()->isIntegerTy() && targetType->isFloatingPointTy()) {
        result = intToFloat(val, targetType, eskiuUnsigned(getExprEskiuType(node->expr)));
    } else if (val->getType()->isFloatingPointTy() && targetType->isIntegerTy()) {
        // float→int: an unsigned target needs FPToUI, else a value above the signed max
        // (e.g. (uint32)3e9) saturates to the signed max instead of the true value.
        result = emitFloatToInt(val, targetType, eskiuUnsigned(node->targetType));
    } else if (val->getType()->isFloatingPointTy() && targetType->isFloatingPointTy()) {
        result = builder->CreateFPCast(val, targetType);
    } else if (val->getType()->isPointerTy() && targetType->isIntegerTy()) {
        result = builder->CreatePtrToInt(val, targetType);
    } else if (val->getType()->isIntegerTy() && targetType->isPointerTy()) {
        result = builder->CreateIntToPtr(val, targetType);
    } else if (val->getType()->isPointerTy() && targetType->isPointerTy()) {
        result = val; // opaque pointers: ptr == ptr, no bitcast needed
    } else {
        throw std::runtime_error("Cannot cast between these types");
    }

    exprValueStack.push(result);
}

void CodeGen::visit(LiteralExpr* node) {
    llvm::Value* result = nullptr;

    switch (node->kind) {
        case LiteralExpr::Kind::INT: {
            // base 0 = auto (dec/hex/oct). Materialize as i64 when the value
            // does not fit in a *signed* 32-bit int, so large literals are not
            // truncated. A literal in [2^31, 2^32) fits u32 but not i32; keeping
            // it i32 would set the high bit and then sign-extend to a negative
            // i64 on assignment, so it must be i64 (matches the self-host).
            unsigned long long uval;
            bool wide;
            try {
                long long sval = std::stoll(node->value, nullptr, 0);
                uval = (unsigned long long)sval;
                wide = (sval < -2147483648LL || sval > 2147483647LL);
            } catch (const std::out_of_range&) {
                uval = std::stoull(node->value, nullptr, 0); // e.g. large uint64 literal
                wide = true;
            }
            llvm::Type* ity = wide ? llvm::Type::getInt64Ty(*context)
                                   : llvm::Type::getInt32Ty(*context);
            result = constIntBits(ity, uval);
            break;
        }
        case LiteralExpr::Kind::FLOAT: {
            // strtod (not stod): a denormal (1e-320) keeps its value and an overflow
            // (1e400) is inf, as in C, instead of an out-of-range exception.
            double val = std::strtod(node->value.c_str(), nullptr);
            result = llvm::ConstantFP::get(llvm::Type::getDoubleTy(*context), val);
            break;
        }
        case LiteralExpr::Kind::STRING: {
            result = builder->CreateGlobalString(node->value);
            break;
        }
        case LiteralExpr::Kind::BOOL: {
            bool val = node->value == "true";
            result = llvm::ConstantInt::get(llvm::Type::getInt1Ty(*context), val);
            break;
        }
        case LiteralExpr::Kind::NULL_VAL: {
            auto ptrType = llvm::PointerType::get(*context, 0);
            result = llvm::ConstantPointerNull::get(ptrType);
            break;
        }
        case LiteralExpr::Kind::CHAR: {
            char val = node->value.empty() ? 0 : node->value[0];
            result = constIntBits(llvm::Type::getInt8Ty(*context), (uint8_t)val);
            break;
        }
    }

    exprValueStack.push(result);
}

void CodeGen::visit(IdentExpr* node) {
    // Bare enum member, e.g. `Red` — an i32 constant.
    if (!lookupSymbol(node->name)) {
        auto ec = enumConstants.find(node->name);
        if (ec != enumConstants.end()) {
            exprValueStack.push(llvm::ConstantInt::get(
                llvm::Type::getInt32Ty(*context), ec->second, /*isSigned=*/true));
            return;
        }
        // Bare algebraic variant with no payload, e.g. `None`.
        if (adtVariants.count(node->name)) {
            exprValueStack.push(buildVariant(node->name, {}));
            return;
        }
    }

    // Look up variable
    llvm::Value* val = lookupSymbol(node->name);

    // A numeric `const` reads as its folded value (so it stays a constant expression).
    if (val) {
        auto cv = constValueOf.find(val);
        if (cv != constValueOf.end()) { exprValueStack.push(cv->second); return; }
    } else if (constEvalDepth > 0) {
        // Folding a top-level constant before the globals are emitted: by name.
        auto cg = constGlobalValues.find(node->name);
        if (cg != constGlobalValues.end()) { exprValueStack.push(cg->second); return; }
    }

    if (!val) {
        // A bare function name used as a value decays to a closure fat pointer.
        if (auto* fn = module->getFunction(node->name)) {
            exprValueStack.push(makeFunctionPointer(fn));
            return;
        }
    }

    if (!val) {
        throw std::runtime_error("Undefined variable or function: " + node->name);
    }

    llvm::Value* result = nullptr;

    bool vol = volatileVars.count(node->name) > 0;
    if (llvm::isa<llvm::AllocaInst>(val)) {
        auto* inst = builder->CreateLoad(
            llvm::cast<llvm::AllocaInst>(val)->getAllocatedType(), val);
        inst->setVolatile(vol);
        result = inst;
    } else if (llvm::isa<llvm::GlobalVariable>(val)) {
        auto* gv = llvm::cast<llvm::GlobalVariable>(val);
        auto* inst = builder->CreateLoad(gv->getValueType(), gv);
        inst->setVolatile(vol);
        result = inst;
    } else {
        // Function argument or function pointer
        result = val;
    }

    exprValueStack.push(result);
}

llvm::Value* CodeGen::evaluateExpr(const ExprPtr& expr) {
    expr->accept(this);
    llvm::Value* result = exprValueStack.top();
    exprValueStack.pop();
    return result;
}

void CodeGen::visit(SizeofExpr* node) {
    // Sema rewrites `sizeof(var)` to the variable's type, except in a generic body
    // (its nodes are shared by the instances): resolve the variable here, per instance.
    std::string tn = node->typeName;
    if (node->operand) tn = getExprEskiuType(node->operand);   // a generic body: never evaluated
    else if (!typeParamOverride.count(tn) && !structTypes.count(tn) && !typeAliases.count(tn) &&
        !enumTypes.count(tn)) {
        std::string vt = lookupVarType(tn);
        if (!vt.empty()) tn = vt;
    }
    llvm::Type* ty   = getTypeFromString(tn);
    uint64_t    size = module->getDataLayout().getTypeAllocSize(ty);
    exprValueStack.push(
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), size));
}

llvm::Value* CodeGen::evaluateLValue(const ExprPtr& expr) {
    if (auto ident = nodeCast<IdentExpr>(expr.get())) {
        llvm::Value* val = lookupSymbol(ident->name);
        if (!val) throw std::runtime_error("Undefined variable: " + ident->name);
        return val;
    }

    // Dereference as lvalue: *ptr = val — load the pointer value, use as the store target
    if (auto unary = nodeCast<UnaryExpr>(expr.get())) {
        if (unary->op == "*") {
            // evaluateExpr gives us the pointer value; that IS the lvalue address
            return safeNonNull(evaluateExpr(unary->operand));
        }
    }

    if (auto member = nodeCast<MemberExpr>(expr.get())) {
        std::string baseType = getExprEskiuType(member->base);
        // A pointer-to-struct base must be dereferenced: the struct pointer is the
        // base's *value* (evaluateExpr loads a local pointer var or yields a param),
        // whereas a value-struct base uses its *address* (evaluateLValue).
        bool baseIsPtr = (!baseType.empty() && (baseType.front() == '*' || baseType.back() == '*'));
        auto baseAddr = [&]() -> llvm::Value* {
            return baseIsPtr ? safeNonNull(evaluateExpr(member->base)) : evaluateLValue(member->base);
        };
        baseType = stripToStructKey(baseType);
        auto fit = structFields.find(baseType);
        if (fit == structFields.end())
            throw std::runtime_error("Unknown struct type: " + baseType);
        const auto& fields = fit->second;
        // Union field access: all fields are at offset 0 — just return the base ptr
        // (the load/store will use the field's type via the caller)
        bool isUnion = unionFields.count(baseType) > 0;
        auto lit = structLayout.find(baseType);
        if (lit != structLayout.end()) {
            auto sit = lit->second.find(member->member);
            if (sit != lit->second.end()) {
                if (sit->second.isBitfield)
                    throw std::runtime_error("cannot take the address of bitfield '"
                                             + member->member + "'");
                llvm::Value* basePtr = baseAddr();
                return layoutFieldAddr(baseType, basePtr, sit->second);
            }
        }
        if (long fi = fieldIndexOf(baseType, fields, member->member); fi >= 0) {
            llvm::Value* basePtr = baseAddr();
            if (isUnion) return unionMemberPtr(basePtr); // offset 0 for all union fields
            return builder->CreateStructGEP(structTypes[baseType], basePtr, (unsigned)fi);
        }
        throw std::runtime_error("Struct/union '" + baseType + "' has no field '" + member->member + "'");
    }

    // An overloaded `w[i]` is a call: its result is an rvalue (materialized below).
    if (auto index = nodeCast<IndexExpr>(expr.get()); index && index->opFunc.empty()) {
        // `a[i] = x` — element address for array / slice / pointer / string bases.
        llvm::Value* idx = indexAsI64(index->index);
        return indexElemAddr(index->base, idx);
    }

    if (lvalueAllowTemp) {
        llvm::Value* v = evaluateExpr(expr);
        llvm::Value* tmp = entryAlloca(v->getType(), nullptr, "rval.tmp");
        builder->CreateStore(v, tmp);
        return tmp;
    }
    throw std::runtime_error("Left-hand side of assignment is not an lvalue");
}

llvm::Value* CodeGen::evaluateAddress(const ExprPtr& expr) {
    bool prev = lvalueAllowTemp;
    lvalueAllowTemp = true;
    llvm::Value* a = evaluateLValue(expr);
    lvalueAllowTemp = prev;
    return a;
}
