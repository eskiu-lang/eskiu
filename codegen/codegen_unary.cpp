// codegen_unary.cpp: ?, ternary, unary, ++/-- and index expressions (split from codegen_expr.cpp).
#include "codegen.h"
#include "../sema/type.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

void CodeGen::visit(QuestionExpr* node) {
    // `expr?` — if expr is an Err Result, return it from the enclosing function;
    // otherwise evaluate to the unwrapped success value.
    llvm::Value* resVal = evaluateExpr(node->operand);
    llvm::StructType* st = llvm::dyn_cast<llvm::StructType>(resVal->getType());
    if (!st || !st->hasName())
        throw std::runtime_error("`?` operator requires a named Result struct value");
    std::string opType = st->getName().str();

    auto fIt = structFields.find(opType);
    if (fIt == structFields.end())
        throw std::runtime_error("`?` operator on non-Result type: " + opType);

    unsigned okIdx = 0, valueIdx = 0;
    std::string valueFieldType;
    for (unsigned i = 0; i < fIt->second.size(); ++i) {
        if (fIt->second[i].name == "ok")    okIdx = i;
        if (fIt->second[i].name == "value") { valueIdx = i; valueFieldType = fIt->second[i].type; }
    }

    // Materialize the Result into a temp so we can read fields and return it whole.
    llvm::Value* tmp = entryAlloca(st, nullptr, "try.tmp");
    builder->CreateStore(resVal, tmp);

    llvm::Value* okPtr = builder->CreateStructGEP(st, tmp, okIdx);
    llvm::Type*  okTy  = st->getElementType(okIdx);
    llvm::Value* okVal = builder->CreateLoad(okTy, okPtr, "try.ok");
    llvm::Value* isErr = builder->CreateICmpEQ(okVal, llvm::ConstantInt::get(okTy, 0), "try.iserr");

    llvm::BasicBlock* errBB  = llvm::BasicBlock::Create(*context, "try.err",  currentFunction);
    llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "try.cont", currentFunction);
    builder->CreateCondBr(isErr, errBB, contBB);

    // Error path: propagate the error out of the enclosing function. Its Result type may
    // differ in the success type only: then a fresh one (zeroed) gets the `ok` flag and
    // the error. This is an early function exit, so run pending defers/finally first.
    builder->SetInsertPoint(errBB);
    llvm::Value* whole = builder->CreateLoad(st, tmp, "try.whole");
    llvm::Type* retTy = currentFunction->getReturnType();
    if (currentSretParam != nullptr)
        if (auto rs = funcSretTypes.find(currentFunction->getName().str()); rs != funcSretTypes.end())
            retTy = rs->second;
    auto* retSt = llvm::dyn_cast<llvm::StructType>(retTy);
    if (retSt && retSt != st && retSt->hasName() && structFields.count(retSt->getName().str())) {
        const auto& rf = structFields[retSt->getName().str()];
        llvm::Value* out = entryAlloca(retSt, nullptr, "try.out");
        builder->CreateStore(llvm::Constant::getNullValue(retSt), out);
        for (unsigned i = 0; i < fIt->second.size(); ++i) {
            const std::string& fname = fIt->second[i].name;
            if (fname != "ok" && fname != "error") continue;
            for (unsigned j = 0; j < rf.size(); ++j)
                if (rf[j].name == fname) {
                    llvm::Value* v = builder->CreateLoad(st->getElementType(i), builder->CreateStructGEP(st, tmp, i));
                    builder->CreateStore(v, builder->CreateStructGEP(retSt, out, j));
                }
        }
        whole = builder->CreateLoad(retSt, out, "try.whole");
    }
    if (currentSretParam != nullptr) {
        builder->CreateStore(whole, currentSretParam);
        runCleanupsToDepth(0, /*errorPath=*/true);      // ? error exit: defers + errdefers
        if (!blockTerminated()) builder->CreateRetVoid();
    } else {
        runCleanupsToDepth(0, /*errorPath=*/true);
        if (!blockTerminated()) builder->CreateRet(whole);
    }

    // Success path: unwrap and yield the value field.
    builder->SetInsertPoint(contBB);
    llvm::Value* valPtr = builder->CreateStructGEP(st, tmp, valueIdx);
    llvm::Type*  valTy  = getTypeFromString(valueFieldType);
    exprValueStack.push(builder->CreateLoad(valTy, valPtr, "try.value"));
}

