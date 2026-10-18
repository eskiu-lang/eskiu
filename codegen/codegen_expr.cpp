#include "codegen.h"
#include "../sema/type.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

// An expression with no side effects: evaluating it twice is the same as once.
static bool isPureExpr(const ExprPtr& e) {
    if (!e) return true;
    if (dynamic_cast<IdentExpr*>(e.get()) || dynamic_cast<LiteralExpr*>(e.get()) ||
        dynamic_cast<SizeofExpr*>(e.get()))
        return true;
    if (auto* m = dynamic_cast<MemberExpr*>(e.get())) return isPureExpr(m->base);
    if (auto* ix = dynamic_cast<IndexExpr*>(e.get()))
        return ix->opFunc.empty() && isPureExpr(ix->base) && isPureExpr(ix->index) && isPureExpr(ix->highIndex);
    if (auto* u = dynamic_cast<UnaryExpr*>(e.get())) return u->opFunc.empty() && isPureExpr(u->operand);
    if (auto* c = dynamic_cast<CastExpr*>(e.get())) return isPureExpr(c->expr);
    if (auto* b = dynamic_cast<BinaryExpr*>(e.get()))
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
        if (auto* id = dynamic_cast<const IdentExpr*>(e)) { res = volatileVars.count(id->name) > 0; break; }
        if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
            if (u->op != "*" || !u->opFunc.empty()) break;
            e = u->operand.get();
        } else if (auto* ix = dynamic_cast<const IndexExpr*>(e)) {
            if (!ix->opFunc.empty()) break;
            e = ix->base.get();
        } else if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
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
    auto* mem = dynamic_cast<MemberExpr*>(node->left.get());
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
        for (Expr* b = node; auto* bin = dynamic_cast<BinaryExpr*>(b); b = bin->left.get()) {
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
    while (auto* l = dynamic_cast<BinaryExpr*>(spine.back()->left.get())) {
        if (l->op == "=" || !overloadOf(l).empty()) break;
        spine.push_back(l);
    }
    llvm::Value* value = evaluateExpr(spine.back()->left);
    for (size_t i = spine.size(); i-- > 0;) value = emitBuiltinBinary(spine[i], value);
    exprValueStack.push(value);
}

void CodeGen::emitAssignment(BinaryExpr* node) {
    // Compound assignment with a side-effecting lvalue: evaluate the lvalue once.
    if (auto* rb = dynamic_cast<BinaryExpr*>(node->right.get())) {
        if (rb->left.get() == node->left.get() && !isPureExpr(node->left)) {
            emitCompoundAssign(node, rb);
            return;
        }
    }
    // Evaluation order: the target's address first, then the right-hand side, then the
    // store (a[f()] = g() calls f before g), as compound assignment does.
    // Bitfield assignment is a read-modify-write, not a plain store.
    if (auto* mem = dynamic_cast<MemberExpr*>(node->left.get())) {
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
            llvm::Value* idx = builder->CreateSExtOrTrunc(
                right, llvm::Type::getInt64Ty(*context), "ptr.idx");
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
            llvm::Value* neg = builder->CreateNeg(
                builder->CreateSExtOrTrunc(right, llvm::Type::getInt64Ty(*context)), "neg");
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
    // A `null` arm is a pointer (`c ? null : &x`), whatever the other arm points to.
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    llvm::Type* tLL = thenTy == "null" ? ptrTy : getTypeFromString(thenTy);
    llvm::Type* eLL = elseTy == "null" ? ptrTy : getTypeFromString(elseTy);
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
    if (auto* mem = dynamic_cast<MemberExpr*>(node->operand.get())) {
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
        llvm::Value* lo = toI64(evaluateExpr(node->index));
        llvm::Value* hi = toI64(evaluateExpr(node->highIndex));
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

    llvm::Value* idx = evaluateExpr(node->index);
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
                ptr = basePtr;
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
        if (auto* id = dynamic_cast<IdentExpr*>(node->expr.get())) {
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
    if (auto ident = dynamic_cast<IdentExpr*>(expr.get())) {
        llvm::Value* val = lookupSymbol(ident->name);
        if (!val) throw std::runtime_error("Undefined variable: " + ident->name);
        return val;
    }

    // Dereference as lvalue: *ptr = val — load the pointer value, use as the store target
    if (auto unary = dynamic_cast<UnaryExpr*>(expr.get())) {
        if (unary->op == "*") {
            // evaluateExpr gives us the pointer value; that IS the lvalue address
            return safeNonNull(evaluateExpr(unary->operand));
        }
    }

    if (auto member = dynamic_cast<MemberExpr*>(expr.get())) {
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
            if (isUnion) return basePtr; // offset 0 for all union fields
            return builder->CreateStructGEP(structTypes[baseType], basePtr, (unsigned)fi);
        }
        throw std::runtime_error("Struct/union '" + baseType + "' has no field '" + member->member + "'");
    }

    // An overloaded `w[i]` is a call: its result is an rvalue (materialized below).
    if (auto index = dynamic_cast<IndexExpr*>(expr.get()); index && index->opFunc.empty()) {
        // `a[i] = x` — element address for array / slice / pointer / string bases.
        llvm::Value* idx = evaluateExpr(index->index);
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
