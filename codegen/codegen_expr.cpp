#include "codegen.h"
#include "../sema/type.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

// An expression with no side effects: evaluating it twice is the same as once.
static bool isPureExpr(const ExprPtr& e) {
    if (!e) return true;
    if (nodeCast<IdentExpr>(e.get()) || nodeCast<LiteralExpr>(e.get()) ||
        nodeCast<SizeofExpr>(e.get()))
        return true;
    if (auto* m = nodeCast<MemberExpr>(e.get())) return isPureExpr(m->base);
    if (auto* ix = nodeCast<IndexExpr>(e.get()))
        return ix->opFunc.empty() && isPureExpr(ix->base) && isPureExpr(ix->index) && isPureExpr(ix->highIndex);
    if (auto* u = nodeCast<UnaryExpr>(e.get())) return u->opFunc.empty() && isPureExpr(u->operand);
    if (auto* c = nodeCast<CastExpr>(e.get())) return isPureExpr(c->expr);
    if (auto* b = nodeCast<BinaryExpr>(e.get()))
        return b->op != "=" && b->opFunc.empty() && isPureExpr(b->left) && isPureExpr(b->right);
    return false;
}

// An access through a `volatile` variable: the variable itself, or a place reached from
// it by `*`, `[]` or `.` (`*reg`, `reg[i]`, `dev.ctrl`). Its loads and stores are volatile.
bool CodeGen::volatileRooted(const Expr* e) const {
    // Each place walked remembers the answer (Expr::volMemo), so a long `a.b.c...` chain,
    // whose every level asks, walks to its root once instead of once per level.
    if (volatileVars.empty()) return false;
    const size_t gen = volatileVars.size();
    std::vector<const Expr*> walked;
    bool res = false;
    while (e) {
        if (e->volMemoSize == gen) { res = e->volMemo; break; }
        walked.push_back(e);
        if (auto* id = nodeCast<const IdentExpr>(e)) { res = volatileVars.count(id->name) > 0; break; }
        if (auto* u = nodeCast<const UnaryExpr>(e)) {
            if (u->op != "*" || !u->opFunc.empty()) break;
            e = u->operand.get();
        } else if (auto* ix = nodeCast<const IndexExpr>(e)) {
            if (!ix->opFunc.empty()) break;
            e = ix->base.get();
        } else if (auto* m = nodeCast<const MemberExpr>(e)) {
            e = m->base.get();
        } else {
            break;
        }
    }
    for (const Expr* w : walked) { w->volMemoSize = gen; w->volMemo = res; }
    return res;
}

llvm::LoadInst* CodeGen::volLoad(llvm::LoadInst* ld, const Expr* root) const {
    if (volatileRooted(root)) ld->setVolatile(true);
    return ld;
}

void CodeGen::emitCompoundAssign(BinaryExpr* node, BinaryExpr* rhsOp) {
    // `lv op= v` (parsed as `lv = lv op v` sharing the lvalue node) where evaluating `lv`
    // has side effects (`a[f()] += 1`, `a[i++] += 1`): compute the lvalue's address ONCE,
    // then run the ordinary `*p = *p op v` through a temporary pointer, so the operator
    // (built-in or overloaded), coercions and --safe checks are the usual ones.
    std::string tmp = "__cmpd." + std::to_string(compoundSeq++);
    llvm::AllocaInst* slot = entryAlloca(llvm::PointerType::get(*context, 0), nullptr, tmp);
    defineSymbol(tmp, slot);
    if (volatileRooted(node->left.get())) volatileVars.insert(tmp);
    ExprPtr target;
    auto* mem = nodeCast<MemberExpr>(node->left.get());
    if (mem && structLayout.count(structBaseTypeOf(mem->base))) {
        // A field of a bitfield-packed struct has no address of its own: evaluate the
        // struct's address once instead, and read-modify-write `(*p).field`.
        std::string bt = getExprEskiuType(mem->base);
        bool baseIsPtr = !bt.empty() && (bt.front() == '*' || bt.back() == '*');
        builder->CreateStore(baseIsPtr ? evaluateExpr(mem->base) : evaluateAddress(mem->base), slot);
        defineVarType(tmp, baseIsPtr ? bt : "*" + bt);
        target = std::make_shared<MemberExpr>(
            std::make_shared<UnaryExpr>("*", std::make_shared<IdentExpr>(tmp)), mem->member);
    } else {
        std::string lt = getExprEskiuType(node->left);
        builder->CreateStore(evaluateLValue(node->left), slot);
        defineVarType(tmp, "*" + lt);
        target = std::make_shared<UnaryExpr>("*", std::make_shared<IdentExpr>(tmp));
    }
    ExprPtr deref = target;
    auto bin = std::make_shared<BinaryExpr>(deref, rhsOp->op, rhsOp->right);
    bin->opFunc = rhsOp->opFunc;
    BinaryExpr assign(deref, "=", bin);
    assign.accept(this);
}