void CodeGen::visit(TernaryExpr* node) {
    // `cond ? a : b` — branch on the condition and evaluate exactly one arm, then phi
    // the results. Both arms are coerced to their common type (see the type checker).
    llvm::Value* cond = evaluateExpr(node->condition);
    cond = emitTruthy(cond);

    std::string thenTy = getExprEskiuType(node->thenExpr);
    std::string elseTy = getExprEskiuType(node->elseExpr);
    // A `null` arm is a pointer (`c ? null : &x`), whatever the other arm points to. In a
    // generic body the checker's table has no entry for the literal, so it is recognized
    // by its node as well as by the resolved spelling.
    auto isNullArm = [](const ExprPtr& e, const std::string& t) {
        if (t == "null") return true;
        auto lit = nodeCast<LiteralExpr>(e.get());
        return lit && lit->kind == LiteralExpr::Kind::NULL_VAL;
    };
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    llvm::Type* tLL = isNullArm(node->thenExpr, thenTy) ? ptrTy : getTypeFromString(thenTy);
    llvm::Type* eLL = isNullArm(node->elseExpr, elseTy) ? ptrTy : getTypeFromString(elseTy);
    llvm::Type* resTy;
    if (tLL == eLL)
        resTy = tLL;
    else if (tLL->isFloatingPointTy() || eLL->isFloatingPointTy())
        resTy = (tLL->isDoubleTy() || eLL->isDoubleTy())
                    ? llvm::Type::getDoubleTy(*context)
                    : llvm::Type::getFloatTy(*context);
    else if (tLL->isIntegerTy() && eLL->isIntegerTy())
        resTy = tLL->getIntegerBitWidth() >= eLL->getIntegerBitWidth() ? tLL : eLL;
    else
        resTy = tLL;
    // Two numeric arms: the type checker's common type is authoritative (a literal arm
    // too wide for `int` makes the result 64-bit; equal-width mixed signedness is unsigned).
    auto isNum = [](llvm::Type* t) { return t && (t->isIntegerTy() || t->isFloatingPointTy()); };
    if (resolvedExprTypes && isNum(tLL) && isNum(eLL)) {
        auto it = resolvedExprTypes->find(node);
        if (it != resolvedExprTypes->end() && it->second != "unknown")
            if (llvm::Type* rt = getTypeFromString(it->second); isNum(rt)) resTy = rt;
    }

    // An interface arm and a struct-pointer arm meet as the interface (the pointer is boxed).
    std::string ifaceTy = !interfaceName(thenTy).empty() ? thenTy
                        : !interfaceName(elseTy).empty() ? elseTy : "";
    // Typed as an interface by its target (`I i = c ? &a : &b`): each arm is boxed with
    // its own struct's vtable.
    if (ifaceTy.empty() && resolvedExprTypes) {
        auto it = resolvedExprTypes->find(node);
        if (it != resolvedExprTypes->end() && !interfaceName(it->second).empty()) ifaceTy = it->second;
    }
    if (!ifaceTy.empty()) resTy = getTypeFromString(ifaceTy);
    const bool isVoid = resTy->isVoidTy();          // `c ? f() : g()` with void arms: a statement
    auto arm = [&](const ExprPtr& e, const std::string& srcEskiu) -> llvm::Value* {
        if (isVoid) return evaluateExpr(e);
        if (!ifaceTy.empty()) return evalForType(e, ifaceTy);
        return coerceValue(evaluateExpr(e), resTy, eskiuUnsigned(srcEskiu));
    };

    // A constant condition selects its arm statically (the other is never evaluated),
    // which keeps a constant ternary a constant expression.
    if (auto* cc = llvm::dyn_cast<llvm::ConstantInt>(cond)) {
        bool pickThen = !cc->isZero();
        exprValueStack.push(pickThen ? arm(node->thenExpr, thenTy) : arm(node->elseExpr, elseTy));
        return;
    }

    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(*context, "tern.then", currentFunction);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(*context, "tern.else", currentFunction);
    llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "tern.cont", currentFunction);
    builder->CreateCondBr(cond, thenBB, elseBB);

    builder->SetInsertPoint(thenBB);
    llvm::Value* tv = arm(node->thenExpr, thenTy);
    llvm::BasicBlock* thenEnd = builder->GetInsertBlock();   // arm may have added blocks
    builder->CreateBr(contBB);

    builder->SetInsertPoint(elseBB);
    llvm::Value* ev = arm(node->elseExpr, elseTy);
    llvm::BasicBlock* elseEnd = builder->GetInsertBlock();
    builder->CreateBr(contBB);

    builder->SetInsertPoint(contBB);
    if (isVoid) { exprValueStack.push(ev); return; }   // no value to merge
    llvm::PHINode* phi = builder->CreatePHI(resTy, 2);
    phi->addIncoming(tv, thenEnd);
    phi->addIncoming(ev, elseEnd);
    exprValueStack.push(phi);
}

