// async_lower_desugar.cpp: the desugaring that makes every await the initializer of a let (hoist) (see async_lower.h).
#include "async_lower.h"

void AsyncLowering::spill(ExprPtr& c, std::vector<BlockItem>& pre) {
    std::string t = typeOf(c.get());
    if (t.empty())
        throw locError(c.get(), "an operand with a side effect is evaluated before an 'await' "
            "in the same expression and its type is not known here; bind it to a local first");
    std::string tn = fresh("__sp_t");
    letTmp(pre, tn, t, c);
    c = ident(tn);
}

void AsyncLowering::rval(ExprPtr& c, std::vector<BlockItem>& pre) {
    if (hasAwait(c)) hoist(c, pre);
    else if (hasSideEffects(c.get())) spill(c, pre);
}

bool AsyncLowering::placeShaped(Expr* x) {
    if (nodeCast<IdentExpr>(x) || nodeCast<MemberExpr>(x) || nodeCast<IndexExpr>(x)) return true;
    auto* u = nodeCast<UnaryExpr>(x);
    return u && u->op == "*";
}

void AsyncLowering::place(ExprPtr& e, std::vector<BlockItem>& pre, bool spillParts) {
    auto sub = [&](ExprPtr& c) {
        if (hasAwait(c)) hoist(c, pre);
        else if (spillParts && hasSideEffects(c.get())) spill(c, pre);
    };
    if (nodeCast<IdentExpr>(e.get())) return;
    if (auto* m = nodeCast<MemberExpr>(e.get())) {
        if (placeShaped(m->base.get())) place(m->base, pre, spillParts); else sub(m->base);
        return;
    }
    if (auto* ix = nodeCast<IndexExpr>(e.get()); ix && ix->opFunc.empty() && !ix->highIndex) {
        if (placeShaped(ix->base.get())) place(ix->base, pre, spillParts); else sub(ix->base);
        sub(ix->index);
        return;
    }
    if (auto* u = nodeCast<UnaryExpr>(e.get()); u && u->op == "*" && u->opFunc.empty()) {
        sub(u->operand);
        return;
    }
    if (hasAwait(e) || (spillParts && hasSideEffects(e.get())))
        throw locError(e.get(), "'await' is not supported with this assignment target; "
            "bind the value to a local first");
}

void AsyncLowering::hoist(ExprPtr& e, std::vector<BlockItem>& pre) {
    if (auto* aw = nodeCast<AwaitExpr>(e.get())) {
        if (hasAwait(aw->operand)) hoist(aw->operand, pre);
        std::string tn = fresh("__aw_t");
        letTmp(pre, tn, awType(aw), e);
        e = ident(tn);
        return;
    }
    if (nodeCast<LambdaExpr>(e.get()) || nodeCast<SizeofExpr>(e.get())) return;   // not evaluated here
    if (auto* b = nodeCast<BinaryExpr>(e.get())) {
        if ((b->op == "&&" || b->op == "||") && hasAwait(b->right)) {
            if (hasAwait(b->left)) hoist(b->left, pre);
            std::string tn = fresh("__sc_t");
            letTmp(pre, tn, "bool", boolLit(false));
            auto setTrue = [&]() { return blockOf({ assign(ident(tn), boolLit(true)) }); };
            std::vector<BlockItem> rp;
            ExprPtr r = b->right;
            hoist(r, rp);
            rp.push_back(StmtPtr(std::make_shared<IfStmt>(r, setTrue())));
            if (b->op == "&&") pre.push_back(StmtPtr(std::make_shared<IfStmt>(b->left, blockOf(rp))));
            else pre.push_back(StmtPtr(std::make_shared<IfStmt>(b->left, setTrue(), blockOf(rp))));
            e = ident(tn);
            return;
        }
        if (isAssignOp(b->op)) {
            bool rAwait = hasAwait(b->right);
            place(b->left, pre, rAwait);
            if (rAwait) hoist(b->right, pre);
            return;
        }
    }
    if (auto* te = nodeCast<TernaryExpr>(e.get()); te && (hasAwait(te->thenExpr) || hasAwait(te->elseExpr))) {
        std::string t = typeOf(e.get());
        if (t.empty())
            throw locError(e.get(), "an 'await' in a '?:' arm needs the expression's type, which is "
                "not known here; use an 'if' statement");
        if (hasAwait(te->condition)) hoist(te->condition, pre);
        std::string tn = fresh("__tn_t");
        letTmp(pre, tn, t, nullptr);
        auto arm = [&](ExprPtr a) {
            std::vector<BlockItem> ap;
            if (hasAwait(a)) hoist(a, ap);
            ap.push_back(assign(ident(tn), a));
            return blockOf(ap);
        };
        pre.push_back(StmtPtr(std::make_shared<IfStmt>(te->condition, arm(te->thenExpr), arm(te->elseExpr))));
        e = ident(tn);
        return;
    }
    if (auto* id = nodeCast<IncDecExpr>(e.get())) { place(id->operand, pre, false); return; }
    if (auto* u = nodeCast<UnaryExpr>(e.get()); u && u->op == "&" && u->opFunc.empty()) {
        place(u->operand, pre, false);
        return;
    }
    // Any other node: its operands in evaluation order. Those before the last one
    // that awaits are held when they have an effect; those after it run after it.
    std::vector<ExprPtr*> ch;
    if (auto* call = nodeCast<CallExpr>(e.get())) {
        if (auto* m = nodeCast<MemberExpr>(call->callee.get())) ch.push_back(&m->base);   // a method's receiver
        else if (!nodeCast<IdentExpr>(call->callee.get())) ch.push_back(&call->callee);
        for (auto& a : call->args) ch.push_back(&a);
    } else {
        astwalk::forEachChildExpr(e.get(), [&](ExprPtr& c) { ch.push_back(&c); });
    }
    int last = -1;
    for (size_t k = 0; k < ch.size(); ++k) if (hasAwait(*ch[k])) last = (int)k;
    for (int k = 0; k < last; ++k) rval(*ch[k], pre);
    if (last >= 0) hoist(*ch[last], pre);
}