std::string CodeGen::resolveOpInTemplate(const std::string& op,
                                         const std::vector<ExprPtr>& operands) const {
    if (typeParamOverride.empty()) return "";
    std::vector<std::string> tys;
    for (const auto& e : operands) tys.push_back(getExprEskiuType(e));
    return resolveOpInTemplateTypes(op, tys);
}

std::string CodeGen::resolveOpInTemplateTypes(const std::string& op,
                                              const std::vector<std::string>& operandTypes) const {
    if (typeParamOverride.empty()) return "";
    static const std::set<std::string> nums = {"int","int8","int16","int32","int64","uint",
        "uint8","uint16","uint32","uint64","float","double","char","bool"};
    std::vector<std::string> tys;
    bool anyNominal = false;
    for (const auto& ot : operandTypes) {
        std::string t = expandAlias(ot);
        if (t.empty() || t == "unknown") return "";
        if (t.rfind("struct:", 0) == 0) t = t.substr(7);
        if (!nums.count(t) && t.front() != '*' && t.back() != '*' && t != "string") anyNominal = true;
        tys.push_back(t);
    }
    if (!anyNominal) return "";
    std::string n = eskiuOpName(op, tys);
    if (!n.empty() && module->getFunction(n)) return n;
    // An overload is named from its declared spellings (`Vec<int>`), while an operand
    // here may carry the instance's mangled struct name (`Vec_int`): retry with the
    // source form of each generic instance.
    std::function<std::string(const std::string&)> sourceForm = [&](const std::string& t) {
        auto ia = templateInstanceArgs.find(t);
        if (ia == templateInstanceArgs.end()) return t;
        std::string s = ia->second.first + "<";
        for (size_t i = 0; i < ia->second.second.size(); ++i)
            s += (i ? "," : "") + sourceForm(ia->second.second[i]);
        return s + ">";
    };
    {
        std::vector<std::string> src;
        for (const auto& t : tys) src.push_back(sourceForm(t));
        if (src != tys) {
            std::string n2 = eskiuOpName(op, src);
            if (!n2.empty() && module->getFunction(n2)) return n2;
            tys = src;
        }
    }
    // A numeric operand may coerce to the overload's declared numeric parameter.
    for (size_t i = 0; i < tys.size(); ++i) {
        if (!nums.count(tys[i])) continue;
        for (const auto& alt : nums) {
            std::vector<std::string> t2 = tys;
            t2[i] = alt;
            std::string n2 = eskiuOpName(op, t2);
            if (!n2.empty() && module->getFunction(n2)) return n2;
        }
    }
    return "";
}