void CodeGen::visit(UnaryExpr* node) {
    // Unary operator overload: sema resolved this to a user `operator -/!/~(V)`. Lower it
    // as a one-arg call to that function.
    std::string opFn = node->opFunc;
    if (opFn.empty() && (node->op == "-" || node->op == "!" || node->op == "~"))
        opFn = resolveOpInTemplate(node->op == "-" ? "u-" : node->op, {node->operand});
    if (!opFn.empty()) {
        auto call = std::make_shared<CallExpr>(
            std::make_shared<IdentExpr>(opFn),
            std::vector<ExprPtr>{node->operand});
        call->accept(this);
        return;
    }

    // Address-of: the operand's lvalue, evaluated once (not also as an rvalue).
    if (node->op == "&") {
        exprValueStack.push(evaluateLValue(node->operand));
        return;
    }

    llvm::Value* val = evaluateExpr(node->operand);

    if (!val) {
        throw std::runtime_error("Unary operand evaluation failed");
    }

    llvm::Value* result = nullptr;

    // C integer promotion: `-`/`~` on an operand narrower than int (bool, char, int8/16,
    // uint8/16) first widens it to int by its own signedness, so `~(uint8)255` is -256.
    if ((node->op == "-" || node->op == "~" || node->op == "+") && val->getType()->isIntegerTy() &&
        val->getType()->getIntegerBitWidth() < 32) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
        bool uns = val->getType()->isIntegerTy(1) || eskiuUnsigned(getExprEskiuType(node->operand));
        val = uns ? builder->CreateZExt(val, i32) : builder->CreateSExt(val, i32);
    }

    if (node->op == "+") {
        result = val;   // unary plus: the (promoted) operand's value
    } else if (node->op == "-") {
        result = val->getType()->isFloatingPointTy()
            ? builder->CreateFNeg(val)
            : builder->CreateNeg(val);
    } else if (node->op == "~") {
        result = builder->CreateNot(val); // bitwise NOT
    } else if (node->op == "!") {
        // Logical NOT: convert to bool
        result = builder->CreateNot(emitTruthy(val));
    } else if (node->op == "*") {
        // Dereference: use Eskiu type info to load the correct element type
        std::string ptrEskiuType = getExprEskiuType(node->operand);
        llvm::Type* elemType = llvm::Type::getInt8Ty(*context); // fallback
        if (!ptrEskiuType.empty()) {
            std::string elemStr;
            if (ptrEskiuType.front() == '*')
                elemStr = ptrEskiuType.substr(1);
            else if (ptrEskiuType.back() == '*')
                elemStr = ptrEskiuType.substr(0, ptrEskiuType.size() - 1);
            if (!elemStr.empty() && elemStr != "void")
                elemType = getTypeFromString(elemStr);
        }
        result = volLoad(builder->CreateLoad(elemType, safeNonNull(val)), node);
    } else {
        throw std::runtime_error("Unknown unary operator: " + node->op);
    }

    exprValueStack.push(result);
}

