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
            if (auto* ifs = nodeCast<IfStmt>(stmt.get()); ifs && ifs->condition) {
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
    if (auto* id = nodeCast<IdentExpr>(e)) return id->name;
    if (auto* m = nodeCast<MemberExpr>(e)) {
        std::string base = lockPlaceKey(m->base.get());
        return base.empty() ? "" : base + "." + m->member;
    }
    return "";
}

// The earliest `await` (by line, then column) in a subtree, lambda bodies excluded.
static void earliestAwait(Stmt* s, AwaitExpr*& best);
static void earliestAwait(Expr* e, AwaitExpr*& best) {
    if (!e) return;
    if (auto* a = nodeCast<AwaitExpr>(e)) {
        if (!best || a->line < best->line || (a->line == best->line && a->col < best->col)) best = a;
    }
    astwalk::forEachChildExpr(e, [&](ExprPtr& c) { earliestAwait(c.get(), best); });
}
static void earliestAwait(const std::vector<BlockItem>& items, AwaitExpr*& best) {
    for (auto& it : items) {
        if (std::holds_alternative<StmtPtr>(it)) earliestAwait(std::get<StmtPtr>(it).get(), best);
        else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) earliestAwait(vd->initializer.get(), best);
    }
}
static void earliestAwait(Stmt* s, AwaitExpr*& best) {
    if (!s) return;
    auto E = [&](const ExprPtr& e) { earliestAwait(e.get(), best); };
    auto S = [&](const StmtPtr& st) { earliestAwait(st.get(), best); };
    if (auto* b = nodeCast<BlockStmt>(s)) earliestAwait(b->items, best);
    else if (auto* i = nodeCast<IfStmt>(s)) {
        for (IfStmt* n = i; n;) {
            E(n->condition); S(n->thenBranch);
            auto* next = nodeCast<IfStmt>(n->elseBranch.get());
            if (!next) S(n->elseBranch);
            n = next;
        }
    }
    else if (auto* f = nodeCast<ForStmt>(s))       { S(f->init); E(f->condition); E(f->step); S(f->body); }
    else if (auto* fi = nodeCast<ForInStmt>(s))    { E(fi->iterable); S(fi->body); }
    else if (auto* w = nodeCast<WhileStmt>(s))     { E(w->condition); S(w->body); }
    else if (auto* dw = nodeCast<DoWhileStmt>(s))  { S(dw->body); E(dw->condition); }
    else if (auto* r = nodeCast<ReturnStmt>(s))    { E(r->value); }
    else if (auto* sw = nodeCast<SwitchStmt>(s)) {
        E(sw->subject);
        for (auto& c : sw->cases) { E(c.value); earliestAwait(c.stmts, best); }
    }
    else if (auto* m = nodeCast<MatchStmt>(s))     { E(m->subject); for (auto& a : m->arms) S(a.body); }
    else if (auto* th = nodeCast<ThrowStmt>(s))    { E(th->value); }
    else if (auto* t = nodeCast<TryStmt>(s))       { S(t->body); for (auto& c : t->catches) S(c.body); S(t->finally); }
    else if (auto* d = nodeCast<DeferStmt>(s))     { S(d->body); }
    else if (auto* tj = nodeCast<ThreadJoinStmt>(s)) { E(tj->tid); }
    else if (auto* a = nodeCast<AsmStmt>(s))       { for (auto& o : a->outputs) E(o.second); for (auto& in : a->inputs) E(in.second); }
    else if (auto* es = nodeCast<ExprStmt>(s))     { E(es->expr); }
}