std::vector<BlockItem> AsyncLowering::desugarItems(const std::vector<BlockItem>& its) {
    std::vector<BlockItem> out2;
    for (auto& it : its) {
        if (std::holds_alternative<DeclPtr>(it)) {
            auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get());
            if (vd && vd->initializer && hasAwait(vd->initializer)) {
                if (auto* aw = nodeCast<AwaitExpr>(vd->initializer.get())) {
                    if (hasAwait(aw->operand)) hoist(aw->operand, out2);
                } else {
                    hoist(vd->initializer, out2);
                }
            }
            out2.push_back(it);
        } else {
            out2.push_back(StmtPtr(desugarStmt(std::get<StmtPtr>(it))));
        }
    }
    return out2;
}

StmtPtr AsyncLowering::breakUnless(ExprPtr c) {
    return std::make_shared<IfStmt>(std::move(c), blockOf({}), blockOf({ StmtPtr(std::make_shared<BreakStmt>()) }));
}

// `st` after the statements `pre` holds (a block of both), or `st` alone.
StmtPtr AsyncLowering::withPre(std::vector<BlockItem>& pre, StmtPtr st) {
    if (pre.empty()) return st;
    pre.push_back(st);
    return blockOf(pre);
}

StmtPtr AsyncLowering::desugarStmt(const StmtPtr& s) {
    if (!s) return s;
    std::vector<BlockItem> pre;
    if (auto* es = nodeCast<ExprStmt>(s.get())) return desugarExpr(es, s, pre);
    if (auto* rs = nodeCast<ReturnStmt>(s.get())) return desugarReturn(rs, s, pre);
    if (auto* ts = nodeCast<ThrowStmt>(s.get())) return desugarThrow(ts, s, pre);
    if (auto* b = nodeCast<BlockStmt>(s.get()))
        return blockOf(desugarItems(b->items));
    if (auto* i = nodeCast<IfStmt>(s.get())) return desugarIf(i, pre);
    if (auto* w = nodeCast<WhileStmt>(s.get())) return desugarWhile(w);
    // A do/while or for whose condition or step awaits: a `while (true)` whose first
    // pass skips the test (do/while) or the step (for), so `continue` still runs them.
    if (auto* dw = nodeCast<DoWhileStmt>(s.get())) return desugarDoWhile(dw);
    if (auto* f = nodeCast<ForStmt>(s.get())) return desugarFor(f);
    if (auto* fi = nodeCast<ForInStmt>(s.get())) return desugarForIn(fi, pre);
    if (auto* sw = nodeCast<SwitchStmt>(s.get())) return desugarSwitch(sw, pre);
    if (auto* m = nodeCast<MatchStmt>(s.get())) return desugarMatch(m, pre);
    if (auto* t = nodeCast<TryStmt>(s.get())) return desugarTry(t);
    if (auto* d = nodeCast<DeferStmt>(s.get())) return desugarDefer(d, s);
    return s;   // break/continue, asm, ...
}

StmtPtr AsyncLowering::desugarExpr(ExprStmt* es, const StmtPtr& s, std::vector<BlockItem>& pre) {
    if (!hasAwait(es->expr)) return s;
    ExprPtr ex = es->expr;
    if (auto* aw = nodeCast<AwaitExpr>(ex.get())) {      // `await E;`: the value is discarded
        if (hasAwait(aw->operand)) hoist(aw->operand, pre);
        letTmp(pre, fresh("__aw_t"), awType(aw), ex);
        return blockOf(pre);
    }
    hoist(ex, pre);
    if (!nodeCast<IdentExpr>(ex.get())) pre.push_back(StmtPtr(std::make_shared<ExprStmt>(ex)));
    return blockOf(pre);
}

