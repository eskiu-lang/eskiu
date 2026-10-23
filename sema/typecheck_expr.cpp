#include "type_checker.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <set>
#include <cstring>
#include <cctype>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include <climits>
#include <cstdint>
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"

// Does the floating value v, truncated toward zero, fit the integer type t (as a C
// conversion requires)? The bounds are powers of two, exact in a double; for 64 bits no
// double lies strictly between -2^63-1 and -2^63, so `>=` is exact there.
bool floatConstFitsInt(double v, const std::string& t) {
    int bits = 32;
    bool uns = t.size() > 4 && t.compare(0, 4, "uint") == 0;
    if (t == "int8" || t == "uint8" || t == "char") bits = 8;
    else if (t == "int16" || t == "uint16") bits = 16;
    else if (t == "int64" || t == "uint64") bits = 64;
    if (t == "uint") uns = true;
    if (uns) return v > -1.0 && v < std::ldexp(1.0, bits);
    double lim = std::ldexp(1.0, bits - 1);
    return (bits == 64 ? v >= -lim : v > -lim - 1.0) && v < lim;
}

// ============================================================================

// TypeChecker — expression visitors (operators, calls, member/index access,
// literals, lambdas, await, template calls, struct init).
// Part of the type_checker.cpp split; see type_checker.h.

// A closure captures an enclosing local by value (a copy in its environment), so an
// assignment to one inside the lambda would change only that copy: an error. A global
// or a `static` local is not captured (the lambda uses the one cell), so it may be set.
// A field or element of a captured struct/array value (`p.a = 5`, `arr[0] = 9`) is part
// of the copy too; a write through a pointer (`ptr.a`, `*p`, `s[i]` of a slice or a
// string) reaches the shared object and is allowed.
std::string TypeChecker::capturedRoot(Expr* target) {
    if (captureBoundary.empty()) return "";
    while (target) {
        if (auto* m = dynamic_cast<MemberExpr*>(target)) {
            if (tyq::isPtr(getExpressionType(m->base.get()))) return "";
            target = m->base.get(); continue;
        }
        if (auto* ix = dynamic_cast<IndexExpr*>(target)) {
            if (!ix->opFunc.empty() || ix->highIndex) return "";
            std::string bt = normalizeType(getExpressionType(ix->base.get()));
            if (ty::Type::parse(bt).kind != ty::Type::Kind::Array) return "";
            target = ix->base.get(); continue;
        }
        break;
    }
    auto* id = dynamic_cast<IdentExpr*>(target);
    if (!id) return "";
    if (lookupSymbol(id->name).empty()) return "";
    int defIdx = scopeOf(id->name);
    if (defIdx >= 1 && defIdx < captureBoundary.back() && !scopes[defIdx][id->name].isStatic) return id->name;
    return "";
}

void TypeChecker::checkCapturedWrite(ASTNode* at, Expr* target) {
    std::string n = capturedRoot(target);
    if (!n.empty())
        errorAt(at, "cannot assign to captured variable '" + n +
                    "': a closure captures it by value (use a pointer, a global, or a static)");
}

void TypeChecker::checkCapturedAddress(ASTNode* at, Expr* target) {
    std::string n = capturedRoot(target);
    if (!n.empty())
        errorAt(at, "cannot take the address of captured variable '" + n +
                    "': a closure captures it by value, so a write through the address is lost "
                    "(use a pointer, a global, or a static)");
}

// `x.m()` on a captured value: the method gets the address of the closure's copy, so a
// `*T self` that writes through it changes only that copy. A captured value is read-only
// in the lambda (as in a C++ by-value capture): only a `const T* self` method may be
// called on it. Through a captured pointer the call reaches the shared object.
void TypeChecker::checkCapturedMethodCall(ASTNode* at, MemberExpr* member) {
    std::string n = capturedRoot(member->base.get());
    if (!n.empty())
        errorAt(at, "cannot call method '" + member->member + "' on captured variable '" + n +
                    "': a closure captures it by value, and a non-const self may modify the copy "
                    "(use a 'const T* self' method, or capture a pointer)");
}

// Expression visitors
// `lhs = rhs` (also the desugared compound `x op= y`): the target must be a writable
// lvalue, the value assignable to it. Kept out of visit(BinaryExpr) so that visitor's
// frame stays small (it recurses once per operator of a long expression chain).
[[gnu::noinline]] void TypeChecker::checkAssignment(BinaryExpr* node) {
    // Assigning to a `const` binding, a field/element of a const value, or
    // through a pointer-to-const (`const T*`) is an error. See assignsToConst.
    if (isSliceLen(node->left.get()))
        errorAt(node, "cannot assign to the length of a slice: it is read-only (take a new slice instead)");
    else if (!isLvalueExpr(node->left.get()))
        errorAt(node, "cannot assign to this expression: it is not a variable, field, element, or dereference");
    std::string cname;
    if (assignsToConst(node->left.get(), cname))
        errorAt(node, "cannot assign to read-only location '" + cname + "'");
    checkCapturedWrite(node, node->left.get());
    std::string lt = getExpressionType(node->left.get());
    // The target of `p = ...` keeps its declared type even while `p` is narrowed,
    // and the assignment ends the narrowing (unless it stores an address `&x`).
    if (auto* id = dynamic_cast<IdentExpr*>(node->left.get())) {
        std::string declared = lookupSymbol(id->name);
        if (!declared.empty() && declared[0] == '?') {
            lt = declared;
            // `p = &x` stores an address, and `p = p + k` / `p += k` steps a non-null
            // pointer; any other value may be null.
            auto* u = dynamic_cast<UnaryExpr*>(node->right.get());
            auto* ar = dynamic_cast<BinaryExpr*>(node->right.get());
            auto* self = ar ? dynamic_cast<IdentExpr*>(ar->left.get()) : nullptr;
            bool keeps = (u && u->op == "&") ||
                         (ar && (ar->op == "+" || ar->op == "-") && self && self->name == id->name);
            if (!keeps) narrowedNonNull.erase(narrowKey(id->name));
        }
    }
    if (auto* id = dynamic_cast<IdentExpr*>(node->left.get())) {
        trackLocalAddr(id->name, node->right.get());
        // -Wall: storing a local's address in a global leaves it dangling once the
        // function returns (main's locals outlive every use).
        if (auto* u = dynamic_cast<UnaryExpr*>(node->right.get());
            warnAll && u && u->op == "&" && scopeOf(id->name) == 0 && localAddrFn != "main") {
            std::string root = localStorageRoot(u->operand.get());
            if (!root.empty())
                warning(node->line, node->col,
                        "storing the address of local '" + root + "' in global '" + id->name +
                        "' (it dangles once the function returns)");
        }
    }
    std::string rt = getExpressionType(node->right.get());
    if (dropsConstQual(lt, rt))
        errorAt(node, "assignment discards a const qualifier ('" + rt + "' to '" + lt + "')");
    else if (lt != "unknown" && rt != "unknown") {
        // Type compatibility incl. narrowing (a literal that fits the target
        // stays valid). Handled here so `=` gets the same rules as init/return.
        std::string e = assignabilityError(lt, rt, node->right.get());
        if (!e.empty()) errorAt(node, "assignment: " + e);
        // A compound assignment `x op= lit` (desugared to `x = x op lit`, sharing the
        // target node) stores into x's type, so its literal operand must fit it, as
        // for `x = lit`.
        if (auto* cb = dynamic_cast<BinaryExpr*>(node->right.get());
            cb && cb->left.get() == node->left.get() && cb->op != "<<" && cb->op != ">>" &&
            isIntType(normalizeType(lt)))
            if (auto* lit = dynamic_cast<LiteralExpr*>(cb->right.get());
                lit && lit->kind == LiteralExpr::Kind::INT && !intLiteralFits(normalizeType(lt), lit))
                errorAt(node, "compound assignment: integer literal " + lit->value +
                              " is out of range for '" + lt + "'");
    }
    expressionTypes[node] = lt;
}

// A left-leaning chain (`a + b + c ...`, `p && q && ...`) is as deep as it is long, so
// its left spine is walked with a loop: the leftmost operand first, then each operator
// bottom-up (its right operand, then its own type). Only right operands recurse.
void TypeChecker::visit(BinaryExpr* node) {
    std::vector<BinaryExpr*> spine;
    for (BinaryExpr* b = node; b; b = dynamic_cast<BinaryExpr*>(b->left.get())) spine.push_back(b);
    // `f = <lambda>` rebinds a lambda local: the target is not a use of f's value (it would
    // otherwise make f escape and send every env to the heap, unfreed).
    std::string savedCallee = calleeContext;
    if (spine.back()->op == "=" && dynamic_cast<LambdaExpr*>(spine.back()->right.get()))
        if (auto* id = dynamic_cast<IdentExpr*>(spine.back()->left.get()); id && lambdaLocals.count(id->name))
            calleeContext = id->name;
    spine.back()->left->accept(this);
    calleeContext = savedCallee;
    // Narrowing keys of the left operand of the operator just handled: in a run of the
    // same `&&` (or `||`), the next operator's left narrows to those plus the previous
    // right operand's, so each operand is scanned once instead of the whole prefix.
    std::vector<std::string> keys;
    for (size_t i = spine.size(); i-- > 0;) {
        BinaryExpr* b = spine[i];
        // Short-circuit narrowing: in `p != null && *p`, the right operand only runs when
        // the left is true (for `||`, when it is false), so it sees `p` as non-null.
        if (b->op == "&&" || b->op == "||") {
            bool whenTrue = b->op == "&&";
            if (i + 1 < spine.size() && spine[i + 1]->op == b->op) {
                condNarrowings(spine[i + 1]->right.get(), whenTrue, keys);
            } else {
                keys.clear();
                condNarrowings(b->left.get(), whenTrue, keys);
            }
            auto inserted = applyNarrowings(keys);
            b->right->accept(this);
            undoNarrowings(inserted);
        } else if (!b->scriptInit) {      // a script global's initializer was checked at its declaration
            if (b->op == "=") {
                hintIfaceTarget(b->right.get(), getExpressionType(b->left.get()));
                inferVariantTarget(b->right, getExpressionType(b->left.get()));
            }
            b->right->accept(this);
        }
        finishBinary(b);
    }
}

void TypeChecker::finishBinary(BinaryExpr* node) {
    if (node->op == "=") {
        if (!node->scriptInit) checkAssignment(node);   // a script global: checked at its declaration
        // A lambda assigned to a lambda local is bound to it like an initializer.
        if (auto* lam = dynamic_cast<LambdaExpr*>(node->right.get()))
            if (auto* id = dynamic_cast<IdentExpr*>(node->left.get());
                id && lambdaLocals.count(id->name) && !inInstance) {
                lambdaLocal[lam] = id->name;
                if (inAsyncFn) asyncLocalLambdas.insert(lam);
            }
        return;
    }

    std::string leftType = getExpressionType(node->left.get());
    std::string rightType = getExpressionType(node->right.get());

    if (leftType == "unknown" || rightType == "unknown") {
        expressionTypes[node] = "unknown";
        return;
    }

    // -Wextra: comparing a signed and an unsigned integer is a classic bug source
    // (the signed operand is converted to unsigned, so negatives become large).
    if (warnExtra && (node->op == "<" || node->op == ">" || node->op == "<=" ||
                      node->op == ">=" || node->op == "==" || node->op == "!=")) {
        auto isUns = [](const std::string& t) {
            return t == "uint" || t == "uint8" || t == "uint16" || t == "uint32" || t == "uint64";
        };
        auto isSgn = [](const std::string& t) {
            return t == "int" || t == "int8" || t == "int16" || t == "int32" || t == "int64";
        };
        std::string l = normalizeType(leftType), r = normalizeType(rightType);
        if ((isUns(l) && isSgn(r)) || (isSgn(l) && isUns(r)))
            warning(node->line, node->col, "comparison between signed and unsigned integers ('" +
                    leftType + "' and '" + rightType + "')");
    }

    // `s == "abc"` compares two addresses: equal text in two places compares unequal.
    if (node->op == "==" || node->op == "!=") {
        auto isStrLit = [](Expr* e) {
            auto* l = dynamic_cast<LiteralExpr*>(e);
            return l && l->kind == LiteralExpr::Kind::STRING;
        };
        // Only a pointer comparison: `S == "x"` on a struct S is an overloaded operator.
        auto isAddr = [&](Expr* e) {
            std::string t = normalizeType(tyq::strip(getExpressionType(e)));
            return t == "string" || t == "null" || isPointerType(t);
        };
        bool lLit = isStrLit(node->left.get()), rLit = isStrLit(node->right.get());
        if ((lLit && isAddr(node->right.get())) || (rLit && isAddr(node->left.get())))
            noteWarning("string-compare", node->line, node->col,
                        "comparison with a string literal compares addresses, not text; use strcmp(a, b) " +
                        node->op + " 0");
    }

    // Division or remainder by a literal zero is a guaranteed runtime trap; reject
    // it at compile time (the value is statically known).
    bool litZero = false;
    if ((node->op == "/" || node->op == "%")) {
        if (auto* l = dynamic_cast<LiteralExpr*>(node->right.get());
            l && l->kind == LiteralExpr::Kind::INT) {
            bool zero = true;
            for (char c : l->value) if (c != '0' && c != '-' && c != '+') { zero = false; break; }
            litZero = zero;
            if (zero) errorAt(node, std::string(node->op == "/" ? "division" : "remainder") +
                                    " by zero");
        }
    }

    // `p - q` counts elements between two pointers into one array: they must point to
    // the same type (C).
    if (node->op == "-" && isPointerType(leftType) && isPointerType(rightType)) {
        // One spelling per type: aliases, `?`, const and `*T` vs `T*` do not matter.
        std::function<std::string(const std::string&)> canon = [&](const std::string& t) -> std::string {
            std::string n = normalizeType(dealiasOperand(t));
            if (!n.empty() && n[0] == '?') n = n.substr(1);
            if (isPointerType(n)) return "*" + canon(getPointeeType(n));
            return n;
        };
        std::string l = canon(leftType), r = canon(rightType);
        if (l != r) {
            errorAt(node, "pointer subtraction needs pointers to the same type, got '" + leftType +
                          "' and '" + rightType + "'");
            expressionTypes[node] = "int64";
            return;
        }
    }
    std::string resultType = inferBinaryExprType(leftType, node->op, rightType);
    // Stepping a `?*T` does not prove it non-null: `p + n` is still a `?*T`.
    if ((node->op == "+" || node->op == "-") && !leftType.empty() && leftType[0] == '?' &&
        isPointerType(resultType) && resultType[0] != '?')
        resultType = "?" + resultType;

    // An integer division whose operands are constant expressions (names, `sizeof`,
    // casts) is checked like a literal one: by zero, or the most negative value by -1,
    // is undefined in C.
    if ((node->op == "/" || node->op == "%") && isIntType(resultType)) {
        std::string what = node->op == "/" ? "division" : "remainder";
        long long x = 0, y = 0;
        bool haveY = foldConstInt(node->right.get(), y);
        bool wide = resultType == "int64" || resultType == "uint64";
        if (haveY && !wide) y = truncConstInt(resultType, y);
        if (haveY && y == 0 && !litZero)
            errorAt(node, what + " by zero");
        else if (haveY && y == -1 && (resultType == "int" || resultType == "int32" || resultType == "int64") &&
                 foldConstInt(node->left.get(), x) &&
                 (wide ? x == LLONG_MIN : truncConstInt(resultType, x) == INT32_MIN))
            errorAt(node, what + " overflows: " + std::to_string(wide ? x : truncConstInt(resultType, x)) +
                          " " + node->op + " -1 does not fit '" + resultType + "'");
    }

    // A constant shift count must be less than the (promoted) left operand's width and
    // not negative; anything else is undefined behavior in C.
    if ((node->op == "<<" || node->op == ">>") && isIntType(resultType)) {
        long long cnt = 0;
        if (foldConstInt(node->right.get(), cnt)) {
            int width = (resultType == "int64" || resultType == "uint64") ? 64 : 32;
            if (cnt < 0 || cnt >= width)
                errorAt(node, "shift count " + std::to_string(cnt) + " is out of range for a " +
                              std::to_string(width) + "-bit operand (it must be 0.." + std::to_string(width - 1) + ")");
        }
    }

    if (resultType == "error") {
        // Operator overloading: `a op b` on non-built-in operands resolves to a user
        // `operator op(L, R)` declared for these operand types (with numeric coercion).
        // Not found → the operands really are invalid.
        std::string ret, opFn = resolveOperator(node->op, {leftType, rightType}, ret);
        if (!opFn.empty()) {
            if (!inInstance) node->opFunc = opFn;
            operatorCallNodes.insert(node);
            dropGlobalNarrowings();        // the operator is a call: it may assign any global
            calledFns.insert(opFn);        // -Wall: an operator use references it
            expressionTypes[node] = ret;   // the operator's declared return type
        } else {
            errorAt(node,"invalid operands for operator: " + leftType + " and " + rightType);
            expressionTypes[node] = "unknown";
        }
    } else {
        expressionTypes[node] = resultType;
    }
}