// The element a pointer-typed value steps by in `p + n`, `p - q`, `p++`: the pointee of
// `*T` / `T*`, a byte for `string` (a char pointer), `*void` and anything else.
llvm::Type* CodeGen::pointerStrideType(const std::string& eskTy) {
    llvm::Type* i8 = llvm::Type::getInt8Ty(*context);
    std::string t = expandAlias(eskTy);
    if (!t.empty() && t[0] == '?') t = t.substr(1);
    ty::Type pt = ty::Type::parse(t);
    if (!pt.isPointer() || !pt.pointee) return i8;
    std::string pe = pt.pointee->str();
    if (pe == "void" || pe == "char" || pe.empty()) return i8;
    return getTypeFromString(pe);
}

void CodeGen::visit(IncDecExpr* node) {
    // A bitfield has no address: step it with a masked read-modify-write of its storage
    // word (evaluating the base once). Prefix yields the stored, width-wrapped value.
    if (auto* mem = nodeCast<MemberExpr>(node->operand.get())) {
        auto lit = structLayout.find(structBaseTypeOf(mem->base));
        if (lit != structLayout.end()) {
            auto sit = lit->second.find(mem->member);
            if (sit != lit->second.end() && sit->second.isBitfield) {
                const BitfieldSlot* slot = nullptr;
                llvm::Value* gep = bitfieldWordPtr(mem, slot);
                bool vol = volatileRooted(mem);
                llvm::Value* old = loadBitfieldFrom(gep, *slot, vol);
                llvm::Value* one = llvm::ConstantInt::get(old->getType(), 1);
                llvm::Value* nw = node->decrement ? builder->CreateSub(old, one)
                                                  : builder->CreateAdd(old, one);
                storeBitfieldInto(gep, *slot, nw, false, vol);
                // Prefix: the stored value, read as the field reads; postfix: the old value
                // in the declared type (C, as clang).
                exprValueStack.push(node->prefix ? bitfieldReadValue(loadBitfieldFrom(gep, *slot, vol),
                                                                     structBaseTypeOf(mem->base), mem->member)
                                                 : old);
                return;
            }
        }
    }
    llvm::Value* ptr = evaluateLValue(node->operand);
    std::string ety = getExprEskiuType(node->operand);
    llvm::Type* ty = getTypeFromString(ety.empty() ? "int" : ety);
    bool isPtr = ty->isPointerTy();
    llvm::Value* old = volLoad(builder->CreateLoad(ty, ptr), node->operand.get());
    llvm::Value* nw;
    if (isPtr) {
        // pointer (or string) step by one element
        llvm::Value* step = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context),
                                                   node->decrement ? -1 : 1, true);
        nw = builder->CreateGEP(pointerStrideType(ety), old, step);
    } else {
        llvm::Value* one = llvm::ConstantInt::get(ty, 1);
        nw = node->decrement ? builder->CreateSub(old, one) : builder->CreateAdd(old, one);
    }
    builder->CreateStore(nw, ptr)->setVolatile(volatileRooted(node->operand.get()));
    exprValueStack.push(node->prefix ? nw : old);
}

void CodeGen::emitBoundsCheck(llvm::Value* idx, llvm::Value* len) {
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    llvm::Value* idx64 = idx->getType()->isIntegerTy(64) ? idx : builder->CreateSExt(idx, i64);
    llvm::Value* lo  = builder->CreateICmpSLT(idx64, llvm::ConstantInt::get(i64, 0));
    llvm::Value* hi  = builder->CreateICmpSGE(idx64, len);
    llvm::Value* oob = builder->CreateOr(lo, hi);
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    auto* trapBB = llvm::BasicBlock::Create(*context, "bounds.fail", fn);
    auto* contBB = llvm::BasicBlock::Create(*context, "bounds.ok",   fn);
    builder->CreateCondBr(oob, trapBB, contBB);
    builder->SetInsertPoint(trapBB);
    builder->CreateCall(llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::trap));
    builder->CreateUnreachable();
    builder->SetInsertPoint(contBB);
}

