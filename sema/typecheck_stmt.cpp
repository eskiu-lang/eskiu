#include "type_checker.h"
#include <cstdint>
#include <climits>
#include <functional>
#include <set>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"

// ============================================================================

// TypeChecker — statement visitors (blocks, control flow, match/switch,
// try/throw, threads).
// Part of the type_checker.cpp split; see type_checker.h.

// Statement visitors
void TypeChecker::visit(BlockStmt* node) {
    pushScope();

    // Type check items in order, maintaining exact parse order
    // Declarations can be interleaved with statements
    std::vector<std::string> guardNarrowed;   // narrowings from an early-exit guard, undone at block end
    for (const auto& item : node->items) {
        // Check if this item is a declaration or a statement
        if (std::holds_alternative<DeclPtr>(item)) {
            // It's a declaration
            const auto& decl = std::get<DeclPtr>(item);
            decl->accept(this);
        } else {
            // It's a statement
            const auto& stmt = std::get<StmtPtr>(item);
            stmt->accept(this);
            // Early-exit guard: after `if (p == null) return ...;` the rest of the block
            // only runs when the condition was false (resp. true, when only the else exits).
            if (auto* ifs = dynamic_cast<IfStmt*>(stmt.get()); ifs && ifs->condition) {
                bool thenExits = !stmtCanCompleteNormally(ifs->thenBranch.get());
                bool elseExits = ifs->elseBranch && !stmtCanCompleteNormally(ifs->elseBranch.get());
                std::vector<std::string> keys;
                // A call on the path that falls through may have assigned a global.
                if (thenExits && !elseExits) {
                    condNarrowings(ifs->condition.get(), false, keys);
                    if (lastIfElseCalled) dropGlobalKeys(keys);
                } else if (elseExits && !thenExits) {
                    condNarrowings(ifs->condition.get(), true, keys);
                    if (lastIfThenCalled) dropGlobalKeys(keys);
                }
                for (auto& k : applyNarrowings(keys)) guardNarrowed.push_back(k);
                // The branch that falls through may have assigned the variable.
                if (thenExits && !elseExits) dropAssignedIn(ifs->elseBranch.get());
                else if (elseExits && !thenExits) dropAssignedIn(ifs->thenBranch.get());
            }
        }
    }
    undoNarrowings(guardNarrowed);
    if (inAsyncFn) warnAwaitHoldingLock(node);

    popScope();
}

// The place a lock call names: `m`, `a.b.m` (identifiers and fields only), "" otherwise.
static std::string lockPlaceKey(Expr* e) {
    if (auto* id = dynamic_cast<IdentExpr*>(e)) return id->name;
    if (auto* m = dynamic_cast<MemberExpr*>(e)) {
        std::string base = lockPlaceKey(m->base.get());
        return base.empty() ? "" : base + "." + m->member;
    }
    return "";
}

// The earliest `await` (by line, then column) in a subtree, lambda bodies excluded.
static void earliestAwait(Stmt* s, AwaitExpr*& best);
static void earliestAwait(Expr* e, AwaitExpr*& best) {
    if (!e) return;
    if (auto* a = dynamic_cast<AwaitExpr*>(e)) {
        if (!best || a->line < best->line || (a->line == best->line && a->col < best->col)) best = a;
    }
    astwalk::forEachChildExpr(e, [&](ExprPtr& c) { earliestAwait(c.get(), best); });
}
static void earliestAwait(const std::vector<BlockItem>& items, AwaitExpr*& best) {
    for (auto& it : items) {
        if (std::holds_alternative<StmtPtr>(it)) earliestAwait(std::get<StmtPtr>(it).get(), best);
        else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) earliestAwait(vd->initializer.get(), best);
    }
}
static void earliestAwait(Stmt* s, AwaitExpr*& best) {
    if (!s) return;
    auto E = [&](const ExprPtr& e) { earliestAwait(e.get(), best); };
    auto S = [&](const StmtPtr& st) { earliestAwait(st.get(), best); };
    if (auto* b = dynamic_cast<BlockStmt*>(s)) earliestAwait(b->items, best);
    else if (auto* i = dynamic_cast<IfStmt*>(s)) {
        for (IfStmt* n = i; n;) {
            E(n->condition); S(n->thenBranch);
            auto* next = dynamic_cast<IfStmt*>(n->elseBranch.get());
            if (!next) S(n->elseBranch);
            n = next;
        }
    }
    else if (auto* f = dynamic_cast<ForStmt*>(s))       { S(f->init); E(f->condition); E(f->step); S(f->body); }
    else if (auto* fi = dynamic_cast<ForInStmt*>(s))    { E(fi->iterable); S(fi->body); }
    else if (auto* w = dynamic_cast<WhileStmt*>(s))     { E(w->condition); S(w->body); }
    else if (auto* dw = dynamic_cast<DoWhileStmt*>(s))  { S(dw->body); E(dw->condition); }
    else if (auto* r = dynamic_cast<ReturnStmt*>(s))    { E(r->value); }
    else if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        E(sw->subject);
        for (auto& c : sw->cases) { E(c.value); earliestAwait(c.stmts, best); }
    }
    else if (auto* m = dynamic_cast<MatchStmt*>(s))     { E(m->subject); for (auto& a : m->arms) S(a.body); }
    else if (auto* th = dynamic_cast<ThrowStmt*>(s))    { E(th->value); }
    else if (auto* t = dynamic_cast<TryStmt*>(s))       { S(t->body); for (auto& c : t->catches) S(c.body); S(t->finally); }
    else if (auto* d = dynamic_cast<DeferStmt*>(s))     { S(d->body); }
    else if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s)) { E(tj->tid); }
    else if (auto* a = dynamic_cast<AsmStmt*>(s))       { for (auto& o : a->outputs) E(o.second); for (auto& in : a->inputs) E(in.second); }
    else if (auto* es = dynamic_cast<ExprStmt*>(s))     { E(es->expr); }
}

// The mutex a statement locks (`lock` true) or unlocks: `Mutex_lock(&m)` /
// `Mutex_lock(p)` by name, or `m.lock()` on a `Mutex` / `*Mutex`. Its place key, or "".
std::string TypeChecker::mutexCallKey(Stmt* s, bool lock) {
    auto* es = dynamic_cast<ExprStmt*>(s);
    auto* c = es ? dynamic_cast<CallExpr*>(es->expr.get()) : nullptr;
    if (!c) return "";
    if (auto* id = dynamic_cast<IdentExpr*>(c->callee.get())) {
        if (id->name != (lock ? "Mutex_lock" : "Mutex_unlock") || c->args.size() != 1) return "";
        Expr* a = c->args[0].get();
        if (auto* u = dynamic_cast<UnaryExpr*>(a); u && u->op == "&") a = u->operand.get();
        return lockPlaceKey(a);
    }
    if (auto* m = dynamic_cast<MemberExpr*>(c->callee.get())) {
        if (m->member != (lock ? "lock" : "unlock") || !c->args.empty()) return "";
        if (ty::Type::parse(normalizeType(tyq::strip(getExpressionType(m->base.get())))).nominalName() != "Mutex") return "";
        return lockPlaceKey(m->base.get());
    }
    return "";
}