void TypeChecker::visit(QuestionExpr* node) {
    if (finallyDepth > 0)
        errorAt(node, "'?' is not allowed inside a finally block (it would return from the function)");
    node->operand->accept(this);
    std::string opType = normalizeType(getExpressionType(node->operand.get()));
    std::string s = opType;
    if (s.rfind("struct:", 0) == 0) s = s.substr(7);

    auto it = structs.find(s);
    bool hasOk = false, hasValue = false;
    std::string valueType = "unknown", okType;
    if (it != structs.end())
        for (const auto& f : it->second.fields) {
            if (f.name == "ok")    { hasOk = true; okType = normalizeType(f.type); }
            if (f.name == "value") { hasValue = true; valueType = normalizeType(f.type); }
        }

    if (!hasOk || !hasValue) {
        errorAt(node, "`?` operator requires a Result-like value "
                      "(with `ok` and `value` fields), got " + opType);
        expressionTypes[node] = "unknown";
        return;
    }
    // `?` tests `ok` as a flag (zero is the error), so it must be an integer or a bool.
    if (!isIntType(okType) && okType != "bool") {
        errorAt(node, "`?` needs an integer or bool `ok` field, got '" + okType + "'");
        expressionTypes[node] = "unknown";
        return;
    }

    // The enclosing function returns a Result to propagate into: the same type, or one
    // with the same error type (`Result<String, E>?` in a `Result<int, E>` function).
    std::string ret = normalizeType(currentFunctionReturnType);
    if (ret.rfind("struct:", 0) == 0) ret = ret.substr(7);
    if (ret != s) {
        auto errType = [&](const std::string& name, std::string& ok) {
            auto st = structs.find(name);
            std::string e;
            if (st == structs.end()) return e;
            for (const auto& f : st->second.fields) {
                if (f.name == "ok") ok = normalizeType(f.type);
                if (f.name == "error") e = normalizeType(f.type);
            }
            return e;
        };
        std::string opOk, retOk;
        std::string opErr = errType(s, opOk), retErr = errType(ret, retOk);
        bool hasValue = false;
        if (auto st = structs.find(ret); st != structs.end())
            for (const auto& f : st->second.fields) if (f.name == "value") hasValue = true;
        if ((opErr.empty() || opErr != retErr || opOk != retOk || !hasValue) && inTrailingBlock)
            errorAt(node, "`?` is not allowed in a trailing block (the block is a closure)");
        else if (opErr.empty() || opErr != retErr || opOk != retOk || !hasValue)
            errorAt(node, "`?` can only be used in a function returning a Result with the same "
                          "error type; this function returns '" + currentFunctionReturnType + "'");
    }
    expressionTypes[node] = valueType;
}

// An integer literal that does not fit `int` has a wider type (C: the first of int,
// long, unsigned long that holds it), so `c ? x : 10000000000` is 64-bit, not an int.
static std::string literalArmType(Expr* e, const std::string& t) {
    auto* lit = dynamic_cast<LiteralExpr*>(e);
    if (!lit || lit->kind != LiteralExpr::Kind::INT) return t;
    try {
        long long v = std::stoll(lit->value, nullptr, 0);
        if (v >= INT32_MIN && v <= INT32_MAX) return t;
        return "int64";
    } catch (...) {
        // Above INT64_MAX: only an unsigned 64-bit type holds it.
        return "uint64";
    }
}

// A generic variant built where a known instance of its enum is expected (`Opt<int64> a =
// Some(5)`, `return None;`, an argument, an assignment, a struct-literal field, an array
// literal element, a variant's payload) takes that instance's type arguments: the node is
// rewritten to the explicit form (`Some<int64>(5)`, `None<int>()`), which codegen lowers
// as written. A `?:` passes the target to both arms, an array literal its element type
// to each element. In a generic instance, whose body every instance shares, only a
// `raw` target (as written in the source: a declared type, the declared return type)
// is used, and its argument spellings are kept (`return None;` in an `Opt<T>` function
// is `None<T>()`).
void TypeChecker::inferVariantTarget(ExprPtr& e, const std::string& target, bool raw) {
    if (!e || (inInstance && !raw) || target.empty() || target == "unknown") return;
    if (auto* t = dynamic_cast<TernaryExpr*>(e.get())) {
        inferVariantTarget(t->thenExpr, target, raw);
        inferVariantTarget(t->elseExpr, target, raw);
        return;
    }
    if (auto* al = dynamic_cast<ArrayLitExpr*>(e.get())) {
        ty::Type at = ty::Type::parse(tyq::strip(target));
        if (at.kind != ty::Type::Kind::Array || !at.elem) return;
        std::string et = at.elem->str();
        for (auto& el : al->elements) inferVariantTarget(el, et, raw);
        return;
    }
    std::string name;
    std::vector<ExprPtr> args;
    auto* call = dynamic_cast<CallExpr*>(e.get());
    if (call) {
        auto* cid = dynamic_cast<IdentExpr*>(call->callee.get());
        if (!cid) return;
        name = cid->name;
        args = call->args;
    } else if (auto* id = dynamic_cast<IdentExpr*>(e.get())) {
        name = id->name;
    } else {
        return;
    }
    auto gv = genericVariants.find(name);
    if (gv == genericVariants.end() || !lookupSymbol(name).empty()) return;
    auto ge = genericEnumDecls.find(gv->second.first);
    if (ge == genericEnumDecls.end()) return;
    std::vector<std::string> targs;
    if (inInstance) {
        ty::Type rt = ty::Type::parse(tyq::strip(target));
        if (rt.kind != ty::Type::Kind::Template || rt.nullable || rt.name != gv->second.first) return;
        for (const auto& a : rt.args) targs.push_back(a.str());
    } else {
        std::string n = normalizeType(tyq::strip(target));
        if (!n.empty() && n[0] == '?') return;
        auto ia = templateInstanceArgs.find(n);
        if (ia == templateInstanceArgs.end() || ia->second.first != gv->second.first) return;
        targs = ia->second.second;
    }
    if (targs.size() != ge->second->typeParams.size()) return;
    auto tc = std::make_shared<TemplateCallExpr>(name, targs, std::move(args));
    tc->line = e->line;
    tc->col = e->col;
    e = tc;
}

void TypeChecker::hintIfaceTarget(Expr* e, const std::string& target) {
    auto* t = dynamic_cast<TernaryExpr*>(e);
    if (!t || target.empty() || target == "unknown") return;
    std::string n = normalizeType(tyq::strip(target));
    if (n.rfind("interface:", 0) == 0 || interfaceDecls.count(n)) ifaceTargetHint[e] = target;
}

void TypeChecker::visit(TernaryExpr* node) {
    auto hint = ifaceTargetHint.find(node);
    std::string ifaceTarget = hint == ifaceTargetHint.end() ? "" : hint->second;
    if (!ifaceTarget.empty()) {                     // nested `?:` arms box the same way
        hintIfaceTarget(node->thenExpr.get(), ifaceTarget);
        hintIfaceTarget(node->elseExpr.get(), ifaceTarget);
    }
    node->condition->accept(this);
    {
        std::vector<std::string> keys;
        condNarrowings(node->condition.get(), true, keys);
        auto inserted = applyNarrowings(keys);
        node->thenExpr->accept(this);
        undoNarrowings(inserted);
        keys.clear();
        condNarrowings(node->condition.get(), false, keys);
        inserted = applyNarrowings(keys);
        node->elseExpr->accept(this);
        undoNarrowings(inserted);
    }

    std::string ct = getExpressionType(node->condition.get());
    if (!isConditionType(ct))
        errorAt(node, "ternary condition must be a bool, integer, or pointer, got " + ct);

    std::string tt = literalArmType(node->thenExpr.get(), getExpressionType(node->thenExpr.get()));
    std::string et = literalArmType(node->elseExpr.get(), getExpressionType(node->elseExpr.get()));

    // The result type is the arms' common type: identical types pass through, two
    // numerics promote to the wider (C-style), and otherwise the arms must be mutually
    // assignable (else it is a type error).
    std::string result = tt;
    // Bound for an interface: each arm that converts to it is boxed there, so two pointers
    // to different conforming structs meet as the interface.
    if (!ifaceTarget.empty() && tt != "unknown" && et != "unknown" && tt != et &&
        assignabilityError(ifaceTarget, tt, node->thenExpr.get()).empty() &&
        assignabilityError(ifaceTarget, et, node->elseExpr.get()).empty()) {
        expressionTypes[node] = normalizeType(tyq::strip(ifaceTarget));
        return;
    }
    if (tt == "unknown")       result = et;
    else if (et == "unknown")  result = tt;
    else if (tt == et)         result = tt;
    // A `null` arm takes the other arm's pointer type (`c ? null : &x`).
    else if (tt == "null" && (isPointerType(et) || et[0] == '?')) result = et;
    else if (et == "null" && (isPointerType(tt) || tt[0] == '?')) result = tt;
    // Two different numeric types: C's usual arithmetic conversions, so narrow arms
    // (`c ? true : ' '`) meet as int.
    else if (isNumericType(tt) && isNumericType(et)) result = promoteType(intPromoted(tt), intPromoted(et));
    else if (assignabilityError(tt, et, node->elseExpr.get()).empty()) result = tt;
    else if (assignabilityError(et, tt, node->thenExpr.get()).empty()) result = et;
    else {
        errorAt(node, "ternary branches have incompatible types '" + tt + "' and '" + et + "'");
        result = tt;
    }
    expressionTypes[node] = result;
}

std::string TypeChecker::narrowKey(const std::string& name) const {
    int si = scopeOf(name);
    if (si < 0) return "";
    // A `static` local is one cell shared by every call and closure, like a global: its
    // key ends in "@0" so a call ends its narrowing and a lambda body never sees it.
    auto it = scopes[si].find(name);
    bool isStatic = si > 0 && it != scopes[si].end() && it->second.isStatic;
    return name + "@" + std::to_string(si) + (isStatic ? "@0" : "");
}

void TypeChecker::condNarrowings(Expr* cond, bool whenTrue, std::vector<std::string>& keys) {
    Expr* whole = cond;
    auto nullableIdent = [&](Expr* e) -> std::string {
        auto* id = dynamic_cast<IdentExpr*>(e);
        if (!id) return "";
        std::string t = lookupSymbol(id->name);
        if (t.empty() || t[0] != '?') return "";
        // A variable reachable through a pointer (`&p` taken) can be nulled behind the
        // check by a write through it, so the check proves nothing lasting.
        int si = scopeOf(id->name);
        if (si >= 0 && scopes[si][id->name].addrTaken) return "";
        if (si == 0 && globalAddrTaken.count(id->name)) return "";
        return narrowKey(id->name);
    };
    // `!c` flips the sense; `a && b` (when true) and `a || b` (when false) narrow by both
    // operands. The left spine of such a run is followed with a loop; the right operands
    // are scanned afterwards, bottom-up, so keys keep their left-to-right order.
    std::vector<std::pair<Expr*, bool>> rights;
    for (;;) {
        if (auto* u = dynamic_cast<UnaryExpr*>(cond); u && u->op == "!") {
            cond = u->operand.get();
            whenTrue = !whenTrue;
            continue;
        }
        auto* b = dynamic_cast<BinaryExpr*>(cond);
        if (b && ((b->op == "&&" && whenTrue) || (b->op == "||" && !whenTrue))) {
            rights.push_back({b->right.get(), whenTrue});
            cond = b->left.get();
            continue;
        }
        break;
    }
    // A call may assign any global: a global proven non-null by an earlier operand is
    // not proven after this one runs.
    if (exprHasCall(cond)) dropGlobalKeys(keys);
    if (auto* b = dynamic_cast<BinaryExpr*>(cond)) {
        if (b->op == "!=" || b->op == "==") {
            auto isNull = [](Expr* e) {
                auto* l = dynamic_cast<LiteralExpr*>(e);
                return l && l->kind == LiteralExpr::Kind::NULL_VAL;
            };
            std::string k;
            if (isNull(b->right.get())) k = nullableIdent(b->left.get());
            else if (isNull(b->left.get())) k = nullableIdent(b->right.get());
            if (!k.empty() && (b->op == "!=") == whenTrue) keys.push_back(k);
        }
    } else if (whenTrue) {                             // `if (p)`: a pointer tested for non-null
        std::string k = nullableIdent(cond);
        if (!k.empty()) keys.push_back(k);
    }
    for (size_t i = rights.size(); i-- > 0;) condNarrowings(rights[i].first, rights[i].second, keys);
    // A variable the condition itself assigns (`p != null && (p = q) != x`), steps, or takes
    // the address of is not proven by an earlier test in it.
    if (!keys.empty()) {
        std::set<std::string> assigned;
        assignedNames(whole, assigned);
        if (!assigned.empty())
            keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const std::string& k) {
                return assigned.count(k.substr(0, k.rfind('@'))) > 0;
            }), keys.end());
    }
}

// Names of the variables `e` assigns, increments/decrements, or takes the address of (not
// inside a lambda body, whose captures are copies).
void TypeChecker::assignedNames(Expr* e, std::set<std::string>& out) {
    if (!e) return;
    if (auto* b = dynamic_cast<BinaryExpr*>(e); b && b->op == "=")
        if (auto* id = dynamic_cast<IdentExpr*>(b->left.get())) out.insert(id->name);
    if (auto* u = dynamic_cast<UnaryExpr*>(e); u && u->op == "&")
        if (auto* id = dynamic_cast<IdentExpr*>(u->operand.get())) out.insert(id->name);
    if (auto* ic = dynamic_cast<IncDecExpr*>(e))
        if (auto* id = dynamic_cast<IdentExpr*>(ic->operand.get())) out.insert(id->name);
    if (dynamic_cast<LambdaExpr*>(e)) return;
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { assignedNames(c.get(), out); });
}

std::vector<std::string> TypeChecker::applyNarrowings(const std::vector<std::string>& keys) {
    std::vector<std::string> inserted;
    for (const auto& k : keys)
        if (narrowedNonNull.insert(k).second) inserted.push_back(k);
    return inserted;
}

void TypeChecker::markAddrTaken(const std::set<std::string>& names) {
    for (const auto& n : names) {
        int si = scopeOf(n);
        if (si >= 0) scopes[si][n].addrTaken = true;
    }
}

void TypeChecker::markAddrTakenIn(Stmt* s) {
    std::set<std::string> names;
    astwalk::collectAddressTaken(s, names);
    markAddrTaken(names);
}

void TypeChecker::markAddrTakenIn(Expr* e) {
    std::set<std::string> names;
    astwalk::collectAddressTaken(e, names);
    markAddrTaken(names);
}

void TypeChecker::dropGlobalNarrowings() {
    ++callEpoch;
    for (auto it = narrowedNonNull.begin(); it != narrowedNonNull.end();) {
        size_t at = it->rfind('@');
        if (at != std::string::npos && it->compare(at, std::string::npos, "@0") == 0) it = narrowedNonNull.erase(it);
        else ++it;
    }
}

// The keys of global variables (scope 0) removed from `keys`.
void TypeChecker::dropGlobalKeys(std::vector<std::string>& keys) {
    keys.erase(std::remove_if(keys.begin(), keys.end(), [](const std::string& k) {
        size_t at = k.rfind('@');
        return at != std::string::npos && k.compare(at, std::string::npos, "@0") == 0;
    }), keys.end());
}