void CodeGen::emitSliceBoundsCheck(llvm::Value* lo, llvm::Value* hi, llvm::Value* len) {
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    // 0 <= lo <= hi (<= len). `lo == len` is a valid empty slice at the end, so the upper
    // bound is `hi > len`, not `hi >= len` — this is what the element check got wrong.
    llvm::Value* bad = builder->CreateOr(
        builder->CreateICmpSLT(lo, llvm::ConstantInt::get(i64, 0)),
        builder->CreateICmpSLT(hi, lo));
    if (len) bad = builder->CreateOr(bad, builder->CreateICmpSGT(hi, len));
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    auto* trapBB = llvm::BasicBlock::Create(*context, "slice.fail", fn);
    auto* contBB = llvm::BasicBlock::Create(*context, "slice.ok",   fn);
    builder->CreateCondBr(bad, trapBB, contBB);
    builder->SetInsertPoint(trapBB);
    builder->CreateCall(llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::trap));
    builder->CreateUnreachable();
    builder->SetInsertPoint(contBB);
}

// An index or pointer offset as an i64, extended by its own signedness: GEP reads its
// indices as signed, so a uint8 255 left as an i8 would index element -1.
llvm::Value* CodeGen::indexAsI64(const ExprPtr& e) {
    llvm::Value* v = evaluateExpr(e);
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    if (!v->getType()->isIntegerTy() || v->getType()->isIntegerTy(64)) return v;
    return eskiuUnsigned(getExprEskiuType(e)) ? builder->CreateZExt(v, i64) : builder->CreateSExt(v, i64);
}

llvm::Value* CodeGen::indexElemAddr(const ExprPtr& base, llvm::Value* idx, bool doCheck) {
    std::string baseType = getExprEskiuType(base);
    ty::Type bt = ty::Type::parse(baseType);
    if (baseType == "string")
        return builder->CreateGEP(llvm::Type::getInt8Ty(*context), safeNonNull(evaluateExpr(base)), idx);
    if (bt.kind == ty::Type::Kind::Array) {
        llvm::Value* zero = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), 0);
        uint64_t n = 0;
        if ((safeChecks & SafeBounds) && doCheck && resolveArrayDim(bt.dim, n))   // bounds-check against the static length
            emitBoundsCheck(idx, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context), n));
        return builder->CreateGEP(getTypeFromString(baseType), evaluateLValue(base), {zero, idx});
    }
    if (bt.kind == ty::Type::Kind::Slice) {
        llvm::Value* fat  = evaluateExpr(base);                     // evaluate the slice once
        llvm::Value* data = builder->CreateExtractValue(fat, {0});  // fat.ptr
        if ((safeChecks & SafeBounds) && doCheck) emitBoundsCheck(idx, builder->CreateExtractValue(fat, {1}));   // vs fat.len
        return builder->CreateGEP(getTypeFromString(bt.elem->str()), data, idx);
    }
    if (isPointerType(baseType)) {
        std::string elemStr = (!baseType.empty() && baseType.front() == '*')
            ? baseType.substr(1) : baseType.substr(0, baseType.size() - 1);
        return builder->CreateGEP(getTypeFromString(elemStr), safeNonNull(evaluateExpr(base)), idx);
    }
    throw std::runtime_error("Cannot index into type: " + baseType);
}

