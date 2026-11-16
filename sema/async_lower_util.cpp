// async_lower_util.cpp: the small AST builders and walks of the async lowering (see
// async_lower.h).
#include "async_lower.h"

namespace asynclower {

ExprPtr ident(const std::string& n) { return std::make_shared<IdentExpr>(n); }

FrameNames frn;

ExprPtr intlit(long long v) {
    return std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, std::to_string(v));
}

ExprPtr fr(const std::string& field) {
    return std::make_shared<MemberExpr>(ident(frn.ptr), field);
}

ExprPtr binop(ExprPtr l, const std::string& op, ExprPtr r) {
    return std::make_shared<BinaryExpr>(std::move(l), op, std::move(r));
}

StmtPtr exprStmt(ExprPtr e)  { return std::make_shared<ExprStmt>(std::move(e)); }

StmtPtr assign(ExprPtr lhs, ExprPtr rhs) { return exprStmt(binop(std::move(lhs), "=", std::move(rhs))); }

StmtPtr ret(ExprPtr v) { return std::make_shared<ReturnStmt>(std::move(v)); }

void rewrite(ExprPtr& e, const std::set<std::string>& vars) {
    if (!e) return;
    if (auto* id = nodeCast<IdentExpr>(e.get())) {
        if (vars.count(id->name)) e = fr(id->name);
        return;
    }
    astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& c) { rewrite(c, vars); });
}

bool hasAwait(const ExprPtr& e) {
    if (!e) return false;
    if (nodeCast<AwaitExpr>(e.get())) return true;
    bool found = false;
    astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& c) { if (hasAwait(c)) found = true; });
    return found;
}

bool stmtHasAwait(const StmtPtr& s) {
    if (!s) return false;
    if (auto* b = nodeCast<BlockStmt>(s.get())) {
        for (auto& it : b->items) {
            if (std::holds_alternative<DeclPtr>(it)) {
                if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get()))
                    if (vd->initializer && hasAwait(vd->initializer)) return true;
            } else if (stmtHasAwait(std::get<StmtPtr>(it))) return true;
        }
        return false;
    }
    if (auto* i = nodeCast<IfStmt>(s.get()))
        return hasAwait(i->condition) || stmtHasAwait(i->thenBranch) || stmtHasAwait(i->elseBranch);
    if (auto* w = nodeCast<WhileStmt>(s.get()))
        return hasAwait(w->condition) || stmtHasAwait(w->body);
    if (auto* dw = nodeCast<DoWhileStmt>(s.get()))
        return stmtHasAwait(dw->body) || hasAwait(dw->condition);
    if (auto* ds = nodeCast<DeferStmt>(s.get()))
        return stmtHasAwait(ds->body);
    if (auto* ts = nodeCast<ThrowStmt>(s.get()))
        return hasAwait(ts->value);
    if (auto* f = nodeCast<ForStmt>(s.get()))
        return stmtHasAwait(f->init) || hasAwait(f->condition) || hasAwait(f->step) || stmtHasAwait(f->body);
    if (auto* fi = nodeCast<ForInStmt>(s.get()))
        return hasAwait(fi->iterable) || stmtHasAwait(fi->body);
    if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
        if (hasAwait(sw->subject)) return true;
        for (auto& c : sw->cases)
            for (auto& it : c.stmts) {
                if (std::holds_alternative<DeclPtr>(it)) {
                    if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get()))
                        if (vd->initializer && hasAwait(vd->initializer)) return true;
                } else if (stmtHasAwait(std::get<StmtPtr>(it))) return true;
            }
        return false;
    }
    if (auto* m = nodeCast<MatchStmt>(s.get())) {
        if (hasAwait(m->subject)) return true;
        for (auto& arm : m->arms) if (stmtHasAwait(arm.body)) return true;
        return false;
    }
    if (auto* t = nodeCast<TryStmt>(s.get())) {
        if (stmtHasAwait(t->body) || stmtHasAwait(t->finally)) return true;
        for (auto& c : t->catches) if (stmtHasAwait(c.body)) return true;
        return false;
    }
    if (auto* rs = nodeCast<ReturnStmt>(s.get())) return hasAwait(rs->value);
    if (auto* es = nodeCast<ExprStmt>(s.get()))  return hasAwait(es->expr);
    return false;
}