// -Wawait-holding-lock: in an async function, a statement of this block locks a mutex
// and a later statement of the same block awaits before one unlocks the same place
// (a `defer` does not unlock: its body runs at the block's end). Reported at the
// earliest await of the first statement that awaits.
void TypeChecker::warnAwaitHoldingLock(BlockStmt* node) {
    const auto& items = node->items;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!std::holds_alternative<StmtPtr>(items[i])) continue;
        Stmt* ls = std::get<StmtPtr>(items[i]).get();
        std::string key = mutexCallKey(ls, true);
        if (key.empty()) continue;
        for (size_t j = i + 1; j < items.size(); ++j) {
            AwaitExpr* aw = nullptr;
            if (std::holds_alternative<StmtPtr>(items[j])) {
                Stmt* s = std::get<StmtPtr>(items[j]).get();
                if (mutexCallKey(s, false) == key) break;
                if (dynamic_cast<DeferStmt*>(s)) continue;
                earliestAwait(s, aw);
            } else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(items[j]).get())) {
                earliestAwait(vd->initializer.get(), aw);
            }
            if (aw) {
                noteWarning("await-holding-lock", aw->line, aw->col,
                            "await while the mutex '" + key + "' is locked (locked at line " +
                            std::to_string(ls->line) + "); unlock it before the await");
                break;
            }
        }
    }
}

void TypeChecker::visit(IfStmt* node) {
    // An `else if` chain is walked with a loop; each link's else-narrowings stay in
    // force for the links after it and are undone, innermost first, at the end.
    std::vector<std::vector<std::string>> elseNarrowed;
    int thenStart = callEpoch, thenEnd = callEpoch;
    for (IfStmt* n = node; n;) {
        if (n->condition) {
            warnAssignInCondition(n->condition.get());
            checkCondition(n, n->condition.get());
        }
        // Null-narrowing: `if (q != null)` proves `q` non-null in the then-branch (and
        // `if (q == null)` in the else-branch), so a `?*T` may be dereferenced there. An
        // assignment to `q` inside the branch ends the narrowing (see visit(BinaryExpr)).
        if (n->thenBranch) {
            std::vector<std::string> keys;
            condNarrowings(n->condition.get(), true, keys);
            auto inserted = applyNarrowings(keys);
            if (n == node) thenStart = callEpoch;
            n->thenBranch->accept(this);
            if (n == node) thenEnd = callEpoch;
            undoNarrowings(inserted);
        }
        IfStmt* next = nullptr;
        if (n->elseBranch) {
            std::vector<std::string> keys;
            condNarrowings(n->condition.get(), false, keys);
            elseNarrowed.push_back(applyNarrowings(keys));
            next = dynamic_cast<IfStmt*>(n->elseBranch.get());
            if (!next) n->elseBranch->accept(this);
        }
        n = next;
    }
    for (size_t i = elseNarrowed.size(); i-- > 0;) undoNarrowings(elseNarrowed[i]);
    lastIfThenCalled = thenEnd != thenStart;
    lastIfElseCalled = callEpoch != thenEnd;
}

void TypeChecker::visit(ForInStmt* node) {
    node->iterable->accept(this);
    std::string itType = getExpressionType(node->iterable.get());

    // Determine the element type the loop variable will bind.
    std::string elemType;
    ty::Type itT = ty::Type::parse(dealiasOperand(itType));   // through an alias (`*p`, p: *A4)
    if ((itT.kind == ty::Type::Kind::Array || itT.kind == ty::Type::Kind::Slice) && itT.elem) {
        // A fixed-size array (or slice): the element is one step in, so the rows of an
        // `int[2][3]` are `int[3]` (the leftmost bracket is the outer dimension).
        elemType = normalizeType(itT.elem->str());
        if (!inInstance) { node->isArrayIter = true; node->arrayDim = itT.dim; }
    } else {
        ty::Type base = ty::Type::parse(normalizeType(itType));
        while (base.isPointer() && base.pointee) { ty::Type p = *base.pointee; base = p; }
        std::string s = base.isTemplate() ? mangleTemplate(base.str()) : base.nominalName();
        auto it = structs.find(s);
        if (it != structs.end()) {                           // List-like struct
            bool hasSize = false; std::string dataType;
            for (const auto& f : it->second.fields) {
                if (f.name == "size") hasSize = true;
                if (f.name == "data") dataType = f.type;
            }
            if (hasSize && !dataType.empty()) {
                while (!dataType.empty() && dataType.front() == '*') dataType = dataType.substr(1);
                while (!dataType.empty() && dataType.back()  == '*') dataType.pop_back();
                elemType = normalizeType(dataType);
            }
        }
    }

    if (!inInstance) { node->resolvedElemType = elemType; node->resolvedIterType = itType; }
    dropAssignedIn(node->body.get()); markAddrTakenIn(node->body.get());   // later iterations see assignments in the body
    pushScope();
    if (elemType.empty()) {
        errorAt(node, "for-in expects a fixed-size array or a List-like value "
                      "(with `data` and `size` fields), got " + itType);
        defineSymbol(node->varName, "unknown");
    } else {
        defineSymbol(node->varName, elemType, node->line, node->col, /*isParam=*/false);
    }
    loopLabelStack.push_back(node->label);
    if (node->body) node->body->accept(this);
    loopLabelStack.pop_back();
    popScope();
}

void TypeChecker::visit(WhileStmt* node) {
    // A narrowing from outside the loop does not survive an assignment in the body
    // (the condition and later iterations would observe it).
    dropAssignedIn(node->body.get()); markAddrTakenIn(node->body.get());
    dropAssignedIn(node->condition.get());
    if (node->condition) {
        warnAssignInCondition(node->condition.get());
        checkCondition(node, node->condition.get());
    }
    loopLabelStack.push_back(node->label);
    if (node->body) {
        // `while (p != null)` re-tests p before every iteration, so the body sees it non-null.
        std::vector<std::string> keys;
        if (node->condition) condNarrowings(node->condition.get(), true, keys);
        auto inserted = applyNarrowings(keys);
        node->body->accept(this);
        undoNarrowings(inserted);
    }
    loopLabelStack.pop_back();
}

void TypeChecker::visit(DoWhileStmt* node) {
    dropAssignedIn(node->body.get()); markAddrTakenIn(node->body.get());
    dropAssignedIn(node->condition.get());
    loopLabelStack.push_back(node->label);
    if (node->body) node->body->accept(this);
    loopLabelStack.pop_back();
    if (node->condition) {
        warnAssignInCondition(node->condition.get());
        checkCondition(node, node->condition.get());
    }
}