StmtPtr AsyncLowering::desugarReturn(ReturnStmt* rs, const StmtPtr& s, std::vector<BlockItem>& pre) {
    if (!rs->value || !hasAwait(rs->value)) return s;
    ExprPtr v = rs->value;
    hoist(v, pre);
    return withPre(pre, std::make_shared<ReturnStmt>(v));
}

StmtPtr AsyncLowering::desugarThrow(ThrowStmt* ts, const StmtPtr& s, std::vector<BlockItem>& pre) {
    if (!hasAwait(ts->value)) return s;
    ExprPtr v = ts->value;
    hoist(v, pre);
    auto nt = std::make_shared<ThrowStmt>(v);
    nt->line = ts->line; nt->col = ts->col;
    return withPre(pre, nt);
}

StmtPtr AsyncLowering::desugarIf(IfStmt* i, std::vector<BlockItem>& pre) {
    ExprPtr c = i->condition;
    if (hasAwait(c)) hoist(c, pre);
    StmtPtr th = desugarStmt(i->thenBranch);   // in this order on every host compiler
    StmtPtr el = desugarStmt(i->elseBranch);
    return withPre(pre, std::make_shared<IfStmt>(c, th, el));
}

StmtPtr AsyncLowering::desugarWhile(WhileStmt* w) {
    if (!hasAwait(w->condition))
        return std::make_shared<WhileStmt>(w->condition, desugarStmt(w->body));
    // while (C) B  ->  while (true) { <C's awaits>; if (C) {} else break; B }
    ExprPtr c = w->condition;
    std::vector<BlockItem> body;
    hoist(c, body);
    body.push_back(breakUnless(c));
    body.push_back(desugarStmt(w->body));
    return std::make_shared<WhileStmt>(boolLit(true), blockOf(body));
}

StmtPtr AsyncLowering::desugarDoWhile(DoWhileStmt* dw) {
    if (!hasAwait(dw->condition))
        return std::make_shared<DoWhileStmt>(desugarStmt(dw->body), dw->condition);
    std::string first = fresh("__first");
    ExprPtr c = dw->condition;
    std::vector<BlockItem> test;
    hoist(c, test);
    test.push_back(breakUnless(c));
    std::vector<BlockItem> body;
    body.push_back(StmtPtr(std::make_shared<IfStmt>(ident(first),
        blockOf({ assign(ident(first), boolLit(false)) }), blockOf(test))));
    body.push_back(desugarStmt(dw->body));
    std::vector<BlockItem> outer;
    letTmp(outer, first, "bool", boolLit(true));
    outer.push_back(StmtPtr(std::make_shared<WhileStmt>(boolLit(true), blockOf(body))));
    return blockOf(outer);
}

StmtPtr AsyncLowering::desugarFor(ForStmt* f) {
    StmtPtr init = f->init ? desugarStmt(f->init) : nullptr;
    if (!hasAwait(f->condition) && !hasAwait(f->step))
        return std::make_shared<ForStmt>(init, f->condition, f->step, desugarStmt(f->body));
    std::string first = fresh("__first");
    std::vector<BlockItem> step;
    if (f->step) {
        ExprPtr st = f->step;
        if (hasAwait(st)) hoist(st, step);
        if (!nodeCast<IdentExpr>(st.get())) step.push_back(StmtPtr(std::make_shared<ExprStmt>(st)));
    }
    std::vector<BlockItem> body;
    body.push_back(StmtPtr(std::make_shared<IfStmt>(ident(first),
        blockOf({ assign(ident(first), boolLit(false)) }), blockOf(step))));
    if (f->condition) {
        ExprPtr c = f->condition;
        if (hasAwait(c)) hoist(c, body);
        body.push_back(breakUnless(c));
    }
    body.push_back(desugarStmt(f->body));
    std::vector<BlockItem> outer;
    if (init) outer.push_back(init);
    letTmp(outer, first, "bool", boolLit(true));
    outer.push_back(StmtPtr(std::make_shared<WhileStmt>(boolLit(true), blockOf(body))));
    return blockOf(outer);
}