// Whether evaluating `e` makes a call (a lambda body is not evaluated there).
bool TypeChecker::exprHasCall(Expr* e) const {
    if (!e || dynamic_cast<LambdaExpr*>(e)) return false;
    if (dynamic_cast<CallExpr*>(e) || dynamic_cast<TemplateCallExpr*>(e) || dynamic_cast<AwaitExpr*>(e) ||
        dynamic_cast<AllocWithExpr*>(e) || dynamic_cast<ThreadCreateExpr*>(e)) return true;
    if (operatorCallNodes.count(e)) return true;
    bool found = false;
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { if (!found) found = exprHasCall(c.get()); });
    return found;
}

void TypeChecker::undoNarrowings(const std::vector<std::string>& inserted) {
    for (const auto& k : inserted) narrowedNonNull.erase(k);
}

void TypeChecker::dropAssignedIn(Expr* e) {
    if (!e || narrowedNonNull.empty()) return;
    auto dropName = [&](const std::string& n) {
        for (auto it = narrowedNonNull.begin(); it != narrowedNonNull.end();)
            if (it->compare(0, n.size() + 1, n + "@") == 0) it = narrowedNonNull.erase(it); else ++it;
    };
    if (auto* b = dynamic_cast<BinaryExpr*>(e); b && b->op == "=")
        if (auto* id = dynamic_cast<IdentExpr*>(b->left.get())) dropName(id->name);
    if (auto* u = dynamic_cast<UnaryExpr*>(e); u && u->op == "&")
        if (auto* id = dynamic_cast<IdentExpr*>(u->operand.get())) dropName(id->name);
    if (auto* lam = dynamic_cast<LambdaExpr*>(e)) { dropAssignedIn(lam->body.get()); return; }
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { dropAssignedIn(c.get()); });
}

void TypeChecker::dropAssignedIn(Stmt* s) {
    if (!s || narrowedNonNull.empty()) return;
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& it : b->items) {
            if (auto* st = std::get_if<StmtPtr>(&it)) dropAssignedIn(st->get());
            else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) dropAssignedIn(vd->initializer.get());
        }
    } else if (auto* es = dynamic_cast<ExprStmt*>(s)) dropAssignedIn(es->expr.get());
    else if (auto* r = dynamic_cast<ReturnStmt*>(s)) dropAssignedIn(r->value.get());
    else if (auto* i = dynamic_cast<IfStmt*>(s)) {
        dropAssignedIn(i->condition.get()); dropAssignedIn(i->thenBranch.get()); dropAssignedIn(i->elseBranch.get());
    } else if (auto* w = dynamic_cast<WhileStmt*>(s)) { dropAssignedIn(w->condition.get()); dropAssignedIn(w->body.get()); }
    else if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) { dropAssignedIn(dw->condition.get()); dropAssignedIn(dw->body.get()); }
    else if (auto* f = dynamic_cast<ForStmt*>(s)) {
        dropAssignedIn(f->init.get()); dropAssignedIn(f->condition.get());
        dropAssignedIn(f->step.get()); dropAssignedIn(f->body.get());
    } else if (auto* fi = dynamic_cast<ForInStmt*>(s)) { dropAssignedIn(fi->iterable.get()); dropAssignedIn(fi->body.get()); }
    else if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        dropAssignedIn(sw->subject.get());
        for (auto& c : sw->cases)
            for (auto& it : c.stmts) {
                if (auto* st = std::get_if<StmtPtr>(&it)) dropAssignedIn(st->get());
                else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) dropAssignedIn(vd->initializer.get());
            }
    } else if (auto* m = dynamic_cast<MatchStmt*>(s)) {
        dropAssignedIn(m->subject.get());
        for (auto& arm : m->arms) dropAssignedIn(arm.body.get());
    } else if (auto* t = dynamic_cast<TryStmt*>(s)) {
        dropAssignedIn(t->body.get());
        for (auto& c : t->catches) dropAssignedIn(c.body.get());
        dropAssignedIn(t->finally.get());
    } else if (auto* d = dynamic_cast<DeferStmt*>(s)) dropAssignedIn(d->body.get());
    else if (auto* th = dynamic_cast<ThrowStmt*>(s)) dropAssignedIn(th->value.get());
}

void TypeChecker::checkNullableDeref(Expr* operand, const char* how) {
    std::string t = getExpressionType(operand);
    if (t.empty() || t[0] != '?') return;                 // not (or no longer) a nullable pointer
    errorAt(operand, std::string("cannot ") + how + " a possibly-null pointer of type '" + t +
                     "'; check it first (e.g. `if (x != null)`)");
}

// `&s.f` (or `&s.a[i]`) where `f` is aligned below its type's alignment by the packing
// of `s`'s struct gives a pointer the target may not load through (strict-alignment
// CPUs fault on it, as clang -Waddress-of-packed-member warns).
void TypeChecker::warnPackedAddress(UnaryExpr* node) {
    Expr* e = node->operand.get();
    while (auto* ix = dynamic_cast<IndexExpr*>(e)) {
        if (ix->highIndex || ty::Type::parse(getExpressionType(ix->base.get())).kind != ty::Type::Kind::Array) return;
        e = ix->base.get();
    }
    auto* m = dynamic_cast<MemberExpr*>(e);
    if (!m) return;
    std::string bt = ty::Type::parse(normalizeType(tyq::strip(getExpressionType(m->base.get())))).nominalName();
    auto sit = structs.find(bt);
    if (sit == structs.end() || sit->second.packAlign <= 0) return;
    for (const auto& f : sit->second.fields) {
        if (f.name != m->member) continue;
        std::string ft = getExpressionType(node->operand.get());
        unsigned long long size = 0, align = 0;
        if (!constLayout(normalizeType(ft), size, align)) return;
        if (align > (unsigned long long)sit->second.packAlign)
            noteWarning("address-of-packed-member", node->line, node->col,
                        "taking the address of packed member '" + m->member + "' of '" +
                        displayType(tyq::strip(getExpressionType(m->base.get()))) +
                        "' may give an unaligned pointer");
        return;
    }
}

void TypeChecker::visit(UnaryExpr* node) {
    node->operand->accept(this);
    std::string operandType = getExpressionType(node->operand.get());
    if (node->op == "&" && !isLvalueExpr(node->operand.get()))
        errorAt(node, "cannot take the address of this expression: it is not a variable, field, element, or dereference");
    if (node->op == "&") checkCapturedAddress(node, node->operand.get());
    if (node->op == "&")
        if (auto* m = dynamic_cast<MemberExpr*>(node->operand.get())) {
            std::string bt = ty::Type::parse(normalizeType(tyq::strip(getExpressionType(m->base.get())))).nominalName();
            auto sit = structs.find(bt);
            if (sit != structs.end())
                if (const auto* f = sit->second.field(m->member); f && f->bitWidth > 0)
                    errorAt(node, "cannot take the address of bitfield '" + m->member + "'");
        }
    if (node->op == "&") warnPackedAddress(node);
    // `&p` of a narrowed `?*T` is a `*?*T` (a write through it may store null), so the
    // narrowing ends here and the address carries the declared nullable type.
    if (node->op == "&")
        if (auto* id = dynamic_cast<IdentExpr*>(node->operand.get())) {
            std::string declared = lookupSymbol(id->name);
            int si = scopeOf(id->name);
            if (si >= 0) scopes[si][id->name].addrTaken = true;
            if (!declared.empty() && declared[0] == '?') {
                operandType = declared;
                narrowedNonNull.erase(narrowKey(id->name));
            }
        }

    if (node->op == "*") checkNullableDeref(node->operand.get(), "dereference");
    // A `*void` points at no type: there is nothing to read or write through it.
    if (node->op == "*" && operandType != "unknown") {
        std::string pt = normalizeType(tyq::strip(operandType));
        if (!pt.empty() && pt[0] == '?') pt = pt.substr(1);
        if (isPointerType(pt) && isVoidValueType(getPointeeType(pt))) {
            errorAt(node, "cannot dereference '" + operandType + "': it points to 'void' (cast it to a typed pointer first)");
            expressionTypes[node] = "unknown";
            return;
        }
    }

    if (operandType == "unknown") {
        expressionTypes[node] = "unknown";
        return;
    }

    std::string resultType = inferUnaryExprType(node->op, operandType);

    // Address-of a const location yields a pointer *to const*, so writing through
    // it (or assigning it to a mutable pointer) is caught by the const checks.
    // Without this, `const int N; *int p = &N; *p = 99;` would silently mutate N.
    if (node->op == "&" && resultType != "error") {
        std::string dummy;
        if (assignsToConst(node->operand.get(), dummy))
            resultType = "const " + resultType;
    }

    if (resultType == "error") {
        // Unary operator overloading: `-v`/`!v`/`~v` on a non-built-in operand resolves to a
        // user `operator -/!/~(V)`. Deref `*` and address-of `&` are structural, not overloadable.
        std::string lookupOp = (node->op == "-") ? "u-" : node->op;
        std::string ret, opFn;
        if (node->op == "-" || node->op == "!" || node->op == "~")
            opFn = resolveOperator(lookupOp, {operandType}, ret);
        if (!opFn.empty()) {
            if (!inInstance) node->opFunc = opFn;
            operatorCallNodes.insert(node);
            dropGlobalNarrowings();
            calledFns.insert(opFn);
            expressionTypes[node] = ret;
        } else {
            errorAt(node,"invalid operand for unary operator: " + operandType);
            expressionTypes[node] = "unknown";
        }
    } else {
        expressionTypes[node] = resultType;
    }
}

void TypeChecker::visit(IncDecExpr* node) {
    node->operand->accept(this);
    Expr* op = node->operand.get();
    bool isLval = dynamic_cast<IdentExpr*>(op) || dynamic_cast<MemberExpr*>(op) ||
                  dynamic_cast<IndexExpr*>(op) ||
                  (dynamic_cast<UnaryExpr*>(op) && static_cast<UnaryExpr*>(op)->op == "*");
    if (isSliceLen(op))
        errorAt(node, "cannot assign to the length of a slice: it is read-only (take a new slice instead)");
    else if (!isLval)
        errorAt(node, "'++'/'--' requires a modifiable variable");
    std::string cname;
    if (assignsToConst(op, cname))
        errorAt(node, "cannot modify read-only location '" + cname + "'");
    checkCapturedWrite(node, op);
    std::string t = getExpressionType(op);
    std::string td = dealiasOperand(t);
    if (t != "unknown" && !isIntType(td) && !isPointerType(td))
        errorAt(node, "'++'/'--' requires an integer or pointer, got '" + t + "'");
    // `b.f++` of a bitfield yields the old value in the field's declared type, unpromoted
    // (as clang does); every other read of a narrow bitfield is an int.
    if (!node->prefix)
        if (auto* m = dynamic_cast<MemberExpr*>(op)) {
            std::string bt = ty::Type::parse(normalizeType(tyq::strip(getExpressionType(m->base.get())))).nominalName();
            auto sit = structs.find(bt);
            if (sit != structs.end())
                if (const auto* f = sit->second.field(m->member); f && f->bitWidth > 0) t = f->type;
        }
    expressionTypes[node] = t;
}

void TypeChecker::checkVaListArg(ASTNode* at, const std::string& what, const std::vector<ExprPtr>& args) {
    if (args.size() != 1) {
        errorAt(at, "'" + what + "' takes one argument (a va_list), got " + std::to_string(args.size()));
        return;
    }
    std::string t = getExpressionType(args[0].get());
    if (t != "unknown" && normalizeType(t) != "va_list")
        errorAt(at, "'" + what + "' needs a 'va_list', got '" + t + "'");
}

