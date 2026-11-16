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
        if (auto* m = nodeCast<MemberExpr>(target)) {
            if (tyq::isPtr(getExpressionType(m->base.get()))) return "";
            target = m->base.get(); continue;
        }
        if (auto* ix = nodeCast<IndexExpr>(target)) {
            if (!ix->opFunc.empty() || ix->highIndex) return "";
            std::string bt = normalizeType(getExpressionType(ix->base.get()));
            if (ty::Type::parse(bt).kind != ty::Type::Kind::Array) return "";
            target = ix->base.get(); continue;
        }
        break;
    }
    auto* id = nodeCast<IdentExpr>(target);
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
    if (auto* id = nodeCast<IdentExpr>(node->left.get())) {
        std::string declared = lookupSymbol(id->name);
        if (!declared.empty() && declared[0] == '?') {
            lt = declared;
            // `p = &x` stores an address, and `p = p + k` / `p += k` steps a non-null
            // pointer; any other value may be null.
            auto* u = nodeCast<UnaryExpr>(node->right.get());
            auto* ar = nodeCast<BinaryExpr>(node->right.get());
            auto* self = ar ? nodeCast<IdentExpr>(ar->left.get()) : nullptr;
            bool keeps = (u && u->op == "&") ||
                         (ar && (ar->op == "+" || ar->op == "-") && self && self->name == id->name);
            if (!keeps) narrowedNonNull.erase(narrowKey(id->name));
        }
    }
    if (auto* id = nodeCast<IdentExpr>(node->left.get())) {
        trackLocalAddr(id->name, node->right.get());
        // -Wall: storing a local's address in a global leaves it dangling once the
        // function returns (main's locals outlive every use).
        if (auto* u = nodeCast<UnaryExpr>(node->right.get());
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
        if (auto* cb = nodeCast<BinaryExpr>(node->right.get());
            cb && cb->left.get() == node->left.get() && cb->op != "<<" && cb->op != ">>" &&
            isIntType(normalizeType(lt)))
            if (auto* lit = nodeCast<LiteralExpr>(cb->right.get());
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
    for (BinaryExpr* b = node; b; b = nodeCast<BinaryExpr>(b->left.get())) spine.push_back(b);
    // `f = <lambda>` rebinds a lambda local: the target is not a use of f's value (it would
    // otherwise make f escape and send every env to the heap, unfreed).
    std::string savedCallee = calleeContext;
    if (spine.back()->op == "=" && nodeCast<LambdaExpr>(spine.back()->right.get()))
        if (auto* id = nodeCast<IdentExpr>(spine.back()->left.get()); id && lambdaLocals.count(id->name))
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
        if (auto* lam = nodeCast<LambdaExpr>(node->right.get()))
            if (auto* id = nodeCast<IdentExpr>(node->left.get());
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
            auto* l = nodeCast<LiteralExpr>(e);
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
        if (auto* l = nodeCast<LiteralExpr>(node->right.get());
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