void TypeChecker::visit(ForStmt* node) {
    pushScope();

    // Type check init — if it's a BlockStmt wrapping a declaration (for-loop init
    // pattern from the parser), process items directly in the ForStmt scope so
    // the declared variable is accessible in the condition/step/body.
    if (node->init) {
        if (auto block = dynamic_cast<BlockStmt*>(node->init.get())) {
            for (const auto& item : block->items) {
                if (std::holds_alternative<DeclPtr>(item))
                    std::get<DeclPtr>(item)->accept(this);
                else
                    std::get<StmtPtr>(item)->accept(this);
            }
        } else {
            node->init->accept(this);
        }
    }
    // `for (i in A..B)`: the loop variable and the bound share the bounds' common type.
    if (auto* rb = dynamic_cast<BlockStmt*>(node->init.get())) {
        if (rb->items.size() == 2 && std::holds_alternative<DeclPtr>(rb->items[0]) &&
            std::holds_alternative<DeclPtr>(rb->items[1])) {
            auto* lo = dynamic_cast<VarDecl*>(std::get<DeclPtr>(rb->items[0]).get());
            auto* hi = dynamic_cast<VarDecl*>(std::get<DeclPtr>(rb->items[1]).get());
            if (lo && hi && lo->rangeBound && hi->rangeBound) {
                std::string ct = ty::rangeVarType(lo->type, hi->type);
                if (!ct.empty()) {
                    lo->type = hi->type = ct;
                    for (VarDecl* d : {lo, hi}) {
                        auto it = scopes.back().find(d->name);
                        if (it != scopes.back().end()) it->second.type = ct;
                    }
                }
            }
        }
    }

    dropAssignedIn(node->condition.get());
    dropAssignedIn(node->step.get());
    dropAssignedIn(node->body.get()); markAddrTakenIn(node->body.get());

    // Type check condition (for intentionally omits the assign-in-condition warning)
    if (node->condition) {
        checkCondition(node, node->condition.get());
    }

    // Type check body, then the step (which runs after it), both under the
    // condition's narrowing (`for (; p != null; p = p.next)`).
    std::vector<std::string> keys;
    if (node->condition) condNarrowings(node->condition.get(), true, keys);
    auto inserted = applyNarrowings(keys);
    loopLabelStack.push_back(node->label);
    if (node->body) {
        node->body->accept(this);
    }
    loopLabelStack.pop_back();

    // Type check step (its value is discarded, like an expression statement's)
    if (node->step) {
        node->step->accept(this);
        if (std::string fn = discardedMustUse(node->step.get()); !fn.empty())
            errorAt(node->step.get(), "result of '" + fnDisplay(fn) + "' must be used (it is marked must_use)");
    }
    undoNarrowings(inserted);

    popScope();
}

void TypeChecker::visit(ReturnStmt* node) {
    if (finallyDepth > 0) errorAt(node, "'return' is not allowed inside a finally block");
    if (node->value) {
        hintIfaceTarget(node->value.get(), currentFunctionReturnType);
        inferVariantTarget(node->value, currentRawReturnType, true);
        node->value->accept(this);
        // Returning the address of a local or parameter yields a dangling pointer
        // (its stack frame is gone on return). Flag the clear case `return &x` where
        // x is a local/param; `&(*ptr)` or `&ptrParam.field` point into caller memory
        // and are fine, so they are not flagged.
        // A field or element of a local value (`&p.a`, `&arr[1]`) and a slice of a local
        // array dangle the same way; a `static` local lives on.
        // The value may reach the return through a pointer cast or either `?:` arm.
        std::vector<Expr*> leaves{node->value.get()};
        while (!leaves.empty()) {
            Expr* v = leaves.back();
            leaves.pop_back();
            if (auto* c = dynamic_cast<CastExpr*>(v)) {
                std::string ct = normalizeType(c->targetType);
                if (!ct.empty() && ct[0] == '?') ct = ct.substr(1);
                if (isPointerType(ct)) { leaves.push_back(c->expr.get()); continue; }
            }
            if (auto* t = dynamic_cast<TernaryExpr*>(v)) {
                leaves.push_back(t->elseExpr.get());
                leaves.push_back(t->thenExpr.get());
                continue;
            }
            if (auto* u = dynamic_cast<UnaryExpr*>(v); u && u->op == "&") {
                std::string root = localStorageRoot(u->operand.get());
                if (!root.empty())
                    errorAt(node, "returning the address of local '" + root + "' (dangling pointer)");
            }
            if (auto* id = dynamic_cast<IdentExpr*>(v)) {
                auto it = localAddrHolders.find(localAddrKey(id->name));
                const Symbol* sym = findSymbol(id->name);
                if (it != localAddrHolders.end() && !it->second.empty() && sym && !sym->addrTaken &&
                    captureBoundary.empty())
                    noteWarning("dangling-pointer", node->line, node->col,
                                "returning '" + id->name + "', which holds the address of local '" +
                                it->second + "' (dangling pointer)");
            }
            if (auto* sl = dynamic_cast<StructInitExpr*>(v)) {
                for (const auto& fv : sl->fieldInits)
                    if (auto* u = dynamic_cast<UnaryExpr*>(fv.second.get()); u && u->op == "&") {
                        std::string root = localStorageRoot(u->operand.get());
                        if (!root.empty())
                            noteWarning("dangling-pointer", node->line, node->col,
                                        "returning a struct that holds the address of local '" + root +
                                        "' (dangling pointer)");
                    }
            }
            if (auto* ix = dynamic_cast<IndexExpr*>(v);
                ix && ix->highIndex && ty::Type::parse(getExpressionType(ix->base.get())).kind == ty::Type::Kind::Array) {
                std::string root = localStorageRoot(ix->base.get());
                if (!root.empty())
                    errorAt(node, "returning a slice of local array '" + root + "' (dangling)");
            }
        }
        std::string valueType = getExpressionType(node->value.get());
        // A returned integer literal that fits the return type stays valid; other
        // narrowing needs an explicit cast (same rule as init / assignment).
        std::string e = assignabilityError(currentFunctionReturnType, valueType, node->value.get());
        if (!e.empty())
            errorAt(node, "return type mismatch: expected " + currentFunctionReturnType +
                          ", got " + valueType + " (" + e + ")");
    } else if (currentFunctionReturnType != "void") {
        errorAt(node,"return type mismatch: expected " + currentFunctionReturnType +
                    ", got void");
    }
}

// The local (or parameter) whose own storage `e` denotes: a variable, or a field / element
// of a value (not one reached through a pointer). "" for anything else, a global, or a
// `static` local.
std::string TypeChecker::localAddrKey(const std::string& name) {
    return std::to_string(scopeOf(name)) + ":" + name;
}

// Record what `name = rhs` stores for -Wdangling-pointer: the address of a local (also
// `p + k` / `p - k` of the same holder), or anything else, which clears the holder.
void TypeChecker::trackLocalAddr(const std::string& name, Expr* rhs) {
    if (scopeOf(name) < 1) return;
    std::string key = localAddrKey(name);
    if (auto* u = dynamic_cast<UnaryExpr*>(rhs); u && u->op == "&") {
        std::string root = localStorageRoot(u->operand.get());
        auto it = localAddrHolders.find(key);
        if (!root.empty() && (it == localAddrHolders.end() || !it->second.empty())) {
            localAddrHolders[key] = root;
            return;
        }
    }
    if (auto* b = dynamic_cast<BinaryExpr*>(rhs); b && (b->op == "+" || b->op == "-"))
        if (auto* l = dynamic_cast<IdentExpr*>(b->left.get()); l && l->name == name) return;
    localAddrHolders[key] = "";
}