bool stmtHasLabeledBreak(const StmtPtr& s) {
    if (!s) return false;
    if (auto* bs = nodeCast<BreakStmt>(s.get()))    return !bs->label.empty();
    if (auto* cs = nodeCast<ContinueStmt>(s.get())) return !cs->label.empty();
    if (auto* b = nodeCast<BlockStmt>(s.get())) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it) && stmtHasLabeledBreak(std::get<StmtPtr>(it))) return true;
        return false;
    }
    if (auto* i = nodeCast<IfStmt>(s.get()))
        return stmtHasLabeledBreak(i->thenBranch) || stmtHasLabeledBreak(i->elseBranch);
    if (auto* w = nodeCast<WhileStmt>(s.get()))    return stmtHasLabeledBreak(w->body);
    if (auto* d = nodeCast<DoWhileStmt>(s.get()))  return stmtHasLabeledBreak(d->body);
    if (auto* f = nodeCast<ForStmt>(s.get()))      return stmtHasLabeledBreak(f->body);
    if (auto* fi = nodeCast<ForInStmt>(s.get()))   return stmtHasLabeledBreak(fi->body);
    if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
        for (auto& c : sw->cases)
            for (auto& it : c.stmts)
                if (std::holds_alternative<StmtPtr>(it) && stmtHasLabeledBreak(std::get<StmtPtr>(it))) return true;
        return false;
    }
    if (auto* m = nodeCast<MatchStmt>(s.get())) {
        for (auto& arm : m->arms) if (stmtHasLabeledBreak(arm.body)) return true;
        return false;
    }
    if (auto* t = nodeCast<TryStmt>(s.get())) {
        if (stmtHasLabeledBreak(t->body) || stmtHasLabeledBreak(t->finally)) return true;
        for (auto& c : t->catches) if (stmtHasLabeledBreak(c.body)) return true;
        return false;
    }
    return false;
}

void collectAwaits(Expr* e, std::vector<AwaitExpr*>& out) {
    if (!e || nodeCast<LambdaExpr>(e)) return;
    if (auto* a = nodeCast<AwaitExpr>(e)) out.push_back(a);
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { collectAwaits(c.get(), out); });
}

void collectAwaits(Stmt* s, std::vector<AwaitExpr*>& out) {
    if (!s) return;
    auto E = [&](const ExprPtr& e) { collectAwaits(e.get(), out); };
    auto S = [&](const StmtPtr& st) { collectAwaits(st.get(), out); };
    if (auto* b = nodeCast<BlockStmt>(s)) {
        for (auto& it : b->items) {
            if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
            if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) E(vd->initializer);
        }
    }
    else if (auto* i = nodeCast<IfStmt>(s))        { E(i->condition); S(i->thenBranch); S(i->elseBranch); }
    else if (auto* f = nodeCast<ForStmt>(s))       { S(f->init); E(f->condition); E(f->step); S(f->body); }
    else if (auto* fi = nodeCast<ForInStmt>(s))    { E(fi->iterable); S(fi->body); }
    else if (auto* w = nodeCast<WhileStmt>(s))     { E(w->condition); S(w->body); }
    else if (auto* dw = nodeCast<DoWhileStmt>(s))  { S(dw->body); E(dw->condition); }
    else if (auto* r = nodeCast<ReturnStmt>(s))    { E(r->value); }
    else if (auto* sw = nodeCast<SwitchStmt>(s)) {
        E(sw->subject);
        for (auto& c : sw->cases) {
            E(c.value);
            for (auto& it : c.stmts) {
                if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
                if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) E(vd->initializer);
            }
        }
    }
    else if (auto* m = nodeCast<MatchStmt>(s))     { E(m->subject); for (auto& a : m->arms) S(a.body); }
    else if (auto* th = nodeCast<ThrowStmt>(s))    { E(th->value); }
    else if (auto* t = nodeCast<TryStmt>(s))       { S(t->body); for (auto& c : t->catches) S(c.body); S(t->finally); }
    else if (auto* d = nodeCast<DeferStmt>(s))     { S(d->body); }
    else if (auto* es = nodeCast<ExprStmt>(s))     { E(es->expr); }
    else if (auto* tj = nodeCast<ThreadJoinStmt>(s)) { E(tj->tid); }
    else if (auto* as = nodeCast<AsmStmt>(s)) {
        for (auto& o : as->outputs) E(o.second);
        for (auto& in : as->inputs) E(in.second);
    }
}