void TypeChecker::visit(CallExpr* node) {
    // The callee may assign any global: a global's narrowing ends after the call.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    fillDefaultArgs(node, "");
    // A `?:` argument to an interface parameter of a named function boxes arm by arm.
    if (auto* cid = dynamic_cast<IdentExpr*>(node->callee.get()); cid && lookupSymbol(cid->name).empty()) {
        auto sig = functionSignatures.find(cid->name);
        if (sig != functionSignatures.end())
            for (size_t i = 0; i < node->args.size() && i < sig->second.second.size(); ++i) {
                hintIfaceTarget(node->args[i].get(), sig->second.second[i]);
                if (!funcTemplateDecls.count(cid->name)) inferVariantTarget(node->args[i], sig->second.second[i]);
            }
    }
    // Variadic access builtins: va_start(ap) / va_end(ap) — void.
    if (auto* bid = dynamic_cast<IdentExpr*>(node->callee.get())) {
        if ((bid->name == "va_start" || bid->name == "va_end") && lookupSymbol(bid->name).empty()) {
            for (auto& a : node->args) a->accept(this);
            checkVaListArg(node, bid->name, node->args);
            if (bid->name == "va_start" && !inVariadicFn)
                errorAt(node, "'va_start' used in a function with no variadic parameter ('...')");
            expressionTypes[node] = "void";
            return;
        }
        // `dbg(e)`: prints `[dbg line:col] e = value` to stderr and yields e, unless the
        // program has its own `dbg`.
        if (bid->name == "dbg" && node->args.size() == 1 && lookupSymbol("dbg").empty() &&
            !functionSignatures.count("dbg") && !funcTemplateDecls.count("dbg")) {
            node->args[0]->accept(this);
            std::string t = getExpressionType(node->args[0].get());
            if (normalizeType(t) == "void") errorAt(node->callee.get(), "dbg needs a value: its argument is a void call");
            node->dbgBuiltin = true;
            expressionTypes[node] = t;
            return;
        }
        // `exception_text()`: inside a `catch` block, yields a string describing the caught
        // exception (as the uncaught reporter prints it).
        if (bid->name == "exception_text" && lookupSymbol("exception_text").empty() &&
            !functionSignatures.count("exception_text") && !funcTemplateDecls.count("exception_text")) {
            for (auto& a : node->args) a->accept(this);
            if (!node->args.empty())
                errorAt(node, "'exception_text' takes no arguments, got " + std::to_string(node->args.size()));
            if (catchDepth == 0)
                errorAt(node, "'exception_text' is only allowed inside a 'catch' block");
            expressionTypes[node] = "string";
            return;
        }
        // `__exception_text_into(buf)`: the async lowering's copy of the text into a frame field
        // of 512 chars, in the catch handler it synthesizes.
        if (bid->name == "__exception_text_into" && node->args.size() == 1 && catchDepth > 0) {
            node->args[0]->accept(this);
            expressionTypes[node] = "void";
            return;
        }
    }
    // Algebraic variant construction: `Circle(2.0)`, `Some(x)`. The callee names a
    // variant; the call yields the enum value, checking the payload field types.
    if (auto cid = dynamic_cast<IdentExpr*>(node->callee.get())) {
        auto vit = adtVariants.find(cid->name);
        if (vit != adtVariants.end() && lookupSymbol(cid->name).empty()) {
            const std::string& enumName = vit->second.first;
            const auto& payload = enumDecls[enumName]->payloads[vit->second.second];
            for (auto& a : node->args) a->accept(this);
            if (payload.empty())                  // a nullary variant is written bare: `B`
                errorAt(node, "variant '" + cid->name + "' takes no arguments");
            else if (node->args.size() != payload.size())
                errorAt(node, "variant '" + cid->name + "' expects " +
                    std::to_string(payload.size()) + " argument(s), got " +
                    std::to_string(node->args.size()));
            else
                for (size_t i = 0; i < payload.size(); ++i) {
                    std::string at = getExpressionType(node->args[i].get());
                    if (at != "unknown" && !isValidAssignment(payload[i], at))
                        errorAt(node, "variant '" + cid->name + "' argument " +
                            std::to_string(i + 1) + " type mismatch");
                    else checkVariantLiteral(node, cid->name, i, payload[i]);
                }
            expressionTypes[node] = enumName;
            return;
        }
        // Bare generic-variant construction with inference: `Some(5)`. Infer the
        // enum's type args by unifying the variant's payload against the arg types;
        // if they don't fully determine the type, require explicit args (turbofish).
        auto gvit = genericVariants.find(cid->name);
        if (gvit != genericVariants.end() && lookupSymbol(cid->name).empty()) {
            EnumDecl* ge = genericEnumDecls[gvit->second.first];
            const auto& payload = ge->payloads[gvit->second.second];
            for (auto& a : node->args) a->accept(this);
            std::set<std::string> tps(ge->typeParams.begin(), ge->typeParams.end());
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < payload.size() && i < node->args.size(); ++i) {
                std::string at = getExpressionType(node->args[i].get());
                if (at != "unknown" && !at.empty()) unifyTypeParam(payload[i], at, tps, subs);
            }
            bool allBound = true;
            for (const auto& tp : ge->typeParams) if (!subs.count(tp)) allBound = false;
            if (!allBound) {
                errorAt(node, "cannot infer type arguments for variant '" + cid->name +
                    "'; write them explicitly, e.g. " + cid->name + "<...>(...)");
                expressionTypes[node] = "unknown";
                return;
            }
            if (node->args.size() != payload.size())
                errorAt(node, "variant '" + cid->name + "' expects " +
                    std::to_string(payload.size()) + " argument(s), got " + std::to_string(node->args.size()));
            std::string inst = gvit->second.first + "<";
            for (size_t i = 0; i < ge->typeParams.size(); ++i) { if (i) inst += ","; inst += subs[ge->typeParams[i]]; }
            inst += ">";
            expressionTypes[node] = normalizeType(inst);
            return;
        }
    }
    // Method call: callee is MemberExpr (e.g. p.distance(q))
    if (auto member = dynamic_cast<MemberExpr*>(node->callee.get())) {
        member->base->accept(this);
        if (member->member == kCompleteSentinel) {         // --complete-at on `recv.` of a call
            if (!completionDone) { completeMembers(getExpressionType(member->base.get())); completionDone = true; }
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = "unknown";
            return;
        }
        // A method call dereferences its receiver: a `?*T` must be null-checked first.
        checkNullableDeref(member->base.get(), "access a member of");
        // Dot syntax dereferences at most one pointer level.
        if (std::string rt = getExpressionType(member->base.get());
            rt != "unknown" && !ty::Type::parse(normalizeType(rt)).isFn() && pointerDepth(rt) > 1) {
            errorAt(node, "cannot call method '" + member->member + "' through '" + rt +
                          "': dot syntax dereferences one pointer level (dereference it first)");
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = "unknown";
            return;
        }
        // bare nominal: *Rect and Rect both resolve to Rect_method
        std::string baseType = ty::Type::parse(getExpressionType(member->base.get())).nominalName();
        if (typeAliases.count(baseType))   // `*Alias` receiver: the aliased struct's methods
            baseType = ty::Type::parse(normalizeType(baseType)).nominalName();
        {
            // A source-form instance receiver (`Box<int>`, e.g. a field's declared
            // type) names its mangled struct, instantiated so its inline methods exist.
            ty::Type rt = ty::Type::parse(tyq::strip(getExpressionType(member->base.get())));
            while (rt.isPointer() && rt.pointee) { ty::Type inner = *rt.pointee; rt = inner; }
            if (rt.isTemplate()) baseType = ty::Type::parse(normalizeType(rt.str())).nominalName();
        }

        std::string mangled = baseType + "_" + member->member;
        if (auto st = structs.find(baseType); st != structs.end() && !st->second.field(member->member))
            fillDefaultArgs(node, mangled);
        auto mit = functionSignatures.find(mangled);
        if (mit != functionSignatures.end()) {
            const auto& sig = mit->second;
            const auto& paramTypes = sig.second; // first param is "self"
            calledFns.insert(mangled);           // -Wall: `x.m()` references `Type_m`
            noteDeprecatedUse(mangled, member->memberLine > 0 ? member->memberLine : node->line,
                              member->memberLine > 0 ? member->memberCol : node->col);
            recordMemberTooling(member, signatureText(mangled), mangled);
            // Dot syntax passes the receiver's address, so the function's first parameter
            // must be a pointer to the receiver's type (`*P self` or `const P* self`).
            if (paramTypes.empty() || !tyq::isPtr(paramTypes[0]) ||
                ty::Type::parse(normalizeType(tyq::strip(tyq::pointee(paramTypes[0])))).nominalName() != baseType) {
                errorAt(node, "'" + mangled + "' cannot be called as method '" + member->member + "' of '" +
                              baseType + "': its first parameter must be '*" + baseType + " self' (it is '" +
                              (paramTypes.empty() ? std::string("none") : paramTypes[0]) + "')");
                for (auto& a : node->args) a->accept(this);
                expressionTypes[node] = sig.first;
                return;
            }
            if (auto gm = genericMethodInsts.find(mangled); gm != genericMethodInsts.end())
                queueInstance(gm->second.fn, gm->second.owner->typeParams, gm->second.subs,
                              gm->second.owner->name, "." + member->member, mangled,
                              "*" + baseType, gm->second.owner->sourceFile);
            // A method whose `self` is a plain (mutable) pointer may write through it, so
            // it cannot be called on a read-only receiver (a const value, or through a
            // pointer to const) unless it declares `const T* self`.
            {
                std::string rt = getExpressionType(member->base.get());
                std::string cname;
                bool roRecv = tyq::isPtr(rt) ? tyq::baseConst(tyq::pointee(rt))
                                             : assignsToConst(member->base.get(), cname);
                if (roRecv && !paramTypes.empty() && !tyq::baseConst(tyq::pointee(paramTypes[0])))
                    errorAt(node, "cannot call method '" + member->member + "' on a read-only value: its '" +
                                  paramTypes[0] + " self' may modify it (declare it 'const " + baseType +
                                  "* self' if it does not)");
                else if (!tyq::isPtr(rt) && !paramTypes.empty() && !tyq::baseConst(tyq::pointee(paramTypes[0])))
                    checkCapturedMethodCall(node, member);
            }
            size_t selfSkip = 1;
            bool isVariadic = paramTypes.size() > selfSkip && paramTypes.back() == "...";
            size_t fixedCount = isVariadic ? paramTypes.size() - selfSkip - 1
                                           : paramTypes.size() - selfSkip;

            if (!isVariadic && node->args.size() != fixedCount) {
                errorAt(node,"method '" + member->member + "' expects " +
                            std::to_string(fixedCount) + " argument(s), got " +
                            std::to_string(node->args.size()));
            }
            for (size_t i = 0; i < node->args.size(); ++i) {
                node->args[i]->accept(this);
                if (i < fixedCount) {
                    std::string argType = getExpressionType(node->args[i].get());
                    size_t pi = i + selfSkip;
                    if (argType != "unknown" && !isValidAssignment(paramTypes[pi], argType)) {
                        errorAt(node,"argument " + std::to_string(i + 1) + " type mismatch");
                    }
                } else if (isVariadic) {
                    checkVariadicArg(node, node->args[i].get(), i);
                }
            }
            checkFormatCall(node, mangled, node->args, 1);
            expressionTypes[node] = sig.first;
            return;
        }
        if (checkGenericMethodCall(node, member, baseType)) return;
        // Check if baseType is an interface — resolve via interface method list
        {
            auto ifaceIt = interfaceDecls.find(baseType);
            if (ifaceIt != interfaceDecls.end()) {
                for (const auto& sig : ifaceIt->second->methods) {
                    if (sig.name == member->member) {
                        std::string d = sig.returnType + " " + sig.name + "(";
                        for (size_t i = 0; i < sig.params.size(); ++i)
                            d += (i ? ", " : "") + sig.params[i].first +
                                 (sig.params[i].second.empty() ? "" : " " + sig.params[i].second);
                        recordMemberTooling(member, d + ")", "", nullptr);
                        for (auto& arg : node->args) arg->accept(this);
                        std::vector<std::string> pts;
                        for (const auto& p : sig.params) pts.push_back(p.first);
                        checkCallArgs(node, "method '" + member->member + "'", pts);
                        expressionTypes[node] = normalizeType(sig.returnType);
                        return;
                    }
                }
                errorAt(node, "interface '" + baseType + "' has no method '" + member->member + "'");
                expressionTypes[node] = "unknown";
                return;
            }
        }
        // Inside a generic instance, a constrained call `t.m(x)` on a primitive receiver is
        // satisfied by a free function `m(T, ...)` (codegen lowers it to `m(t, x)`).
        static const std::set<std::string> kScalarPrims = {
            "int","int8","int16","int32","int64","uint","uint8","uint16","uint32",
            "uint64","char","bool","float","double"};
        if (inInstance && kScalarPrims.count(baseType)) {
            auto fit = functionSignatures.find(member->member);
            if (fit != functionSignatures.end() && !fit->second.second.empty()) {
                calledFns.insert(member->member);
                const auto& pts = fit->second.second;
                std::string recvT = getExpressionType(member->base.get());
                std::string e = assignabilityError(pts[0], recvT, member->base.get());
                if (!e.empty())
                    errorAt(node, "receiver of '" + member->member + "' type mismatch: expected " +
                                  pts[0] + ", got " + recvT + " (" + e + ")");
                for (auto& arg : node->args) arg->accept(this);
                checkCallArgs(node, "function '" + member->member + "'",
                              std::vector<std::string>(pts.begin() + 1, pts.end()));
                expressionTypes[node] = fit->second.first;
                return;
            }
        }
        // Not a method — maybe a struct field holding a fn pointer: o.op(args).
        size_t errsBeforeMember = errors.size();
        member->accept(this);
        std::string fieldTy = dealiasOperand(getExpressionType(member));
        if (fieldTy.size() > 3 && fieldTy.substr(0, 3) == "fn(") {
            for (auto& arg : node->args) arg->accept(this);
            expressionTypes[node] = checkFnValueCall(node, "'" + member->member + "'", fieldTy);
            return;
        }
        // On a non-struct receiver the error is the undefined method (as the self-host
        // reports it), not also a member access.
        // The same holds on a struct with no such field: the error is the method.
        std::string memberErr = "cannot access member '" + member->member + "' on non-struct type";
        std::string fieldErr = "has no member '" + member->member + "'";
        for (size_t k = errsBeforeMember; k < errors.size();)
            if (errors[k].find(memberErr) != std::string::npos || errors[k].find(fieldErr) != std::string::npos)
                errors.erase(errors.begin() + (long)k);
            else ++k;
        errorAt(node,"undefined method '" + member->member + "' on type '" + baseType + "'" +
                     didYouMean(member->member, 'm', getExpressionType(member->base.get())));
        expressionTypes[node] = "unknown";
        return;
    }

    // Regular function call
    std::string funcName;
    if (auto identExpr = dynamic_cast<IdentExpr*>(node->callee.get())) {
        funcName = identExpr->name;
        if (funcName == kCompleteSentinel) {               // --complete-at on a callee
            identExpr->accept(this);
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = "unknown";
            return;
        }
        calledFns.insert(funcName);  // -Wall: mark referenced
        noteDeprecatedUse(funcName, identExpr->line, identExpr->col);
        // Record use-site for go-to-definition
        if (inPrimaryFile()) useLocations[{identExpr->line, identExpr->col}] = funcName;
    } else {
        // Any other callee expression (`mk()(1)`, `(*pf)(x)`): call its fn-typed value.
        node->callee->accept(this);
        for (auto& a : node->args) a->accept(this);
        std::string ct = getExpressionType(node->callee.get());
        expressionTypes[node] = ty::Type::parse(normalizeType(ct)).isFn()
            ? checkFnValueCall(node, "the called function value", ct) : "unknown";
        return;
    }

    // Check if funcName is a variable holding a function pointer (fn(T,...)->R)
    {
        std::string varType = lookupSymbol(funcName);
        if (!varType.empty() && varType.size() > 3 && varType.substr(0, 3) == "fn(") {
            // The callee is a fn-pointer *variable*. Visit it so that, inside a
            // lambda, an outer-scope fn pointer used in callee position is
            // registered as a capture (visit(CallExpr) otherwise resolves the
            // name directly and never reaches visit(IdentExpr)).
            // calleeContext marks this occurrence as a *call* of the var, so a
            // watched closure param used here is not counted as escaping.
            { std::string prev = calleeContext; calleeContext = funcName;
              node->callee->accept(this); calleeContext = prev; }
            for (auto& a : node->args) a->accept(this);
            expressionTypes[node] = checkFnValueCall(node, "'" + funcName + "'", varType);
            return;
        }
        // A variable that is not a closure (a local may shadow a function of the same
        // name) cannot be called.
        if (!varType.empty() && !ty::Type::parse(normalizeType(varType)).isFn()) {
            for (auto& a : node->args) a->accept(this);
            errorAt(node->callee.get(), "undefined function '" + funcName + "' ('" + funcName +
                                        "' is a variable of type '" + varType + "')");
            expressionTypes[node] = "unknown";
            return;
        }
    }

    // Look up function signature
    auto it = functionSignatures.find(funcName);
    if (it == functionSignatures.end()) {
        // Template function called without explicit type arguments: infer each type
        // parameter from an argument whose parameter type is exactly that type param.
        auto tmplIt = funcTemplateDecls.find(funcName);
        if (tmplIt != funcTemplateDecls.end()) {
            FunctionDecl* fd = tmplIt->second;
            for (size_t j = 0; j < node->args.size(); ++j) visitGenericCallArg(fd, j, node->args[j]);
            std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
            std::map<std::string, std::string> subs;
            // A parameter spelled as a bare type parameter (`T a`) is a by-value deduction;
            // the others bind structurally first and take precedence.
            auto bareParam = [&](size_t j) {
                std::string p = tyq::strip(fd->params[j].first);
                return tps.count(p) ? p : std::string();
            };
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                std::string at = getExpressionType(node->args[j].get());
                if (at != "unknown" && !at.empty() && bareParam(j).empty())
                    unifyTypeParam(fd->params[j].first, at, tps, subs);
            }
            // By-value deductions of one type parameter must agree; integer ones that differ
            // meet at their common type by C's usual arithmetic conversions
            // (`maxof(1, big)` is `maxof<int64>`), any other disagreement is an error.
            std::map<std::string, std::pair<std::string, size_t>> byValue;
            bool conflict = false;
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                std::string tp = bareParam(j);
                if (tp.empty() || subs.count(tp)) continue;
                std::string at = getExpressionType(node->args[j].get());
                if (at == "unknown" || at.empty() || at == "null") continue;
                std::map<std::string, std::string> one;
                unifyTypeParam(fd->params[j].first, at, tps, one);
                if (!one.count(tp)) continue;
                at = one[tp];
                auto bv = byValue.find(tp);
                if (bv == byValue.end()) { byValue[tp] = {at, j}; continue; }
                std::string cur = bv->second.first;
                if (normalizeType(cur) == normalizeType(at)) continue;
                std::string common = ty::rangeVarType(normalizeType(cur), normalizeType(at));
                if (!common.empty()) { bv->second.first = common; continue; }
                errorAt(node, "argument " + std::to_string(j + 1) + " type mismatch: type parameter '" + tp +
                              "' is deduced as '" + cur + "' from argument " +
                              std::to_string(bv->second.second + 1) + " and as '" + at + "' here");
                conflict = true;
            }
            for (const auto& kv : byValue) subs[kv.first] = kv.second.first;
            if (conflict) {
                expressionTypes[node] = "unknown";
                return;
            }
            std::string unbound;
            for (const auto& tpName : fd->typeParams)
                if (!subs.count(tpName)) unbound += (unbound.empty() ? "" : ", ") + tpName;
            if (unbound.empty()) {
                size_t errsBefore = errors.size();
                checkConstraints(node, fd->constraints, subs);
                // The inferred instantiation's parameter types must accept every argument.
                std::vector<std::string> pts;
                for (const auto& p : fd->params) pts.push_back(substType(p.first, subs));
                checkCallArgs(node, "function '" + funcName + "'", pts);
                if (errors.size() == errsBefore) {
                    std::string mangled = funcName;
                    for (const auto& tpn : fd->typeParams) mangled += "_" + mangleTemplate(subs[tpn]);
                    queueInstance(fd, fd->typeParams, subs, funcName, "", mangled, "", fd->sourceFile);
                }
                expressionTypes[node] = genericCallRet(fd, subs);
                return;
            }
            errorAt(node, "cannot infer type argument(s) " + unbound + " of generic function '" +
                          funcName + "' from the call; write them explicitly, e.g. " + funcName + "<...>(...)");
            expressionTypes[node] = "unknown";
            return;
        }
        errorAt(node->callee.get(), "undefined function '" + funcName + "'" + didYouMean(funcName, 'v'));
        expressionTypes[node] = "unknown";
        return;
    }

    const auto& sig = it->second;
    const auto& expectedParamTypes = sig.second;

    bool isVariadic = !expectedParamTypes.empty() && expectedParamTypes.back() == "...";
    size_t fixedCount = isVariadic ? expectedParamTypes.size() - 1 : expectedParamTypes.size();

    // Check argument count
    if (isVariadic) {
        if (node->args.size() < fixedCount) {
            errorAt(node,"function '" + funcName + "' expects at least " +
                        std::to_string(fixedCount) + " arguments, got " +
                        std::to_string(node->args.size()));
            expressionTypes[node] = sig.first;
            return;
        }
    } else if (node->args.size() != fixedCount) {
        errorAt(node,"function '" + funcName + "' expects " +
                    std::to_string(fixedCount) + " arguments, got " +
                    std::to_string(node->args.size()));
        expressionTypes[node] = sig.first;
        return;
    }

    // Per-param escaping flags for this callee (empty if none declared).
    auto escIt = functionParamEscaping.find(funcName);
    const std::vector<bool>* escVec =
        (escIt != functionParamEscaping.end() && !escIt->second.empty())
            ? &escIt->second : nullptr;

    // Type check fixed arguments; visit (but do not type-check) variadic extras
    for (size_t i = 0; i < node->args.size(); ++i) {
        // A watched (non-escaping) closure param passed straight to a non-escaping
        // param of an Eskiu function does not escape either: the callee may only
        // call it. (A C function's fn-typed param is a C function pointer, not this.)
        bool forwards = false;
        if (auto* aid = dynamic_cast<IdentExpr*>(node->args[i].get())) {
            bool paramEscapes = escVec && i < escVec->size() && (*escVec)[i];
            forwards = nonEscapingFnParams.count(aid->name) && !paramEscapes &&
                       !externFnNames.count(funcName) && i < fixedCount;
        }
        if (forwards) {
            std::string prev = calleeContext;
            calleeContext = static_cast<IdentExpr*>(node->args[i].get())->name;
            node->args[i]->accept(this);
            calleeContext = prev;
        } else {
            node->args[i]->accept(this);
        }
        // Escape optimization: a lambda passed directly to a NON-escaping
        // parameter does not outlive the call (the callee may only call it —
        // enforced by the soundness check), so its env can stay on the stack.
        if (auto* lam = dynamic_cast<LambdaExpr*>(node->args[i].get())) {
            bool paramEscapes = escVec && i < escVec->size() && (*escVec)[i];
            // Set both ways: the check after the async lowering sees an async fn's closure
            // params as escaping (the frame keeps them), overriding the first check.
            if (!inInstance) lam->escapes = paramEscapes;
            else {
                // Decided after the instances; an async callee's frame keeps its closures.
                bool asyncCallee = false;
                auto fa = fnDeclsAll.find(funcName);
                if (fa != fnDeclsAll.end())
                    for (const FunctionDecl* d : fa->second) asyncCallee = asyncCallee || d->isAsync;
                deferredLambdas[lam].escapes |= paramEscapes || asyncCallee;
            }
        }
        if (i < fixedCount) {
            std::string argType = getExpressionType(node->args[i].get());
            // A C function's fn-typed parameter is a C function pointer: C can only call
            // a named top-level function (or get null), never a closure's environment.
            if (externFnNames.count(funcName)
                    && ty::Type::parse(normalizeType(expectedParamTypes[i])).isFn()) {
                auto* id = dynamic_cast<IdentExpr*>(node->args[i].get());
                auto* lit = dynamic_cast<LiteralExpr*>(node->args[i].get());
                if (lit && lit->kind == LiteralExpr::Kind::NULL_VAL) continue;
                if (!(id && lookupSymbol(id->name).empty() && functionSignatures.count(id->name))) {
                    errorAt(node, "argument " + std::to_string(i + 1) + " of extern '" + funcName +
                                  "' is a C function pointer: pass a top-level function by name, not a closure value");
                    continue;
                }
            }
            std::string e = assignabilityError(expectedParamTypes[i], argType, node->args[i].get());
            if (!e.empty())
                errorAt(node,"argument " + std::to_string(i + 1) + " type mismatch: expected " +
                            expectedParamTypes[i] + ", got " + argType + " (" + e + ")");
        } else if (isVariadic) {
            checkVariadicArg(node, node->args[i].get(), i);
        }
    }

    if (isVariadic) checkFormatCall(node, funcName, node->args, 0);
    expressionTypes[node] = sig.first;
}