std::string TypeChecker::localStorageRoot(Expr* e) {
    while (e) {
        if (auto* id = dynamic_cast<IdentExpr*>(e)) {
            int si = scopeOf(id->name);
            if (si < 1 || scopes[si].find(id->name)->second.isStatic) return "";
            return id->name;
        }
        if (auto* m = dynamic_cast<MemberExpr*>(e)) {
            if (tyq::isPtr(getExpressionType(m->base.get()))) return "";
            e = m->base.get();
        } else if (auto* ix = dynamic_cast<IndexExpr*>(e); ix && !ix->highIndex) {
            if (ty::Type::parse(getExpressionType(ix->base.get())).kind != ty::Type::Kind::Array) return "";
            e = ix->base.get();
        } else {
            return "";
        }
    }
    return "";
}

void TypeChecker::visit(BreakStmt* node) {
    // A bare break needs an enclosing loop or switch of the SAME function (a lambda body
    // starts a fresh context); a labeled break must name an enclosing loop label.
    if (node->label.empty() && loopLabelStack.empty() && switchDepth == 0)
        errorAt(node, "'break' outside of a loop or switch");
    if (!node->label.empty()) {
        bool found = false;
        for (auto& l : loopLabelStack) if (l == node->label) { found = true; break; }
        if (!found) errorAt(node, "labeled 'break " + node->label + "' has no enclosing loop labeled '" + node->label + "'");
    }
}

void TypeChecker::visit(ExprStmt* node) {
    if (node->expr) {
        node->expr->accept(this);
    }
    // A bare call to a `must_use` function discards its result — reject it.
    if (!mustUseFuncs.empty() && node->expr) {
        std::string fn = discardedMustUse(node->expr.get());
        if (!fn.empty())
            errorAt(node->line > 0 ? static_cast<ASTNode*>(node) : node->expr.get(),
                    "result of '" + fnDisplay(fn) + "' must be used (it is marked must_use)");
    }
    if (node->expr) warnUnusedResult(node);
}

// -Wunused-result: a call statement (`f(x);`, `x.m();`, `f<T>(x);`) whose value is a
// `Result<...>` instance (also through an alias) drops the error it may carry.
void TypeChecker::warnUnusedResult(ExprStmt* node) {
    Expr* e = node->expr.get();
    std::string fn;
    if (auto* c = dynamic_cast<CallExpr*>(e)) {
        if (c->dbgBuiltin) return;
        if (auto* id = dynamic_cast<IdentExpr*>(c->callee.get())) fn = id->name;
        else if (auto* m = dynamic_cast<MemberExpr*>(c->callee.get())) fn = m->member;
        else return;
    } else if (auto* tc = dynamic_cast<TemplateCallExpr*>(e)) {
        fn = tc->templateName;
    } else {
        return;
    }
    ty::Type t = ty::Type::parse(normalizeType(tyq::strip(getExpressionType(e))));
    if (t.kind == ty::Type::Kind::Pointer) return;
    bool isResult = t.kind == ty::Type::Kind::Template && t.name == "Result";
    if (auto it = templateInstanceArgs.find(t.nominalName()); it != templateInstanceArgs.end())
        isResult = isResult || it->second.first == "Result";
    if (!isResult) return;
    ASTNode* at = node->line > 0 ? static_cast<ASTNode*>(node) : e;
    noteWarning("unused-result", at->line, at->col,
                "result of '" + fn + "' is a Result and is ignored (check it or use '?')");
}

// The `must_use` function whose result `e`, evaluated for its side effects only, would
// discard: a call (also through a method or an operator) or either arm of a `?:`. "" if none.
std::string TypeChecker::discardedMustUse(Expr* e) {
    std::string fn;
    if (auto* c = dynamic_cast<CallExpr*>(e)) {
        if (auto* id = dynamic_cast<IdentExpr*>(c->callee.get())) fn = id->name;
        else if (auto* m = dynamic_cast<MemberExpr*>(c->callee.get())) {
            // Method-call syntax `x.m()` resolves to `Type_m` (see visit(CallExpr)).
            std::string bt = ty::Type::parse(getExpressionType(m->base.get())).nominalName();
            if (mustUseFuncs.count(bt + "_" + m->member)) fn = bt + "_" + m->member;
            else if (auto ti = templateInstanceArgs.find(
                         ty::Type::parse(normalizeType(getExpressionType(m->base.get()))).nominalName());
                     ti != templateInstanceArgs.end())
                fn = ti->second.first + "_" + m->member;   // a generic `S_m<T>` (checkGenericMethodCall)
        }
    } else if (auto* tc = dynamic_cast<TemplateCallExpr*>(e)) {
        fn = tc->templateName;
    } else if (auto* t = dynamic_cast<TernaryExpr*>(e)) {
        fn = discardedMustUse(t->thenExpr.get());
        if (fn.empty()) fn = discardedMustUse(t->elseExpr.get());
    } else if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
        fn = b->opFunc;
    } else if (auto* u = dynamic_cast<UnaryExpr*>(e)) {
        fn = u->opFunc;
    } else if (auto* ix = dynamic_cast<IndexExpr*>(e)) {
        fn = ix->opFunc;
    }
    return (!fn.empty() && mustUseFuncs.count(fn)) ? fn : "";
}

void TypeChecker::visit(ContinueStmt* node) {
    // Valid inside a loop of the same function. A labeled continue must name an
    // enclosing loop label.
    if (node->label.empty() && loopLabelStack.empty())
        errorAt(node, "'continue' outside of a loop");
    if (!node->label.empty()) {
        bool found = false;
        for (auto& l : loopLabelStack) if (l == node->label) { found = true; break; }
        if (!found) errorAt(node, "labeled 'continue " + node->label + "' has no enclosing loop labeled '" + node->label + "'");
    }
}