// A left-leaning chain of built-in operators (`a + b + c ...`, `p && q && ...`) is as
// deep as it is long, so its left spine is emitted with a loop: evaluate the leftmost
// operand, then apply each operator bottom-up to the running value. An assignment or an
// overloaded operator ends the spine (it is evaluated as an ordinary operand).
void CodeGen::visit(BinaryExpr* node) {
    if (node->op == "=") { emitAssignment(node); return; }
    // In a template body the overload lookup derives operand types from the AST, so each
    // chain node's type is derived once (see chainTypeMemo) instead of once per operator.
    std::unordered_map<const Expr*, std::optional<std::string>> memo;
    struct MemoScope {
        CodeGen& cg;
        decltype(cg.chainTypeMemo) saved;
        ~MemoScope() { cg.chainTypeMemo = saved; }
    } memoScope{*this, chainTypeMemo};
    if (!typeParamOverride.empty()) {
        for (Expr* b = node; auto* bin = nodeCast<BinaryExpr>(b); b = bin->left.get()) {
            if (bin->op == "=") break;
            memo[bin];
        }
        chainTypeMemo = &memo;
    }
    auto overloadOf = [&](BinaryExpr* b) {
        if (b->op == "&&" || b->op == "||") return std::string();
        return b->opFunc.empty() ? resolveOpInTemplate(b->op, {b->left, b->right}) : b->opFunc;
    };
    // Operator overload: sema resolved this to a user `operator op(...)`. Lower it as a call
    // to that function (reusing the struct-by-value call ABI) instead of a built-in op.
    std::string opFn = overloadOf(node);
    if (!opFn.empty()) {
        auto call = std::make_shared<CallExpr>(
            std::make_shared<IdentExpr>(opFn),
            std::vector<ExprPtr>{node->left, node->right});
        call->accept(this);
        return;
    }
    std::vector<BinaryExpr*> spine{node};
    while (auto* l = nodeCast<BinaryExpr>(spine.back()->left.get())) {
        if (l->op == "=" || !overloadOf(l).empty()) break;
        spine.push_back(l);
    }
    llvm::Value* value = evaluateExpr(spine.back()->left);
    for (size_t i = spine.size(); i-- > 0;) value = emitBuiltinBinary(spine[i], value);
    exprValueStack.push(value);
}

void CodeGen::emitAssignment(BinaryExpr* node) {
    // Compound assignment with a side-effecting lvalue: evaluate the lvalue once.
    if (auto* rb = nodeCast<BinaryExpr>(node->right.get())) {
        if (rb->left.get() == node->left.get() && !isPureExpr(node->left)) {
            emitCompoundAssign(node, rb);
            return;
        }
    }
    // Evaluation order: the target's address first, then the right-hand side, then the
    // store (a[f()] = g() calls f before g), as compound assignment does.
    // Bitfield assignment is a read-modify-write, not a plain store.
    if (auto* mem = nodeCast<MemberExpr>(node->left.get())) {
        auto lit = structLayout.find(structBaseTypeOf(mem->base));
        if (lit != structLayout.end()) {
            auto sit = lit->second.find(mem->member);
            if (sit != lit->second.end() && sit->second.isBitfield) {
                const BitfieldSlot* slot = nullptr;
                llvm::Value* gep = bitfieldWordPtr(mem, slot);
                llvm::Value* rhs = evaluateExpr(node->right);
                storeBitfieldInto(gep, *slot, rhs, eskiuUnsigned(getExprEskiuType(node->right)),
                                  volatileRooted(mem));
                exprValueStack.push(rhs);
                return;
            }
        }
    }
    llvm::Value* lhs = evaluateLValue(node->left);
    llvm::Value* rhs = evalForType(node->right, getExprEskiuType(node->left));
    // Coerce RHS to match the lvalue's expected element type.
    // Prefer the LHS's declared (static) scalar type: a union member lvalue
    // collapses to the union's base pointer (all fields at offset 0), so the
    // alloca/GEP type encodes the union storage, not the selected field — and
    // a double would be stored whole into a float field without truncation.
    llvm::Type* elemType = nullptr;
    std::string lhsEskiu = getExprEskiuType(node->left);
    if (!lhsEskiu.empty()) {
        llvm::Type* st = getTypeFromString(lhsEskiu);
        if (st && (st->isFloatingPointTy() || st->isIntegerTy()))
            elemType = st;
    }
    if (!elemType) {
        if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(lhs))
            elemType = alloca->getAllocatedType();
        else if (auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(lhs))
            elemType = gep->getResultElementType();
    }
    if (elemType)
        rhs = coerceValue(rhs, elemType, eskiuUnsigned(getExprEskiuType(node->right)));
    bool storeVol = volatileRooted(node->left.get());
    auto* si = builder->CreateStore(rhs, lhs);
    si->setVolatile(storeVol);
    exprValueStack.push(rhs);
}

// Under --safe, trap when the pointer about to be dereferenced is null.
llvm::Value* CodeGen::safeNonNull(llvm::Value* p) {
    if (!(safeChecks & SafeNull) || !p->getType()->isPointerTy() || llvm::isa<llvm::AllocaInst>(p) || llvm::isa<llvm::GlobalValue>(p)) return p;
    emitSafeTrap(builder->CreateIsNull(p), "null");
    return p;
}