// `x.m(args)` where x is an instance `S<A..>` of a generic struct and `S_m<T..>` is a
// generic free function taking the receiver first (the stdlib's `Type_method`
// convention, e.g. `List_push<T>(List<T>* self, T item)`): the call is `S_m(&x, args)`
// (or `S_m(x, args)` for a pointer receiver), with the type arguments unified from
// the receiver and then the arguments. Returns false when no such function exists.
bool TypeChecker::checkGenericMethodCall(CallExpr* node, MemberExpr* member, const std::string& baseType) {
    auto ti = templateInstanceArgs.find(baseType);
    if (ti == templateInstanceArgs.end())   // a source-form receiver type (`Chan<int>*`)
        ti = templateInstanceArgs.find(
            ty::Type::parse(normalizeType(getExpressionType(member->base.get()))).nominalName());
    if (ti == templateInstanceArgs.end()) return false;
    std::string fnName = ti->second.first + "_" + member->member;
    auto ft = funcTemplateDecls.find(fnName);
    if (ft == funcTemplateDecls.end() || ft->second->params.empty()) return false;
    FunctionDecl* fd = ft->second;
    appendDefaultArgs(node->args, node->args.size() + 1, fnName, node);
    calledFns.insert(fnName);
    noteDeprecatedUse(fnName, member->memberLine > 0 ? member->memberLine : node->line,
                      member->memberLine > 0 ? member->memberCol : node->col);
    recordMemberTooling(member, signatureText(fnName), fnName);
    for (auto& a : node->args) a->accept(this);

    const std::string& selfT = fd->params[0].first;
    std::string recvT = getExpressionType(member->base.get());
    bool recvPtr = tyq::isPtr(recvT), selfPtr = tyq::isPtr(selfT);
    std::string recvAsSelf = recvT;
    if (selfPtr && !recvPtr) recvAsSelf = "*" + recvT;
    else if (!selfPtr && recvPtr) recvAsSelf = tyq::pointee(recvT);
    if (selfPtr && recvPtr) {                                  // `*Alias` receiver
        std::string pe = tyq::pointee(recvT), de = dealiasOperand(pe);
        if (de != pe) recvAsSelf = "*" + de;
    }

    std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
    std::map<std::string, std::string> subs;
    unifyTypeParam(selfT, recvAsSelf, tps, subs);
    for (size_t j = 1; j < fd->params.size() && j - 1 < node->args.size(); ++j) {
        std::string at = getExpressionType(node->args[j - 1].get());
        if (at != "unknown" && !at.empty()) unifyTypeParam(fd->params[j].first, at, tps, subs);
    }
    std::string unbound;
    for (const auto& tp : fd->typeParams)
        if (!subs.count(tp)) unbound += (unbound.empty() ? "" : ", ") + tp;
    if (!unbound.empty()) {
        errorAt(node, "cannot infer type argument(s) " + unbound + " of generic function '" +
                      fnName + "' from the method call; call it explicitly, e.g. " + fnName + "<...>(...)");
        expressionTypes[node] = "unknown";
        return true;
    }
    size_t errsBefore = errors.size();
    checkConstraints(node, fd->constraints, subs);
    // A plain (mutable) pointer `self` may write through it, so it cannot take a
    // read-only receiver (a const value, or one reached through a pointer to const).
    if (selfPtr) {
        std::string cname;
        bool roRecv = recvPtr ? tyq::baseConst(tyq::pointee(recvT))
                              : assignsToConst(member->base.get(), cname);
        if (roRecv && !tyq::baseConst(tyq::pointee(selfT)))
            errorAt(node, "cannot call method '" + member->member + "' on a read-only value: '" +
                          fnName + "' takes '" + selfT + "' and may modify it");
        else if (!recvPtr && !tyq::baseConst(tyq::pointee(selfT)))
            checkCapturedMethodCall(node, member);
    }
    std::vector<std::string> pts;
    for (size_t j = 1; j < fd->params.size(); ++j) pts.push_back(substType(fd->params[j].first, subs));
    checkCallArgs(node, "method '" + member->member + "'", pts);
    if (errors.size() == errsBefore) {
        std::string mangled = fnName;
        for (const auto& tpn : fd->typeParams) mangled += "_" + mangleTemplate(subs[tpn]);
        queueInstance(fd, fd->typeParams, subs, fnName, "", mangled, "", fd->sourceFile);
    }
    expressionTypes[node] = genericCallRet(fd, subs);
    return true;
}

// The argument classes a printf conversion can take, after C's default promotions.
enum { FmtI32 = 1, FmtI64, FmtFloat, FmtStr, FmtPtr, FmtOther };

int TypeChecker::formatArgClass(Expr* arg) {
    if (auto* l = dynamic_cast<LiteralExpr*>(arg); l && l->kind == LiteralExpr::Kind::NULL_VAL) return FmtPtr;
    std::string t = normalizeType(tyq::strip(getExpressionType(arg)));
    if (!t.empty() && t[0] == '?') t = t.substr(1);
    static const std::set<std::string> i32 = {"bool", "char", "int8", "uint8", "int16", "uint16",
                                              "int", "int32", "uint", "uint32"};
    if (i32.count(t)) return FmtI32;
    if (t == "int64" || t == "uint64") return FmtI64;
    if (t == "float" || t == "double") return FmtFloat;
    if (t == "string") return FmtStr;
    if (isPointerType(t)) {
        std::string pt = tyq::strip(normalizeType(getPointeeType(t)));
        if (pt == "char" || pt == "uint8" || pt == "int8") return FmtStr;
        return FmtPtr;
    }
    return FmtOther;
}

// printf-style functions: the format argument's index among the call's arguments.
static int formatIndexOf(const std::string& fn) {
    static const std::map<std::string, int> table = {
        {"printf", 0}, {"fprintf", 1}, {"dprintf", 1}, {"sprintf", 1}, {"snprintf", 2},
        {"String_format", 1}, {"String_appendf", 1}, {"log_at", 1}, {"log_trace", 0},
        {"log_debug", 0}, {"log_info", 0}, {"log_warn", 0}, {"log_error", 0}};
    auto it = table.find(fn);
    return it == table.end() ? -1 : it->second;
}

void TypeChecker::checkFormatCall(Expr* call, const std::string& fn, const std::vector<ExprPtr>& args, int shift) {
    int fi = formatIndexOf(fn);
    if (fi < 0) return;
    fi -= shift;
    if (fi < 0 || fi >= (int)args.size()) return;
    auto* lit = dynamic_cast<LiteralExpr*>(args[fi].get());
    if (!lit || lit->kind != LiteralExpr::Kind::STRING) return;
    bool win = targetTriple.empty()
#ifdef _WIN32
        ? true
#else
        ? false
#endif
        : (targetTriple.find("windows") != std::string::npos || targetTriple.find("mingw") != std::string::npos ||
           targetTriple.find("win32") != std::string::npos);
    const std::string& f = lit->value;
    size_t next = fi + 1;
    auto need = [&](int cls, const std::string& conv) {
        if (next >= args.size()) { ++next; return; }
        Expr* a = args[next].get();
        int got = formatArgClass(a);
        ++next;
        if (got == cls || (cls == FmtPtr && got == FmtStr)) return;
        std::string at = normalizeType(getExpressionType(a));
        if (at == "unknown" || at == "void") return;   // already an error
        static const char* what[] = {"", "an int", "a 64-bit integer", "a double", "a string", "a pointer", ""};
        noteWarning("format", call->line, call->col,
                    "format '" + conv + "' expects " + what[cls] + ", but argument " + std::to_string(next + shift) +
                    " has type '" + displayType(getExpressionType(a)) + "'");
    };
    for (size_t i = 0; i < f.size(); ++i) {
        if (f[i] != '%') continue;
        size_t st = i++;
        if (i < f.size() && f[i] == '%') continue;
        while (i < f.size() && std::strchr("-+ #0'", f[i])) ++i;
        if (i < f.size() && f[i] == '*') { need(FmtI32, "*"); ++i; }
        else while (i < f.size() && std::isdigit((unsigned char)f[i])) ++i;
        if (i < f.size() && f[i] == '.') {
            ++i;
            if (i < f.size() && f[i] == '*') { need(FmtI32, ".*"); ++i; }
            else while (i < f.size() && std::isdigit((unsigned char)f[i])) ++i;
        }
        std::string len;
        while (i < f.size() && std::strchr("hlLqjzt", f[i])) len += f[i++];
        if (i >= f.size()) {
            noteWarning("format", call->line, call->col, "incomplete conversion '" + f.substr(st) + "' at the end of the format string");
            break;
        }
        char c = f[i];
        std::string conv = f.substr(st, i - st + 1);
        bool wide = len == "ll" || len == "j" || len == "z" || len == "t" || len == "q" || (len == "l" && !win);
        if (std::strchr("diouxXc", c)) need(c == 'c' ? FmtI32 : (wide ? FmtI64 : FmtI32), conv);
        else if (std::strchr("fFeEgGaA", c)) need(FmtFloat, conv);
        else if (c == 's') need(FmtStr, conv);
        else if (c == 'p' || c == 'n') need(FmtPtr, conv);
        else {
            noteWarning("format", call->line, call->col, "unknown conversion '" + conv + "' in the format string");
            return;
        }
    }
    if (next > args.size())
        noteWarning("format", call->line, call->col, "the format string needs " + std::to_string(next - fi - 1) +
                    " argument(s), but " + std::to_string(args.size() - fi - 1) + " are given");
    else if (next < args.size())
        noteWarning("format", call->line, call->col, "the format string uses " + std::to_string(next - fi - 1) +
                    " argument(s), but " + std::to_string(args.size() - fi - 1) + " are given");
}

// An argument passed through `...` must have a value: a `void` call has none.
void TypeChecker::checkVariadicArg(Expr* call, Expr* arg, size_t i) {
    if (normalizeType(getExpressionType(arg)) == "void")
        errorAt(call, "argument " + std::to_string(i + 1) + " has type 'void' (a void call has no value)");
}

// A parameter default folded to a constant: a literal, a negative float literal, an
// integer constant expression over literals, enum members and `const` globals (folded in
// the global scope, so a caller's locals never rebind it), or a struct literal whose
// values are all of these (`V3 up = V3{0.0, 1.0, 0.0}`). Null when it is none of them.
ExprPtr TypeChecker::foldParamDefault(Expr* def) {
    if (auto* l = dynamic_cast<LiteralExpr*>(def)) return std::make_shared<LiteralExpr>(l->kind, l->value);
    auto* u = dynamic_cast<UnaryExpr*>(def);
    auto* ul = u && u->op == "-" ? dynamic_cast<LiteralExpr*>(u->operand.get()) : nullptr;
    if (ul && ul->kind == LiteralExpr::Kind::FLOAT) return std::make_shared<LiteralExpr>(ul->kind, "-" + ul->value);
    if (auto* si = dynamic_cast<StructInitExpr*>(def)) {
        std::vector<std::pair<std::string, ExprPtr>> fields;
        for (auto& f : si->fieldInits) {
            ExprPtr v = f.second ? foldParamDefault(f.second.get()) : nullptr;
            if (!v) return nullptr;
            v->line = f.second->line;
            v->col = f.second->col;
            fields.emplace_back(f.first, v);
        }
        auto out = std::make_shared<StructInitExpr>(si->structName, std::move(fields));
        out->line = si->line;
        out->col = si->col;
        return out;
    }
    long long v = 0;
    globalLookupOnly = true;
    bool ok = foldConstInt(def, v);
    globalLookupOnly = false;
    if (ok) return std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, std::to_string(v));
    return nullptr;
}

namespace {
// A fresh copy of a folded default (a literal, or a struct literal of them) at a call.
ExprPtr cloneParamDefault(const Expr* d, int line, int col) {
    ExprPtr out;
    if (auto* l = dynamic_cast<const LiteralExpr*>(d)) {
        out = std::make_shared<LiteralExpr>(l->kind, l->value);
    } else if (auto* si = dynamic_cast<const StructInitExpr*>(d)) {
        std::vector<std::pair<std::string, ExprPtr>> fields;
        for (const auto& f : si->fieldInits) fields.emplace_back(f.first, cloneParamDefault(f.second.get(), line, col));
        out = std::make_shared<StructInitExpr>(si->structName, std::move(fields));
    }
    if (out) {
        out->line = line;
        out->col = col;
    }
    return out;
}
}  // namespace

