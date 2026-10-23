#include "async_transform.h"
#include "../ast/ast_walk.h"
#include "../template_utils.h"
#include <stdexcept>
#include <cctype>
#include <set>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>

// ── Small AST builders ───────────────────────────────────────────────────────
namespace {

ExprPtr ident(const std::string& n) { return std::make_shared<IdentExpr>(n); }

// Names the lowering synthesizes for the async function being lowered: the frame
// pointer and the frame's bookkeeping fields. Each is chosen (AsyncTransform::run) to
// be distinct from every name spelled in the function, so a user local, parameter, or
// field named `__fr` / `st` / `ret` / `awaiting` cannot collide with them.
struct FrameNames { std::string ptr = "__fr", st = "st", ret = "ret", awaiting = "awaiting"; };
FrameNames frn;
ExprPtr intlit(long long v) {
    return std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, std::to_string(v));
}
// fr.<field>
ExprPtr fr(const std::string& field) {
    return std::make_shared<MemberExpr>(ident(frn.ptr), field);
}
ExprPtr binop(ExprPtr l, const std::string& op, ExprPtr r) {
    return std::make_shared<BinaryExpr>(std::move(l), op, std::move(r));
}
StmtPtr exprStmt(ExprPtr e)  { return std::make_shared<ExprStmt>(std::move(e)); }
StmtPtr assign(ExprPtr lhs, ExprPtr rhs) { return exprStmt(binop(std::move(lhs), "=", std::move(rhs))); }
StmtPtr ret(ExprPtr v) { return std::make_shared<ReturnStmt>(std::move(v)); }


// Rename frame-hoisted locals (params + body locals) to `fr.<name>` member accesses
// throughout an expression, in place. Recurses via the shared child enumeration
// (astwalk::forEachChildExpr), so every expression node (struct literals, alloc_with,
// etc.) is covered.
void rewrite(ExprPtr& e, const std::set<std::string>& vars) {
    if (!e) return;
    if (auto* id = dynamic_cast<IdentExpr*>(e.get())) {
        if (vars.count(id->name)) e = fr(id->name);
        return;
    }
    astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& c) { rewrite(c, vars); });
}

// True if an expression contains an AwaitExpr anywhere (used to require `await`
// be bound in a `let` rather than nested inside a larger expression).
bool hasAwait(const ExprPtr& e) {
    if (!e) return false;
    if (dynamic_cast<AwaitExpr*>(e.get())) return true;
    bool found = false;
    astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& c) { if (hasAwait(c)) found = true; });
    return found;
}

// True if a statement contains an `await` anywhere (decides plain vs state-split).
bool stmtHasAwait(const StmtPtr& s) {
    if (!s) return false;
    if (auto* b = dynamic_cast<BlockStmt*>(s.get())) {
        for (auto& it : b->items) {
            if (std::holds_alternative<DeclPtr>(it)) {
                if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get()))
                    if (vd->initializer && hasAwait(vd->initializer)) return true;
            } else if (stmtHasAwait(std::get<StmtPtr>(it))) return true;
        }
        return false;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s.get()))
        return hasAwait(i->condition) || stmtHasAwait(i->thenBranch) || stmtHasAwait(i->elseBranch);
    if (auto* w = dynamic_cast<WhileStmt*>(s.get()))
        return hasAwait(w->condition) || stmtHasAwait(w->body);
    if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get()))
        return stmtHasAwait(dw->body) || hasAwait(dw->condition);
    if (auto* ds = dynamic_cast<DeferStmt*>(s.get()))
        return stmtHasAwait(ds->body);
    if (auto* ts = dynamic_cast<ThrowStmt*>(s.get()))
        return hasAwait(ts->value);
    if (auto* f = dynamic_cast<ForStmt*>(s.get()))
        return stmtHasAwait(f->init) || hasAwait(f->condition) || hasAwait(f->step) || stmtHasAwait(f->body);
    if (auto* fi = dynamic_cast<ForInStmt*>(s.get()))
        return hasAwait(fi->iterable) || stmtHasAwait(fi->body);
    if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
        if (hasAwait(sw->subject)) return true;
        for (auto& c : sw->cases)
            for (auto& it : c.stmts) {
                if (std::holds_alternative<DeclPtr>(it)) {
                    if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get()))
                        if (vd->initializer && hasAwait(vd->initializer)) return true;
                } else if (stmtHasAwait(std::get<StmtPtr>(it))) return true;
            }
        return false;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
        if (hasAwait(m->subject)) return true;
        for (auto& arm : m->arms) if (stmtHasAwait(arm.body)) return true;
        return false;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
        if (stmtHasAwait(t->body) || stmtHasAwait(t->finally)) return true;
        for (auto& c : t->catches) if (stmtHasAwait(c.body)) return true;
        return false;
    }
    if (auto* rs = dynamic_cast<ReturnStmt*>(s.get())) return hasAwait(rs->value);
    if (auto* es = dynamic_cast<ExprStmt*>(s.get()))  return hasAwait(es->expr);
    return false;
}

// True if a statement contains a labeled `break`/`continue` anywhere. Async bodies are
// always state-split, and a labeled jump can't be threaded through the resume state
// machine (it may target a loop that no longer exists as a real loop), so we reject it.
bool stmtHasLabeledBreak(const StmtPtr& s) {
    if (!s) return false;
    if (auto* bs = dynamic_cast<BreakStmt*>(s.get()))    return !bs->label.empty();
    if (auto* cs = dynamic_cast<ContinueStmt*>(s.get())) return !cs->label.empty();
    if (auto* b = dynamic_cast<BlockStmt*>(s.get())) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it) && stmtHasLabeledBreak(std::get<StmtPtr>(it))) return true;
        return false;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s.get()))
        return stmtHasLabeledBreak(i->thenBranch) || stmtHasLabeledBreak(i->elseBranch);
    if (auto* w = dynamic_cast<WhileStmt*>(s.get()))    return stmtHasLabeledBreak(w->body);
    if (auto* d = dynamic_cast<DoWhileStmt*>(s.get()))  return stmtHasLabeledBreak(d->body);
    if (auto* f = dynamic_cast<ForStmt*>(s.get()))      return stmtHasLabeledBreak(f->body);
    if (auto* fi = dynamic_cast<ForInStmt*>(s.get()))   return stmtHasLabeledBreak(fi->body);
    if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
        for (auto& c : sw->cases)
            for (auto& it : c.stmts)
                if (std::holds_alternative<StmtPtr>(it) && stmtHasLabeledBreak(std::get<StmtPtr>(it))) return true;
        return false;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
        for (auto& arm : m->arms) if (stmtHasLabeledBreak(arm.body)) return true;
        return false;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
        if (stmtHasLabeledBreak(t->body) || stmtHasLabeledBreak(t->finally)) return true;
        for (auto& c : t->catches) if (stmtHasLabeledBreak(c.body)) return true;
        return false;
    }
    return false;
}

// Every AwaitExpr in a function body (lambda bodies excluded: an await belongs to
// the async function it is spelled in).
void collectAwaits(Expr* e, std::vector<AwaitExpr*>& out) {
    if (!e || dynamic_cast<LambdaExpr*>(e)) return;
    if (auto* a = dynamic_cast<AwaitExpr*>(e)) out.push_back(a);
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { collectAwaits(c.get(), out); });
}
void collectAwaits(Stmt* s, std::vector<AwaitExpr*>& out) {
    if (!s) return;
    auto E = [&](const ExprPtr& e) { collectAwaits(e.get(), out); };
    auto S = [&](const StmtPtr& st) { collectAwaits(st.get(), out); };
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& it : b->items) {
            if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
            if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) E(vd->initializer);
        }
    }
    else if (auto* i = dynamic_cast<IfStmt*>(s))        { E(i->condition); S(i->thenBranch); S(i->elseBranch); }
    else if (auto* f = dynamic_cast<ForStmt*>(s))       { S(f->init); E(f->condition); E(f->step); S(f->body); }
    else if (auto* fi = dynamic_cast<ForInStmt*>(s))    { E(fi->iterable); S(fi->body); }
    else if (auto* w = dynamic_cast<WhileStmt*>(s))     { E(w->condition); S(w->body); }
    else if (auto* dw = dynamic_cast<DoWhileStmt*>(s))  { S(dw->body); E(dw->condition); }
    else if (auto* r = dynamic_cast<ReturnStmt*>(s))    { E(r->value); }
    else if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        E(sw->subject);
        for (auto& c : sw->cases) {
            E(c.value);
            for (auto& it : c.stmts) {
                if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
                if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) E(vd->initializer);
            }
        }
    }
    else if (auto* m = dynamic_cast<MatchStmt*>(s))     { E(m->subject); for (auto& a : m->arms) S(a.body); }
    else if (auto* th = dynamic_cast<ThrowStmt*>(s))    { E(th->value); }
    else if (auto* t = dynamic_cast<TryStmt*>(s))       { S(t->body); for (auto& c : t->catches) S(c.body); S(t->finally); }
    else if (auto* d = dynamic_cast<DeferStmt*>(s))     { S(d->body); }
    else if (auto* es = dynamic_cast<ExprStmt*>(s))     { E(es->expr); }
    else if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s)) { E(tj->tid); }
    else if (auto* as = dynamic_cast<AsmStmt*>(s)) {
        for (auto& o : as->outputs) E(o.second);
        for (auto& in : as->inputs) E(in.second);
    }
}

// Whether evaluating `e` may have a side effect: a call (a user operator counts), an
// assignment, `++`/`--`, an await, `?` (it may return), `alloc_with`, `thread_create` or
// `free_closure`. A lambda body is not evaluated there; `sizeof` never evaluates.
bool isAssignOp(const std::string& op) {
    return op == "=" || (op.size() >= 2 && op.back() == '=' && op != "==" && op != "!=" && op != "<=" && op != ">=");
}
bool hasSideEffects(Expr* e) {
    if (!e || dynamic_cast<LambdaExpr*>(e) || dynamic_cast<SizeofExpr*>(e)) return false;
    if (dynamic_cast<CallExpr*>(e) || dynamic_cast<TemplateCallExpr*>(e) || dynamic_cast<AwaitExpr*>(e)
        || dynamic_cast<IncDecExpr*>(e) || dynamic_cast<QuestionExpr*>(e) || dynamic_cast<AllocWithExpr*>(e)
        || dynamic_cast<ThreadCreateExpr*>(e) || dynamic_cast<FreeClosureExpr*>(e)) return true;
    if (auto* b = dynamic_cast<BinaryExpr*>(e); b && (isAssignOp(b->op) || !b->opFunc.empty())) return true;
    if (auto* u = dynamic_cast<UnaryExpr*>(e); u && !u->opFunc.empty()) return true;
    if (auto* ix = dynamic_cast<IndexExpr*>(e); ix && !ix->opFunc.empty()) return true;
    bool found = false;
    astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { if (!found) found = hasSideEffects(c.get()); });
    return found;
}

// Whether a statement contains a `return` (a lambda is an expression: not descended).
bool stmtHasReturn(const StmtPtr& s) {
    if (!s) return false;
    if (dynamic_cast<ReturnStmt*>(s.get())) return true;
    auto items = [](const std::vector<BlockItem>& its) {
        for (auto& it : its)
            if (std::holds_alternative<StmtPtr>(it) && stmtHasReturn(std::get<StmtPtr>(it))) return true;
        return false;
    };
    if (auto* b = dynamic_cast<BlockStmt*>(s.get())) return items(b->items);
    if (auto* i = dynamic_cast<IfStmt*>(s.get())) return stmtHasReturn(i->thenBranch) || stmtHasReturn(i->elseBranch);
    if (auto* w = dynamic_cast<WhileStmt*>(s.get())) return stmtHasReturn(w->body);
    if (auto* d = dynamic_cast<DoWhileStmt*>(s.get())) return stmtHasReturn(d->body);
    if (auto* f = dynamic_cast<ForStmt*>(s.get())) return stmtHasReturn(f->body);
    if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) return stmtHasReturn(fi->body);
    if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
        for (auto& c : sw->cases) if (items(c.stmts)) return true;
        return false;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
        for (auto& a : m->arms) if (stmtHasReturn(a.body)) return true;
        return false;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
        if (stmtHasReturn(t->body) || stmtHasReturn(t->finally)) return true;
        for (auto& c : t->catches) if (stmtHasReturn(c.body)) return true;
        return false;
    }
    return false;
}

// `t` with every subtree spelled `from` replaced by `to`.
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

// A type in a generic function, in terms of the template's type parameters, from its
// value in each checked instance (`recs`: the instance's bindings and the type there): a
// spelling S with substType(S, subs) equal to the type in every instance. Candidates put
// a type parameter in place of each subtree spelled as its argument (every subset of the
// parameters, most first); any consistent one is right for every instance there is. ""
// when none fits.
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