// Extended asm: an output (`"=r"(y)`, `"+r"(y)`, `"=m"(y)`) is written by the asm, so it
// must be a writable lvalue of a scalar type (an integer other than bool, a float or a
// pointer); an input's constraint cannot be an output's.
void TypeChecker::visit(AsmStmt* node) {
    for (auto& [constraint, expr] : node->outputs) {
        if (!expr) continue;
        expr->accept(this);
        if (constraint.empty() || (constraint[0] != '=' && constraint[0] != '+')) {
            errorAt(node, "asm output constraint '" + constraint + "' must start with '=' or '+'");
            continue;
        }
        if (isSliceLen(expr.get()) || !isLvalueExpr(expr.get())) {
            errorAt(node, "asm output operand must be an lvalue (a variable, field, element, or dereference)");
            continue;
        }
        std::string cname;
        if (assignsToConst(expr.get(), cname))
            errorAt(node, "asm output operand is read-only ('" + cname + "')");
        checkCapturedWrite(node, expr.get());
        if (auto* m = dynamic_cast<MemberExpr*>(expr.get())) {
            std::string bt = ty::Type::parse(normalizeType(tyq::strip(getExpressionType(m->base.get())))).nominalName();
            auto sit = structs.find(bt);
            if (sit != structs.end())
                if (const auto* f = sit->second.field(m->member); f && f->bitWidth > 0)
                    errorAt(node, "asm output operand cannot be a bitfield ('" + m->member + "')");
        }
        std::string t = getExpressionType(expr.get());
        std::string n = normalizeType(t);
        if (!n.empty() && n[0] == '?') n = n.substr(1);
        bool scalar = (isNumericType(n) && n != "bool") || n == "string" ||
                      (isPointerType(n) && n.back() != ']');
        if (t != "unknown" && !scalar)
            errorAt(node, "asm output operand must have an integer, floating-point or pointer type, got '" + t + "'");
        if (auto* id = dynamic_cast<IdentExpr*>(expr.get()))
            narrowedNonNull.erase(narrowKey(id->name));   // the asm may store null
    }
    for (auto& [constraint, expr] : node->inputs) {
        if (!constraint.empty() && (constraint[0] == '=' || constraint[0] == '+'))
            errorAt(node, "asm input constraint '" + constraint + "' cannot start with '=' or '+'");
        if (expr) expr->accept(this);
    }
}

void TypeChecker::visit(ThreadJoinStmt* node) {
    node->tid->accept(this);
    dropGlobalNarrowings();   // the joined thread may have assigned any global
    std::string t = getExpressionType(node->tid.get());
    std::string n = normalizeType(t);
    if (!n.empty() && n[0] == '?') n = n.substr(1);
    if (t != "unknown" && n != "*void")
        errorAt(node, "thread_join expects a thread handle ('*void' from thread_create), got '" + t + "'");
}

void TypeChecker::visit(ThrowStmt* node) {
    if (node->value) {
        node->value->accept(this);
        if (isVoidValueType(getExpressionType(node->value.get())))
            errorAt(node, "cannot throw a 'void' value");
        if (!inInstance) node->valueType = getExpressionType(node->value.get());
    }
}

void TypeChecker::visit(TryStmt* node) {
    if (node->body) node->body->accept(this);
    for (auto& c : node->catches) {
        pushScope();
        defineSymbol(c.name, c.type, c.line, c.col, false);
        if (c.body) c.body->accept(this);
        popScope();
    }
    // A `finally` runs on every exit, including an exception unwinding through it, so a
    // `return` there would silently discard the pending exit (the Java/C# footgun).
    if (node->finally) {
        ++finallyDepth;
        node->finally->accept(this);
        --finallyDepth;
    }
}

void TypeChecker::visit(DeferStmt* node) {
    // Type-check the deferred body in its own scope.
    pushScope();
    if (node->body) node->body->accept(this);
    popScope();

    // A defer body runs during scope-exit cleanup, so it may not transfer control
    // out of itself: a `return`, or a `break`/`continue` not enclosed by a loop or
    // switch *within* the body, would jump to a target that is no longer valid. A `?`
    // is an early return too (a lambda in the body is its own function).
    std::function<void(Expr*)> checkExpr = [&](Expr* e) {
        if (!e || dynamic_cast<LambdaExpr*>(e)) return;
        if (dynamic_cast<QuestionExpr*>(e))
            errorAt(e, "'?' is not allowed inside a defer body (it would return from the function)");
        astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { checkExpr(c.get()); });
    };
    std::function<void(Stmt*, int)> check = [&](Stmt* s, int loopDepth) {
        if (!s) return;
        if (auto* es = dynamic_cast<ExprStmt*>(s)) checkExpr(es->expr.get());
        else if (auto* ts = dynamic_cast<ThrowStmt*>(s)) checkExpr(ts->value.get());
        if (dynamic_cast<ReturnStmt*>(s)) {
            errorAt(s, "'return' is not allowed inside a defer body");
        } else if (dynamic_cast<BreakStmt*>(s) || dynamic_cast<ContinueStmt*>(s)) {
            // A bare break/continue is fine if a loop/switch *inside* the defer body encloses
            // it (loopDepth>0). A *labeled* one may target a loop outside the defer body, so it
            // is rejected regardless of depth.
            std::string lbl;
            if (auto* bs = dynamic_cast<BreakStmt*>(s)) lbl = bs->label;
            else if (auto* cs = dynamic_cast<ContinueStmt*>(s)) lbl = cs->label;
            if (loopDepth == 0 || !lbl.empty())
                errorAt(s, "'break'/'continue' inside a defer body may not escape it");
        } else if (auto* b = dynamic_cast<BlockStmt*>(s)) {
            for (auto& it : b->items) {
                if (auto* st = std::get_if<StmtPtr>(&it)) check(st->get(), loopDepth);
                else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) checkExpr(vd->initializer.get());
            }
        } else if (auto* i = dynamic_cast<IfStmt*>(s)) {
            checkExpr(i->condition.get());
            check(i->thenBranch.get(), loopDepth);
            check(i->elseBranch.get(), loopDepth);
        } else if (auto* w = dynamic_cast<WhileStmt*>(s)) {
            checkExpr(w->condition.get());
            check(w->body.get(), loopDepth + 1);
        } else if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) {
            checkExpr(dw->condition.get());
            check(dw->body.get(), loopDepth + 1);
        } else if (auto* f = dynamic_cast<ForStmt*>(s)) {
            check(f->init.get(), loopDepth);
            checkExpr(f->condition.get()); checkExpr(f->step.get());
            check(f->body.get(), loopDepth + 1);
        } else if (auto* fi = dynamic_cast<ForInStmt*>(s)) {
            checkExpr(fi->iterable.get());
            check(fi->body.get(), loopDepth + 1);
        } else if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
            checkExpr(sw->subject.get());
            for (auto& c : sw->cases)
                for (auto& it : c.stmts) {
                    if (auto* st = std::get_if<StmtPtr>(&it)) check(st->get(), loopDepth + 1);
                    else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) checkExpr(vd->initializer.get());
                }
        } else if (auto* m = dynamic_cast<MatchStmt*>(s)) {
            checkExpr(m->subject.get());
            for (auto& a : m->arms) check(a.body.get(), loopDepth);
        } else if (auto* t = dynamic_cast<TryStmt*>(s)) {
            check(t->body.get(), loopDepth);
            for (auto& c : t->catches) check(c.body.get(), loopDepth);
            check(t->finally.get(), loopDepth);
        } else if (auto* d = dynamic_cast<DeferStmt*>(s)) {
            check(d->body.get(), loopDepth);
        }
    };
    if (node->body) check(node->body.get(), 0);
}