// The folded default values of function `fn`'s parameters (see paramDefaultCache and
// foldParamDefault); each call site gets a fresh copy. Errors are reported once, at the
// default.
const std::vector<ExprPtr>& TypeChecker::paramDefaultsOf(const std::string& fn) {
    auto cached = paramDefaultCache.find(fn);
    if (cached != paramDefaultCache.end()) return cached->second;
    auto& out = paramDefaultCache[fn];
    auto all = fnDeclsAll.find(fn);
    if (all == fnDeclsAll.end()) return out;
    std::string saved = curFile;
    const FunctionDecl* fd = nullptr;
    for (const FunctionDecl* d : all->second) {
        if (d->paramDefaults.empty()) continue;
        if (fd) {
            if (!d->sourceFile.empty()) curFile = d->sourceFile;
            errorAt(const_cast<FunctionDecl*>(d), "default values of '" + fn +
                    "' are given twice (give them on one declaration)");
            curFile = saved;
            continue;
        }
        fd = d;
    }
    if (!fd) return out;
    if (!fd->sourceFile.empty()) curFile = fd->sourceFile;
    const std::string self = fnDecls[fn].second;
    bool genericOwner = !self.empty() && templateDecls.count(self.substr(1));
    if (!self.empty()) out.push_back(nullptr);
    bool seen = false;
    for (size_t i = 0; i < fd->params.size(); ++i) {
        Expr* def = i < fd->paramDefaults.size() ? fd->paramDefaults[i].get() : nullptr;
        const std::string& pname = fd->params[i].second;
        if (fd->params[i].first == "...") {
            if (seen) errorAt(const_cast<FunctionDecl*>(fd), "'" + fn +
                              "' is variadic: its parameters cannot have default values");
            out.push_back(nullptr);
            continue;
        }
        if (!def) {
            if (seen) errorAt(const_cast<FunctionDecl*>(fd), "parameter '" + pname +
                              "' needs a default value: it follows a parameter that has one");
            out.push_back(nullptr);
            continue;
        }
        seen = true;
        // The names a default reads are uses (definition, references, rename).
        if (fd->sourceFile.empty() || fd->sourceFile == sourceFile) {
            std::function<void(Expr*)> noteUses = [&](Expr* e) {
                if (auto* id = dynamic_cast<IdentExpr*>(e)) {
                    if (id->line > 0) useLocations[{id->line, id->col}] = id->name;
                    return;
                }
                astwalk::forEachChildExpr(e, [&](ExprPtr& c) { if (c) noteUses(c.get()); });
            };
            noteUses(def);
        }
        if (genericOwner) {
            errorAt(def, "a method of generic struct '" + self.substr(1) + "' cannot have default values");
            out.push_back(nullptr);
            continue;
        }
        ExprPtr lit = foldParamDefault(def);
        if (!lit) {
            errorAt(def, "default value of parameter '" + pname +
                         "' is not a constant (a literal, an integer constant expression or a struct literal of those)");
            out.push_back(nullptr);
            continue;
        }
        if (fd->typeParams.empty()) {   // its type is checked where it is declared
            lit->accept(this);
            std::string e = assignabilityError(fd->params[i].first, getExpressionType(lit.get()), lit.get());
            if (!e.empty()) errorAt(def, "default value of parameter '" + pname + "': " + e);
        }
        out.push_back(lit);
    }
    curFile = saved;
    return out;
}

// A call that omits trailing arguments with default values gets a copy of each (at the
// call's position): `connect("h")` is `connect("h", 443)`. Direct calls of a function
// (not hidden by a variable) here; a dot-call once its method is resolved (`given`
// then counts the receiver).
void TypeChecker::fillDefaultArgs(CallExpr* node, const std::string& method) {
    std::string fn = method;
    size_t given = node->args.size() + (method.empty() ? 0 : 1);
    if (method.empty()) {
        auto* id = dynamic_cast<IdentExpr*>(node->callee.get());
        if (!id || !lookupSymbol(id->name).empty()) return;
        fn = id->name;
    }
    appendDefaultArgs(node->args, given, fn, node);
}

// The defaults of `fn`'s parameters from `given` on, appended to `args` at `at`'s
// position (a call, or an explicit generic call `f<int>(x)`).
void TypeChecker::appendDefaultArgs(std::vector<ExprPtr>& args, size_t given, const std::string& fn, const Expr* at) {
    if (!fnDeclsAll.count(fn)) return;
    const auto& defs = paramDefaultsOf(fn);
    if (given >= defs.size()) return;
    for (size_t i = given; i < defs.size(); ++i)
        if (!defs[i]) return;
    for (size_t i = given; i < defs.size(); ++i) args.push_back(cloneParamDefault(defs[i].get(), at->line, at->col));
}

void TypeChecker::checkCallArgs(CallExpr* node, const std::string& what,
                                const std::vector<std::string>& paramTypes) {
    bool variadic = !paramTypes.empty() && paramTypes.back() == "...";
    size_t fixed = variadic ? paramTypes.size() - 1 : paramTypes.size();
    if (variadic ? node->args.size() < fixed : node->args.size() != fixed) {
        errorAt(node, what + " expects " + (variadic ? "at least " : "") + std::to_string(fixed) +
                      " argument(s), got " + std::to_string(node->args.size()));
        return;
    }
    for (size_t i = 0; i < fixed; ++i) {
        std::string at = getExpressionType(node->args[i].get());
        std::string e = assignabilityError(paramTypes[i], at, node->args[i].get());
        if (!e.empty())
            errorAt(node, "argument " + std::to_string(i + 1) + " type mismatch: expected " +
                          paramTypes[i] + ", got " + at + " (" + e + ")");
    }
    if (variadic)
        for (size_t i = fixed; i < node->args.size(); ++i) checkVariadicArg(node, node->args[i].get(), i);
}

std::string TypeChecker::checkFnValueCall(CallExpr* node, const std::string& what, const std::string& fnType) {
    ty::Type ft = ty::Type::parse(normalizeType(fnType));
    if (!ft.isFn()) return "unknown";
    std::vector<std::string> pts;
    for (const auto& p : ft.params) pts.push_back(p.str());
    checkCallArgs(node, what, pts);
    return ft.ret ? normalizeType(ft.ret->str()) : "unknown";
}

// An integer literal argument of a variant constructor must fit its payload type, as for
// a function argument (`A(300)` for `A(int8)`).
void TypeChecker::checkVariantLiteral(ASTNode* node, const std::string& variant, size_t i,
                                      const std::string& payloadType) {
    Expr* arg = nullptr;
    if (auto* c = dynamic_cast<CallExpr*>(node)) arg = c->args[i].get();
    else if (auto* t = dynamic_cast<TemplateCallExpr*>(node)) arg = t->args[i].get();
    auto* lit = dynamic_cast<LiteralExpr*>(arg);
    std::string pt = normalizeType(payloadType);
    if (lit && lit->kind == LiteralExpr::Kind::INT && isIntType(pt) && !intLiteralFits(pt, lit))
        errorAt(node, "variant '" + variant + "' argument " + std::to_string(i + 1) + ": integer literal " +
                      lit->value + " is out of range for '" + payloadType + "'");
}

void TypeChecker::visit(IndexExpr* node) {
    node->base->accept(this);
    node->index->accept(this);
    if (node->highIndex) node->highIndex->accept(this);
    checkNullableDeref(node->base.get(), "index");
    // A slice of a fixed array refers to the array's storage (a string or pointer base does not).
    if (node->highIndex && ty::Type::parse(normalizeType(getExpressionType(node->base.get()))).kind ==
                               ty::Type::Kind::Array)
        checkCapturedAddress(node, node->base.get());

    std::string baseType = getExpressionType(node->base.get());
    std::string indexType = getExpressionType(node->index.get());

    if (indexType != "unknown" && !isIntType(indexType)) {
        errorAt(node,"array index must be integer, got " + indexType);
    }
    if (node->highIndex) {
        std::string hiType = getExpressionType(node->highIndex.get());
        if (hiType != "unknown" && !isIntType(hiType))
            errorAt(node, "slice bound must be integer, got " + hiType);
    }

    // Determine the element type of the base (array / slice / pointer / string), through
    // an alias of one (`type PA = *Sq[2]`, an element of type `IS` for `type IS = int[]`).
    std::string shape = dealiasOperand(baseType);
    ty::Type bt = ty::Type::parse(shape);
    std::string elem;
    bool haveElem = false;
    if (shape == "string") { elem = "char"; haveElem = true; }
    else if (bt.kind == ty::Type::Kind::Array || bt.kind == ty::Type::Kind::Slice) {
        elem = bt.elem->str(); haveElem = true;
        // Constant indices and bounds (a literal, an enum member, a `const`, or an
        // expression over them) into a fixed array are checked at compile time. The
        // dimension is a number or an enum member; a `const` dimension is not resolved here.
        long long dimV = -1;
        {
            const std::string& dim = bt.dim;
            if (!dim.empty() && std::all_of(dim.begin(), dim.end(), [](unsigned char c){ return std::isdigit(c); })) {
                try { dimV = std::stoll(dim); } catch (...) { dimV = -1; }
            } else if (auto ec = enumConstants.find(dim); ec != enumConstants.end()) {
                dimV = ec->second;
            }
        }
        const std::string dimS = std::to_string(dimV);
        // A slice lets its elements be written, so a read-only array has no slice.
        if (node->highIndex && bt.kind == ty::Type::Kind::Array) {
            std::string cname;
            if (assignsToConst(node->base.get(), cname))
                errorAt(node, "cannot slice read-only array '" + cname + "': a slice allows writing its elements");
            else if (!isLvalueExpr(node->base.get()))
                errorAt(node, "cannot slice a temporary array: store it in a variable first");
        }
        // Constant slice bounds `a[lo..hi]`: 0 <= lo <= hi on any base, and <= N into a fixed array.
        if (node->highIndex) {
            long long lo = 0, hi = 0;
            bool haveLo = foldConstInt(node->index.get(), lo), haveHi = foldConstInt(node->highIndex.get(), hi);
            if ((haveLo && lo < 0) || (haveHi && hi < 0))
                errorAt(node, "slice bound is negative");
            else if (haveLo && haveHi && lo > hi)
                errorAt(node, "slice bounds out of order: " + std::to_string(lo) + ".." + std::to_string(hi));
            else if (dimV >= 0 && ((haveHi && hi > dimV) || (haveLo && lo > dimV)))
                errorAt(node, "slice bound " + std::to_string(haveHi && hi > dimV ? hi : lo) +
                              " is out of bounds for array of size " + dimS);
        }
        if (!node->highIndex && bt.kind == ty::Type::Kind::Array) {
            long long idx = 0;
            if (foldConstInt(node->index.get(), idx)) {
                if (idx < 0)
                    errorAt(node, "array index " + std::to_string(idx) + " is out of bounds");
                else if (dimV >= 0 && idx >= dimV)
                    errorAt(node, "array index " + std::to_string(idx) +
                                  " is out of bounds for array of size " + dimS);
            }
        }
    } else if (isPointerType(shape)) {
        elem = getPointeeType(shape); haveElem = true;
    }

    // Overloaded subscript: `base[i]` on a non-built-in indexable resolves to a user
    // `operator [](Base, Index)` (read/rvalue form; a slice `base[lo..hi]` is not overloaded).
    if (!haveElem && !node->highIndex) {
        std::string ret, opFn = resolveOperator("[]", {baseType, indexType}, ret);
        if (!opFn.empty()) {
            if (!inInstance) node->opFunc = opFn;
            operatorCallNodes.insert(node);
            dropGlobalNarrowings();
            calledFns.insert(opFn);
            expressionTypes[node] = ret;
            return;
        }
    }

    if (!haveElem) {
        // Only a base certain to be unindexable is reported: a number, or a struct/sum-type
        // value with no `operator []` (other spellings, e.g. an array of fn values, are
        // left to the element resolution in codegen).
        std::string nb = normalizeType(tyq::strip(baseType));
        if (isNumericType(nb) || nb.rfind("struct:", 0) == 0 || adtEnums.count(nb) || nb == "null")
            errorAt(node, "cannot index into a value of type '" + ty::Type::parse(baseType).nominalName() + "'");
        expressionTypes[node] = "unknown";
        return;
    }

    // `base[lo..hi]` yields a slice of the element type; `base[i]` yields the element.
    expressionTypes[node] = node->highIndex ? (elem + "[]") : elem;
}

void TypeChecker::visit(MemberExpr* node) {
    node->base->accept(this);
    checkNullableDeref(node->base.get(), "access a member of");

    std::string baseType = getExpressionType(node->base.get());
    if (!baseType.empty() && baseType[0] == '?') baseType = baseType.substr(1);   // `?*T` derefs like `*T`
    if (baseType.rfind("const ", 0) == 0) baseType = baseType.substr(6);         // `const *T` derefs like `*T`
    // Through an alias (`type PSq = *Sq`, `type IS = int[]`) the member is the target's.
    baseType = dealiasOperand(baseType);
    if (!baseType.empty() && baseType[0] == '?') baseType = baseType.substr(1);
    if (node->member == kCompleteSentinel) {           // --complete-at after `recv.`
        if (!completionDone) { completeMembers(baseType); completionDone = true; }
        expressionTypes[node] = "unknown";
        return;
    }

    // Slice `.len` → int64 (the fat pointer's length field).
    if (node->member == "len" && ty::Type::parse(baseType).kind == ty::Type::Kind::Slice) {
        expressionTypes[node] = "int64";
        return;
    }

    // Auto-deref pointer-to-struct: *Point, struct:Point*, Point* → struct:Point
    if (hasPointerSuffix(baseType)) {
        baseType = extractBaseType(baseType);
    } else if (!baseType.empty() && baseType.front() == '*') {
        baseType = baseType.substr(1); // strip leading *
    }
    baseType = normalizeType(baseType); // "Point" → "struct:Point" if registered

    // Check if base is a struct type
    if (baseType.find("struct:") == 0) {
        // Extract struct name (remove "struct:" prefix)
        std::string structName = baseType.substr(7);  // strlen("struct:") = 7

        // Look up struct in registry
        auto it = structs.find(structName);
        if (it == structs.end()) {
            errorAt(node, "undefined struct '" + structName + "'");
            expressionTypes[node] = "unknown";
            return;
        }

        // Look for the member in struct's fields
        const auto& structInfo = it->second;
        if (const StructDecl::Field* fp = structInfo.field(node->member)) {
            const StructDecl::Field& field = *fp;
            {
                std::string owner = structName;
                if (auto ti = templateInstanceArgs.find(structName); ti != templateInstanceArgs.end())
                    owner = ti->second.first;
                auto fl = fieldLocations.find(owner + "." + field.name);
                recordMemberTooling(node, field.type, "",
                                    fl != fieldLocations.end() && fl->second.line > 0 ? &fl->second : nullptr);
                // Found the member, return its type (a narrow bitfield reads as `int`, C)
                expressionTypes[node] = field.type;
                if (field.bitWidth > 0 &&
                    tyq::bitfieldReadType(dealiasOperand(tyq::strip(field.type)), field.bitWidth) == "int")
                    expressionTypes[node] = "int";
                return;
            }
        }

        // Member not found in struct
        errorAt(node,"struct '" + structName + "' has no member '" + node->member + "'" +
                     didYouMean(node->member, 'm', baseType));
        expressionTypes[node] = "unknown";
    } else if (baseType == "unknown") {
        // Base type is unknown, can't validate member access
        expressionTypes[node] = "unknown";
    } else {
        // Base is not a struct
        errorAt(node,"cannot access member '" + node->member + "' on non-struct type '" + baseType + "'");
        expressionTypes[node] = "unknown";
    }
}