bool isAssignOp(const std::string& op) {
    return op == "=" || (op.size() >= 2 && op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=");
}

bool hasSideEffects(Expr* e) {
    if (!e || nodeCast<LambdaExpr>(e) || nodeCast<SizeofExpr>(e)) return false;
    if (nodeCast<CallExpr>(e) || nodeCast<TemplateCallExpr>(e) || nodeCast<AwaitExpr>(e)
        || nodeCast<IncDecExpr>(e) || nodeCast<QuestionExpr>(e) || nodeCast<AllocWithExpr>(e)
        || nodeCast<ThreadCreateExpr>(e) || nodeCast<FreeClosureExpr>(e)) return true;
    if (auto* b = nodeCast<BinaryExpr>(e); b && (isAssignOp(b->op) || !b->opFunc.empty())) return true;
    if (auto* u = nodeCast<UnaryExpr>(e); u && !u->opFunc.empty()) return true;
    if (auto* ix = nodeCast<IndexExpr>(e); ix && !ix->opFunc.empty()) return true;
    bool found = false;
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { if (!found) found = hasSideEffects(c.get()); });
    return found;
}

bool stmtHasReturn(const StmtPtr& s) {
    if (!s) return false;
    if (nodeCast<ReturnStmt>(s.get())) return true;
    auto items = [](const std::vector<BlockItem>& its) {
        for (auto& it : its)
            if (std::holds_alternative<StmtPtr>(it) && stmtHasReturn(std::get<StmtPtr>(it))) return true;
        return false;
    };
    if (auto* b = nodeCast<BlockStmt>(s.get())) return items(b->items);
    if (auto* i = nodeCast<IfStmt>(s.get())) return stmtHasReturn(i->thenBranch) || stmtHasReturn(i->elseBranch);
    if (auto* w = nodeCast<WhileStmt>(s.get())) return stmtHasReturn(w->body);
    if (auto* d = nodeCast<DoWhileStmt>(s.get())) return stmtHasReturn(d->body);
    if (auto* f = nodeCast<ForStmt>(s.get())) return stmtHasReturn(f->body);
    if (auto* fi = nodeCast<ForInStmt>(s.get())) return stmtHasReturn(fi->body);
    if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
        for (auto& c : sw->cases) if (items(c.stmts)) return true;
        return false;
    }
    if (auto* m = nodeCast<MatchStmt>(s.get())) {
        for (auto& a : m->arms) if (stmtHasReturn(a.body)) return true;
        return false;
    }
    if (auto* t = nodeCast<TryStmt>(s.get())) {
        if (stmtHasReturn(t->body) || stmtHasReturn(t->finally)) return true;
        for (auto& c : t->catches) if (stmtHasReturn(c.body)) return true;
        return false;
    }
    return false;
}

ty::Type replaceSubtree(const ty::Type& t, const std::string& from, const std::string& to) {
    if (t.str() == from) return ty::Type::parse(to);
    ty::Type r = t;
    auto in = [&](std::shared_ptr<ty::Type>& p) {
        if (p) p = std::make_shared<ty::Type>(replaceSubtree(*p, from, to));
    };
    in(r.pointee); in(r.elem); in(r.ret);
    for (auto& a : r.args)   a = replaceSubtree(a, from, to);
    for (auto& a : r.params) a = replaceSubtree(a, from, to);
    return r;
}

std::string generalizeType(const AsyncTransform::InstanceTypes& recs, const std::vector<std::string>& tps) {
    if (recs.empty()) return "";
    auto canon = [](const std::string& s) { return ty::Type::parse(s).str(); };
    const auto& first = recs.front();
    size_t n = tps.size();
    std::vector<unsigned> masks;
    for (unsigned m = 0; m < (1u << n); ++m) masks.push_back(m);
    std::stable_sort(masks.begin(), masks.end(), [](unsigned a, unsigned b) {
        return __builtin_popcount(a) > __builtin_popcount(b);
    });
    for (unsigned m : masks) {
        ty::Type c = ty::Type::parse(first.second);
        for (size_t i = 0; i < n; ++i) {
            auto it = first.first.find(tps[i]);
            if ((m >> i & 1) && it != first.first.end()) c = replaceSubtree(c, canon(it->second), tps[i]);
        }
        std::string cand = c.str();
        bool ok = true;
        for (const auto& inst : recs)
            if (canon(substType(cand, inst.first)) != canon(inst.second)) { ok = false; break; }
        if (ok) return cand;
    }
    return "";
}