void TypeChecker::visit(MatchStmt* node) {
    node->subject->accept(this);
    std::string rawSt = getExpressionType(node->subject.get());
    std::string st = normalizeType(rawSt);
    // An alias of a classic enum (`type C = Col`) names that enum's variant set.
    if (plainEnumDecls.count(tyq::strip(dealiasOperand(rawSt)))) rawSt = tyq::strip(dealiasOperand(rawSt));
    // A classic (payload-less) enum normalizes to `int`, so keep its declared name for a
    // `match`: that is the only place the variant set (for exhaustiveness) is recoverable.
    if (!enumDecls.count(st) && !templateInstanceArgs.count(st) && plainEnumDecls.count(rawSt))
        st = rawSt;
    // Resolve the enum decl + (for a generic instance like Option_int) the type-
    // parameter substitutions, so payload types come out concrete.
    EnumDecl* ed = nullptr;
    std::map<std::string, std::string> subs;
    if (enumDecls.count(st)) {
        ed = enumDecls[st];                                  // concrete ADT enum
    } else if (templateInstanceArgs.count(st)) {             // generic instance
        auto& inst = templateInstanceArgs[st];
        auto git = genericEnumDecls.find(inst.first);
        if (git != genericEnumDecls.end()) {
            ed = git->second;
            for (size_t i = 0; i < ed->typeParams.size() && i < inst.second.size(); ++i)
                subs[ed->typeParams[i]] = inst.second[i];
        }
    } else if (plainEnumDecls.count(st)) {                   // classic int enum (no payloads)
        ed = plainEnumDecls[st];
    }
    if (!ed && st != "unknown")
        errorAt(node, "match subject must be an enum, got " + st);
    if (!inInstance) node->enumName = st;
    // Index of a variant within `ed` by name (-1 if absent).
    auto variantIndex = [&](const std::string& v) -> int {
        if (!ed) return -1;
        for (size_t i = 0; i < ed->members.size(); ++i)
            if (ed->members[i].first == v) return (int)i;
        return -1;
    };
    bool hasDefault = false;
    std::set<std::string> covered;
    // A classic enum may give two members the same value (legal, as in C); a `match` is a
    // switch on the value, so two arms for equal values would be one duplicate case.
    bool plain = ed && plainEnumDecls.count(st) && plainEnumDecls[st] == ed;
    std::map<long long, std::string> valueArm;
    auto memberValue = [&](const std::string& v, long long& out) {
        for (const auto& m : ed->members) if (m.first == v) { out = m.second; return true; }
        return false;
    };
    for (size_t ai = 0; ai < node->arms.size(); ++ai) {
        auto& arm = node->arms[ai];
        if (arm.variant.empty()) {
            if (hasDefault) errorAt(node, "duplicate match arm for variant '_'");
            hasDefault = true;
            if (ai + 1 < node->arms.size())   // arms after `_` can never match
                warning(node->line, node->col, "match arms after the `_` default are unreachable");
        }
        else if (!covered.insert(arm.variant).second)
            errorAt(node, "duplicate match arm for variant '" + arm.variant + "'");
        else if (long long val = 0; plain && memberValue(arm.variant, val)) {
            auto [it, fresh] = valueArm.insert({val, arm.variant});
            if (!fresh)
                errorAt(node, "duplicate match value: '" + arm.variant + "' has the same value (" +
                              std::to_string(val) + ") as '" + it->second + "'");
        }
        pushScope();
        if (!arm.variant.empty() && ed) {
            int vi = variantIndex(arm.variant);
            if (vi < 0) {
                errorAt(node, "'" + arm.variant + "' is not a variant of " + st);
            } else {
                static const std::vector<std::string> noPayload;   // classic enums carry none
                const auto& payload = (vi < (int)ed->payloads.size()) ? ed->payloads[vi] : noPayload;
                if (arm.bindings.size() != payload.size())
                    errorAt(node, "variant '" + arm.variant + "' binds " +
                        std::to_string(payload.size()) + " field(s), got " +
                        std::to_string(arm.bindings.size()));
                std::set<std::string> bound;
                for (const auto& b : arm.bindings)
                    if (b != "_" && !bound.insert(b).second)
                        errorAt(node, "duplicate binding '" + b + "' in match arm '" + arm.variant + "'");
                // The async lowering keeps a binding that lives across an await in a frame
                // field of this type (a generic body's nodes are shared: not stamped).
                if (!inInstance) arm.bindingTypes.clear();
                std::vector<std::string> instBts;
                for (size_t i = 0; i < arm.bindings.size() && i < payload.size(); ++i) {
                    std::string bt = normalizeType(substType(payload[i], subs));
                    defineSymbol(arm.bindings[i], bt, node->line, node->col, /*isParam=*/false);
                    if (!inInstance) arm.bindingTypes.push_back(bt);
                    else instBts.push_back(bt);
                }
                if (inInstance) arm.instanceBindingTypes.push_back({instSubs, instBts});
            }
        }
        if (arm.body) arm.body->accept(this);
        popScope();
    }
    // Exhaustiveness: without a `_` default, every variant must be covered.
    if (ed && !hasDefault) {
        std::string missing;
        for (const auto& m : ed->members)
            if (!covered.count(m.first) && !(plain && valueArm.count(m.second)))   // an equal-valued arm covers it
                missing += (missing.empty() ? "'" : ", '") + m.first + "'";
        if (!missing.empty())
            errorAt(node, "non-exhaustive match on '" + st + "': missing " + missing +
                          " (add those arms or a `_` default)");
    }
}

// Does the folded case value `v` lie in the range of integer type `t`? A 64-bit subject
// holds every folded value (it is folded in 64 bits).
static bool caseValueFits(const std::string& raw, long long v) {
    std::string t = tyq::strip(raw);
    if (t == "bool") return v >= 0 && v <= 1;
    if (t == "char" || t == "uint8") return v >= 0 && v <= 255;
    if (t == "int8") return v >= -128 && v <= 127;
    if (t == "uint16") return v >= 0 && v <= 65535;
    if (t == "int16") return v >= -32768 && v <= 32767;
    if (t == "uint" || t == "uint32") return v >= 0 && v <= 4294967295LL;
    if (t == "int" || t == "int32") return v >= INT_MIN && v <= INT_MAX;
    return true;
}

void TypeChecker::visit(SwitchStmt* node) {
    node->subject->accept(this);
    std::string subjType = getExpressionType(node->subject.get());
    if (subjType != "unknown" && !isIntType(normalizeType(subjType)))   // a classic enum counts as int
        errorAt(node,"switch subject must be integer type, got " + subjType);
    std::set<long long> seenCases;   // detect duplicate case values (else codegen
                                     // emits a switch the IR verifier rejects)
    bool seenDefault = false;
    pushScope();                       // the switch body is one scope (C): a case's
                                       // declaration is visible in the cases after it
    for (auto& c : node->cases) {
        if (!c.value) {
            if (seenDefault) errorAt(node, "multiple 'default' labels in one switch");
            seenDefault = true;
        }
        if (c.value) {
            c.value->accept(this);
            std::string caseType = getExpressionType(c.value.get());
            if (caseType != "unknown" && subjType != "unknown") {
                if (!isValidAssignment(subjType, caseType) &&
                    !(isIntType(subjType) && isIntType(caseType))) {
                    errorAt(c.value.get(),
                        "case value type '" + caseType +
                        "' is incompatible with switch subject type '" + subjType + "'");
                }
            }
            if (!isConstIntExpr(c.value.get()))
                errorAt(c.value.get(), "switch case value must be a constant integer expression");
            // Fold the label to its value so two spellings of one value (`7` and
            // `(4 * 2) - 1`) are caught as duplicates, as in C.
            long long cv = 0;
            bool haveCv = foldConstInt(c.value.get(), cv);
            // A label the subject's type cannot hold never matches (and, truncated to that
            // type, could collide with another label): reject it.
            if (haveCv && subjType != "unknown" && !caseValueFits(normalizeType(subjType), cv)) {
                errorAt(c.value.get(), "case value " + std::to_string(cv) +
                                       " is out of range for switch subject type '" + subjType + "'");
                haveCv = false;
            }
            if (haveCv) {
                if (seenCases.count(cv))
                    errorAt(c.value.get(), "duplicate case value in switch");
                else
                    seenCases.insert(cv);
            }
        }
        ++switchDepth;
        for (auto& it : c.stmts) {
            if (auto* st = std::get_if<StmtPtr>(&it)) (*st)->accept(this);
            else std::get<DeclPtr>(it)->accept(this);
        }
        --switchDepth;
    }
    popScope();
}