void TypeChecker::visit(CastExpr* node) {
    node->expr->accept(this);
    // `(void)e` (C): evaluate `e` and discard its value, e.g. to mark a result as unused.
    if (tyq::strip(node->targetType) == "void") {
        expressionTypes[node] = "void";
        return;
    }
    // Validate that struct types exist in casts
    std::string normalizedType = normalizeType(node->targetType);
    validateStructType(normalizedType, node);
    // A cast converts between scalars: numbers (incl. bool/char/enums), pointers, and
    // integer<->pointer. An aggregate (struct, union, sum type, array, slice, closure)
    // casts only to its own type; float<->pointer has no meaning.
    {
        std::string from = normalizeType(tyq::strip(getExpressionType(node->expr.get())));
        std::string to = normalizeType(tyq::strip(normalizedType));
        if (!from.empty() && from[0] == '?') from = from.substr(1);
        if (!to.empty() && to[0] == '?') to = to.substr(1);
        bool fnName = false;   // a top-level function cast to a pointer is its raw C address
        if (auto* id = dynamic_cast<IdentExpr*>(node->expr.get()))
            fnName = lookupSymbol(id->name).empty() && functionSignatures.count(id->name);
        auto isPtr = [&](const std::string& t) { return isPointerType(t) || t == "null"; };
        bool ok = from == "unknown" || to == "unknown" || from == to || (fnName && isPtr(to)) ||
                  (isNumericType(from) && isNumericType(to)) ||
                  (isPtr(from) && isPtr(to)) ||
                  (isIntType(from) && isPtr(to)) || (isPtr(from) && isIntType(to));
        if (!ok)
            errorAt(node, "cannot cast '" + getExpressionType(node->expr.get()) + "' to '" + node->targetType + "'");
        // A floating constant cast to an integer type must fit it: the conversion is
        // undefined in C, and the backends would not agree on a value.
        const Expr* src = node->expr.get();
        bool neg = false;
        if (auto* u = dynamic_cast<const UnaryExpr*>(src); u && u->op == "-") { neg = true; src = u->operand.get(); }
        auto* lit = dynamic_cast<const LiteralExpr*>(src);
        bool isInt = true; long long iv = 0; double v = 0;
        if (ok && isIntType(to) && to != "bool" && foldConstNum(node->expr.get(), isInt, iv, v) &&
            !isInt && !floatConstFitsInt(v, to)) {
            if (lit && lit->kind == LiteralExpr::Kind::FLOAT)
                errorAt(node, "floating constant " + std::string(neg ? "-" : "") + lit->value +
                              " is out of range for '" + node->targetType + "'");
            else {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%g", v);
                errorAt(node, "floating constant expression (value " + std::string(buf) +
                              ") is out of range for '" + node->targetType + "'");
            }
        }
    }
    expressionTypes[node] = normalizedType;
}

void TypeChecker::visit(LiteralExpr* node) {
    switch (node->kind) {
        case LiteralExpr::Kind::INT:
            // C: an unsuffixed literal is the first of int, int64 that holds it (codegen
            // emits the value in that width); only an unsigned 64-bit type holds more.
            try {
                long long v = std::stoll(node->value, nullptr, 0);
                expressionTypes[node] = (v >= INT32_MIN && v <= INT32_MAX) ? "int" : "int64";
            } catch (...) {
                expressionTypes[node] = "uint64";
            }
            break;
        case LiteralExpr::Kind::FLOAT:
            // A float literal lowers to a `double` constant (the lexer has no
            // float/double distinction; codegen emits ConstantFP::getDoubleTy).
            // Sema previously said "float" — a latent disagreement with codegen.
            expressionTypes[node] = "double";
            break;
        case LiteralExpr::Kind::STRING:
            expressionTypes[node] = "string";
            break;
        case LiteralExpr::Kind::CHAR:
            expressionTypes[node] = "char";
            break;
        case LiteralExpr::Kind::BOOL:
            expressionTypes[node] = "bool";
            break;
        case LiteralExpr::Kind::NULL_VAL:
            expressionTypes[node] = "null";
            break;
        default:
            expressionTypes[node] = "unknown";
    }
}

void TypeChecker::visit(IdentExpr* node) {
    if (node->name == kCompleteSentinel) {             // --complete-at
        if (!completionDone) { completeScope(); completionDone = true; }
        expressionTypes[node] = "unknown";
        return;
    }
    // -Wall: a function referenced as a value counts as used.
    if (functionSignatures.count(node->name)) {
        calledFns.insert(node->name);
        noteDeprecatedUse(node->name, node->line, node->col);
    }

    // Escape soundness: a watched closure param referenced anywhere other than
    // as the immediate callee of a call escapes (see visit(FunctionDecl)).
    if (nonEscapingFnParams.count(node->name) && node->name != calleeContext)
        escapedFnParams.insert(node->name);
    if (lambdaLocals.count(node->name) && node->name != calleeContext)
        escapedLambdaLocals.insert(node->name);

    std::string type = lookupSymbol(node->name);
    if (!type.empty() && !scriptGlobalPos.empty() && scopeOf(node->name) == 0) {
        // A script's statement before the declaration of one of its globals.
        auto sp = scriptGlobalPos.find(node->name);
        if (sp != scriptGlobalPos.end() && sp->second > std::make_pair(node->line, node->col)) type.clear();
    }
    if (type.empty() && enumConstants.count(node->name)) {
        // Bare enum member, e.g. `Red` — an int constant.
        expressionTypes[node] = "int";
        if (inPrimaryFile()) useLocations[{node->line, node->col}] = node->name;
        return;
    }
    if (type.empty() && adtVariants.count(node->name)) {
        // Bare algebraic variant, e.g. `None` — constructs the enum value. Variants
        // with a payload must be called (`Some(x)`), handled in visit(CallExpr).
        auto& info = adtVariants[node->name];
        const auto& payload = enumDecls[info.first]->payloads[info.second];
        if (!payload.empty())
            errorAt(node, "variant '" + node->name + "' needs " +
                std::to_string(payload.size()) + " argument(s)");
        expressionTypes[node] = info.first;     // the enum value type
        return;
    }
    // A top-level function used as a value decays to a `fn(params)->ret` pointer.
    if (type.empty() && functionSignatures.count(node->name)) {
        const auto& sig = functionSignatures[node->name];   // (returnType, paramTypes)
        std::string t = "fn(";
        for (size_t i = 0; i < sig.second.size(); ++i) {
            if (i) t += ",";
            t += sig.second[i];
        }
        t += ")->" + sig.first;
        expressionTypes[node] = t;
        if (inPrimaryFile()) useLocations[{node->line, node->col}] = node->name;
        return;
    }
    if (type.empty() && genericVariants.count(node->name)) {
        // A generic enum's variant does not name its type arguments by itself (they are
        // not inferred from the target type): `None<int>()`.
        errorAt(node, "variant '" + node->name + "' of generic enum '" + genericVariants[node->name].first +
                      "' needs explicit type arguments, as in " + node->name + "<int>()");
        expressionTypes[node] = "unknown";
    } else if (type.empty()) {
        errorAt(node,"undefined variable '" + node->name + "'" + didYouMean(node->name, 'v'));
        expressionTypes[node] = "unknown";
    } else if (type[0] == '?' && narrowedNonNull.count(narrowKey(node->name))) {
        expressionTypes[node] = type.substr(1);   // proven non-null here: a plain `*T`
    } else {
        expressionTypes[node] = type;
    }
    // Record use-site so go-to-definition can map cursor → definition: a local or
    // parameter maps to the exact symbol found; a global maps by name.
    if (inPrimaryFile()) {
        const Symbol* sym = findSymbol(node->name);
        auto global = scopes.front().find(node->name);
        bool isGlobal = global != scopes.front().end() && &global->second == sym;
        if (sym && !isGlobal && sym->line > 0)
            useDefs[{node->line, node->col}] = {(int)node->name.size(), {sym->line, sym->col, sym->file}};
        else
            useLocations[{node->line, node->col}] = node->name;
    }

    // Capture detection: inside a lambda, a name is captured when it resolves to
    // a variable in an enclosing scope (below the lambda's own scopes). We key off
    // the variable's defining scope index, NOT functionSignatures — a param or
    // local that shadows a same-named top-level function must still be captured.
    if (!captureStack.empty() && !type.empty()) {
        int defIdx = scopeOf(node->name);
        // Capture only enclosing-function scopes: index >= 1 (the global scope
        // at 0 is module-level and accessed directly, not captured by value).
        // A variable must be captured by EVERY enclosing lambda it is outer to,
        // not just the innermost one, so a nested lambda's use is threaded
        // through each intervening lambda's environment (transitive capture).
        // Without this, an inner lambda that references a variable two scopes up
        // would read the enclosing lambda's non-captured value and miscompile
        // ("Referring to an instruction in another function").
        // A `static` local has static storage: like a global, the lambda body refers
        // to the one cell directly instead of capturing a copy.
        if (defIdx >= 1 && !scopes[defIdx][node->name].isStatic) {
            for (size_t k = 0; k < captureStack.size(); ++k) {
                if (defIdx < captureBoundary[k])
                    captureStack[k][node->name] = type;
            }
        }
    }
}

void TypeChecker::visit(LambdaExpr* node) {
    // Build fn(T,U)->R type string
    std::string sig = "fn(";
    for (size_t i = 0; i < node->params.size(); ++i) {
        if (i > 0) sig += ",";
        sig += node->params[i].first;
    }
    sig += ")->" + node->returnType;

    // Push a capture collector before entering the lambda scope.
    // visit(IdentExpr) will add outer-scope vars to captureStack.back().
    captureStack.push_back({});
    captureBoundary.push_back((int)scopes.size());   // scopes below this are "outer"

    pushScope();
    std::string savedReturn = currentFunctionReturnType, savedRaw = currentRawReturnType;
    currentFunctionReturnType = node->returnType;
    currentRawReturnType = node->returnType;
    // The body is its own function: a break/continue there cannot target a loop (or
    // switch) of the enclosing function.
    std::vector<std::string> savedLoops = std::move(loopLabelStack);
    loopLabelStack.clear();
    int savedSwitch = switchDepth;
    switchDepth = 0;
    // A lambda is its own (non-async) function: an `await` in its body does not belong to
    // an enclosing async function.
    bool savedAsync = inAsyncFn, savedAwait = awaitSeenInFn, savedVariadic = inVariadicFn;
    bool savedTrailing = inTrailingBlock;
    inTrailingBlock = node->trailing;
    int savedFinally = finallyDepth, savedCatch = catchDepth;
    inAsyncFn = false;
    finallyDepth = 0;
    catchDepth = 0;
    inVariadicFn = false;
    // Captures are by value: an assignment to a captured name inside the body changes the
    // lambda's copy, so it must not end a narrowing of the enclosing variable.
    std::set<std::string> savedNarrowed = narrowedNonNull;
    // The body runs later, when a global (or `static` local) may have been nulled.
    for (auto it = narrowedNonNull.begin(); it != narrowedNonNull.end();) {
        size_t at = it->rfind('@');
        if (at != std::string::npos && it->compare(at, std::string::npos, "@0") == 0) it = narrowedNonNull.erase(it);
        else ++it;
    }
    for (const auto& p : node->params) {
        if (scopes.back().count(p.second)) errorAt(node, "duplicate parameter '" + p.second + "' in lambda");
        defineSymbol(p.second, normalizeType(p.first), node->line, node->col, /*isParam=*/true);
        if (tyq::bindingConst(p.first)) scopes.back()[p.second].isConst = true;
    }
    // Mark param names so IdentExpr doesn't treat them as captures
    std::set<std::string> paramNames;
    for (const auto& p : node->params) paramNames.insert(p.second);

    if (node->body) node->body->accept(this);
    // A lambda returning a value must return on every path, like a function.
    if (node->body && tyq::strip(node->returnType) != "void" && stmtCanCompleteNormally(node->body.get()))
        errorAt(node, "missing return in lambda returning '" + node->returnType +
                      "' (control can reach the end without returning a value)");
    currentFunctionReturnType = savedReturn;
    currentRawReturnType = savedRaw;
    loopLabelStack = std::move(savedLoops);
    switchDepth = savedSwitch;
    inAsyncFn = savedAsync;
    finallyDepth = savedFinally;
    catchDepth = savedCatch;
    inVariadicFn = savedVariadic;
    // An `await` in a trailing block is reported there; the function does not also lack one.
    awaitSeenInFn = savedAwait || (node->trailing && awaitSeenInFn);
    inTrailingBlock = savedTrailing;
    narrowedNonNull = savedNarrowed;
    popScope();

    // Harvest captures: only outer-scope vars, not params. A generic body's lambdas
    // keep the source-form captures TemplateCapturePass recorded (shared by instances).
    if (!inInstance) {
        node->captures.clear();
        for (const auto& [name, type] : captureStack.back()) {
            if (!paramNames.count(name)) {
                node->captures.push_back({name, type});
                if (nonEscapingFnParams.count(name)) watchedCaptures.push_back({node, name});
                if (lambdaLocals.count(name)) localCaptures.push_back({node, name});
            }
        }
    }
    captureStack.pop_back();
    captureBoundary.pop_back();

    expressionTypes[node] = sig;
}

void TypeChecker::visit(SizeofExpr* node) {
    if (node->operand) {
        // `sizeof(expr)`: the size of the operand's type; the operand is checked, never
        // evaluated. Outside a generic body the node becomes `sizeof(<that type>)`, so
        // the later phases (and the async lowering) see a plain type operand.
        node->operand->accept(this);
        std::string t = getExpressionType(node->operand.get());
        if (t == "void" || isVoidValueType(t))
            errorAt(node, "sizeof of 'void': a void value has no size");
        if (!inInstance && t != "unknown" && !t.empty()) {
            node->typeName = tyq::strip(t);
            node->operand = nullptr;
        }
        expressionTypes[node] = "int64";
        return;
    }
    // `sizeof(x)` of a variable measures the variable's type (C semantics). The parser
    // reads the operand as a type spelling, so resolve a bare name that is a variable
    // (not a type) here and rewrite the operand to that variable's type for codegen.
    const std::string tn = node->typeName;
    std::string resolved = tn;
    bool isTypeName = isPrimitiveType(tn) || structs.count(tn) || typeAliases.count(tn) ||
                      enumTypes.count(tn) || interfaceDecls.count(tn) || tn == "va_list";
    if (!isTypeName) {
        std::string vt = lookupSymbol(tn);
        if (!vt.empty() && vt != "unknown" && vt != "struct:" + tn) resolved = tyq::strip(vt);
    }
    if (!inInstance) node->typeName = resolved;   // a generic body's nodes are shared by its instances
    validateStructType(normalizeType(resolved), node);
    if (isVoidValueType(resolved))
        errorAt(node, "sizeof of 'void': a void value has no size");
    expressionTypes[node] = "int64";
}

void TypeChecker::visit(AwaitExpr* node) {
    // The callee may assign any global: a global's narrowing ends after the call.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    if (!inAsyncFn)
        errorAt(node, inTrailingBlock ? "await is not allowed in a trailing block (the block is a closure)"
                                      : "await is only allowed inside an async function");
    awaitSeenInFn = true;
    node->operand->accept(this);
    std::string t = getExpressionType(node->operand.get());

    // Strip pointer decorators: the operand should be *Future<T> (or Future<T>*).
    std::string inner = t;
    while (!inner.empty() && inner.front() == '*') inner = inner.substr(1);
    while (!inner.empty() && inner.back()  == '*') inner.pop_back();

    // A `*Future<T>` reaches here in two spellings — source form (`Future<int>`,
    // from a plain function) or already-mangled (`struct:Future_int`, from a
    // template call whose return type went through normalizeType). Normalizing
    // once collapses both to `struct:Future_int` AND registers the reverse map, so
    // a single lookup recovers T regardless of how the future was produced.
    std::string base;
    std::vector<std::string> args;
    std::string norm = normalizeType(inner);
    if (norm.rfind("struct:", 0) == 0) {
        auto ti = templateInstanceArgs.find(norm.substr(7));  // "Future_int"
        if (ti != templateInstanceArgs.end()) { base = ti->second.first; args = ti->second.second; }
    }
    if (base == "Future") {
        std::string res = (args.size() == 1) ? normalizeType(args[0]) : "unknown";
        expressionTypes[node] = res;
        if (!inInstance) node->resolvedType = res;   // consumed by the async transform
        else if (args.size() == 1) node->instanceTypes.push_back({instSubs, args[0]});
    } else {
        if (t != "unknown")
            errorAt(node, "await expects a *Future<T>, got " + t);
        expressionTypes[node] = "unknown";
    }
}

void TypeChecker::visit(FreeClosureExpr* node) {
    node->closure->accept(this);
    std::string t = getExpressionType(node->closure.get());
    if (t != "unknown" && !(t.size() > 3 && t.substr(0, 3) == "fn("))
        errorAt(node, "free_closure expects a closure (fn(...)->R), got " + t);
    expressionTypes[node] = "void";
}