StmtPtr AsyncLowering::desugarForIn(ForInStmt* fi, std::vector<BlockItem>& pre) {
    // The iterable is evaluated once, before the loop (an await in it is hoisted).
    ExprPtr iterable = fi->iterable;
    if (hasAwait(iterable)) hoist(iterable, pre);
    // Desugar `for (x in it)` into a counted C-style for, mirroring codegen
    // but using the type checker's stamp (this pass has no types). Then the
    // ordinary for lowering handles the await + break/continue. The index
    // and element vars are hoisted to frame fields like any other local.
    if (fi->resolvedElemType.empty())
        return withPre(pre, std::make_shared<ForInStmt>(fi->varName, iterable, desugarStmt(fi->body)));
    std::string idxName = astwalk::freshName("__forin_i_" + std::to_string(forinSeq++), used);
    auto idx = [&]() { return ident(idxName); };
    ExprPtr lengthExpr, elemExpr;
    // The iterable is evaluated once (a call is held in a local).
    std::vector<BlockItem> initItems;
    if (!astwalk::isStablePlace(iterable.get()) && !fi->resolvedIterType.empty()) {
        std::string itName = astwalk::freshName(idxName + "_v", used);
        initItems.push_back(DeclPtr(std::make_shared<VarDecl>(itName, fi->resolvedIterType, iterable)));
        iterable = ident(itName);
    }
    if (fi->isArrayIter) {
        bool numeric = !fi->arrayDim.empty();
        for (char c : fi->arrayDim) if (c < '0' || c > '9') numeric = false;
        lengthExpr = numeric ? intlit(std::stoll(fi->arrayDim))
                   : fi->arrayDim.empty() ? ExprPtr(std::make_shared<MemberExpr>(iterable, "len"))   // a slice
                   : ident(fi->arrayDim);
        elemExpr   = std::make_shared<IndexExpr>(iterable, idx());
    } else {
        lengthExpr = std::make_shared<MemberExpr>(iterable, "size");
        elemExpr   = std::make_shared<IndexExpr>(
            std::make_shared<MemberExpr>(iterable, "data"), idx());
    }
    initItems.push_back(DeclPtr(std::make_shared<VarDecl>(idxName, "int", intlit(0))));
    StmtPtr init = std::make_shared<BlockStmt>(initItems);
    ExprPtr cond = binop(idx(), "<", lengthExpr);
    ExprPtr step = binop(idx(), "=", binop(idx(), "+", intlit(1)));
    std::vector<BlockItem> bodyItems;
    bodyItems.push_back(DeclPtr(std::make_shared<VarDecl>(fi->varName, fi->resolvedElemType, elemExpr)));
    bodyItems.push_back(StmtPtr(desugarStmt(fi->body)));
    return withPre(pre, std::make_shared<ForStmt>(init, cond, step,
        std::make_shared<BlockStmt>(bodyItems)));
}

StmtPtr AsyncLowering::desugarSwitch(SwitchStmt* sw, std::vector<BlockItem>& pre) {
    // The subject is evaluated once, before the dispatch. Normalize awaits inside
    // each case. Wrap the case body in a block so its statements run through
    // desugarItems (which let-binds awaits and may introduce VarDecls); all
    // locals are hoisted to frame fields anyway, so the extra block does not
    // change fall-through visibility.
    ExprPtr subj = sw->subject;
    if (hasAwait(subj)) hoist(subj, pre);
    auto out = std::make_shared<SwitchStmt>(subj, std::vector<SwitchStmt::Case>{});
    for (auto& c : sw->cases) {
        SwitchStmt::Case nc; nc.value = c.value;
        nc.stmts.push_back(StmtPtr(std::make_shared<BlockStmt>(desugarItems(c.stmts))));
        out->cases.push_back(nc);
    }
    return withPre(pre, out);
}

StmtPtr AsyncLowering::desugarMatch(MatchStmt* m, std::vector<BlockItem>& pre) {
    ExprPtr subj = m->subject;
    if (hasAwait(subj)) hoist(subj, pre);
    auto out = std::make_shared<MatchStmt>(subj, std::vector<MatchStmt::Arm>{});
    out->enumName = m->enumName;
    out->line = m->line; out->col = m->col;
    for (auto& arm : m->arms) {
        MatchStmt::Arm na = arm;
        na.body = desugarStmt(arm.body);
        out->arms.push_back(na);
    }
    return withPre(pre, out);
}

StmtPtr AsyncLowering::desugarTry(TryStmt* t) {
    std::vector<TryStmt::CatchClause> cs;
    for (auto& c : t->catches) {
        TryStmt::CatchClause nc = c;
        nc.body = desugarStmt(c.body);
        cs.push_back(nc);
    }
    StmtPtr body = desugarStmt(t->body);   // in this order on every host compiler
    StmtPtr fin = desugarStmt(t->finally);
    auto nt = std::make_shared<TryStmt>(body, cs, fin);
    nt->line = t->line; nt->col = t->col;
    return nt;
}

StmtPtr AsyncLowering::desugarDefer(DeferStmt* d, const StmtPtr& s) {
    if (!stmtHasAwait(d->body)) return s;
    auto nd = std::make_shared<DeferStmt>(desugarStmt(d->body), d->isErr);
    nd->line = d->line; nd->col = d->col;
    return nd;
}