bool TypeChecker::isLvalueExpr(Expr* e) {
    if (auto* id = dynamic_cast<IdentExpr*>(e))
        return !lookupSymbol(id->name).empty() || !functionSignatures.count(id->name);
    if (auto* u = dynamic_cast<UnaryExpr*>(e)) return u->op == "*";
    // A field or an array element is storage only when its struct or array is: through a
    // pointer (or a slice), or itself stored (`mk().x` and `arr()[0]` are temporaries).
    auto storedBase = [&](Expr* base) {
        ty::Type bt = ty::Type::parse(normalizeType(dealiasOperand(getExpressionType(base))));
        if (bt.kind != ty::Type::Kind::Array && bt.kind != ty::Type::Kind::Struct &&
            bt.kind != ty::Type::Kind::Template && bt.kind != ty::Type::Kind::Named)
            return true;
        return isLvalueExpr(base);
    };
    if (auto* m = dynamic_cast<MemberExpr*>(e)) return !isSliceLen(e) && storedBase(m->base.get());
    if (auto* ix = dynamic_cast<IndexExpr*>(e))
        return !ix->highIndex && ix->opFunc.empty() && storedBase(ix->base.get());
    return false;
}

// `s.len` of a slice: a read-only view of the fat pointer's length.
bool TypeChecker::isSliceLen(Expr* e) {
    auto* m = dynamic_cast<MemberExpr*>(e);
    if (!m || m->member != "len") return false;
    std::string bt = getExpressionType(m->base.get());
    if (!bt.empty() && bt[0] == '?') bt = bt.substr(1);
    return ty::Type::parse(bt).kind == ty::Type::Kind::Slice;
}

// Value of an integer constant expression built from literals, enum members, unary
// and binary operators and casts. Returns false when the value is not known here
// (`sizeof`, a non-constant name, division by zero), so callers never guess.
bool TypeChecker::foldConstInt(Expr* e, long long& out) {
    ty::CInt c;
    if (!foldConstIntT(e, c)) return false;
    out = c.v;
    return true;
}

// The typed fold behind foldConstInt: the value keeps its C type (after promotion), so
// the operators follow C's usual arithmetic conversions (ty::cintBinary).
bool TypeChecker::foldConstIntT(Expr* e, ty::CInt& out) {
    if (auto* l = dynamic_cast<LiteralExpr*>(e)) {
        if (l->kind == LiteralExpr::Kind::INT) {
            // A negative literal (`-2147483648`) is one token, typed by its value as the
            // program sees it at run time: `int` when the value fits, else `int64`.
            bool neg = !l->value.empty() && l->value[0] == '-';
            unsigned long long m = 0;
            try { m = std::stoull(neg ? l->value.substr(1) : l->value, nullptr, 0); }
            catch (...) { return false; }
            out = neg ? ty::cintNegLiteral(m) : ty::cintLiteral(m);
            return true;
        }
        if (l->kind == LiteralExpr::Kind::CHAR) { out = ty::cintMake(l->value.empty() ? 0 : (unsigned char)l->value[0], 32, false); return true; }
        if (l->kind == LiteralExpr::Kind::BOOL) { out = ty::cintMake(l->value == "true" ? 1 : 0, 32, false); return true; }
        return false;
    }
    if (auto* id = dynamic_cast<IdentExpr*>(e)) {
        if (const Symbol* sym = findSymbol(id->name)) {
            // A `const` integer folds through its initializer (bounded, so a
            // self-referential const can't recurse forever), converted to its type.
            std::string st = normalizeType(sym->type);
            if (!sym->isConst || !sym->constInit || !isIntType(st)) return false;
            if (foldDepth > 64) return false;
            ++foldDepth;
            ty::CInt v;
            bool ok = foldConstIntT(sym->constInit, v);
            --foldDepth;
            if (!ok) return false;
            if (!ty::cintCast(tyq::strip(st), v, out)) out = v;
            return true;
        }
        auto it = enumConstants.find(id->name);
        if (it == enumConstants.end()) return false;
        out = it->second >= INT32_MIN && it->second <= INT32_MAX ? ty::cintMake(it->second, 32, false)
                                                                 : ty::cintMake(it->second, 64, false);
        return true;
    }
    if (auto* u = dynamic_cast<UnaryExpr*>(e)) {
        ty::CInt v;
        if (!foldConstIntT(u->operand.get(), v)) return false;
        return ty::cintUnary(u->op, v, out);
    }
    if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
        // A left-leaning chain (`A + B + C ...`) is folded along its spine with a loop.
        std::vector<BinaryExpr*> spine{b};
        while (auto* l = dynamic_cast<BinaryExpr*>(spine.back()->left.get())) spine.push_back(l);
        ty::CInt x;
        if (!foldConstIntT(spine.back()->left.get(), x)) return false;
        for (size_t i = spine.size(); i-- > 0;) {
            const std::string& op = spine[i]->op;
            // `&&` / `||` short-circuit: an unevaluated right operand need not fold.
            if (op == "&&" || op == "||") {
                if ((op == "&&") == (x.v == 0)) { x = ty::cintMake(op == "||", 32, false); continue; }
                ty::CInt y;
                if (!foldConstIntT(spine[i]->right.get(), y)) return false;
                x = ty::cintMake(y.v != 0, 32, false);
                continue;
            }
            ty::CInt y;
            if (!foldConstIntT(spine[i]->right.get(), y)) return false;
            if (!ty::cintBinary(op, x, y, x)) return false;
        }
        out = x;
        return true;
    }
    if (auto* t = dynamic_cast<TernaryExpr*>(e)) {
        ty::CInt c, a, other;
        if (!foldConstIntT(t->condition.get(), c)) return false;
        if (!foldConstIntT(c.v != 0 ? t->thenExpr.get() : t->elseExpr.get(), a)) return false;
        // The arms meet at their common type (when the other one folds too).
        out = a;
        if (foldConstIntT(c.v != 0 ? t->elseExpr.get() : t->thenExpr.get(), other)) {
            int r = a.rank > other.rank ? a.rank : other.rank;
            bool un = a.rank == other.rank ? (a.uns || other.uns) : (a.rank > other.rank ? a.uns : other.uns);
            out = ty::cintMake(a.v, r, un);
        }
        return true;
    }
    if (auto* c = dynamic_cast<CastExpr*>(e)) {
        std::string to = tyq::strip(normalizeType(c->targetType));
        bool isInt = true; ty::CInt i; double d = 0;
        if (!foldConstNumT(c->expr.get(), isInt, i, d)) return false;
        if (!isInt) {
            // A floating value converts toward zero; out of range it has no value (C).
            if (!isIntType(to) || (to != "bool" && !floatConstFitsInt(d, to))) return false;
            long long iv;
            if (to == "bool") iv = d != 0;
            else iv = d >= 9223372036854775808.0 ? (long long)(unsigned long long)d : (long long)d;
            i = ty::cintMake(iv, 64, to == "uint64");
        }
        if (!ty::cintCast(to, i, out)) out = i;
        return true;
    }
    if (auto* z = dynamic_cast<SizeofExpr*>(e)) {
        // A type whose target layout is known here (scalars, pointers, arrays, structs
        // and unions of those) folds; the rest is left to codegen. A size is unsigned
        // 64-bit (C's size_t).
        if (inInstance || z->operand) return false;
        std::string zt = z->typeName;
        if (!isPrimitiveType(zt) && !structs.count(zt) && !typeAliases.count(zt) && !enumTypes.count(zt)) {
            std::string vt = lookupSymbol(zt);
            if (!vt.empty() && vt != "unknown" && vt != "struct:" + zt) zt = tyq::strip(vt);
        }
        long long sz = constSizeof(zt);
        out = ty::cintMake(sz, 64, true);
        return sz != 0;
    }
    return false;
}