void CodeGen::visit(IndexExpr* node) {
    // Overloaded subscript: sema resolved `base[i]` to a user `operator [](B, I)`.
    if (!node->opFunc.empty()) {
        auto call = std::make_shared<CallExpr>(
            std::make_shared<IdentExpr>(node->opFunc),
            std::vector<ExprPtr>{node->base, node->index});
        call->accept(this);
        return;
    }
    std::string baseType = getExprEskiuType(node->base);
    ty::Type bt = ty::Type::parse(baseType);
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    auto toI64 = [&](llvm::Value* v) -> llvm::Value* {
        if (v->getType()->isIntegerTy(64)) return v;
        return builder->CreateSExt(v, i64);
    };

    // Slice construction: base[lo..hi] → a fat pointer { &base[lo], hi - lo }.
    if (node->highIndex) {
        llvm::Value* lo = toI64(indexAsI64(node->index));
        llvm::Value* hi = toI64(indexAsI64(node->highIndex));
        // Element type of the base: string→char, array/slice→elem, pointer→pointee.
        // Slicing a raw pointer (`*T`) yields `T[]`, so heap buffers can become slices.
        std::string elemStr;
        if (baseType == "string") {
            elemStr = "char";
        } else if ((bt.kind == ty::Type::Kind::Array || bt.kind == ty::Type::Kind::Slice) && bt.elem) {
            elemStr = bt.elem->str();
        } else if (isPointerType(baseType)) {
            elemStr = (!baseType.empty() && baseType.front() == '*')
                ? baseType.substr(1)
                : baseType.substr(0, baseType.size() - 1);
        } else {
            elemStr = "uint8";  // unreachable: sema rejects non-indexable slice bases
        }
        // Address of base[lo] WITHOUT the element bounds check (lo == len is a valid empty
        // slice), plus the base length for a proper --safe slice check (null = unknown).
        llvm::Value* data = nullptr;
        llvm::Value* baseLen = nullptr;
        if (bt.kind == ty::Type::Kind::Slice) {
            llvm::Value* fat = evaluateExpr(node->base);            // evaluate once
            data    = builder->CreateGEP(getTypeFromString(elemStr),
                                         builder->CreateExtractValue(fat, {0}), lo);
            baseLen = builder->CreateExtractValue(fat, {1});
        } else if (bt.kind == ty::Type::Kind::Array) {
            uint64_t n = 0;
            if (resolveArrayDim(bt.dim, n)) baseLen = llvm::ConstantInt::get(i64, n);
            data = indexElemAddr(node->base, lo, /*doCheck=*/false);
        } else {
            data = indexElemAddr(node->base, lo, /*doCheck=*/false);   // string / pointer: len unknown
        }
        if (safeChecks & SafeBounds) emitSliceBoundsCheck(lo, hi, baseLen);
        llvm::Value* len = builder->CreateSub(hi, lo);
        llvm::Type* sliceTy = getTypeFromString(elemStr + "[]");
        llvm::Value* s = llvm::UndefValue::get(sliceTy);
        s = builder->CreateInsertValue(s, data, {0});
        s = builder->CreateInsertValue(s, len,  {1});
        exprValueStack.push(s);
        return;
    }

    llvm::Value* idx = indexAsI64(node->index);
    // A read: an array-valued rvalue base (`mk().a[1]`) is materialized, not rejected.
    struct AllowTemp { CodeGen* c; bool p; ~AllowTemp() { c->lvalueAllowTemp = p; } } allowTemp{this, lvalueAllowTemp};
    lvalueAllowTemp = true;

    // Slice element: s[i] → load from the fat pointer's data at i.
    if (bt.kind == ty::Type::Kind::Slice) {
        llvm::Type* elemType = getTypeFromString(bt.elem->str());
        exprValueStack.push(volLoad(builder->CreateLoad(elemType, indexElemAddr(node->base, idx)), node));
        return;
    }
    // String: string[i] → char.
    if (baseType == "string") {
        exprValueStack.push(volLoad(builder->CreateLoad(
            llvm::Type::getInt8Ty(*context), indexElemAddr(node->base, idx)), node));
        return;
    }
    // Fixed-size array: T[N] (for T[N][M], indexing peels the outer dimension → T[M]).
    if (bt.kind == ty::Type::Kind::Array) {
        llvm::Type* elemType = getTypeFromString(bt.elem->str());
        exprValueStack.push(volLoad(builder->CreateLoad(elemType, indexElemAddr(node->base, idx)), node));
        return;
    }
    // Pointer: *T or T*.
    if (isPointerType(baseType)) {
        std::string elemStr = (!baseType.empty() && baseType.front() == '*')
            ? baseType.substr(1)
            : baseType.substr(0, baseType.size() - 1);
        exprValueStack.push(volLoad(builder->CreateLoad(
            getTypeFromString(elemStr), indexElemAddr(node->base, idx)), node));
        return;
    }

    throw std::runtime_error("Cannot index into type: " + baseType);
}