// Trap when `cond` holds (a --safe check).
void CodeGen::emitSafeTrap(llvm::Value* cond, const char* name) {
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    auto* trapBB = llvm::BasicBlock::Create(*context, std::string(name) + ".fail", fn);
    auto* contBB = llvm::BasicBlock::Create(*context, std::string(name) + ".ok", fn);
    builder->CreateCondBr(cond, trapBB, contBB);
    builder->SetInsertPoint(trapBB);
    builder->CreateCall(llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::trap));
    builder->CreateUnreachable();
    builder->SetInsertPoint(contBB);
}

// A signed `+ - *`: wrapping by default; under --safe an overflow traps (as Zig's
// ReleaseSafe and -fsanitize=signed-integer-overflow -fsanitize-trap). `id` is the
// matching llvm.s*.with.overflow intrinsic.
llvm::Value* CodeGen::safeArith(llvm::Intrinsic::ID id, llvm::Value* l, llvm::Value* r, bool uns) {
    if (!(safeChecks & SafeOverflow) || uns || !l->getType()->isIntegerTy() || l->getType()->getIntegerBitWidth() < 32) {
        if (id == llvm::Intrinsic::sadd_with_overflow) return builder->CreateAdd(l, r);
        if (id == llvm::Intrinsic::ssub_with_overflow) return builder->CreateSub(l, r);
        return builder->CreateMul(l, r);
    }
    llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(module.get(), id, {l->getType()});
    llvm::Value* pr = builder->CreateCall(f, {l, r});
    emitSafeTrap(builder->CreateExtractValue(pr, {1}), "overflow");
    return builder->CreateExtractValue(pr, {0});
}

// Integer `/` and `%` with defined results: a zero divisor traps, and a signed
// division of the minimum value by -1 wraps (the quotient is the minimum, the
// remainder 0), as signed overflow wraps elsewhere. A constant divisor other than
// 0 and -1 needs neither check, so division by a constant keeps its fast form.
llvm::Value* CodeGen::emitIntDivRem(llvm::Value* left, llvm::Value* right, bool uns, bool rem) {
    llvm::Type* ty = right->getType();
    auto* c = llvm::dyn_cast<llvm::ConstantInt>(right);
    bool maybeZero = !c || c->isZero();
    bool maybeNegOne = !uns && (!c || c->isMinusOne());
    if (maybeZero) {
        llvm::Function* fn = builder->GetInsertBlock()->getParent();
        llvm::BasicBlock* trap = llvm::BasicBlock::Create(*context, "div.zero", fn);
        llvm::BasicBlock* ok = llvm::BasicBlock::Create(*context, "div.ok", fn);
        builder->CreateCondBr(builder->CreateICmpEQ(right, llvm::ConstantInt::get(ty, 0)), trap, ok);
        builder->SetInsertPoint(trap);
        builder->CreateCall(llvm::Intrinsic::getOrInsertDeclaration(module.get(), llvm::Intrinsic::trap));
        builder->CreateUnreachable();
        builder->SetInsertPoint(ok);
    }
    if (uns) return rem ? builder->CreateURem(left, right) : builder->CreateUDiv(left, right);
    if (!maybeNegOne) return rem ? builder->CreateSRem(left, right) : builder->CreateSDiv(left, right);
    llvm::Value* isNegOne = builder->CreateICmpEQ(right, llvm::ConstantInt::getSigned(ty, -1));
    llvm::Value* safe = builder->CreateSelect(isNegOne, llvm::ConstantInt::get(ty, 1), right);
    if (rem) return builder->CreateSRem(left, safe);
    return builder->CreateSelect(isNegOne, builder->CreateNeg(left), builder->CreateSDiv(left, safe));
}

// A shift count is taken modulo the operand width (what x86 and AArch64 shifts do),
// so a count of the width or more, or a negative one, is never LLVM poison.
llvm::Value* CodeGen::maskShiftCount(llvm::Value* count) {
    unsigned w = count->getType()->getIntegerBitWidth();
    if (w <= 1) return count;
    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(count))
        if (c->getValue().ult(w)) return count;
    return builder->CreateAnd(count, llvm::ConstantInt::get(count->getType(), w - 1));
}