bool TypeChecker::foldConstNum(Expr* e, bool& isInt, long long& i, double& d) {
    ty::CInt c;
    if (!foldConstNumT(e, isInt, c, d)) return false;
    i = c.v;
    return true;
}

bool TypeChecker::foldConstNumT(Expr* e, bool& isInt, ty::CInt& i, double& d) {
    auto toFloat = [](const std::string& t, double v) { return t == "float" ? (double)(float)v : v; };
    auto asDouble = [](const ty::CInt& c) { return c.uns ? (double)(unsigned long long)c.v : (double)c.v; };
    if (auto* l = dynamic_cast<LiteralExpr*>(e); l && l->kind == LiteralExpr::Kind::FLOAT) {
        isInt = false; d = std::strtod(l->value.c_str(), nullptr);
        return true;
    }
    if (auto* id = dynamic_cast<IdentExpr*>(e)) {
        const Symbol* sym = findSymbol(id->name);
        std::string t = sym ? tyq::strip(normalizeType(sym->type)) : "";
        if (sym && sym->isConst && sym->constInit && (t == "float" || t == "double")) {
            if (foldDepth > 64) return false;
            ++foldDepth;
            bool ok = foldConstNumT(sym->constInit, isInt, i, d);
            --foldDepth;
            if (!ok) return false;
            if (isInt) d = asDouble(i);
            isInt = false; d = toFloat(t, d);
            return true;
        }
    }
    if (auto* u = dynamic_cast<UnaryExpr*>(e); u && u->op == "-") {
        if (!foldConstNumT(u->operand.get(), isInt, i, d)) return false;
        if (isInt) return ty::cintUnary("-", i, i);
        d = -d;
        return true;
    }
    if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
        std::vector<BinaryExpr*> spine{b};
        while (auto* l = dynamic_cast<BinaryExpr*>(spine.back()->left.get())) spine.push_back(l);
        if (!foldConstNumT(spine.back()->left.get(), isInt, i, d)) return false;
        for (size_t k = spine.size(); k-- > 0;) {
            bool yInt = true; ty::CInt yi; double yd = 0;
            const std::string& op = spine[k]->op;
            if (!foldConstNumT(spine[k]->right.get(), yInt, yi, yd)) return false;
            if (isInt && yInt) {
                if (!ty::cintBinary(op, i, yi, i)) return false;
                continue;
            }
            double x = isInt ? asDouble(i) : d, y = yInt ? asDouble(yi) : yd;
            if (op == "+") d = x + y;
            else if (op == "-") d = x - y;
            else if (op == "*") d = x * y;
            else if (op == "/") d = x / y;
            else return false;
            isInt = false;
        }
        return true;
    }
    if (auto* c = dynamic_cast<CastExpr*>(e)) {
        std::string to = tyq::strip(normalizeType(c->targetType));
        if (to == "float" || to == "double") {
            if (!foldConstNumT(c->expr.get(), isInt, i, d)) return false;
            if (isInt) d = asDouble(i);
            isInt = false; d = toFloat(to, d);
            return true;
        }
    }
    isInt = true;
    return foldConstIntT(e, i);
}

// `v` converted to the integer type `raw` (C: truncate, then sign- or zero-extend);
// a non-integer or 64-bit type leaves it unchanged.
long long TypeChecker::truncConstInt(const std::string& raw, long long v) {
    std::string t = tyq::strip(raw);
    if (t == "bool") return v != 0;
    if (t == "char" || t == "uint8") return (long long)(uint8_t)v;
    if (t == "int8") return (long long)(int8_t)v;
    if (t == "uint16") return (long long)(uint16_t)v;
    if (t == "int16") return (long long)(int16_t)v;
    if (t == "uint" || t == "uint32") return (long long)(uint32_t)v;
    if (t == "int" || t == "int32") return (long long)(int32_t)v;
    return v;
}

bool TypeChecker::isConstIntExpr(Expr* e) {
    if (auto* l = dynamic_cast<LiteralExpr*>(e))
        return l->kind == LiteralExpr::Kind::INT || l->kind == LiteralExpr::Kind::CHAR ||
               l->kind == LiteralExpr::Kind::BOOL;
    if (auto* id = dynamic_cast<IdentExpr*>(e)) {
        std::string t = lookupSymbol(id->name);
        if (t.empty()) return enumConstants.count(id->name) > 0;
        const Symbol* sym = findSymbol(id->name);   // `const int K = 7` folds (a const parameter does not)
        return sym && sym->isConst && sym->constInit && isIntType(normalizeType(t));
    }
    if (auto* u = dynamic_cast<UnaryExpr*>(e))
        return (u->op == "-" || u->op == "~" || u->op == "!") && isConstIntExpr(u->operand.get());
    if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
        // Down the left spine with a loop (a long `A + B + C ...` is as deep as it is long).
        for (;;) {
            if (b->op == "=" || b->op == "&&" || b->op == "||") return false;
            if (!isConstIntExpr(b->right.get())) return false;
            auto* l = dynamic_cast<BinaryExpr*>(b->left.get());
            if (!l) return isConstIntExpr(b->left.get());
            b = l;
        }
    }
    if (auto* c = dynamic_cast<CastExpr*>(e)) return isConstIntExpr(c->expr.get());
    if (dynamic_cast<SizeofExpr*>(e)) return true;
    return false;
}