// The mutex a statement locks (`lock` true) or unlocks: `Mutex_lock(&m)` /
// `Mutex_lock(p)` by name, or `m.lock()` on a `Mutex` / `*Mutex`. Its place key, or "".
std::string TypeChecker::mutexCallKey(Stmt* s, bool lock) {
    auto* es = nodeCast<ExprStmt>(s);
    auto* c = es ? nodeCast<CallExpr>(es->expr.get()) : nullptr;
    if (!c) return "";
    if (auto* id = nodeCast<IdentExpr>(c->callee.get())) {
        if (id->name != (lock ? "Mutex_lock" : "Mutex_unlock") || c->args.size() != 1) return "";
        Expr* a = c->args[0].get();
        if (auto* u = nodeCast<UnaryExpr>(a); u && u->op == "&") a = u->operand.get();
        return lockPlaceKey(a);
    }
    if (auto* m = nodeCast<MemberExpr>(c->callee.get())) {
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
                if (nodeCast<DeferStmt>(s)) continue;
                earliestAwait(s, aw);
            } else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(items[j]).get())) {
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
            next = nodeCast<IfStmt>(n->elseBranch.get());
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
        if (auto block = nodeCast<BlockStmt>(node->init.get())) {
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
    if (auto* rb = nodeCast<BlockStmt>(node->init.get())) {
        if (rb->items.size() == 2 && std::holds_alternative<DeclPtr>(rb->items[0]) &&
            std::holds_alternative<DeclPtr>(rb->items[1])) {
            auto* lo = nodeCast<VarDecl>(std::get<DeclPtr>(rb->items[0]).get());
            auto* hi = nodeCast<VarDecl>(std::get<DeclPtr>(rb->items[1]).get());
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
            if (auto* c = nodeCast<CastExpr>(v)) {
                std::string ct = normalizeType(c->targetType);
                if (!ct.empty() && ct[0] == '?') ct = ct.substr(1);
                if (isPointerType(ct)) { leaves.push_back(c->expr.get()); continue; }
            }
            if (auto* t = nodeCast<TernaryExpr>(v)) {
                leaves.push_back(t->elseExpr.get());
                leaves.push_back(t->thenExpr.get());
                continue;
            }
            if (auto* u = nodeCast<UnaryExpr>(v); u && u->op == "&") {
                std::string root = localStorageRoot(u->operand.get());
                if (!root.empty())
                    errorAt(node, "returning the address of local '" + root + "' (dangling pointer)");
            }
            if (auto* id = nodeCast<IdentExpr>(v)) {
                auto it = localAddrHolders.find(localAddrKey(id->name));
                const Symbol* sym = findSymbol(id->name);
                if (it != localAddrHolders.end() && !it->second.empty() && sym && !sym->addrTaken &&
                    captureBoundary.empty())
                    noteWarning("dangling-pointer", node->line, node->col,
                                "returning '" + id->name + "', which holds the address of local '" +
                                it->second + "' (dangling pointer)");
            }
            if (auto* sl = nodeCast<StructInitExpr>(v)) {
                for (const auto& fv : sl->fieldInits)
                    if (auto* u = nodeCast<UnaryExpr>(fv.second.get()); u && u->op == "&") {
                        std::string root = localStorageRoot(u->operand.get());
                        if (!root.empty())
                            noteWarning("dangling-pointer", node->line, node->col,
                                        "returning a struct that holds the address of local '" + root +
                                        "' (dangling pointer)");
                    }
            }
            if (auto* ix = nodeCast<IndexExpr>(v);
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
        if (!e.empty() && inTrailingBlock)
            errorAt(node, "a trailing block returns nothing: `return` here leaves the block, not the function");
        else if (!e.empty())
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
    if (auto* u = nodeCast<UnaryExpr>(rhs); u && u->op == "&") {
        std::string root = localStorageRoot(u->operand.get());
        auto it = localAddrHolders.find(key);
        if (!root.empty() && (it == localAddrHolders.end() || !it->second.empty())) {
            localAddrHolders[key] = root;
            return;
        }
    }
    if (auto* b = nodeCast<BinaryExpr>(rhs); b && (b->op == "+" || b->op == "-"))
        if (auto* l = nodeCast<IdentExpr>(b->left.get()); l && l->name == name) return;
    localAddrHolders[key] = "";
}

std::string TypeChecker::localStorageRoot(Expr* e) {
    while (e) {
        if (auto* id = nodeCast<IdentExpr>(e)) {
            int si = scopeOf(id->name);
            if (si < 1 || scopes[si].find(id->name)->second.isStatic) return "";
            return id->name;
        }
        if (auto* m = nodeCast<MemberExpr>(e)) {
            if (tyq::isPtr(getExpressionType(m->base.get()))) return "";
            e = m->base.get();
        } else if (auto* ix = nodeCast<IndexExpr>(e); ix && !ix->highIndex) {
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
        errorAt(node, inTrailingBlock ? "'break' cannot leave a trailing block (the block is a closure)"
                                      : "'break' outside of a loop or switch");
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
    if (auto* c = nodeCast<CallExpr>(e)) {
        if (c->dbgBuiltin) return;
        if (auto* id = nodeCast<IdentExpr>(c->callee.get())) fn = id->name;
        else if (auto* m = nodeCast<MemberExpr>(c->callee.get())) fn = m->member;
        else return;
    } else if (auto* tc = nodeCast<TemplateCallExpr>(e)) {
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
    if (auto* c = nodeCast<CallExpr>(e)) {
        if (auto* id = nodeCast<IdentExpr>(c->callee.get())) fn = id->name;
        else if (auto* m = nodeCast<MemberExpr>(c->callee.get())) {
            // Method-call syntax `x.m()` resolves to `Type_m` (see visit(CallExpr)).
            std::string bt = ty::Type::parse(getExpressionType(m->base.get())).nominalName();
            if (mustUseFuncs.count(bt + "_" + m->member)) fn = bt + "_" + m->member;
            else if (auto ti = templateInstanceArgs.find(
                         ty::Type::parse(normalizeType(getExpressionType(m->base.get()))).nominalName());
                     ti != templateInstanceArgs.end())
                fn = ti->second.first + "_" + m->member;   // a generic `S_m<T>` (checkGenericMethodCall)
        }
    } else if (auto* tc = nodeCast<TemplateCallExpr>(e)) {
        fn = tc->templateName;
    } else if (auto* t = nodeCast<TernaryExpr>(e)) {
        fn = discardedMustUse(t->thenExpr.get());
        if (fn.empty()) fn = discardedMustUse(t->elseExpr.get());
    } else if (auto* b = nodeCast<BinaryExpr>(e)) {
        fn = b->opFunc;
    } else if (auto* u = nodeCast<UnaryExpr>(e)) {
        fn = u->opFunc;
    } else if (auto* ix = nodeCast<IndexExpr>(e)) {
        fn = ix->opFunc;
    }
    return (!fn.empty() && mustUseFuncs.count(fn)) ? fn : "";
}

void TypeChecker::visit(ContinueStmt* node) {
    // Valid inside a loop of the same function. A labeled continue must name an
    // enclosing loop label.
    if (node->label.empty() && loopLabelStack.empty())
        errorAt(node, inTrailingBlock ? "'continue' cannot leave a trailing block (the block is a closure)"
                                      : "'continue' outside of a loop");
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
        if (auto* m = nodeCast<MemberExpr>(expr.get())) {
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
        if (auto* id = nodeCast<IdentExpr>(expr.get()))
            narrowedNonNull.erase(narrowKey(id->name));   // the asm may store null
    }
    for (auto& [constraint, expr] : node->inputs) {
        if (!constraint.empty() && (constraint[0] == '=' || constraint[0] == '+'))
            errorAt(node, "asm input constraint '" + constraint + "' cannot start with '=' or '+'");
        if (expr) expr->accept(this);
    }
}