// float -> integer saturates: a value beyond the target range becomes its minimum or
// maximum and NaN becomes 0 (plain fptosi/fptoui would be poison there).
llvm::Value* CodeGen::emitFloatToInt(llvm::Value* val, llvm::Type* target, bool uns) {
    llvm::Intrinsic::ID id = uns ? llvm::Intrinsic::fptoui_sat : llvm::Intrinsic::fptosi_sat;
    llvm::Function* f = llvm::Intrinsic::getOrInsertDeclaration(module.get(), id, {target, val->getType()});
    return builder->CreateCall(f, {val});
}

llvm::Value* CodeGen::emitBuiltinBinary(BinaryExpr* node, llvm::Value* left) {
    // Short-circuit logical operators: the RHS must be evaluated ONLY when the LHS
    // doesn't already decide the result, so a guarded expression like
    // `p != null && p.field` (or any RHS unsafe when the LHS is false/true) is not
    // executed. Evaluating both operands eagerly — as the plain path below does —
    // was a correctness bug.
    if (node->op == "&&" || node->op == "||") {
        llvm::Value* l = left;
        l = emitTruthy(l);
        // A constant left operand decides statically (keeps a constant `a && b` constant).
        if (auto* lc = llvm::dyn_cast<llvm::ConstantInt>(l)) {
            if (lc->isZero() == (node->op == "&&")) return lc;
            llvm::Value* r = evaluateExpr(node->right);
            r = emitTruthy(r);
            return r;
        }
        llvm::BasicBlock* startBB = builder->GetInsertBlock();
        llvm::BasicBlock* rhsBB  = llvm::BasicBlock::Create(*context, "sc.rhs", currentFunction);
        llvm::BasicBlock* contBB = llvm::BasicBlock::Create(*context, "sc.cont", currentFunction);
        if (node->op == "&&")
            builder->CreateCondBr(l, rhsBB, contBB);   // l true → eval RHS; false → result false
        else
            builder->CreateCondBr(l, contBB, rhsBB);   // l true → result true; false → eval RHS
        builder->SetInsertPoint(rhsBB);
        llvm::Value* r = evaluateExpr(node->right);
        r = emitTruthy(r);
        llvm::BasicBlock* rhsEndBB = builder->GetInsertBlock();   // RHS may have added blocks
        builder->CreateBr(contBB);
        builder->SetInsertPoint(contBB);
        llvm::PHINode* phi = builder->CreatePHI(llvm::Type::getInt1Ty(*context), 2);
        phi->addIncoming(builder->getInt1(node->op == "||"), startBB);  // short-circuit value
        phi->addIncoming(r, rhsEndBB);
        return phi;
    }

    llvm::Value* right = evaluateExpr(node->right);

    if (!left || !right) {
        throw std::runtime_error("Binary expression operand evaluation failed");
    }
    // An interface value compares with `null` by its data pointer.
    if ((node->op == "==" || node->op == "!=") &&
        left->getType()->isStructTy() != right->getType()->isStructTy()) {
        if (left->getType()->isStructTy()) left = builder->CreateExtractValue(left, 0);
        else right = builder->CreateExtractValue(right, 0);
    }

    llvm::Value* result = nullptr;

    // Integer signedness of each operand, from its Eskiu type. Drives sign- vs
    // zero-extension when widening, and signed vs unsigned div/rem/shr/compare.
    auto isUnsignedEsk = [&](const ExprPtr& e) -> bool {
        return eskiuUnsigned(getExprEskiuType(e));
    };
    bool lUns = isUnsignedEsk(node->left);
    bool rUns = isUnsignedEsk(node->right);
    // C integer promotions: an integer operand narrower than `int` (bool, char, int8/16,
    // uint8/16) is converted to `int` before arithmetic, bitwise, shift, or comparison,
    // extending by its own signedness. After promotion it is a signed int, so e.g.
    // (uint8)200 > (int8)-1 compares 200 with -1, and (uint8)200 + (uint8)100 is 300.
    if (isIntPromotingOp(node->op)) {
        llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
        auto promote = [&](llvm::Value*& v, bool& uns) {
            if (v->getType()->isIntegerTy() && v->getType()->getIntegerBitWidth() < 32) {
                v = uns ? builder->CreateZExt(v, i32) : builder->CreateSExt(v, i32);
                uns = false;
            }
        };
        promote(left, lUns);
        promote(right, rUns);
    }
    // Signedness of a signed-vs-unsigned op, by C's usual arithmetic conversions: after
    // both operands widen to a common width, the op is unsigned only when the unsigned
    // operand's rank (width) is at least the signed one's. A wider signed type represents
    // every value of a narrower unsigned one, so there the op stays signed. (Same-rank
    // mixed → unsigned, as in C.)
    bool opUnsigned;
    if (lUns == rUns) {
        opUnsigned = lUns;
    } else if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
        unsigned lw = left->getType()->getIntegerBitWidth();
        unsigned rw = right->getType()->getIntegerBitWidth();
        opUnsigned = lUns ? (lw >= rw) : (rw >= lw);
    } else {
        opUnsigned = lUns || rUns;   // a float is involved: signedness is irrelevant here
    }
    auto extTo = [&](llvm::Value* v, llvm::Type* ty, bool uns) {
        return uns ? builder->CreateZExt(v, ty) : builder->CreateSExt(v, ty);
    };

    // Promote to common type: int→float, float→double
    auto promoteToFloat = [&]() {
        if (left->getType()->isFloatingPointTy() && right->getType()->isIntegerTy())
            right = intToFloat(right, left->getType(), rUns);
        else if (right->getType()->isFloatingPointTy() && left->getType()->isIntegerTy())
            left = intToFloat(left, right->getType(), lUns);
        // float × double: widen float → double
        else if (left->getType()->isFloatingPointTy() && right->getType()->isFloatingPointTy()
                 && left->getType() != right->getType()) {
            if (left->getType()->getPrimitiveSizeInBits() <
                right->getType()->getPrimitiveSizeInBits())
                left  = builder->CreateFPCast(left,  right->getType());
            else
                right = builder->CreateFPCast(right, left->getType());
        }
    };

    // Widen the narrower integer to match the wider one, extending each operand
    // according to ITS OWN signedness (sign-extend signed, zero-extend unsigned).
    auto widenInts = [&]() {
        if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()
                && left->getType() != right->getType()) {
            unsigned lw = llvm::cast<llvm::IntegerType>(left->getType())->getBitWidth();
            unsigned rw = llvm::cast<llvm::IntegerType>(right->getType())->getBitWidth();
            if (lw < rw) left  = extTo(left,  right->getType(), lUns);
            else          right = extTo(right, left->getType(),  rUns);
        }
    };
    auto widenForBitwise = widenInts;
    auto widenForArith   = widenInts;

    // The element type pointer arithmetic steps by (see pointerStrideType).
    auto ptrElemType = [&]() -> llvm::Type* {
        return pointerStrideType(getExprEskiuType(node->left));
    };

    if (node->op == "+") {
        if (left->getType()->isPointerTy()) {
            llvm::Value* idx = rUns
                ? builder->CreateZExtOrTrunc(right, llvm::Type::getInt64Ty(*context), "ptr.idx")
                : builder->CreateSExtOrTrunc(right, llvm::Type::getInt64Ty(*context), "ptr.idx");
            result = builder->CreateGEP(ptrElemType(), left, idx, "ptr.add");
        } else {
            promoteToFloat();
            widenForArith();
            result = left->getType()->isFloatingPointTy()
                ? builder->CreateFAdd(left, right)
                : safeArith(llvm::Intrinsic::sadd_with_overflow, left, right, opUnsigned);
        }
    } else if (node->op == "-") {
        if (left->getType()->isPointerTy() && right->getType()->isPointerTy()) {
            // ptr - ptr: the distance in elements (C), as an i64, not in bytes.
            result = builder->CreatePtrDiff(ptrElemType(), left, right, "ptrdiff");
        } else if (left->getType()->isPointerTy()) {
            llvm::Value* neg = builder->CreateNeg(rUns
                ? builder->CreateZExtOrTrunc(right, llvm::Type::getInt64Ty(*context))
                : builder->CreateSExtOrTrunc(right, llvm::Type::getInt64Ty(*context)), "neg");
            result = builder->CreateGEP(ptrElemType(), left, neg, "ptr.sub");
        } else {
            promoteToFloat(); widenForArith();
            result = left->getType()->isFloatingPointTy()
                ? builder->CreateFSub(left, right)
                : safeArith(llvm::Intrinsic::ssub_with_overflow, left, right, opUnsigned);
        }
    } else if (node->op == "*") {
        promoteToFloat(); widenForArith();
        result = left->getType()->isFloatingPointTy()
            ? builder->CreateFMul(left, right)
            : safeArith(llvm::Intrinsic::smul_with_overflow, left, right, opUnsigned);
    } else if (node->op == "/") {
        promoteToFloat(); widenForArith();
        result = left->getType()->isFloatingPointTy()
            ? builder->CreateFDiv(left, right)
            : emitIntDivRem(left, right, opUnsigned, false);
    } else if (node->op == "%") {
        promoteToFloat(); widenForArith();
        result = left->getType()->isFloatingPointTy()
            ? builder->CreateFRem(left, right)
            : emitIntDivRem(left, right, opUnsigned, true);
    } else if (node->op == "==") {
        promoteToFloat();   // mixed float/int or float/double: bring both to a common float type
        if (left->getType()->isFloatingPointTy())
            result = builder->CreateFCmpOEQ(left, right);
        else {
            widenInts();   // equality is bit-equal; widening just needs the right extend
            result = builder->CreateICmpEQ(left, right);
        }
    } else if (node->op == "!=" || node->op == "<" || node->op == ">" ||
               node->op == "<=" || node->op == ">=") {
        promoteToFloat();   // mixed float/int or float/double: bring both to a common float type
        bool isFloat = left->getType()->isFloatingPointTy();
        if (!isFloat) widenInts();
        if (node->op == "!=") {
            result = isFloat ? builder->CreateFCmpUNE(left, right)
                             : builder->CreateICmpNE(left, right);
        } else if (node->op == "<") {
            result = isFloat ? builder->CreateFCmpOLT(left, right)
                   : (opUnsigned ? builder->CreateICmpULT(left, right)
                                 : builder->CreateICmpSLT(left, right));
        } else if (node->op == ">") {
            result = isFloat ? builder->CreateFCmpOGT(left, right)
                   : (opUnsigned ? builder->CreateICmpUGT(left, right)
                                 : builder->CreateICmpSGT(left, right));
        } else if (node->op == "<=") {
            result = isFloat ? builder->CreateFCmpOLE(left, right)
                   : (opUnsigned ? builder->CreateICmpULE(left, right)
                                 : builder->CreateICmpSLE(left, right));
        } else {
            result = isFloat ? builder->CreateFCmpOGE(left, right)
                   : (opUnsigned ? builder->CreateICmpUGE(left, right)
                                 : builder->CreateICmpSGE(left, right));
        }
    } else if (node->op == "&&") {
        result = builder->CreateLogicalAnd(left, right);
    } else if (node->op == "||") {
        result = builder->CreateLogicalOr(left, right);
    // Bitwise operators (widen narrower integer before operating)
    } else if (node->op == "&") {
        widenForBitwise(); result = builder->CreateAnd(left, right);
    } else if (node->op == "|") {
        widenForBitwise(); result = builder->CreateOr(left, right);
    } else if (node->op == "^") {
        widenForBitwise(); result = builder->CreateXor(left, right);
    } else if (node->op == "<<" || node->op == ">>") {
        // A shift computes in the (promoted) width of the value shifted; the count is
        // converted to that width and never widens the result (C).
        if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
            right = builder->CreateZExtOrTrunc(right, left->getType());
            right = maskShiftCount(right);
        }
        // The shift kind follows the value being shifted (the left operand) only; the
        // count's signedness is irrelevant, so a signed value keeps an arithmetic shift.
        if (node->op == "<<") result = builder->CreateShl(left, right);
        else result = lUns ? builder->CreateLShr(left, right) : builder->CreateAShr(left, right);
    } else {
        throw std::runtime_error("Unknown binary operator: " + node->op);
    }

    return result;
}