void TypeChecker::visit(ThreadCreateExpr* node) {
    // The thread may run at once and assign any global: a global's narrowing ends here.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    node->worker->accept(this);
    std::string t = getExpressionType(node->worker.get());
    if (t != "unknown") {
        ty::Type ft = ty::Type::parse(normalizeType(t));
        bool ok = ft.isFn() && ft.params.empty() && ft.ret && ft.ret->kind == ty::Type::Kind::Void;
        if (!ok) errorAt(node, "thread_create expects a closure 'fn()->void', got '" + t + "'");
    }
    expressionTypes[node] = "*void";
}

// Argument `i` of a call to the generic `fd`. A closure param of the instance being checked,
// passed to a non-escaping param of `fd`, escapes only if that param does; a lambda passed
// to one is decided after every instance is checked.
void TypeChecker::visitGenericCallArg(FunctionDecl* fd, size_t i, const ExprPtr& arg) {
    bool escapes = fd->isAsync || (i < fd->paramEscaping.size() && fd->paramEscaping[i]) ||
                   i >= fd->params.size() || fd->params[i].first == "...";
    auto* id = dynamic_cast<IdentExpr*>(arg.get());
    long own = -1;
    if (id && !escapes && inInstance && curTemplate && nonEscapingFnParams.count(id->name))
        for (size_t j = 0; j < curTemplate->params.size(); ++j)
            if (curTemplate->params[j].second == id->name) own = (long)j;
    if (own >= 0) {
        std::string prev = calleeContext;
        calleeContext = id->name;
        arg->accept(this);
        calleeContext = prev;
        genericParamForwards[{curTemplate, (size_t)own}].insert({fd, i});
    } else {
        arg->accept(this);
    }
    if (auto* lam = dynamic_cast<LambdaExpr*>(arg.get())) {
        auto& d = deferredLambdas[lam];
        if (escapes) d.escapes = true;
        else d.via.insert({fd, i});
    }
}

void TypeChecker::visit(TemplateCallExpr* node) {
    noteDeprecatedUse(node->templateName, node->line, node->col);
    // The callee may assign any global: a global's narrowing ends after the call.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    // Variadic access: va_arg<T>(ap) yields the next argument as T.
    if (node->templateName == "va_arg" && node->typeArgs.size() == 1) {
        for (auto& a : node->args) a->accept(this);
        checkVaListArg(node, "va_arg", node->args);
        std::string t = normalizeType(node->typeArgs[0]);
        if (isAggregateValue(t) || isVoidValueType(t)) {
            errorAt(node, "'va_arg' cannot read a '" + node->typeArgs[0] +
                          "': a variadic argument is an integer, a floating-point value or a pointer");
        } else {
            // A variadic argument undergoes the default argument promotions (C): a float
            // arrives as a double and a narrow integer as an int, so reading the narrow
            // type would take the wrong bytes.
            std::string d = tyq::strip(dealiasOperand(t));
            std::string promoted = d == "float" ? "double"
                : (d == "bool" || d == "char" || d == "int8" || d == "int16" || d == "uint8" || d == "uint16") ? "int" : "";
            if (!promoted.empty())
                errorAt(node, "'va_arg' cannot read a '" + node->typeArgs[0] + "': a variadic '" + d +
                              "' is promoted to '" + promoted + "'; read va_arg<" + promoted + "> and convert it");
        }
        expressionTypes[node] = t;
        return;
    }
    // Generic algebraic-variant construction with explicit type args:
    // `Some<int>(5)`, `Left<int,string>(x)`, `None<int>()`.
    auto gv = genericVariants.find(node->templateName);
    if (gv != genericVariants.end()) {
        EnumDecl* ge = genericEnumDecls[gv->second.first];
        std::map<std::string, std::string> subs;
        for (size_t i = 0; i < ge->typeParams.size() && i < node->typeArgs.size(); ++i)
            subs[ge->typeParams[i]] = node->typeArgs[i];
        const auto& payload = ge->payloads[gv->second.second];
        // A generic variant payload takes the payload's instance (`Some(Some(5))` as an
        // `Opt<Opt<int64>>`: the inner one is an `Opt<int64>`).
        for (size_t i = 0; i < node->args.size() && i < payload.size(); ++i)
            inferVariantTarget(node->args[i], substType(payload[i], subs), true);
        for (auto& a : node->args) a->accept(this);
        if (node->args.size() != payload.size())
            errorAt(node, "variant '" + node->templateName + "' expects " +
                std::to_string(payload.size()) + " argument(s), got " + std::to_string(node->args.size()));
        else
            for (size_t i = 0; i < payload.size(); ++i) {
                std::string want = normalizeType(substType(payload[i], subs));
                std::string at = getExpressionType(node->args[i].get());
                if (at != "unknown" && !isValidAssignment(want, at))
                    errorAt(node, "variant '" + node->templateName + "' argument " +
                        std::to_string(i + 1) + " type mismatch");
                else checkVariantLiteral(node, node->templateName, i, want);
            }
        // Build the instance type name (Option<int>) and normalize -> Option_int.
        std::string inst = gv->second.first + "<";
        for (size_t i = 0; i < node->typeArgs.size(); ++i) { if (i) inst += ","; inst += node->typeArgs[i]; }
        inst += ">";
        expressionTypes[node] = normalizeType(inst);
        return;
    }
    auto templ = funcTemplateDecls.find(node->templateName);
    if (templ == funcTemplateDecls.end()) {
        errorAt(node,"undefined template function '" + node->templateName + "'");
        expressionTypes[node] = "unknown";
        return;
    }
    FunctionDecl* fd = templ->second;
    appendDefaultArgs(node->args, node->args.size(), node->templateName, node);
    auto& tp = fd->typeParams;
    size_t errsBefore = errors.size();
    std::map<std::string, std::string> subs;
    for (size_t i = 0; i < tp.size() && i < node->typeArgs.size(); ++i)
        subs[tp[i]] = resolveInstType(node->typeArgs[i]);
    if (node->typeArgs.size() != tp.size())
        errorAt(node, "generic function '" + node->templateName + "' expects " + std::to_string(tp.size()) +
                      " type argument(s), got " + std::to_string(node->typeArgs.size()));
    for (const auto& ta : node->typeArgs) validateStructType(normalizeType(ta), node);
    bool variadic = !fd->params.empty() && fd->params.back().first == "...";
    size_t fixed = variadic ? fd->params.size() - 1 : fd->params.size();
    if (variadic ? node->args.size() < fixed : node->args.size() != fixed)
        errorAt(node, "function '" + node->templateName + "' expects " + (variadic ? "at least " : "") +
                      std::to_string(fixed) + " argument(s), got " + std::to_string(node->args.size()));
    for (size_t i = fixed; i < node->args.size(); ++i) {
        node->args[i]->accept(this);
        checkVariadicArg(node, node->args[i].get(), i);
    }

    checkConstraints(node, fd->constraints, subs);

    // Type-check arguments. A generic variant argument takes the parameter's instance
    // (`get<int64>(Some(8))`), from the type arguments as written.
    std::map<std::string, std::string> writtenSubs;
    for (size_t i = 0; i < tp.size() && i < node->typeArgs.size(); ++i) writtenSubs[tp[i]] = node->typeArgs[i];
    for (size_t i = 0; i < node->args.size() && i < fixed; ++i) {
        if (node->typeArgs.size() == tp.size())
            inferVariantTarget(node->args[i], substType(fd->params[i].first, writtenSubs), true);
        visitGenericCallArg(fd, i, node->args[i]);
        std::string expected = substType(fd->params[i].first, subs);
        std::string got      = getExpressionType(node->args[i].get());
        std::string e = assignabilityError(expected, got, node->args[i].get());
        if (!e.empty())
            errorAt(node,"argument " + std::to_string(i+1) + ": expected " + expected +
                        ", got " + got + " (" + e + ")");
    }

    std::string retType = genericCallRet(fd, subs);
    expressionTypes[node] = retType;
    if (errors.size() == errsBefore && node->typeArgs.size() == tp.size()) {
        std::string mangled = node->templateName;
        for (const auto& t : node->typeArgs) mangled += "_" + mangleTemplate(resolveInstType(t));
        queueInstance(fd, tp, subs, node->templateName, "", mangled, "", fd->sourceFile);
    }
}

void TypeChecker::visit(AllocWithExpr* node) {
    // A call to the allocator's alloc method, which may assign any global.
    struct GlobalNarrowDrop { TypeChecker* t; ~GlobalNarrowDrop() { t->dropGlobalNarrowings(); } } dropAfter{this};
    node->allocator->accept(this);
    node->count->accept(this);
    std::string countType = getExpressionType(node->count.get());
    if (countType != "unknown" && !isIntType(dealiasOperand(countType)))
        errorAt(node,"alloc_with count must be integer, got " + countType);
    // The element type needs a size: a known, non-void type.
    size_t unknownBefore = errors.size();
    validateStructType(normalizeType(node->elemType), node);
    if (errors.size() == unknownBefore && isVoidValueType(node->elemType))
        errorAt(node, "alloc_with of 'void': a void element has no size");
    // The allocator is passed as `self`, so it must be a pointer to the allocator; its
    // type names its alloc method, `*void <Type>_alloc(*<Type> self, int64 size)`.
    std::string at = getExpressionType(node->allocator.get());
    if (at != "unknown") {
        std::string raw = normalizeType(at);
        if (!raw.empty() && raw[0] == '?') raw = raw.substr(1);
        at = raw;
        while (!at.empty() && at.front() == '*') at = at.substr(1);
        while (!at.empty() && at.back()  == '*') at.pop_back();
        if (at.rfind("struct:", 0) == 0) at = at.substr(7);
        bool onePtr = isPointerType(raw) && !isPointerType(getPointeeType(raw));
        auto sig = functionSignatures.find(at + "_alloc");
        if (!onePtr) {
            std::string shown = tyq::strip(getExpressionType(node->allocator.get()));
            for (size_t p; (p = shown.find("struct:")) != std::string::npos;) shown.erase(p, 7);
            errorAt(node, "alloc_with: the allocator must be a pointer to it, got '" + shown +
                          "' (pass its address, as in alloc_with(&a, T, n))");
        } else if (sig == functionSignatures.end()) {
            errorAt(node, "alloc_with: allocator type '" + at + "' has no alloc method (" + at + "_alloc)");
        } else {
            const auto& ps = sig->second.second;
            bool ok = ps.size() == 2 && isPointerType(normalizeType(ps[0])) &&
                      isIntType(dealiasOperand(ps[1])) && isPointerType(normalizeType(sig->second.first));
            if (ok) {
                std::string self = normalizeType(getPointeeType(normalizeType(ps[0])));
                if (self.rfind("struct:", 0) == 0) self = self.substr(7);
                ok = self == at;
            }
            if (!ok)
                errorAt(node, "alloc_with: '" + at + "_alloc' must have the shape '*void " + at + "_alloc(*" + at +
                              " self, int64 size)'");
        }
    }
    expressionTypes[node] = "*" + node->elemType;
}

void TypeChecker::visit(ArrayLitExpr* node) {
    // Untyped: the element type / size come from the target at the declaration
    // site (checked in visit(VarDecl)). Here we just walk the elements.
    for (auto& el : node->elements) el->accept(this);
    expressionTypes[node] = "array-literal";
}

void TypeChecker::visit(StructInitExpr* node) {
    // A template literal (Pair<int,float> { ... }) names an instantiation; run it
    // through normalizeType so the concrete struct gets registered, then resolve.
    std::string sname = node->structName;
    // A literal through a type alias (`type LI = Box<int>; LI{v: 3}`) names the aliased
    // struct; the node is rewritten to it so codegen sees the struct itself (an alias is
    // top-level, so the rewrite is the same in every generic instance).
    for (int hops = 0; hops < 32 && !structs.count(sname); ++hops) {
        auto al = typeAliases.find(sname);
        if (al == typeAliases.end()) break;
        sname = al->second;
        node->structName = sname;
    }
    if (sname.find('<') != std::string::npos) {
        std::string norm = normalizeType(sname);
        if (norm.rfind("struct:", 0) == 0) sname = norm.substr(7);
    }
    auto it = structs.find(sname);
    if (it == structs.end()) {
        std::string base = sname.substr(0, sname.find('<'));
        if (base == sname && templateDecls.count(sname))
            errorAt(node, "generic struct '" + sname + "' needs type arguments, as in " + sname + "<int>{...}");
        else if (base != sname && structs.count(base) && !templateDecls.count(base))
            errorAt(node, "struct '" + base + "' is not generic: write " + base + "{...}");
        else
            errorAt(node,"undefined struct '" + node->structName + "'");
        expressionTypes[node] = "unknown";
        return;
    }

    const auto& fields = it->second.fields;
    bool named = !node->fieldInits.empty() && !node->fieldInits[0].first.empty();
    // A named field is a use of its declaration (references, rename).
    if (inPrimaryFile())
        for (size_t i = 0; i < node->fieldInits.size() && i < node->fieldPositions.size(); ++i) {
            const auto& fname = node->fieldInits[i].first;
            auto fl = fieldLocations.find(node->structName + "." + fname);
            if (!fname.empty() && fl != fieldLocations.end() && node->fieldPositions[i].first > 0)
                useDefs[node->fieldPositions[i]] = {(int)fname.size(), fl->second};
        }
    std::set<std::string> seenFields;
    // A literal is either positional or named: mixed, a value would land in the field
    // its position names and could initialize a field a name also sets (`P{1, a: 2}`).
    for (const auto& fi : node->fieldInits)
        if (fi.first.empty() == named) {
            errorAt(node, "cannot mix positional and named field initializers in a '" +
                          node->structName + "' literal");
            break;
        }
    // A union's members overlap: its literal initializes exactly one of them (C).
    if (it->second.isUnion && node->fieldInits.size() > 1)
        errorAt(node, "a union literal initializes one member ('" + node->structName +
                      "' literal has " + std::to_string(node->fieldInits.size()) + ")");

    for (size_t i = 0; i < node->fieldInits.size(); ++i) {
        // A generic variant field value takes the field's instance (`S{ o: Some(3) }`). The
        // declared field types of a non-generic struct are the same in every instance.
        if (node->structName.find('<') == std::string::npos && !templateDecls.count(node->structName)) {
            std::string ft;
            if (named) { if (const auto* f = it->second.field(node->fieldInits[i].first)) ft = f->type; }
            else if (i < fields.size()) ft = fields[i].type;
            if (!ft.empty()) inferVariantTarget(node->fieldInits[i].second, ft, true);
        }
        const auto& [fname, expr] = node->fieldInits[i];
        expr->accept(this);
        ASTNode* at = (expr->line > 0) ? static_cast<ASTNode*>(expr.get()) : node;

        std::string fieldType, shownName;
        if (named) {
            if (const auto* f = it->second.field(fname)) fieldType = f->type;
            shownName = fname;
            if (fieldType.empty())
                errorAt(at, "struct '" + node->structName + "' has no field '" + fname + "'");
            else if (!seenFields.insert(fname).second)
                errorAt(at, "field '" + fname + "' is initialized more than once in '" +
                            node->structName + "' literal");
        } else if (i < fields.size()) {
            fieldType = fields[i].type;
            shownName = fields[i].name;
        } else if (i == fields.size()) {
            errorAt(at, "too many initializers for struct '" + node->structName + "' (it has " +
                        std::to_string(fields.size()) + " field(s), got " +
                        std::to_string(node->fieldInits.size()) + ")");
        }

        if (!fieldType.empty()) {
            std::string valType = getExpressionType(expr.get());
            if (valType == "array-literal") {
                // `{...}` fills an array field (its elements are checked like an array init).
                if (ty::Type::parse(normalizeType(fieldType)).kind != ty::Type::Kind::Array)
                    errorAt(at, "field '" + shownName + "': an array literal '{...}' can only initialize an array type, not '" +
                                fieldType + "'");
            } else if (valType != "unknown") {
                std::string e = dropsConstQual(fieldType, valType)
                    ? "conversion discards a const qualifier ('" + valType + "' to '" + fieldType + "')"
                    : assignabilityError(fieldType, valType, expr.get());
                if (!e.empty()) errorAt(at, "field '" + shownName + "': " + e);
            }
        }
    }

    expressionTypes[node] = "struct:" + sname;
}