// The awaited type of `aw` in a generic async function, in terms of its type parameters.
std::string genericAwaitType(const AwaitExpr* aw, const std::vector<std::string>& tps) {
    return generalizeType(aw->instanceTypes, tps);
}

// The closure  void() { fr.st = <state>; __<name>_resume(fr); }  used as a waker.
// Captures the frame pointer `fr` by value. Because this AST is synthesized after
// the type checker runs, we populate `captures` ourselves (sema would otherwise).
// A call to the resume function; for a generic async function it is itself generic,
// called with the enclosing template's own type parameters.
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

// Closure locals of an async body that own the env of the lambda bound to them. Every
// local is a frame field, so a lambda bound to one gets a heap env (it must survive a
// suspension). A local qualifies when each of its bindings is a lambda literal (its
// initializer, or a statement `f = <lambda>`), at least one exists, and every other use
// is a call `f(...)` outside any lambda (a lambda that names it would copy the env
// pointer). Names are unique here (ShadowRenamer ran first).
struct OwnedClosureScan {
    std::map<std::string, int> uses, allowed, bindings;
    std::set<std::string> bad;
    void expr(Expr* e, bool inLambda) {
        if (!e) return;
        if (auto* id = dynamic_cast<IdentExpr*>(e)) { uses[id->name]++; return; }
        if (auto* lam = dynamic_cast<LambdaExpr*>(e)) { stmt(lam->body.get(), true); return; }
        if (auto* c = dynamic_cast<CallExpr*>(e); c && !inLambda)
            if (auto* id = dynamic_cast<IdentExpr*>(c->callee.get())) allowed[id->name]++;
        astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { expr(c.get(), inLambda); });
    }
    void decl(VarDecl* vd, bool inLambda, bool forInit) {
        if (!inLambda) {
            if (forInit || (vd->initializer && !dynamic_cast<LambdaExpr*>(vd->initializer.get())))
                bad.insert(vd->name);
            else if (vd->initializer) bindings[vd->name]++;
        }
        expr(vd->initializer.get(), inLambda);
    }
    void items(const std::vector<BlockItem>& its, bool inLambda, bool forInit = false) {
        for (auto& it : its) {
            if (std::holds_alternative<StmtPtr>(it)) { stmt(std::get<StmtPtr>(it).get(), inLambda); continue; }
            if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) decl(vd, inLambda, forInit);
        }
    }
    void stmt(Stmt* s, bool inLambda) {
        if (!s) return;
        auto E = [&](const ExprPtr& e) { expr(e.get(), inLambda); };
        auto S = [&](const StmtPtr& st) { stmt(st.get(), inLambda); };
        if (auto* b = dynamic_cast<BlockStmt*>(s)) items(b->items, inLambda);
        else if (auto* es = dynamic_cast<ExprStmt*>(s)) {
            auto* a = dynamic_cast<BinaryExpr*>(es->expr.get());
            if (a && a->op == "=" && !inLambda && dynamic_cast<LambdaExpr*>(a->right.get()))
                if (auto* id = dynamic_cast<IdentExpr*>(a->left.get())) { allowed[id->name]++; bindings[id->name]++; }
            E(es->expr);
        }
        else if (auto* i = dynamic_cast<IfStmt*>(s)) { E(i->condition); S(i->thenBranch); S(i->elseBranch); }
        else if (auto* f = dynamic_cast<ForStmt*>(s)) {
            if (auto* ib = dynamic_cast<BlockStmt*>(f->init.get())) items(ib->items, inLambda, true);
            else S(f->init);
            E(f->condition); E(f->step); S(f->body);
        }
        else if (auto* fi = dynamic_cast<ForInStmt*>(s))    { E(fi->iterable); S(fi->body); }
        else if (auto* w = dynamic_cast<WhileStmt*>(s))     { E(w->condition); S(w->body); }
        else if (auto* dw = dynamic_cast<DoWhileStmt*>(s))  { S(dw->body); E(dw->condition); }
        else if (auto* r = dynamic_cast<ReturnStmt*>(s))    { E(r->value); }
        else if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
            E(sw->subject);
            for (auto& c : sw->cases) { E(c.value); items(c.stmts, inLambda); }
        }
        else if (auto* m = dynamic_cast<MatchStmt*>(s))     { E(m->subject); for (auto& a : m->arms) S(a.body); }
        else if (auto* th = dynamic_cast<ThrowStmt*>(s))    { E(th->value); }
        else if (auto* t = dynamic_cast<TryStmt*>(s))       { S(t->body); for (auto& c : t->catches) S(c.body); S(t->finally); }
        else if (auto* d = dynamic_cast<DeferStmt*>(s))     { S(d->body); }
        else if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s)) { E(tj->tid); }
        else if (auto* as = dynamic_cast<AsmStmt*>(s)) {
            for (auto& o : as->outputs) E(o.second);
            for (auto& in : as->inputs) E(in.second);
        }
    }
    std::set<std::string> owned() const {
        std::set<std::string> out;
        for (const auto& [n, k] : bindings) {
            auto u = uses.find(n), a = allowed.find(n);
            int nu = u == uses.end() ? 0 : u->second, na = a == allowed.end() ? 0 : a->second;
            if (k > 0 && !bad.count(n) && nu == na) out.insert(n);
        }
        return out;
    }
};

// Before each binding of an owned closure local, free the env it holds (the field is
// zero before the first one, and free(null) is a no-op): a loop rebinding it each pass
// keeps one env alive.
StmtPtr freeClosureOf(const std::string& n) {
    return exprStmt(std::make_shared<FreeClosureExpr>(ident(n)));
}
void freeBeforeRebind(std::vector<BlockItem>& its, const std::set<std::string>& owned);
StmtPtr freeBeforeRebind(const StmtPtr& s, const std::set<std::string>& owned) {
    if (!s) return s;
    auto S = [&](StmtPtr& st) { st = freeBeforeRebind(st, owned); };
    if (auto* b = dynamic_cast<BlockStmt*>(s.get())) freeBeforeRebind(b->items, owned);
    else if (auto* es = dynamic_cast<ExprStmt*>(s.get())) {
        auto* a = dynamic_cast<BinaryExpr*>(es->expr.get());
        auto* id = a && a->op == "=" ? dynamic_cast<IdentExpr*>(a->left.get()) : nullptr;
        if (id && owned.count(id->name) && dynamic_cast<LambdaExpr*>(a->right.get()))
            return std::make_shared<BlockStmt>(std::vector<BlockItem>{ freeClosureOf(id->name), s });
    }
    else if (auto* i = dynamic_cast<IfStmt*>(s.get())) { S(i->thenBranch); S(i->elseBranch); }
    else if (auto* f = dynamic_cast<ForStmt*>(s.get())) S(f->body);
    else if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) S(fi->body);
    else if (auto* w = dynamic_cast<WhileStmt*>(s.get())) S(w->body);
    else if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) S(dw->body);
    else if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) { for (auto& c : sw->cases) freeBeforeRebind(c.stmts, owned); }
    else if (auto* m = dynamic_cast<MatchStmt*>(s.get())) { for (auto& a : m->arms) S(a.body); }
    else if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
        S(t->body);
        for (auto& c : t->catches) S(c.body);
        S(t->finally);
    }
    else if (auto* d = dynamic_cast<DeferStmt*>(s.get())) S(d->body);
    return s;
}
void freeBeforeRebind(std::vector<BlockItem>& its, const std::set<std::string>& owned) {
    std::vector<BlockItem> out;
    for (auto& it : its) {
        if (std::holds_alternative<DeclPtr>(it)) {
            auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get());
            // Every declaration resets the field (to its initializer, or to zero), so the
            // env the field held is freed first (null on the first pass: a no-op).
            if (vd && owned.count(vd->name)) out.push_back(freeClosureOf(vd->name));
        } else it = BlockItem(freeBeforeRebind(std::get<StmtPtr>(it), owned));
        out.push_back(it);
    }
    its = std::move(out);
}

// Every local of an async function becomes a frame field keyed by its name, so two
// declarations of one name (a nested-block `let x` shadowing an outer `x`, or two
// sibling blocks that each declare `x`) would collapse into one field. Before
// hoisting, give every re-declaration a unique name (`x__sN`) and rewrite the
// references in its lexical scope, so each binding gets its own field.
struct ShadowRenamer {
    std::set<std::string> seen;                              // names declared so far
    std::vector<std::map<std::string, std::string>> scopes;  // name -> current spelling
    std::set<std::string>* used = nullptr;                   // every name in the function (renames avoid them)
    // `static` locals of the async function itself (not of a lambda in it): one cell
    // for every call, so they are not frame fields. Each gets a fresh name and is
    // taken out of the body; the lowering declares them at the top of the resume.
    std::vector<DeclPtr> statics;
    int lambdaDepth = 0;

    std::string lookup(const std::string& n) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(n);
            if (f != it->end()) return f->second;
        }
        return n;
    }
    std::string declare(const std::string& n) {
        std::string nn = n;
        if (seen.count(n)) nn = astwalk::freshName(n + "__s", *used);
        seen.insert(n);
        scopes.back()[n] = nn;
        return nn;
    }
    void expr(ExprPtr& e) {
        if (!e) return;
        if (auto* id = dynamic_cast<IdentExpr*>(e.get())) { id->name = lookup(id->name); return; }
        if (auto* lam = dynamic_cast<LambdaExpr*>(e.get())) {
            // A lambda body referring to a renamed local must follow the rename (and so
            // must its capture list); its own params shadow the enclosing names.
            for (auto& cap : lam->captures) cap.first = lookup(cap.first);
            scopes.emplace_back();
            for (const auto& p : lam->params) scopes.back()[p.second] = p.second;
            ++lambdaDepth;
            stmt(lam->body);
            --lambdaDepth;
            scopes.pop_back();
            return;
        }
        astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& c) { expr(c); });
    }
    void items(std::vector<BlockItem>& its) {
        std::vector<BlockItem> kept;
        for (auto& it : its) {
            if (std::holds_alternative<DeclPtr>(it)) {
                if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) {
                    // The initializer still sees the outer binding.
                    expr(vd->initializer);
                    if (vd->isStatic && lambdaDepth == 0) {
                        std::string nn = astwalk::freshName(vd->name + "__st", *used);
                        seen.insert(vd->name);
                        scopes.back()[vd->name] = nn;
                        vd->name = nn;
                        statics.push_back(std::get<DeclPtr>(it));
                        continue;
                    }
                    vd->name = declare(vd->name);
                }
            } else stmt(std::get<StmtPtr>(it));
            kept.push_back(it);
        }
        its = std::move(kept);
    }
    void scoped(StmtPtr& s) { scopes.emplace_back(); stmt(s); scopes.pop_back(); }
    void stmt(StmtPtr& s) {
        if (!s) return;
        if (auto* b = dynamic_cast<BlockStmt*>(s.get())) { scopes.emplace_back(); items(b->items); scopes.pop_back(); }
        else if (auto* i = dynamic_cast<IfStmt*>(s.get())) { expr(i->condition); scoped(i->thenBranch); scoped(i->elseBranch); }
        else if (auto* w = dynamic_cast<WhileStmt*>(s.get())) { expr(w->condition); scoped(w->body); }
        else if (auto* d = dynamic_cast<DoWhileStmt*>(s.get())) { scoped(d->body); expr(d->condition); }
        else if (auto* f = dynamic_cast<ForStmt*>(s.get())) {
            scopes.emplace_back();
            if (auto* ib = dynamic_cast<BlockStmt*>(f->init.get())) items(ib->items);
            else stmt(f->init);
            expr(f->condition); expr(f->step); scoped(f->body);
            scopes.pop_back();
        } else if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) {
            expr(fi->iterable);
            scopes.emplace_back();
            fi->varName = declare(fi->varName);
            scoped(fi->body);
            scopes.pop_back();
        } else if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
            expr(sw->subject);
            scopes.emplace_back();
            for (auto& c : sw->cases) { expr(c.value); items(c.stmts); }
            scopes.pop_back();
        } else if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
            expr(m->subject);
            for (auto& arm : m->arms) {
                scopes.emplace_back();
                // A binding gets its own spelling too, so a hoisted local of the same
                // name is not substituted for it.
                for (auto& bn : arm.bindings) if (bn != "_") bn = declare(bn);
                scoped(arm.body);
                scopes.pop_back();
            }
        } else if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
            scoped(t->body);
            for (auto& c : t->catches) {
                scopes.emplace_back();
                if (!c.type.empty() && !c.name.empty()) c.name = declare(c.name);
                scoped(c.body);
                scopes.pop_back();
            }
            scoped(t->finally);
        } else if (auto* r = dynamic_cast<ReturnStmt*>(s.get())) expr(r->value);
        else if (auto* es = dynamic_cast<ExprStmt*>(s.get())) expr(es->expr);
        else if (auto* th = dynamic_cast<ThrowStmt*>(s.get())) expr(th->value);
        else if (auto* df = dynamic_cast<DeferStmt*>(s.get())) scoped(df->body);
        else if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s.get())) expr(tj->tid);
        else if (auto* as = dynamic_cast<AsmStmt*>(s.get())) {
            for (auto& out : as->outputs) expr(out.second);
            for (auto& in : as->inputs) expr(in.second);
        }
    }
};

} // namespace

