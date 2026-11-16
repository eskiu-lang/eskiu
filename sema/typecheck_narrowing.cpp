// typecheck_narrowing.cpp: variant targets, ternaries, nullable narrowing, unary and ++/-- expressions (split from typecheck_expr.cpp).
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

// An integer literal that does not fit `int` has a wider type (C: the first of int,
// long, unsigned long that holds it), so `c ? x : 10000000000` is 64-bit, not an int.
static std::string literalArmType(Expr* e, const std::string& t) {
    auto* lit = nodeCast<LiteralExpr>(e);
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
    if (auto* t = nodeCast<TernaryExpr>(e.get())) {
        inferVariantTarget(t->thenExpr, target, raw);
        inferVariantTarget(t->elseExpr, target, raw);
        return;
    }
    if (auto* al = nodeCast<ArrayLitExpr>(e.get())) {
        ty::Type at = ty::Type::parse(tyq::strip(target));
        if (at.kind != ty::Type::Kind::Array || !at.elem) return;
        std::string et = at.elem->str();
        for (auto& el : al->elements) inferVariantTarget(el, et, raw);
        return;
    }
    std::string name;
    std::vector<ExprPtr> args;
    auto* call = nodeCast<CallExpr>(e.get());
    if (call) {
        auto* cid = nodeCast<IdentExpr>(call->callee.get());
        if (!cid) return;
        name = cid->name;
        args = call->args;
    } else if (auto* id = nodeCast<IdentExpr>(e.get())) {
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
    auto* t = nodeCast<TernaryExpr>(e);
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
        auto* id = nodeCast<IdentExpr>(e);
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
        if (auto* u = nodeCast<UnaryExpr>(cond); u && u->op == "!") {
            cond = u->operand.get();
            whenTrue = !whenTrue;
            continue;
        }
        auto* b = nodeCast<BinaryExpr>(cond);
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
    if (auto* b = nodeCast<BinaryExpr>(cond)) {
        if (b->op == "!=" || b->op == "==") {
            auto isNull = [](Expr* e) {
                auto* l = nodeCast<LiteralExpr>(e);
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
    if (auto* b = nodeCast<BinaryExpr>(e); b && b->op == "=")
        if (auto* id = nodeCast<IdentExpr>(b->left.get())) out.insert(id->name);
    if (auto* u = nodeCast<UnaryExpr>(e); u && u->op == "&")
        if (auto* id = nodeCast<IdentExpr>(u->operand.get())) out.insert(id->name);
    if (auto* ic = nodeCast<IncDecExpr>(e))
        if (auto* id = nodeCast<IdentExpr>(ic->operand.get())) out.insert(id->name);
    if (nodeCast<LambdaExpr>(e)) return;
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
    if (!e || nodeCast<LambdaExpr>(e)) return false;
    if (nodeCast<CallExpr>(e) || nodeCast<TemplateCallExpr>(e) || nodeCast<AwaitExpr>(e) ||
        nodeCast<AllocWithExpr>(e) || nodeCast<ThreadCreateExpr>(e)) return true;
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
    if (auto* b = nodeCast<BinaryExpr>(e); b && b->op == "=")
        if (auto* id = nodeCast<IdentExpr>(b->left.get())) dropName(id->name);
    if (auto* u = nodeCast<UnaryExpr>(e); u && u->op == "&")
        if (auto* id = nodeCast<IdentExpr>(u->operand.get())) dropName(id->name);
    if (auto* lam = nodeCast<LambdaExpr>(e)) { dropAssignedIn(lam->body.get()); return; }
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { dropAssignedIn(c.get()); });
}

void TypeChecker::dropAssignedIn(Stmt* s) {
    if (!s || narrowedNonNull.empty()) return;
    if (auto* b = nodeCast<BlockStmt>(s)) {
        for (auto& it : b->items) {
            if (auto* st = std::get_if<StmtPtr>(&it)) dropAssignedIn(st->get());
            else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) dropAssignedIn(vd->initializer.get());
        }
    } else if (auto* es = nodeCast<ExprStmt>(s)) dropAssignedIn(es->expr.get());
    else if (auto* r = nodeCast<ReturnStmt>(s)) dropAssignedIn(r->value.get());
    else if (auto* i = nodeCast<IfStmt>(s)) {
        dropAssignedIn(i->condition.get()); dropAssignedIn(i->thenBranch.get()); dropAssignedIn(i->elseBranch.get());
    } else if (auto* w = nodeCast<WhileStmt>(s)) { dropAssignedIn(w->condition.get()); dropAssignedIn(w->body.get()); }
    else if (auto* dw = nodeCast<DoWhileStmt>(s)) { dropAssignedIn(dw->condition.get()); dropAssignedIn(dw->body.get()); }
    else if (auto* f = nodeCast<ForStmt>(s)) {
        dropAssignedIn(f->init.get()); dropAssignedIn(f->condition.get());
        dropAssignedIn(f->step.get()); dropAssignedIn(f->body.get());
    } else if (auto* fi = nodeCast<ForInStmt>(s)) { dropAssignedIn(fi->iterable.get()); dropAssignedIn(fi->body.get()); }
    else if (auto* sw = nodeCast<SwitchStmt>(s)) {
        dropAssignedIn(sw->subject.get());
        for (auto& c : sw->cases)
            for (auto& it : c.stmts) {
                if (auto* st = std::get_if<StmtPtr>(&it)) dropAssignedIn(st->get());
                else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) dropAssignedIn(vd->initializer.get());
            }
    } else if (auto* m = nodeCast<MatchStmt>(s)) {
        dropAssignedIn(m->subject.get());
        for (auto& arm : m->arms) dropAssignedIn(arm.body.get());
    } else if (auto* t = nodeCast<TryStmt>(s)) {
        dropAssignedIn(t->body.get());
        for (auto& c : t->catches) dropAssignedIn(c.body.get());
        dropAssignedIn(t->finally.get());
    } else if (auto* d = nodeCast<DeferStmt>(s)) dropAssignedIn(d->body.get());
    else if (auto* th = nodeCast<ThrowStmt>(s)) dropAssignedIn(th->value.get());
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
    while (auto* ix = nodeCast<IndexExpr>(e)) {
        if (ix->highIndex || ty::Type::parse(getExpressionType(ix->base.get())).kind != ty::Type::Kind::Array) return;
        e = ix->base.get();
    }
    auto* m = nodeCast<MemberExpr>(e);
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
        if (auto* m = nodeCast<MemberExpr>(node->operand.get())) {
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
        if (auto* id = nodeCast<IdentExpr>(node->operand.get())) {
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
    bool isLval = nodeCast<IdentExpr>(op) || nodeCast<MemberExpr>(op) ||
                  nodeCast<IndexExpr>(op) ||
                  (nodeCast<UnaryExpr>(op) && static_cast<UnaryExpr*>(op)->op == "*");
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
        if (auto* m = nodeCast<MemberExpr>(op)) {
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