std::string genericAwaitType(const AwaitExpr* aw, const std::vector<std::string>& tps) {
    return generalizeType(aw->instanceTypes, tps);
}

ExprPtr resumeCall(const std::string& resumeName, const std::vector<std::string>& tps) {
    std::vector<ExprPtr> args{ ident(frn.ptr) };
    if (tps.empty()) return std::make_shared<CallExpr>(ident(resumeName), args);
    return std::make_shared<TemplateCallExpr>(resumeName, tps, args);
}

ExprPtr resumeWaker(const std::string& resumeName, int state, const std::string& framePtrTy,
                    const std::vector<std::string>& tps) {
    std::vector<BlockItem> body;
    body.push_back(assign(fr(frn.st), intlit(state)));
    body.push_back(exprStmt(resumeCall(resumeName, tps)));
    auto blk = std::make_shared<BlockStmt>(body);
    auto lam = std::make_shared<LambdaExpr>(
        std::vector<std::pair<std::string,std::string>>{}, "void", blk);
    lam->captures.push_back({frn.ptr, framePtrTy});
    return lam;
}

StmtPtr freeClosureOf(const std::string& n) {
    return exprStmt(std::make_shared<FreeClosureExpr>(ident(n)));
}

StmtPtr freeBeforeRebind(const StmtPtr& s, const std::set<std::string>& owned) {
    if (!s) return s;
    auto S = [&](StmtPtr& st) { st = freeBeforeRebind(st, owned); };
    if (auto* b = nodeCast<BlockStmt>(s.get())) freeBeforeRebind(b->items, owned);
    else if (auto* es = nodeCast<ExprStmt>(s.get())) {
        auto* a = nodeCast<BinaryExpr>(es->expr.get());
        auto* id = a && a->op == "=" ? nodeCast<IdentExpr>(a->left.get()) : nullptr;
        if (id && owned.count(id->name) && nodeCast<LambdaExpr>(a->right.get()))
            return std::make_shared<BlockStmt>(std::vector<BlockItem>{ freeClosureOf(id->name), s });
    }
    else if (auto* i = nodeCast<IfStmt>(s.get())) { S(i->thenBranch); S(i->elseBranch); }
    else if (auto* f = nodeCast<ForStmt>(s.get())) S(f->body);
    else if (auto* fi = nodeCast<ForInStmt>(s.get())) S(fi->body);
    else if (auto* w = nodeCast<WhileStmt>(s.get())) S(w->body);
    else if (auto* dw = nodeCast<DoWhileStmt>(s.get())) S(dw->body);
    else if (auto* sw = nodeCast<SwitchStmt>(s.get())) { for (auto& c : sw->cases) freeBeforeRebind(c.stmts, owned); }
    else if (auto* m = nodeCast<MatchStmt>(s.get())) { for (auto& a : m->arms) S(a.body); }
    else if (auto* t = nodeCast<TryStmt>(s.get())) {
        S(t->body);
        for (auto& c : t->catches) S(c.body);
        S(t->finally);
    }
    else if (auto* d = nodeCast<DeferStmt>(s.get())) S(d->body);
    return s;
}

void freeBeforeRebind(std::vector<BlockItem>& its, const std::set<std::string>& owned) {
    std::vector<BlockItem> out;
    for (auto& it : its) {
        if (std::holds_alternative<DeclPtr>(it)) {
            auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get());
            // Every declaration resets the field (to its initializer, or to zero), so the
            // env the field held is freed first (null on the first pass: a no-op).
            if (vd && owned.count(vd->name)) out.push_back(freeClosureOf(vd->name));
        } else it = BlockItem(freeBeforeRebind(std::get<StmtPtr>(it), owned));
        out.push_back(it);
    }
    its = std::move(out);
}

} // namespace asynclower