// A checked type as a declaration spells it: the `struct:`/`interface:` tags dropped and
// a generic instance's mangled name (`List_int`) written as the template (`List<int>`).
std::string AsyncTransform::declType(const std::string& checked) const {
    std::string out;
    size_t i = 0, n = checked.size();
    auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    while (i < n) {
        if (!isId(checked[i])) { out += checked[i++]; continue; }
        size_t j = i;
        while (j < n && isId(checked[j])) ++j;
        std::string id = checked.substr(i, j - i);
        i = j;
        if ((id == "struct" || id == "interface") && i < n && checked[i] == ':') { ++i; continue; }
        const std::pair<std::string, std::vector<std::string>>* inst = nullptr;
        if (instanceArgs) {
            auto it = instanceArgs->find(id);
            if (it != instanceArgs->end()) inst = &it->second;
        }
        if (inst) {
            out += inst->first + "<";
            for (size_t k = 0; k < inst->second.size(); ++k)
                out += (k ? "," : "") + declType(inst->second[k]);
            out += ">";
        } else {
            out += id;
        }
    }
    return out;
}

void AsyncTransform::run(Program* program) {
    std::vector<DeclPtr> out;
    std::set<std::string> topNames;   // top-level declarations (the frame type / resume fn avoid them)
    for (auto& decl : program->declarations) topNames.insert(decl->name);

    for (auto& decl : program->declarations) {
        auto* fn = dynamic_cast<FunctionDecl*>(decl.get());
        if (!fn || !fn->isAsync) { out.push_back(decl); continue; }

        const std::string name   = fn->name;
        // A generic async function is lowered once, generically: the frame struct, the
        // resume function, and the constructor are templates over its type parameters,
        // instantiated per use like any generic. The awaited types come from the type
        // checker's per-instance records (genericAwaitType). Never instantiated: nothing
        // to lower (and nothing codegen will emit).
        const std::vector<std::string>& tps = fn->typeParams;
        const bool generic = !tps.empty();
        std::map<AwaitExpr*, std::string> genAwTy;
        if (generic) {
            std::vector<AwaitExpr*> aws;
            collectAwaits(fn->body.get(), aws);
            if (aws.empty() || aws.front()->instanceTypes.empty()) { out.push_back(decl); continue; }
            for (auto* aw : aws) {
                std::string t = genericAwaitType(aw, tps);
                if (t.empty())
                    throw std::runtime_error("async function '" + name + "': cannot express an awaited "
                        "type in terms of its type parameters");
                genAwTy[aw] = t;
            }
        }
        auto awType = [&](AwaitExpr* aw) -> std::string {
            return generic ? genAwTy[aw] : declType(aw->resolvedType);
        };
        // Every synthesized name avoids the names already spelled in the function (and
        // the program's top-level names, for the frame type and resume function).
        std::set<std::string> used = topNames;
        astwalk::collectNames(fn->body.get(), used);
        for (const auto& p : fn->params) used.insert(p.second);
        const std::string frameName = astwalk::freshName("__" + name + "_frame", used);
        std::string frameT = frameName;       // the frame type's spelling (`__f_frame<T>` if generic)
        if (generic) {
            frameT += "<";
            for (size_t i = 0; i < tps.size(); ++i) frameT += (i ? "," : "") + tps[i];
            frameT += ">";
        }
        const std::string resumeN = astwalk::freshName("__" + name + "_resume", used);
        frn.ptr = astwalk::freshName("__fr", used);
        frn.st = astwalk::freshName("st", used);
        frn.ret = astwalk::freshName("ret", used);
        frn.awaiting = astwalk::freshName("awaiting", used);
        const std::string T = fn->returnType;                 // declared return type
        const bool isVoid = (T == "void");
        // `async void` uses a 1-byte unit (uint8) as the Future's value type.
        const std::string Tret = isVoid ? "uint8" : T;

        auto* block = dynamic_cast<BlockStmt*>(fn->body.get());
        if (!block)
            throw std::runtime_error("async function '" + name + "': missing body");
        if (stmtHasLabeledBreak(fn->body))
            throw std::runtime_error("async function '" + name + "': labeled 'break'/'continue' "
                "is not supported inside an async function");
        std::vector<DeclPtr> statics;
        {
            ShadowRenamer sr;
            sr.used = &used;
            sr.scopes.emplace_back();
            for (const auto& p : fn->params) { sr.seen.insert(p.second); sr.scopes.back()[p.second] = p.second; }
            sr.items(block->items);
            statics = std::move(sr.statics);
        }

        // A lowering error at `at`: `file:line:col: async function 'f': msg`.
        auto locError = [&](ASTNode* at, const std::string& msg) {
            int ln = (at && at->line) ? at->line : fn->line, cl = (at && at->line) ? at->col : fn->col;
            return std::runtime_error(fn->sourceFile + ":" + std::to_string(ln) + ":" +
                std::to_string(cl) + ": async function '" + name + "': " + msg);
        };
        // Whether a checked type is one a temporary can be declared with.
        auto knownType = [](const std::string& t) {
            return !(t.empty() || t == "unknown" || t == "null" || t == "void");
        };
        // A generic body's type from its per-instance records, generalized over `tps`.
        auto generalized = [&](const AsyncTransform::InstanceTypes& recs) -> std::string {
            AsyncTransform::InstanceTypes dr;
            for (const auto& [subs, t] : recs) {
                if (!knownType(t)) return "";
                std::map<std::string, std::string> ds;
                for (const auto& [k, v] : subs) ds[k] = declType(v);
                dr.push_back({ds, declType(t)});
            }
            return generalizeType(dr, tps);
        };
        // The checked type of `e` ("" when unknown). In a generic function it is spelled
        // with the type parameters.
        auto typeOf = [&](Expr* e) -> std::string {
            if (!e) return "";
            if (generic) {
                if (!instanceExprTypes) return "";
                auto it = instanceExprTypes->find(e);
                return it == instanceExprTypes->end() ? "" : generalized(it->second);
            }
            if (!exprTypes) return "";
            auto it = exprTypes->find(e);
            if (it == exprTypes->end() || !knownType(it->second)) return "";
            return declType(it->second);
        };
        auto boolLit = [](bool v) { return std::make_shared<LiteralExpr>(LiteralExpr::Kind::BOOL, v ? "true" : "false"); };
        auto blockOf = [](std::vector<BlockItem> its) -> StmtPtr { return std::make_shared<BlockStmt>(std::move(its)); };

        // ── Hoist every await into a `let` of its own, in evaluation order, recursing
        //    into control flow, so afterwards every await is the whole initializer of a let:
        //    `return await E;`       -> `let __awN = await E; return __awN;`
        //    `x += g() + await E;`   -> `let __spN = g(); let __awN = await E; x += __spN + __awN;`
        //    An operand evaluated before an await in the same expression is held in a
        //    temporary first when it has a side effect, so effects keep the order of the
        //    synchronous expression; an assignment target's parts (an index, a pointer)
        //    are evaluated once, before the await. `&&`/`||` and `?:` become ifs, so an
        //    await in a conditional operand runs only when that operand is evaluated. A
        //    loop condition or step that awaits is re-evaluated at the top of each pass.
        int tmpN = 0;
        int forinSeq = 0;
        auto fresh = [&](const std::string& base) { return astwalk::freshName(base + std::to_string(tmpN++), used); };
        auto letTmp = [&](std::vector<BlockItem>& pre, const std::string& tn, const std::string& ty, ExprPtr init) {
            pre.push_back(DeclPtr(std::make_shared<VarDecl>(tn, ty, std::move(init))));
        };
        std::function<void(ExprPtr&, std::vector<BlockItem>&)> hoist;
        auto spill = [&](ExprPtr& c, std::vector<BlockItem>& pre) {
            std::string t = typeOf(c.get());
            if (t.empty())
                throw locError(c.get(), "an operand with a side effect is evaluated before an 'await' "
                    "in the same expression and its type is not known here; bind it to a local first");
            std::string tn = fresh("__sp_t");
            letTmp(pre, tn, t, c);
            c = ident(tn);
        };
        auto rval = [&](ExprPtr& c, std::vector<BlockItem>& pre) {
            if (hasAwait(c)) hoist(c, pre);
            else if (hasSideEffects(c.get())) spill(c, pre);
        };
        auto placeShaped = [](Expr* x) {
            if (dynamic_cast<IdentExpr*>(x) || dynamic_cast<MemberExpr*>(x) || dynamic_cast<IndexExpr*>(x)) return true;
            auto* u = dynamic_cast<UnaryExpr*>(x);
            return u && u->op == "*";
        };
        // An assignment target keeps its place; the sub-expressions that compute it are
        // hoisted, or held when a later await follows (`spillParts`).
        std::function<void(ExprPtr&, std::vector<BlockItem>&, bool)> place =
            [&](ExprPtr& e, std::vector<BlockItem>& pre, bool spillParts) {
                auto sub = [&](ExprPtr& c) {
                    if (hasAwait(c)) hoist(c, pre);
                    else if (spillParts && hasSideEffects(c.get())) spill(c, pre);
                };
                if (dynamic_cast<IdentExpr*>(e.get())) return;
                if (auto* m = dynamic_cast<MemberExpr*>(e.get())) {
                    if (placeShaped(m->base.get())) place(m->base, pre, spillParts); else sub(m->base);
                    return;
                }
                if (auto* ix = dynamic_cast<IndexExpr*>(e.get()); ix && ix->opFunc.empty() && !ix->highIndex) {
                    if (placeShaped(ix->base.get())) place(ix->base, pre, spillParts); else sub(ix->base);
                    sub(ix->index);
                    return;
                }
                if (auto* u = dynamic_cast<UnaryExpr*>(e.get()); u && u->op == "*" && u->opFunc.empty()) {
                    sub(u->operand);
                    return;
                }
                if (hasAwait(e) || (spillParts && hasSideEffects(e.get())))
                    throw locError(e.get(), "'await' is not supported with this assignment target; "
                        "bind the value to a local first");
            };
        hoist = [&](ExprPtr& e, std::vector<BlockItem>& pre) {
            if (auto* aw = dynamic_cast<AwaitExpr*>(e.get())) {
                if (hasAwait(aw->operand)) hoist(aw->operand, pre);
                std::string tn = fresh("__aw_t");
                letTmp(pre, tn, awType(aw), e);
                e = ident(tn);
                return;
            }
            if (dynamic_cast<LambdaExpr*>(e.get()) || dynamic_cast<SizeofExpr*>(e.get())) return;   // not evaluated here
            if (auto* b = dynamic_cast<BinaryExpr*>(e.get())) {
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
            if (auto* te = dynamic_cast<TernaryExpr*>(e.get()); te && (hasAwait(te->thenExpr) || hasAwait(te->elseExpr))) {
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
            if (auto* id = dynamic_cast<IncDecExpr*>(e.get())) { place(id->operand, pre, false); return; }
            if (auto* u = dynamic_cast<UnaryExpr*>(e.get()); u && u->op == "&" && u->opFunc.empty()) {
                place(u->operand, pre, false);
                return;
            }
            // Any other node: its operands in evaluation order. Those before the last one
            // that awaits are held when they have an effect; those after it run after it.
            std::vector<ExprPtr*> ch;
            if (auto* call = dynamic_cast<CallExpr*>(e.get())) {
                if (auto* m = dynamic_cast<MemberExpr*>(call->callee.get())) ch.push_back(&m->base);   // a method's receiver
                else if (!dynamic_cast<IdentExpr*>(call->callee.get())) ch.push_back(&call->callee);
                for (auto& a : call->args) ch.push_back(&a);
            } else {
                astwalk::forEachChildExpr(e.get(), [&](ExprPtr& c) { ch.push_back(&c); });
            }
            int last = -1;
            for (size_t k = 0; k < ch.size(); ++k) if (hasAwait(*ch[k])) last = (int)k;
            for (int k = 0; k < last; ++k) rval(*ch[k], pre);
            if (last >= 0) hoist(*ch[last], pre);
        };

        std::function<StmtPtr(const StmtPtr&)> desugarStmt;
        std::function<std::vector<BlockItem>(const std::vector<BlockItem>&)> desugarItems =
            [&](const std::vector<BlockItem>& its) {
                std::vector<BlockItem> out2;
                for (auto& it : its) {
                    if (std::holds_alternative<DeclPtr>(it)) {
                        auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get());
                        if (vd && vd->initializer && hasAwait(vd->initializer)) {
                            if (auto* aw = dynamic_cast<AwaitExpr*>(vd->initializer.get())) {
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
            };
        // `if (c) {} else break;` : leave the enclosing loop when `c` is false.
        auto breakUnless = [&](ExprPtr c) -> StmtPtr {
            return std::make_shared<IfStmt>(std::move(c), blockOf({}), blockOf({ StmtPtr(std::make_shared<BreakStmt>()) }));
        };
        desugarStmt = [&](const StmtPtr& s) -> StmtPtr {
            if (!s) return s;
            std::vector<BlockItem> pre;
            auto withPre = [&](StmtPtr st) -> StmtPtr {
                if (pre.empty()) return st;
                pre.push_back(st);
                return blockOf(pre);
            };
            if (auto* es = dynamic_cast<ExprStmt*>(s.get())) {
                if (!hasAwait(es->expr)) return s;
                ExprPtr ex = es->expr;
                if (auto* aw = dynamic_cast<AwaitExpr*>(ex.get())) {      // `await E;`: the value is discarded
                    if (hasAwait(aw->operand)) hoist(aw->operand, pre);
                    letTmp(pre, fresh("__aw_t"), awType(aw), ex);
                    return blockOf(pre);
                }
                hoist(ex, pre);
                if (!dynamic_cast<IdentExpr*>(ex.get())) pre.push_back(StmtPtr(std::make_shared<ExprStmt>(ex)));
                return blockOf(pre);
            }
            if (auto* rs = dynamic_cast<ReturnStmt*>(s.get())) {
                if (!rs->value || !hasAwait(rs->value)) return s;
                ExprPtr v = rs->value;
                hoist(v, pre);
                return withPre(std::make_shared<ReturnStmt>(v));
            }
            if (auto* ts = dynamic_cast<ThrowStmt*>(s.get())) {
                if (!hasAwait(ts->value)) return s;
                ExprPtr v = ts->value;
                hoist(v, pre);
                auto nt = std::make_shared<ThrowStmt>(v);
                nt->line = ts->line; nt->col = ts->col;
                return withPre(nt);
            }
            if (auto* b = dynamic_cast<BlockStmt*>(s.get()))
                return blockOf(desugarItems(b->items));
            if (auto* i = dynamic_cast<IfStmt*>(s.get())) {
                ExprPtr c = i->condition;
                if (hasAwait(c)) hoist(c, pre);
                return withPre(std::make_shared<IfStmt>(c, desugarStmt(i->thenBranch), desugarStmt(i->elseBranch)));
            }
            if (auto* w = dynamic_cast<WhileStmt*>(s.get())) {
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
            // A do/while or for whose condition or step awaits: a `while (true)` whose first
            // pass skips the test (do/while) or the step (for), so `continue` still runs them.
            if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) {
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
            if (auto* f = dynamic_cast<ForStmt*>(s.get())) {
                StmtPtr init = f->init ? desugarStmt(f->init) : nullptr;
                if (!hasAwait(f->condition) && !hasAwait(f->step))
                    return std::make_shared<ForStmt>(init, f->condition, f->step, desugarStmt(f->body));
                std::string first = fresh("__first");
                std::vector<BlockItem> step;
                if (f->step) {
                    ExprPtr st = f->step;
                    if (hasAwait(st)) hoist(st, step);
                    if (!dynamic_cast<IdentExpr*>(st.get())) step.push_back(StmtPtr(std::make_shared<ExprStmt>(st)));
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
            if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) {
                // The iterable is evaluated once, before the loop (an await in it is hoisted).
                ExprPtr iterable = fi->iterable;
                if (hasAwait(iterable)) hoist(iterable, pre);
                // Desugar `for (x in it)` into a counted C-style for, mirroring codegen
                // but using the type checker's stamp (this pass has no types). Then the
                // ordinary for lowering handles the await + break/continue. The index
                // and element vars are hoisted to frame fields like any other local.
                if (fi->resolvedElemType.empty())
                    return withPre(std::make_shared<ForInStmt>(fi->varName, iterable, desugarStmt(fi->body)));
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
                return withPre(std::make_shared<ForStmt>(init, cond, step,
                    std::make_shared<BlockStmt>(bodyItems)));
            }
            if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
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
                return withPre(out);
            }
            if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
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
                return withPre(out);
            }
            if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
                std::vector<TryStmt::CatchClause> cs;
                for (auto& c : t->catches) {
                    TryStmt::CatchClause nc = c;
                    nc.body = desugarStmt(c.body);
                    cs.push_back(nc);
                }
                auto nt = std::make_shared<TryStmt>(desugarStmt(t->body), cs, desugarStmt(t->finally));
                nt->line = t->line; nt->col = t->col;
                return nt;
            }
            if (auto* d = dynamic_cast<DeferStmt*>(s.get())) {
                if (!stmtHasAwait(d->body)) return s;
                auto nd = std::make_shared<DeferStmt>(desugarStmt(d->body), d->isErr);
                nd->line = d->line; nd->col = d->col;
                return nd;
            }
            return s;   // break/continue, asm, ...
        };
        std::vector<BlockItem> items = desugarItems(block->items);

        // ── Collect awaits (source order, recursing into control flow) and all
        //    locals (hoisted to frame fields). Each await gets an __aw<i>. ─────
        struct AwaitSite { VarDecl* var; AwaitExpr* expr; };
        std::vector<AwaitSite> awaits;
        std::map<AwaitExpr*, int> awIdx;
        std::vector<std::string> awNames;   // frame field of each await's future
        std::set<std::string> vars;
        std::vector<StructDecl::Field> fields;
        fields.push_back({"Future<" + Tret + ">", frn.ret});
        fields.push_back({"int", frn.st});
        fields.push_back({"FutureHdr*", frn.awaiting});
        for (const auto& p : fn->params) { vars.insert(p.second); fields.push_back({p.first, p.second}); }

        std::function<void(const StmtPtr&)> scanS;
        std::function<void(const std::vector<BlockItem>&)> scanB =
            [&](const std::vector<BlockItem>& its) {
                for (auto& it : its) {
                    if (std::holds_alternative<DeclPtr>(it)) {
                        auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get());
                        if (!vd) continue;
                        if (!vars.count(vd->name)) { vars.insert(vd->name); fields.push_back({vd->type, vd->name}); }
                        if (vd->initializer)
                            if (auto* aw = dynamic_cast<AwaitExpr*>(vd->initializer.get())) {
                                awIdx[aw] = (int)awaits.size();
                                awNames.push_back(astwalk::freshName("__aw" + std::to_string(awaits.size()), used));
                                fields.push_back({"*Future<" + awType(aw) + ">", awNames.back()});
                                awaits.push_back({vd, aw});
                            }
                    } else scanS(std::get<StmtPtr>(it));
                }
            };
        scanS = [&](const StmtPtr& s) {
            if (!s) return;
            if (auto* b = dynamic_cast<BlockStmt*>(s.get())) scanB(b->items);
            else if (auto* i = dynamic_cast<IfStmt*>(s.get())) { scanS(i->thenBranch); scanS(i->elseBranch); }
            else if (auto* w = dynamic_cast<WhileStmt*>(s.get())) scanS(w->body);
            else if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) scanS(dw->body);
            else if (auto* ds = dynamic_cast<DeferStmt*>(s.get())) scanS(ds->body);
            else if (auto* f = dynamic_cast<ForStmt*>(s.get())) { scanS(f->init); scanS(f->body); }
            else if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) scanS(fi->body);
            else if (auto* sw = dynamic_cast<SwitchStmt*>(s.get()))
                for (auto& c : sw->cases) scanB(c.stmts);
            else if (auto* m = dynamic_cast<MatchStmt*>(s.get()))
                for (auto& arm : m->arms) scanS(arm.body);
            else if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
                scanS(t->body);
                for (auto& c : t->catches) scanS(c.body);
                scanS(t->finally);
            }
        };
        scanB(items);
        {
            // Every await is now the initializer of a let, except where it can't be placed.
            auto body = blockOf(items);
            std::vector<AwaitExpr*> all;
            collectAwaits(body.get(), all);
            for (auto* aw : all)
                if (!awIdx.count(aw))
                    throw locError(aw, "'await' is not supported here (a 'sizeof' operand, an 'asm' "
                        "input or a 'thread_join'); bind the value to a local first");
        }
        if (awaits.empty())
            throw std::runtime_error("async function '" + name + "': expected at least one `await`");

        // ── Closure locals owning their env (OwnedClosureScan): each binding frees the
        //    env bound before it, and the frame's end frees the last one (excFree).
        std::set<std::string> ownedClosures;
        {
            OwnedClosureScan oc;
            oc.items(items, false);
            ownedClosures = oc.owned();
            if (!ownedClosures.empty()) freeBeforeRebind(items, ownedClosures);
        }

        // ── State graph ──────────────────────────────────────────────────────
        std::vector<std::vector<BlockItem>> states;
        auto goTo = [&](int s, int target) { states[s].push_back(assign(fr(frn.st), intlit(target))); };

        // break/continue inside an await-split loop can't stay literal — they would
        // break/continue the resume function's own `while(true)` dispatch loop. So
        // when a loop is lowered into states we push its exit/continue target states
        // here, and a `break`/`continue` becomes a transition to them.
        std::vector<int> brkTargets, contTargets;
        // `defer` in a state-split block: the block no longer exists as a real scope, so
        // its defer bodies are kept here, one frame per split block (innermost last), and
        // emitted LIFO at each exit: the block's fall-through end, a `return` (every
        // frame), and a `break`/`continue` (the frames above the target's depth, kept
        // parallel to brkTargets/contTargets). errdefer only runs on a `?` error exit,
        // which async lowering does not produce, so it is dropped. A split `try`'s
        // `finally` is a frame of its own (below its body's frames). A body without an
        // await is kept rewritten (rewritePlain); one that awaits is kept as written and
        // lowered into states at each exit (runFrames).
        using Frames = std::vector<std::vector<StmtPtr>>;
        Frames deferFrames;
        // Whether a frame entry in frames[from, to) awaits.
        auto cleanupAwaits = [&](const Frames& frs, size_t from, size_t to) {
            for (size_t f = from; f < to && f < frs.size(); ++f)
                for (auto& b : frs[f]) if (stmtHasAwait(b)) return true;
            return false;
        };
        // Lowering the cleanup a cancelled future runs: nobody can drop the frame again,
        // so its awaits record no drop sites.
        bool cancelCleanup = false;
        std::vector<char> stateCancel;
        std::vector<size_t> brkDeferDepth, contDeferDepth;

        // A `try` split into states: the states of its body form one region, those of its
        // handlers another. Each state of a region is wrapped, at the end, in
        //   try { <state> } catch (T x) { <pending defers>; fr.e = x; fr.st = <handler>; }
        //   <finally, on unwinding only> { <pending defers>; <the try's finally> }
        // (innermost region first), so an exception thrown before or after a suspension
        // reaches the right handler. `startDepth` is the defer depth of the try (its
        // `finally` frame sits there), `base` the depth where the region's frames begin.
        struct CatchInfo { std::string type, var, txtField; int state; };
        struct Region { int parent; size_t startDepth, base; std::vector<CatchInfo> catches; StmtPtr fin; };
        std::vector<Region> regions;
        // Rewrite `exception_text()` calls belonging to a split `catch` clause's body
        // (stopping at nested lambdas and nested `catch` clauses) to a frame field filled
        // in the synthetic catch handler before leaving the landingpad.
        auto rewriteCatchExText = [&](StmtPtr& root) -> std::string {
            std::string tf;
            std::function<void(ExprPtr&)> rwE = [&](ExprPtr& e) {
                if (!e || dynamic_cast<LambdaExpr*>(e.get())) return;
                if (auto* call = dynamic_cast<CallExpr*>(e.get()))
                    if (auto* id = dynamic_cast<IdentExpr*>(call->callee.get()))
                        if (id->name == "exception_text" && call->args.empty()) {
                            if (tf.empty()) {
                                tf = astwalk::freshName("__extxt", used);
                                vars.insert(tf);
                                fields.push_back({"string", tf});
                            }
                            e = ident(tf);
                            return;
                        }
                astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& ch) { rwE(ch); });
            };
            std::function<void(StmtPtr&)> rwS = [&](StmtPtr& s) {
                if (!s) return;
                auto items = [&](std::vector<BlockItem>& its) {
                    for (auto& it : its) {
                        if (std::holds_alternative<StmtPtr>(it)) rwS(std::get<StmtPtr>(it));
                        else if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) rwE(vd->initializer);
                    }
                };
                if (auto* b = dynamic_cast<BlockStmt*>(s.get())) items(b->items);
                else if (auto* i = dynamic_cast<IfStmt*>(s.get())) { rwE(i->condition); rwS(i->thenBranch); rwS(i->elseBranch); }
                else if (auto* w = dynamic_cast<WhileStmt*>(s.get())) { rwE(w->condition); rwS(w->body); }
                else if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) { rwS(dw->body); rwE(dw->condition); }
                else if (auto* f = dynamic_cast<ForStmt*>(s.get())) { rwS(f->init); rwE(f->condition); rwE(f->step); rwS(f->body); }
                else if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) { rwE(fi->iterable); rwS(fi->body); }
                else if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
                    rwE(sw->subject);
                    for (auto& c : sw->cases) { rwE(c.value); items(c.stmts); }
                } else if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
                    rwE(m->subject);
                    for (auto& arm : m->arms) rwS(arm.body);
                } else if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
                    rwS(t->body);
                    rwS(t->finally);
                } else if (auto* d = dynamic_cast<DeferStmt*>(s.get())) rwS(d->body);
                else if (auto* r = dynamic_cast<ReturnStmt*>(s.get())) rwE(r->value);
                else if (auto* th = dynamic_cast<ThrowStmt*>(s.get())) rwE(th->value);
                else if (auto* es = dynamic_cast<ExprStmt*>(s.get())) rwE(es->expr);
                else if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s.get())) rwE(tj->tid);
                else if (auto* as = dynamic_cast<AsmStmt*>(s.get())) {
                    for (auto& o : as->outputs) rwE(o.second);
                    for (auto& in : as->inputs) rwE(in.second);
                }
            };
            rwS(root);
            return tf;
        };
        int curRegion = -1;
        std::vector<int> stateRegion;
        std::vector<std::vector<std::vector<StmtPtr>>> stateFrames;   // defer frames pending in each state
        auto newStateIn = [&](int region, size_t depth) -> int {
            states.push_back({});
            stateRegion.push_back(region);
            stateCancel.push_back(cancelCleanup);
            stateFrames.emplace_back(deferFrames.begin(),
                deferFrames.begin() + (long)std::min(depth, deferFrames.size()));
            return (int)states.size() - 1;
        };
        auto newState = [&]() -> int { return newStateIn(curRegion, deferFrames.size()); };
        // Each await a future can be dropped at, with the defer frames pending there: a
        // cancelled future runs them once, from its on_drop.
        std::vector<std::pair<int, Frames>> dropSites;

        auto enterLoop = [&](int brk, int cont) {
            brkTargets.push_back(brk); contTargets.push_back(cont);
            brkDeferDepth.push_back(deferFrames.size()); contDeferDepth.push_back(deferFrames.size());
        };
        auto leaveLoop = [&]() {
            brkTargets.pop_back(); contTargets.pop_back();
            brkDeferDepth.pop_back(); contDeferDepth.pop_back();
        };
        auto emitDefers = [&](std::vector<BlockItem>& st, size_t downTo) {
            if (cleanupAwaits(deferFrames, downTo, deferFrames.size()))
                throw std::runtime_error("async function '" + name + "': internal error: an awaiting "
                    "cleanup emitted in place");
            for (size_t f = deferFrames.size(); f-- > downTo;)
                for (auto it = deferFrames[f].rbegin(); it != deferFrames[f].rend(); ++it)
                    st.push_back(*it);
        };
        // Does `s` contain a `break`/`continue` that binds to an *enclosing* loop, i.e.
        // one not shadowed by a nested loop/switch? Such a statement can't be emitted
        // verbatim by rewritePlain inside a split loop; it must be lowered structurally so
        // the break/continue reach the transitions above. (A switch captures only `break`.)
        std::function<bool(const StmtPtr&, bool)> escapesIn = [&](const StmtPtr& s, bool contOnly) -> bool {
            if (!s) return false;
            if (dynamic_cast<ContinueStmt*>(s.get())) return true;
            if (dynamic_cast<BreakStmt*>(s.get())) return !contOnly;
            auto items = [&](const std::vector<BlockItem>& its, bool co) {
                for (auto& it : its)
                    if (std::holds_alternative<StmtPtr>(it) && escapesIn(std::get<StmtPtr>(it), co)) return true;
                return false;
            };
            if (auto* b = dynamic_cast<BlockStmt*>(s.get())) return items(b->items, contOnly);
            if (auto* i = dynamic_cast<IfStmt*>(s.get()))
                return escapesIn(i->thenBranch, contOnly) || escapesIn(i->elseBranch, contOnly);
            if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
                for (auto& c : sw->cases) if (items(c.stmts, true)) return true;
                return false;
            }
            if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
                for (auto& a : m->arms) if (escapesIn(a.body, contOnly)) return true;
                return false;
            }
            if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
                if (escapesIn(t->body, contOnly) || escapesIn(t->finally, contOnly)) return true;
                for (auto& c : t->catches) if (escapesIn(c.body, contOnly)) return true;
                return false;
            }
            return false;
        };
        auto loopEscapes = [&](const StmtPtr& s) { return escapesIn(s, false); };

        // Does `s` definitely transfer control away (so nothing after it in the
        // same state is reachable)? A `return`/`throw`, a block that reaches one,
        // or an if whose both branches do. Used so a plain (no-await) terminating
        // statement returns -1 from lowerStmt — otherwise the caller appends a
        // state transition *after* the return, producing a mid-block terminator.
        std::function<bool(const StmtPtr&)> stmtTerminates = [&](const StmtPtr& s) -> bool {
            if (!s) return false;
            if (dynamic_cast<ReturnStmt*>(s.get()) || dynamic_cast<ThrowStmt*>(s.get())) return true;
            if (auto* b = dynamic_cast<BlockStmt*>(s.get())) {
                for (auto& it : b->items)
                    if (std::holds_alternative<StmtPtr>(it) && stmtTerminates(std::get<StmtPtr>(it)))
                        return true;
                return false;
            }
            if (auto* i = dynamic_cast<IfStmt*>(s.get()))
                return i->elseBranch && stmtTerminates(i->thenBranch) && stmtTerminates(i->elseBranch);
            return false;
        };

        // Releases the exceptions captured for an awaiting cleanup that never rethrew them
        // (filled in once every capture is known), run when the frame is finished.
        auto excFree = std::make_shared<BlockStmt>(std::vector<BlockItem>{});
        // Publish the completion (the value is already in fr.ret.value), then return.
        auto publish = [&](std::vector<BlockItem>& st) {
            st.push_back(StmtPtr(excFree));
            ExprPtr swap = std::make_shared<CallExpr>(ident("atomic_swap"),
                std::vector<ExprPtr>{ std::make_shared<UnaryExpr>("&",
                    std::make_shared<MemberExpr>(fr(frn.ret), "state")), intlit(2) });
            std::vector<BlockItem> wk;
            wk.push_back(exprStmt(std::make_shared<CallExpr>(
                std::make_shared<MemberExpr>(fr(frn.ret), "waker"), std::vector<ExprPtr>{})));
            st.push_back(std::make_shared<IfStmt>(binop(swap, "==", intlit(1)),
                std::make_shared<BlockStmt>(wk)));
            st.push_back(ret(nullptr));
        };
        // Complete the future with `v` (already rewritten), then return. Pending defers
        // run after the value is computed, before the completion is published.
        auto completeInto = [&](std::vector<BlockItem>& st, ExprPtr v) {
            st.push_back(assign(std::make_shared<MemberExpr>(fr(frn.ret), "value"), v));
            emitDefers(st, 0);
            publish(st);
        };
        std::function<int(const StmtPtr&, int)> lowerStmt;
        // Run the pending defer frames above `downTo`, innermost first, from state `st`. A
        // frame below a region's base runs in a state of the enclosing region (so an
        // exception a `finally` throws is not caught by its own try's handlers), and a
        // cleanup that awaits is lowered into states of its own, with the cleanups under
        // it still pending. Returns the state where control continues (-1: never).
        auto runFrames = [&](int st, size_t downTo) -> int {
            int reg = stateRegion[st];
            for (size_t f = deferFrames.size(); f-- > downTo;) {
                while (reg != -1 && f < regions[reg].base) {
                    int p = regions[reg].parent;
                    int s2 = newStateIn(p, f);
                    goTo(st, s2);
                    st = s2; reg = p;
                }
                const std::vector<StmtPtr> frame = deferFrames[f];
                for (size_t k = frame.size(); k-- > 0;) {
                    if (!stmtHasAwait(frame[k])) { states[st].push_back(frame[k]); continue; }
                    Frames saved = deferFrames;
                    int savedRegion = curRegion;
                    deferFrames.resize(f + 1);
                    deferFrames[f].resize(k);
                    curRegion = reg;
                    int n = newState();
                    goTo(st, n);
                    int e = lowerStmt(frame[k], n);
                    deferFrames = std::move(saved);
                    curRegion = savedRegion;
                    if (e == -1) return -1;
                    st = e;
                }
            }
            return st;
        };
        // Leave state `st` for the enclosing depth `downTo`: run the pending defer frames
        // above it (runFrames), then jump to `target`, or (`complete`) publish the
        // completion outside every region.
        auto emitExit = [&](int st, size_t downTo, int target, bool complete) {
            st = runFrames(st, downTo);
            if (st == -1) return;
            if (!complete) { goTo(st, target); return; }
            for (int reg = stateRegion[st]; reg != -1; reg = regions[reg].parent) {
                int s2 = newStateIn(regions[reg].parent, downTo);
                goTo(st, s2);
                st = s2;
            }
            publish(states[st]);
        };

        // `let x = E` of a hoisted local: `fr.x = E`. An array literal `{...}` is only a
        // variable initializer, so it fills a temporary that is then copied to the field.
        int initTmp = 0;
        auto initField = [&](VarDecl* vd) -> StmtPtr {
            // `T x;` resets its field to zero at each declaration (a loop iteration), as a
            // local does: a fresh local of the type (zeroed by codegen) is copied in.
            if (!vd->initializer) {
                std::string tn = astwalk::freshName("__zero_t" + std::to_string(initTmp++), used);
                std::vector<BlockItem> blk;
                blk.push_back(DeclPtr(std::make_shared<VarDecl>(tn, vd->type, nullptr)));
                blk.push_back(assign(fr(vd->name), ident(tn)));
                return std::make_shared<BlockStmt>(blk);
            }
            rewrite(vd->initializer, vars);
            if (!dynamic_cast<ArrayLitExpr*>(vd->initializer.get()))
                return assign(fr(vd->name), vd->initializer);
            std::string tn = astwalk::freshName("__init_t" + std::to_string(initTmp++), used);
            std::vector<BlockItem> blk;
            blk.push_back(DeclPtr(std::make_shared<VarDecl>(tn, vd->type, vd->initializer)));
            blk.push_back(assign(fr(vd->name), ident(tn)));
            return std::make_shared<BlockStmt>(blk);
        };

        // Recursively rewrite a NO-await statement for inclusion in a state:
        // let -> fr.x = E; return -> completion; recurse into control-flow bodies.
        std::function<StmtPtr(const StmtPtr&)> rewritePlain = [&](const StmtPtr& s) -> StmtPtr {
            if (auto* b = dynamic_cast<BlockStmt*>(s.get())) {
                std::vector<BlockItem> out2;
                for (auto& it : b->items) {
                    if (std::holds_alternative<DeclPtr>(it)) {
                        auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get());
                        if (vd) out2.push_back(initField(vd));
                    } else out2.push_back(rewritePlain(std::get<StmtPtr>(it)));
                }
                return std::make_shared<BlockStmt>(out2);
            }
            if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
                std::vector<TryStmt::CatchClause> cs;
                for (auto& c : t->catches) {
                    TryStmt::CatchClause nc = c;
                    nc.body = c.body ? rewritePlain(c.body) : nullptr;
                    cs.push_back(nc);
                }
                return std::make_shared<TryStmt>(rewritePlain(t->body), cs,
                    t->finally ? rewritePlain(t->finally) : nullptr);
            }
            if (auto* rs = dynamic_cast<ReturnStmt*>(s.get())) {
                ExprPtr v = rs->value ? rs->value : intlit(0); rewrite(v, vars);
                std::vector<BlockItem> cb; completeInto(cb, v);
                return std::make_shared<BlockStmt>(cb);
            }
            if (auto* es = dynamic_cast<ExprStmt*>(s.get())) { rewrite(es->expr, vars); return s; }
            // An asm operand naming a local reads or writes its frame field.
            if (auto* as = dynamic_cast<AsmStmt*>(s.get())) {
                for (auto& o : as->outputs) rewrite(o.second, vars);
                for (auto& in : as->inputs) rewrite(in.second, vars);
                return s;
            }
            if (auto* i = dynamic_cast<IfStmt*>(s.get())) {
                rewrite(i->condition, vars);
                return std::make_shared<IfStmt>(i->condition, rewritePlain(i->thenBranch),
                    i->elseBranch ? rewritePlain(i->elseBranch) : nullptr);
            }
            if (auto* w = dynamic_cast<WhileStmt*>(s.get())) {
                rewrite(w->condition, vars);
                return std::make_shared<WhileStmt>(w->condition, rewritePlain(w->body));
            }
            if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) {
                rewrite(dw->condition, vars);
                return std::make_shared<DoWhileStmt>(rewritePlain(dw->body), dw->condition);
            }
            if (auto* ds = dynamic_cast<DeferStmt*>(s.get()))
                return std::make_shared<DeferStmt>(rewritePlain(ds->body), ds->isErr);
            if (auto* ts = dynamic_cast<ThrowStmt*>(s.get())) { rewrite(ts->value, vars); return s; }
            if (auto* f = dynamic_cast<ForStmt*>(s.get())) {
                StmtPtr in2 = f->init ? rewritePlain(f->init) : nullptr;
                rewrite(f->condition, vars); rewrite(f->step, vars);
                return std::make_shared<ForStmt>(in2, f->condition, f->step, rewritePlain(f->body));
            }
            if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) {
                rewrite(fi->iterable, vars);
                return std::make_shared<ForInStmt>(fi->varName, fi->iterable, rewritePlain(fi->body));
            }
            if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
                rewrite(sw->subject, vars);
                auto out2 = std::make_shared<SwitchStmt>(sw->subject, std::vector<SwitchStmt::Case>{});
                for (auto& c : sw->cases) {
                    SwitchStmt::Case nc; nc.value = c.value;
                    for (auto& it : c.stmts) {
                        if (std::holds_alternative<DeclPtr>(it)) {
                            auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get());
                            if (vd) nc.stmts.push_back(initField(vd));
                        } else nc.stmts.push_back(rewritePlain(std::get<StmtPtr>(it)));
                    }
                    out2->cases.push_back(nc);
                }
                return out2;
            }
            if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
                rewrite(m->subject, vars);
                auto out2 = std::make_shared<MatchStmt>(m->subject, std::vector<MatchStmt::Arm>{});
                out2->enumName = m->enumName;   // stamped by the type checker
                for (auto& arm : m->arms) {
                    MatchStmt::Arm na; na.variant = arm.variant; na.bindings = arm.bindings;
                    na.body = arm.body ? rewritePlain(arm.body) : nullptr;
                    out2->arms.push_back(na);
                }
                return out2;
            }
            return s;   // break/continue/etc.
        };

        // Lower a statement that CONTAINS an await into the state graph; lowerSeq
        // threads a list. Each returns the state where control continues.
        std::function<int(const std::vector<BlockItem>&, int)> lowerSeq;
        auto lowerItem = [&](BlockItem& it, int cur) -> int {
            if (std::holds_alternative<DeclPtr>(it)) {
                auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get());
                if (!vd) return cur;
                if (!vd->initializer) { states[cur].push_back(initField(vd)); return cur; }
                if (auto* aw = dynamic_cast<AwaitExpr*>(vd->initializer.get())) {
                    int i = awIdx[aw]; std::string awf = awNames[i];
                    const std::string Tp = awType(aw);   // the future's inner type T'
                    ExprPtr callE = aw->operand; rewrite(callE, vars);
                    states[cur].push_back(assign(fr(awf), callE));
                    int next = newState();
                    ExprPtr poll = std::make_shared<TemplateCallExpr>("Future_poll",
                        std::vector<std::string>{Tp},
                        std::vector<ExprPtr>{ fr(awf), resumeWaker(resumeN, next, "*" + frameT, tps) });
                    std::vector<BlockItem> pk;
                    pk.push_back(assign(fr(frn.awaiting), std::make_shared<CastExpr>("*FutureHdr", fr(awf))));
                    pk.push_back(ret(nullptr));
                    states[cur].push_back(std::make_shared<IfStmt>(binop(poll, "==", intlit(0)),
                        std::make_shared<BlockStmt>(pk)));
                    goTo(cur, next);
                    // Dropped while parked here: the pending defers and finally blocks run.
                    bool pending = false;
                    for (auto& f : deferFrames) if (!f.empty()) pending = true;
                    if (pending && !cancelCleanup) dropSites.push_back({cur, deferFrames});
                    // extract into `next`
                    states[next].push_back(assign(fr(frn.awaiting), std::make_shared<CastExpr>("*FutureHdr", intlit(0))));
                    states[next].push_back(assign(fr(vd->name), std::make_shared<MemberExpr>(fr(awf), "value")));
                    states[next].push_back(exprStmt(std::make_shared<FreeClosureExpr>(
                        std::make_shared<MemberExpr>(fr(awf), "waker"))));
                    states[next].push_back(exprStmt(std::make_shared<TemplateCallExpr>("Future_free",
                        std::vector<std::string>{Tp}, std::vector<ExprPtr>{ fr(awf) })));
                    return next;
                }
                if (hasAwait(vd->initializer))
                    throw std::runtime_error("async function '" + name + "': await must be the whole "
                        "initializer of a `let`, not part of a larger expression");
                states[cur].push_back(initField(vd));
                return cur;
            }
            return lowerStmt(std::get<StmtPtr>(it), cur);
        };
        // lowerSeq/lowerStmt return -1 when control definitely terminates (a
        // `return` was emitted on every path) — so callers don't append a
        // terminator or a transition to an unreachable state.
        lowerSeq = [&](const std::vector<BlockItem>& its, int entry) -> int {
            int cur = entry;
            deferFrames.push_back({});           // this split block's defers
            for (auto& it : its) {
                if (cur == -1) break;            // rest is unreachable
                if (std::holds_alternative<StmtPtr>(it))
                    if (auto* ds = dynamic_cast<DeferStmt*>(std::get<StmtPtr>(it).get())) {
                        if (!ds->isErr) {
                            deferFrames.back().push_back(stmtHasAwait(ds->body) ? ds->body : rewritePlain(ds->body));
                            // In a split try a state's pending defers are fixed: begin a new one.
                            if (stateRegion[cur] != -1) { int n = newStateIn(stateRegion[cur], deferFrames.size()); goTo(cur, n); cur = n; }
                        }
                        continue;
                    }
                BlockItem copy = it; cur = lowerItem(copy, cur);
            }
            bool hadDefers = !deferFrames.back().empty();
            if (cur != -1) cur = runFrames(cur, deferFrames.size() - 1);   // fall-through exit
            deferFrames.pop_back();
            if (cur != -1 && hadDefers && stateRegion[cur] != -1) {
                int n = newStateIn(stateRegion[cur], deferFrames.size());
                goTo(cur, n); cur = n;
            }
            return cur;
        };
        lowerStmt = [&](const StmtPtr& s, int cur) -> int {
            // return E  -> complete the future and terminate this path.
            if (auto* rs = dynamic_cast<ReturnStmt*>(s.get())) {
                if (hasAwait(rs->value))
                    throw std::runtime_error("async function '" + name + "': `return await ...` "
                        "must be bound first (`let r = await ...; return r;`)");
                ExprPtr v = rs->value ? rs->value : intlit(0); rewrite(v, vars);
                states[cur].push_back(assign(std::make_shared<MemberExpr>(fr(frn.ret), "value"), v));
                emitExit(cur, 0, 0, true);
                return -1;
            }
            // break/continue bind to the enclosing split loop -> state transition.
            if (dynamic_cast<BreakStmt*>(s.get())) {
                if (brkTargets.empty())
                    throw std::runtime_error("async function '" + name + "': `break` outside a loop");
                emitExit(cur, brkDeferDepth.back(), brkTargets.back(), false);
                return -1;
            }
            if (dynamic_cast<ContinueStmt*>(s.get())) {
                if (contTargets.empty())
                    throw std::runtime_error("async function '" + name + "': `continue` outside a loop");
                emitExit(cur, contDeferDepth.back(), contTargets.back(), false);
                return -1;
            }
            // Emit verbatim only if there's no await AND no break/continue that would
            // escape into the resume loop, and (in a split try, or under a cleanup that
            // awaits) no `return`, whose exit must leave the try's region or run the
            // cleanup in states; otherwise fall through to structural lowering.
            if (!stmtHasAwait(s) && !loopEscapes(s) && !((stateRegion[cur] != -1
                    || cleanupAwaits(deferFrames, 0, deferFrames.size())) && stmtHasReturn(s))) {
                states[cur].push_back(rewritePlain(s));
                return stmtTerminates(s) ? -1 : cur;   // -1: control left this state
            }
            if (auto* i = dynamic_cast<IfStmt*>(s.get())) {
                rewrite(i->condition, vars);
                int thenE = newState(), elseE = newState(), join = newState();
                std::vector<BlockItem> tb; tb.push_back(assign(fr(frn.st), intlit(thenE)));
                std::vector<BlockItem> eb; eb.push_back(assign(fr(frn.st), intlit(elseE)));
                states[cur].push_back(std::make_shared<IfStmt>(i->condition,
                    std::make_shared<BlockStmt>(tb), std::make_shared<BlockStmt>(eb)));
                int te = lowerStmt(i->thenBranch, thenE);
                int ee = i->elseBranch ? lowerStmt(i->elseBranch, elseE) : elseE;
                if (te != -1) goTo(te, join);
                if (ee != -1) goTo(ee, join);
                return (te == -1 && ee == -1) ? -1 : join;
            }
            if (auto* w = dynamic_cast<WhileStmt*>(s.get())) {
                rewrite(w->condition, vars);
                int header = newState(), bodyE = newState(), after = newState();
                goTo(cur, header);
                std::vector<BlockItem> tb; tb.push_back(assign(fr(frn.st), intlit(bodyE)));
                std::vector<BlockItem> eb; eb.push_back(assign(fr(frn.st), intlit(after)));
                states[header].push_back(std::make_shared<IfStmt>(w->condition,
                    std::make_shared<BlockStmt>(tb), std::make_shared<BlockStmt>(eb)));
                enterLoop(after, header);           // break -> after; continue -> re-test
                int be = lowerStmt(w->body, bodyE);
                leaveLoop();
                if (be != -1) goTo(be, header);    // back-edge
                return after;                       // the loop may not execute -> reachable
            }
            if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) {
                // do body while (cond): body first, then cond(test) -> body | after. The
                // test is its own state so `continue` re-tests the condition (C semantics).
                rewrite(dw->condition, vars);
                int bodyE = newState(), test = newState(), after = newState();
                goTo(cur, bodyE);
                std::vector<BlockItem> tb; tb.push_back(assign(fr(frn.st), intlit(bodyE)));
                std::vector<BlockItem> eb; eb.push_back(assign(fr(frn.st), intlit(after)));
                states[test].push_back(std::make_shared<IfStmt>(dw->condition,
                    std::make_shared<BlockStmt>(tb), std::make_shared<BlockStmt>(eb)));
                enterLoop(after, test);             // break -> after; continue -> test
                int be = lowerStmt(dw->body, bodyE);
                leaveLoop();
                if (be != -1) goTo(be, test);
                return after;
            }
            if (auto* f = dynamic_cast<ForStmt*>(s.get())) {
                // for (init; cond; step) body  — init/step run as plain code; the
                // loop is header(cond) -> body -> step -> back-edge -> header.
                if (f->init) {
                    if (stmtHasAwait(f->init)) {            // a range bound that awaits
                        cur = lowerStmt(f->init, cur);
                        if (cur == -1) return -1;
                    } else {
                        states[cur].push_back(rewritePlain(f->init));
                    }
                }
                // header(cond) -> body -> step -> back-edge -> header. The step gets
                // its own state so `continue` runs it before re-testing (C semantics).
                int header = newState(), bodyE = newState(), step = newState(), after = newState();
                goTo(cur, header);
                if (f->condition) {
                    rewrite(f->condition, vars);
                    std::vector<BlockItem> tb; tb.push_back(assign(fr(frn.st), intlit(bodyE)));
                    std::vector<BlockItem> eb; eb.push_back(assign(fr(frn.st), intlit(after)));
                    states[header].push_back(std::make_shared<IfStmt>(f->condition,
                        std::make_shared<BlockStmt>(tb), std::make_shared<BlockStmt>(eb)));
                } else {
                    goTo(header, bodyE);            // no condition -> infinite loop
                }
                if (f->step) { ExprPtr stp = f->step; rewrite(stp, vars); states[step].push_back(exprStmt(stp)); }
                goTo(step, header);                // step -> back-edge
                enterLoop(after, step);            // break -> after; continue -> step
                int be = lowerStmt(f->body, bodyE);
                leaveLoop();
                if (be != -1) goTo(be, step);      // body exit -> step
                return after;
            }
            if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
                // Dispatch on the subject in `cur`, then C-style fall-through between
                // per-case entry states; `break` (pushed below) exits to `join`.
                rewrite(sw->subject, vars);
                int n = (int)sw->cases.size();
                std::vector<int> entry(n);
                for (int k = 0; k < n; ++k) entry[k] = newState();
                int join = newState();
                int dflt = join;                              // no default -> skip to join
                for (int k = 0; k < n; ++k) if (!sw->cases[k].value) { dflt = entry[k]; break; }
                // if (subj==V0) st=entry0; else if (subj==V1) st=entry1; ... else st=dflt
                StmtPtr chain = assign(fr(frn.st), intlit(dflt));
                for (int k = n - 1; k >= 0; --k) {
                    if (!sw->cases[k].value) continue;        // default is the else
                    ExprPtr cv = sw->cases[k].value; rewrite(cv, vars);
                    std::vector<BlockItem> tb; tb.push_back(assign(fr(frn.st), intlit(entry[k])));
                    chain = std::make_shared<IfStmt>(binop(sw->subject, "==", cv),
                        std::make_shared<BlockStmt>(tb),
                        std::make_shared<BlockStmt>(std::vector<BlockItem>{ chain }));
                }
                states[cur].push_back(chain);
                brkTargets.push_back(join);                   // break -> switch end
                brkDeferDepth.push_back(deferFrames.size());
                for (int k = 0; k < n; ++k) {
                    int e = lowerSeq(sw->cases[k].stmts, entry[k]);
                    if (e != -1) goTo(e, (k + 1 < n) ? entry[k + 1] : join);  // fall through
                }
                brkTargets.pop_back(); brkDeferDepth.pop_back();
                return join;
            }
            if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
                // Dispatch on the subject in `cur`: each arm copies its payload bindings to
                // frame fields and selects the arm's entry state; the arms join after.
                rewrite(m->subject, vars);
                int n = (int)m->arms.size();
                std::vector<int> entry(n);
                for (int k = 0; k < n; ++k) entry[k] = newState();
                int join = newState();
                auto disp = std::make_shared<MatchStmt>(m->subject, std::vector<MatchStmt::Arm>{});
                disp->enumName = m->enumName;
                disp->line = m->line; disp->col = m->col;
                for (int k = 0; k < n; ++k) {
                    const auto& arm = m->arms[k];
                    std::vector<std::string> bts = arm.bindingTypes;
                    if (generic) {
                        bts.clear();
                        for (size_t b = 0; b < arm.bindings.size(); ++b) {
                            AsyncTransform::InstanceTypes recs;
                            for (const auto& [subs, ts] : arm.instanceBindingTypes)
                                if (b < ts.size()) recs.push_back({subs, ts[b]});
                            std::string t = recs.size() == arm.instanceBindingTypes.size() ? generalized(recs) : "";
                            if (t.empty()) break;
                            bts.push_back(t);
                        }
                    }
                    if (bts.size() != arm.bindings.size()) {
                        std::vector<AwaitExpr*> aws;
                        collectAwaits(s.get(), aws);
                        throw locError(aws.empty() ? (ASTNode*)m : (ASTNode*)aws.front(), "'await' in a 'match' "
                            "arm that binds a payload is not supported in a generic async function");
                    }
                    MatchStmt::Arm na;
                    na.variant = arm.variant;
                    na.bindingTypes = bts;
                    std::vector<BlockItem> bi;
                    for (size_t b = 0; b < arm.bindings.size(); ++b) {
                        const std::string& bn = arm.bindings[b];
                        if (bn == "_") { na.bindings.push_back(bn); continue; }
                        if (!vars.count(bn)) { vars.insert(bn); fields.push_back({bts[b], bn}); }
                        std::string tn = astwalk::freshName("__mb", used);
                        na.bindings.push_back(tn);
                        bi.push_back(assign(fr(bn), ident(tn)));
                    }
                    bi.push_back(assign(fr(frn.st), intlit(entry[k])));
                    na.body = std::make_shared<BlockStmt>(bi);
                    disp->arms.push_back(na);
                }
                goTo(cur, join);                     // a value no arm names (a classic enum)
                states[cur].push_back(disp);
                for (int k = 0; k < n; ++k) {
                    int e = m->arms[k].body ? lowerStmt(m->arms[k].body, entry[k]) : entry[k];
                    if (e != -1) goTo(e, join);
                }
                return join;
            }
            if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
                // The body's states form one region and the handlers' another (see Region);
                // the `finally` is a defer frame run on every exit, outside both regions.
                if (t->finally && (loopEscapes(t->finally) || stmtHasReturn(t->finally)))
                    throw locError(t, "a 'finally' that leaves by 'break'/'continue' is not "
                        "supported in an async function");
                StmtPtr fin = !t->finally ? nullptr : stmtHasAwait(t->finally) ? t->finally : rewritePlain(t->finally);
                int after = newState();
                size_t startDepth = deferFrames.size();
                if (fin) deferFrames.push_back({fin});
                int parent = curRegion;
                int rb = (int)regions.size();
                regions.push_back({parent, startDepth, deferFrames.size(), {}, fin});
                int rh = (int)regions.size();
                regions.push_back({parent, startDepth, deferFrames.size(), {}, fin});
                curRegion = rh;
                std::vector<int> centry;
                for (auto& c : t->catches) {
                    if (!c.type.empty() && !c.name.empty() && !vars.count(c.name)) { vars.insert(c.name); fields.push_back({c.type, c.name}); }
                    std::string tf = rewriteCatchExText(c.body);
                    centry.push_back(newState());
                    regions[rb].catches.push_back({c.type, c.type.empty() ? "" : c.name, tf, centry.back()});
                }
                curRegion = rb;
                int bodyE = newState();
                goTo(cur, bodyE);
                bool reach = false;
                int be = t->body ? lowerStmt(t->body, bodyE) : bodyE;
                if (be != -1) { emitExit(be, startDepth, after, false); reach = true; }
                curRegion = rh;
                for (size_t k = 0; k < t->catches.size(); ++k) {
                    int ce = t->catches[k].body ? lowerStmt(t->catches[k].body, centry[k]) : centry[k];
                    if (ce != -1) { emitExit(ce, startDepth, after, false); reach = true; }
                }
                curRegion = parent;
                if (fin) deferFrames.pop_back();
                return reach ? after : -1;
            }
            if (auto* b = dynamic_cast<BlockStmt*>(s.get())) return lowerSeq(b->items, cur);
            throw locError(s.get(), "'await' is not supported inside this statement");
        };

        int entry = newState();                 // state 0
        int exit  = lowerSeq(items, entry);
        // Fall off the end of a reachable exit state: void completes with unit 0;
        // a non-void fn that falls off is a user error, but emit a bare return.
        if (exit != -1) {
            if (isVoid) completeInto(states[exit], intlit(0));
            else        states[exit].push_back(ret(nullptr));
        }

        // ── Cancellation: dropped while parked at an await, the future runs the defers
        //    and finally blocks pending there once, from states of their own (the on_drop
        //    closure resumes into them). A cleanup that awaits runs detached: on_drop marks
        //    the frame DETACHED (4), so Future_drop leaves the frame alone, and the last
        //    cleanup state frees it (or, when it ends inside Future_drop, lets that free it).
        struct DropState { int parked, cleanup; bool detached; };
        std::vector<DropState> dropStates;
        auto nullOf = [](const std::string& t) { return std::make_shared<CastExpr>(t, intlit(0)); };
        auto buildDrop = [&](const std::pair<int, Frames>& site) {
            Frames saved = deferFrames;
            int savedRegion = curRegion;
            bool savedCancel = cancelCleanup;
            deferFrames = site.second;
            curRegion = -1;
            cancelCleanup = true;
            bool detached = cleanupAwaits(deferFrames, 0, deferFrames.size());
            int d = newStateIn(-1, 0);
            int e = runFrames(d, 0);
            if (e != -1) {
                states[e].push_back(StmtPtr(excFree));
                if (detached) {
                    // if (atomic_cas(&fr.ret.state, 4, 6)) return;  free((*void)fr); return;
                    ExprPtr cas = std::make_shared<CallExpr>(ident("atomic_cas"), std::vector<ExprPtr>{
                        std::make_shared<UnaryExpr>("&", std::make_shared<MemberExpr>(fr(frn.ret), "state")),
                        intlit(4), intlit(6) });
                    states[e].push_back(std::make_shared<IfStmt>(cas, blockOf({ ret(nullptr) })));
                    states[e].push_back(exprStmt(std::make_shared<CallExpr>(ident("free"),
                        std::vector<ExprPtr>{ std::make_shared<CastExpr>("*void", ident(frn.ptr)) })));
                }
                states[e].push_back(ret(nullptr));
            }
            deferFrames = std::move(saved);
            curRegion = savedRegion;
            cancelCleanup = savedCancel;
            dropStates.push_back({site.first, d, detached});
        };

        // ── Each state of a split try runs inside its regions' handlers (see Region),
        //    innermost first. A handler first runs the defers pending in that state inside
        //    the region, as an exception leaving a block does. When those cleanups await,
        //    a typed handler jumps to states that run them before the handler, and the
        //    unwinding path captures the exception (a catch-all keeping a copy), runs the
        //    cleanups in states of the enclosing region, then throws the copy again.
        const std::string cxName = astwalk::freshName("__cx", used);
        std::map<int, std::string> excFields;   // region -> its captured exception's frame field
        auto excField = [&](int r) -> std::string {
            auto it = excFields.find(r);
            if (it != excFields.end()) return it->second;
            std::string f = astwalk::freshName("__exc" + std::to_string(r), used);
            fields.push_back({"*uint8", f});
            excFields[r] = f;
            return f;
        };
        auto freeExc = [&](const std::string& f) -> StmtPtr {
            return std::make_shared<IfStmt>(binop(fr(f), "!=", nullOf("*uint8")), blockOf({
                exprStmt(std::make_shared<CallExpr>(ident("__cxa_free_exception"),
                    std::vector<ExprPtr>{ std::make_shared<CastExpr>("*void", fr(f)) })),
                assign(fr(f), nullOf("*uint8")) }));
        };
        auto wrapState = [&](size_t si) {
            int r = stateRegion[si];
            if (r == -1) return;
            const Frames frames = stateFrames[si];
            bool savedCancel = cancelCleanup;
            cancelCleanup = stateCancel[si];
            size_t top = frames.size();
            StmtPtr x = std::make_shared<BlockStmt>(states[si]);
            // Lower the cleanups of `frames` above `downTo` from a new state of `region` whose
            // pending frames are the first `depth`; returns {entry, continuation}. The
            // continuation is a state whose pending frames are the first `downTo`, so the
            // cleanups that ran are no longer pending there.
            auto cleanupStates = [&](int region, size_t depth, size_t downTo) -> std::pair<int, int> {
                Frames saved = deferFrames;
                int savedRegion = curRegion;
                deferFrames.assign(frames.begin(), frames.begin() + (long)top);
                curRegion = region;
                int s0 = newStateIn(region, depth);
                int e = runFrames(s0, downTo);
                if (e != -1) {
                    int z = newStateIn(stateRegion[e], downTo);
                    goTo(e, z);
                    e = z;
                }
                deferFrames = std::move(saved);
                curRegion = savedRegion;
                return {s0, e};
            };
            for (; r != -1; r = regions[r].parent) {
                const Region rg = regions[r];
                bool pendAwaits = cleanupAwaits(frames, rg.base, top);
                std::vector<BlockItem> pend;
                if (!pendAwaits)
                    for (size_t f = top; f-- > rg.base;)
                        for (auto it = frames[f].rbegin(); it != frames[f].rend(); ++it) pend.push_back(*it);
                std::vector<TryStmt::CatchClause> cs;
                for (auto& c : rg.catches) {
                    std::vector<BlockItem> hb = pend;
                    if (!c.var.empty()) hb.push_back(assign(fr(c.var), ident(cxName)));
                    if (!c.txtField.empty())
                        hb.push_back(assign(fr(c.txtField),
                            std::make_shared<CallExpr>(ident("exception_text"), std::vector<ExprPtr>{})));
                    int target = c.state;
                    if (pendAwaits) {
                        auto se = cleanupStates(stateRegion[c.state], rg.base, rg.base);
                        if (se.second != -1) goTo(se.second, c.state);
                        target = se.first;
                    }
                    hb.push_back(assign(fr(frn.st), intlit(target)));
                    cs.push_back({c.type, c.type.empty() ? "_" : cxName, std::make_shared<BlockStmt>(hb)});
                }
                StmtPtr ub = nullptr;
                if (cleanupAwaits(frames, rg.startDepth, top)) {
                    std::string ef = excField(r);
                    auto se = cleanupStates(rg.parent, rg.startDepth, rg.startDepth);
                    if (se.second != -1) {
                        // { *uint8 t = fr.ef; fr.ef = null; <throw t again>; }
                        int z = se.second;
                        std::string tn = astwalk::freshName("__rx", used);
                        auto th = std::make_shared<ThrowStmt>(ident(tn));
                        th->rethrowCaptured = true;
                        states[z].push_back(blockOf({ DeclPtr(std::make_shared<VarDecl>(tn, "*uint8", fr(ef))),
                            assign(fr(ef), nullOf("*uint8")), StmtPtr(th) }));
                    }
                    TryStmt::CatchClause cc{"*uint8", cxName, blockOf({ freeExc(ef),
                        assign(fr(ef), ident(cxName)), assign(fr(frn.st), intlit(se.first)) })};
                    cc.captureAll = true;
                    cs.push_back(cc);
                } else {
                    std::vector<BlockItem> ubi = pend;
                    if (rg.fin) ubi.push_back(rg.fin);
                    if (!ubi.empty()) ub = std::make_shared<BlockStmt>(ubi);
                }
                if (!cs.empty() || ub) {
                    auto t = std::make_shared<TryStmt>(x, cs, ub);
                    t->unwindOnly = true;
                    x = t;
                }
                top = rg.startDepth;
            }
            states[si] = { x };
            cancelCleanup = savedCancel;
        };
        // Wrapping and the cancellation states can each produce states the other needs.
        for (size_t wrapped = 0, dsDone = 0;;) {
            for (; dsDone < dropSites.size(); ++dsDone) buildDrop(dropSites[dsDone]);
            if (wrapped == states.size()) break;
            for (; wrapped < states.size(); ++wrapped) wrapState(wrapped);
        }
        for (auto& [r, f] : excFields) excFree->items.push_back(freeExc(f));
        for (const auto& n : ownedClosures)
            excFree->items.push_back(exprStmt(std::make_shared<FreeClosureExpr>(fr(n))));

        // ── Lambdas capturing a frame-hoisted local. The resume function has no local of
        //    that name (it lives in `fr.<name>`, and rewrite() stops at a lambda), so each
        //    statement whose own expressions create such a lambda is wrapped as
        //    `{ T x = fr.x; <stmt> }`: a by-value snapshot at the creation point, which is
        //    exactly what the closure's capture takes.
        std::map<std::string, std::string> fieldTy;
        for (auto& f : fields) fieldTy[f.name] = f.type;
        std::function<void(const ExprPtr&, std::vector<std::string>&)> lambdaCaps =
            [&](const ExprPtr& e, std::vector<std::string>& caps) {
                if (!e) return;
                if (auto* lam = dynamic_cast<LambdaExpr*>(e.get())) {
                    for (auto& c : lam->captures)
                        if (vars.count(c.first) && std::find(caps.begin(), caps.end(), c.first) == caps.end())
                            caps.push_back(c.first);
                    return;
                }
                astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& ch) { lambdaCaps(ch, caps); });
            };
        std::function<StmtPtr(const StmtPtr&)> wrapCaps = [&](const StmtPtr& s) -> StmtPtr {
            if (!s) return s;
            std::vector<std::string> caps;
            if (auto* b = dynamic_cast<BlockStmt*>(s.get())) {
                for (auto& it : b->items)
                    if (std::holds_alternative<StmtPtr>(it)) it = BlockItem(wrapCaps(std::get<StmtPtr>(it)));
                return s;
            }
            if (auto* i = dynamic_cast<IfStmt*>(s.get())) {
                lambdaCaps(i->condition, caps);
                i->thenBranch = wrapCaps(i->thenBranch); i->elseBranch = wrapCaps(i->elseBranch);
            } else if (auto* w = dynamic_cast<WhileStmt*>(s.get())) {
                lambdaCaps(w->condition, caps); w->body = wrapCaps(w->body);
            } else if (auto* dw = dynamic_cast<DoWhileStmt*>(s.get())) {
                lambdaCaps(dw->condition, caps); dw->body = wrapCaps(dw->body);
            } else if (auto* f = dynamic_cast<ForStmt*>(s.get())) {
                f->init = wrapCaps(f->init);
                lambdaCaps(f->condition, caps); lambdaCaps(f->step, caps);
                f->body = wrapCaps(f->body);
            } else if (auto* fi = dynamic_cast<ForInStmt*>(s.get())) {
                lambdaCaps(fi->iterable, caps); fi->body = wrapCaps(fi->body);
            } else if (auto* sw = dynamic_cast<SwitchStmt*>(s.get())) {
                lambdaCaps(sw->subject, caps);
                for (auto& c : sw->cases) {
                    lambdaCaps(c.value, caps);
                    for (auto& it : c.stmts)
                        if (std::holds_alternative<StmtPtr>(it)) it = BlockItem(wrapCaps(std::get<StmtPtr>(it)));
                }
            } else if (auto* m = dynamic_cast<MatchStmt*>(s.get())) {
                lambdaCaps(m->subject, caps);
                for (auto& arm : m->arms) arm.body = wrapCaps(arm.body);
            } else if (auto* ds = dynamic_cast<DeferStmt*>(s.get())) {
                ds->body = wrapCaps(ds->body);
            } else if (auto* t = dynamic_cast<TryStmt*>(s.get())) {
                t->body = wrapCaps(t->body);
                for (auto& c : t->catches) c.body = wrapCaps(c.body);
                t->finally = wrapCaps(t->finally);
            } else if (auto* rs = dynamic_cast<ReturnStmt*>(s.get())) {
                lambdaCaps(rs->value, caps);
            } else if (auto* es = dynamic_cast<ExprStmt*>(s.get())) {
                lambdaCaps(es->expr, caps);
            } else if (auto* ts = dynamic_cast<ThrowStmt*>(s.get())) {
                lambdaCaps(ts->value, caps);
            }
            if (caps.empty()) return s;
            std::vector<BlockItem> blk;
            for (auto& n : caps) blk.push_back(DeclPtr(std::make_shared<VarDecl>(n, fieldTy[n], fr(n))));
            blk.push_back(s);
            return std::make_shared<BlockStmt>(blk);
        };
        for (auto& st : states)
            for (auto& it : st)
                if (std::holds_alternative<StmtPtr>(it)) it = BlockItem(wrapCaps(std::get<StmtPtr>(it)));

        // ── Resume:  while (true) { if(st==0){..} else if(st==1){..} ... else return; }
        StmtPtr chain = ret(nullptr);           // terminal: unknown state -> return
        for (int s = (int)states.size() - 1; s >= 0; --s)
            chain = std::make_shared<IfStmt>(binop(fr(frn.st), "==", intlit(s)),
                std::make_shared<BlockStmt>(states[s]), chain);
        std::vector<BlockItem> loopBody; loopBody.push_back(chain);
        std::vector<BlockItem> resumeBody;
        for (auto& sd : statics) resumeBody.push_back(sd);
        resumeBody.push_back(std::make_shared<WhileStmt>(
            std::make_shared<LiteralExpr>(LiteralExpr::Kind::BOOL, "true"),
            std::make_shared<BlockStmt>(loopBody)));
        auto resumeFn = std::make_shared<FunctionDecl>(
            resumeN, "void",
            std::vector<std::pair<std::string,std::string>>{ {"*" + frameT, frn.ptr} },
            std::make_shared<BlockStmt>(resumeBody));
        resumeFn->typeParams = tps;

        // ── Constructor:  *Future<T> name(params) { ... } ────────────────────
        std::vector<BlockItem> ctor;
        ctor.push_back(std::make_shared<VarDecl>(frn.ptr, "*" + frameT,
            std::make_shared<TemplateCallExpr>("alloc", std::vector<std::string>{frameT},
                std::vector<ExprPtr>{ intlit(1) })));
        ctor.push_back(assign(fr(frn.st), intlit(0)));
        ctor.push_back(assign(fr(frn.awaiting), std::make_shared<CastExpr>("*FutureHdr", intlit(0))));
        ctor.push_back(assign(std::make_shared<MemberExpr>(fr(frn.ret), "state"), intlit(0)));
        // ret is raw-allocated (not via future_new); init waker to a no-op so
        // Future_free/Future_drop can free_closure it safely.
        ctor.push_back(assign(std::make_shared<MemberExpr>(fr(frn.ret), "waker"),
            std::make_shared<LambdaExpr>(
                std::vector<std::pair<std::string,std::string>>{}, "void",
                std::make_shared<BlockStmt>(std::vector<BlockItem>{}))));
        // ret.on_drop: if cancelled while suspended, cascade-drop the awaited
        // future, then free the frame (== free &ret, the embedded first field).
        {
            std::vector<BlockItem> cascade;
            cascade.push_back(exprStmt(std::make_shared<CallExpr>(
                ident("Future_drop"), std::vector<ExprPtr>{ fr(frn.awaiting) })));
            std::vector<BlockItem> dropBody;
            dropBody.push_back(std::make_shared<IfStmt>(
                binop(fr(frn.awaiting), "!=", std::make_shared<CastExpr>("*FutureHdr", intlit(0))),
                std::make_shared<BlockStmt>(cascade)));
            // Then the cleanups pending at the await it is parked at run once; one that
            // awaits runs detached (the frame is marked DETACHED, 4, first).
            // Parked where no cleanup is pending, the frame's end runs from here.
            StmtPtr cleanup = excFree->items.empty() ? nullptr : StmtPtr(excFree);
            for (auto it = dropStates.rbegin(); it != dropStates.rend(); ++it) {
                std::vector<BlockItem> go;
                if (it->detached)
                    go.push_back(exprStmt(std::make_shared<CallExpr>(ident("atomic_store"), std::vector<ExprPtr>{
                        std::make_shared<UnaryExpr>("&", std::make_shared<MemberExpr>(fr(frn.ret), "state")),
                        intlit(4) })));
                go.push_back(assign(fr(frn.st), intlit(it->cleanup)));
                go.push_back(exprStmt(resumeCall(resumeN, tps)));
                cleanup = std::make_shared<IfStmt>(binop(fr(frn.st), "==", intlit(it->parked)),
                    std::make_shared<BlockStmt>(go), cleanup);
            }
            if (cleanup) dropBody.push_back(cleanup);
            // NOTE: do not free the frame here — Future_drop frees it (== free &ret)
            // after this on_drop returns, and frees this closure's env too.
            auto dropLam = std::make_shared<LambdaExpr>(
                std::vector<std::pair<std::string,std::string>>{}, "void",
                std::make_shared<BlockStmt>(dropBody));
            dropLam->captures.push_back({frn.ptr, "*" + frameT});
            ctor.push_back(assign(std::make_shared<MemberExpr>(fr(frn.ret), "on_drop"), dropLam));
        }
        for (const auto& p : fn->params)
            ctor.push_back(assign(fr(p.second), ident(p.second)));
        ctor.push_back(exprStmt(resumeCall(resumeN, tps)));
        ctor.push_back(ret(std::make_shared<UnaryExpr>("&",
            std::make_shared<MemberExpr>(ident(frn.ptr), frn.ret))));
        auto ctorFn = std::make_shared<FunctionDecl>(
            name, "*Future<" + Tret + ">", fn->params, std::make_shared<BlockStmt>(ctor));
        // Preserve the original async fn's per-parameter `escaping` flags. The ctor
        // stores each param into the heap coroutine frame (it outlives the call), so an
        // escaping closure param must keep that flag or codegen would stack-allocate its
        // env at the call site and the frame would hold a dangling pointer (a UAF that
        // surfaced as an intermittent SIGILL calling a garbage closure on Linux).
        ctorFn->paramEscaping = fn->paramEscaping;
        // Every closure param is retained by the frame (the resume calls it after the
        // call returns), so it escapes even when the async fn only calls it.
        ctorFn->paramEscaping.resize(fn->params.size(), false);
        for (size_t i = 0; i < fn->params.size(); ++i)
            if (ty::Type::parse(fn->params[i].first).isFn()) ctorFn->paramEscaping[i] = true;
        ctorFn->typeParams = tps;
        ctorFn->constraints = fn->constraints;
        ctorFn->sourceFile = fn->sourceFile;

        // ── Emit frame struct + resume + constructor in place of the async fn ─
        auto frameDecl = std::make_shared<StructDecl>(frameName, fields);
        frameDecl->typeParams = tps;
        out.push_back(frameDecl);
        out.push_back(resumeFn);
        out.push_back(ctorFn);
    }

    program->declarations = std::move(out);
}
