#pragma once
// ast_walk.h — the single source of truth for "which sub-expressions does each
// expression node have".
//
// Several passes need to recurse over the expressions inside an expression:
// the async transform renames frame variables (`rewrite`), tests for a contained
// `await` (`hasAwait`), and analyses captures (`TemplateCapturePass`). Each used
// to hand-roll its own dynamic_cast chain, so adding an expression node — or
// forgetting one — silently broke a pass (e.g. `rewrite` once skipped
// `StructInitExpr`, so a frame variable used in a struct literal after an await
// did not get renamed). Centralising the child enumeration here means a new
// expression node is handled everywhere by editing one list.
//
// `f` receives each child by mutable reference so callers may replace it (used by
// `rewrite`). Read-only callers simply ignore that.
//
// LambdaExpr is intentionally NOT descended into: a lambda has its own scope, so
// frame-variable renaming and await-search must stop at its boundary. Passes that
// need the body (capture analysis) handle LambdaExpr explicitly before delegating.

#include <functional>
#include <set>
#include <string>
#include <vector>
#include "ast.h"

namespace astwalk {

inline void forEachChildExpr(Expr* e, const std::function<void(ExprPtr&)>& f) {
    if (!e) return;
    if (auto* b = dynamic_cast<BinaryExpr*>(e))        { f(b->left); f(b->right); }
    else if (auto* u = dynamic_cast<UnaryExpr*>(e))    { f(u->operand); }
    else if (auto* id = dynamic_cast<IncDecExpr*>(e))  { f(id->operand); }
    else if (auto* al = dynamic_cast<ArrayLitExpr*>(e)){ for (auto& el : al->elements) f(el); }
    else if (auto* m = dynamic_cast<MemberExpr*>(e))   { f(m->base); }
    else if (auto* ix = dynamic_cast<IndexExpr*>(e))   { f(ix->base); f(ix->index); if (ix->highIndex) f(ix->highIndex); }
    else if (auto* c = dynamic_cast<CastExpr*>(e))     { f(c->expr); }
    else if (auto* q = dynamic_cast<QuestionExpr*>(e)) { f(q->operand); }
    else if (auto* te = dynamic_cast<TernaryExpr*>(e)) { f(te->condition); f(te->thenExpr); f(te->elseExpr); }
    else if (auto* a = dynamic_cast<AwaitExpr*>(e))    { f(a->operand); }
    else if (auto* call = dynamic_cast<CallExpr*>(e))  { f(call->callee); for (auto& arg : call->args) f(arg); }
    else if (auto* tc = dynamic_cast<TemplateCallExpr*>(e)) { for (auto& arg : tc->args) f(arg); }
    else if (auto* si = dynamic_cast<StructInitExpr*>(e))   { for (auto& fi : si->fieldInits) f(fi.second); }
    else if (auto* aw = dynamic_cast<AllocWithExpr*>(e))    { f(aw->allocator); f(aw->count); }
    else if (auto* fc = dynamic_cast<FreeClosureExpr*>(e))  { f(fc->closure); }
    else if (auto* tcr = dynamic_cast<ThreadCreateExpr*>(e)) { f(tcr->worker); }
    else if (auto* sz = dynamic_cast<SizeofExpr*>(e)) { if (sz->operand) f(sz->operand); }
    // Leaves / scope boundaries (no child-expr to descend for these purposes):
    // LiteralExpr, IdentExpr, SizeofExpr of a type, LambdaExpr.
}

// Like forEachChildExpr, but a long left-leaning operator chain (`a + b + c ...`, as
// deep as it is long) costs no recursion: when `e` is a BinaryExpr, a left operand that
// is itself a non-assignment BinaryExpr is not handed to `f`; its own operands are,
// down the left spine by a loop. `f` sees the chain's operands in source order (the
// leftmost first, then each right operand). Only for walkers that do nothing at a
// non-assignment BinaryExpr except recurse into it.
inline void forEachChildExprFlat(Expr* e, const std::function<void(ExprPtr&)>& f) {
    auto* b = dynamic_cast<BinaryExpr*>(e);
    if (!b) { forEachChildExpr(e, f); return; }
    std::vector<BinaryExpr*> spine{b};
    while (auto* l = dynamic_cast<BinaryExpr*>(spine.back()->left.get())) {
        if (l->op == "=") break;
        spine.push_back(l);
    }
    f(spine.back()->left);
    for (size_t i = spine.size(); i-- > 0;) f(spine[i]->right);
}

// Every name spelled in a subtree: identifiers, declared locals/params (lambda bodies
// included), loop/catch/match binders, and member names. A pass that synthesizes a
// local (the async frame pointer, a range bound) picks a name outside this set, so it
// can never capture or shadow a user identifier.
inline void collectNames(Stmt* s, std::set<std::string>& out);
inline void collectNames(Expr* e, std::set<std::string>& out) {
    if (!e) return;
    if (auto* id = dynamic_cast<IdentExpr*>(e)) { out.insert(id->name); return; }
    if (auto* sz = dynamic_cast<SizeofExpr*>(e); sz && !sz->operand) { out.insert(sz->typeName); return; }
    if (auto* m = dynamic_cast<MemberExpr*>(e)) out.insert(m->member);
    if (auto* lam = dynamic_cast<LambdaExpr*>(e)) {
        for (const auto& p : lam->params) out.insert(p.second);
        for (const auto& c : lam->captures) out.insert(c.first);
        collectNames(lam->body.get(), out);
        return;
    }
    forEachChildExprFlat(e, [&](ExprPtr& c) { collectNames(c.get(), out); });
}
inline void collectNames(Stmt* s, std::set<std::string>& out) {
    if (!s) return;
    auto E = [&](const ExprPtr& e) { collectNames(e.get(), out); };
    auto S = [&](const StmtPtr& st) { collectNames(st.get(), out); };
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& it : b->items) {
            if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
            if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) { out.insert(vd->name); E(vd->initializer); }
        }
    }
    else if (auto* i = dynamic_cast<IfStmt*>(s)) {
        // An `else if` chain is walked with a loop.
        for (IfStmt* n = i; n;) {
            E(n->condition); S(n->thenBranch);
            auto* next = dynamic_cast<IfStmt*>(n->elseBranch.get());
            if (!next) S(n->elseBranch);
            n = next;
        }
    }
    else if (auto* f = dynamic_cast<ForStmt*>(s))       { S(f->init); E(f->condition); E(f->step); S(f->body); }
    else if (auto* fi = dynamic_cast<ForInStmt*>(s))    { out.insert(fi->varName); E(fi->iterable); S(fi->body); }
    else if (auto* w = dynamic_cast<WhileStmt*>(s))     { E(w->condition); S(w->body); }
    else if (auto* dw = dynamic_cast<DoWhileStmt*>(s))  { S(dw->body); E(dw->condition); }
    else if (auto* r = dynamic_cast<ReturnStmt*>(s))    { E(r->value); }
    else if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        E(sw->subject);
        for (auto& c : sw->cases) {
            E(c.value);
            for (auto& it : c.stmts) {
                if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
                if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) { out.insert(vd->name); E(vd->initializer); }
            }
        }
    }
    else if (auto* m = dynamic_cast<MatchStmt*>(s))     { E(m->subject); for (auto& a : m->arms) { for (auto& bn : a.bindings) out.insert(bn); S(a.body); } }
    else if (auto* th = dynamic_cast<ThrowStmt*>(s))    { E(th->value); }
    else if (auto* t = dynamic_cast<TryStmt*>(s))       { S(t->body); for (auto& c : t->catches) { if (!c.type.empty()) out.insert(c.name); S(c.body); } S(t->finally); }
    else if (auto* d = dynamic_cast<DeferStmt*>(s))     { S(d->body); }
    else if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s)) { E(tj->tid); }
    else if (auto* a = dynamic_cast<AsmStmt*>(s))       { for (auto& in : a->inputs) E(in.second); }
    else if (auto* es = dynamic_cast<ExprStmt*>(s))     { E(es->expr); }
}

// Names whose address is taken (`&x`) anywhere in a subtree, lambda bodies included.
inline void collectAddressTaken(Stmt* s, std::set<std::string>& out);
inline void collectAddressTaken(Expr* e, std::set<std::string>& out) {
    if (!e) return;
    if (auto* u = dynamic_cast<UnaryExpr*>(e); u && u->op == "&")
        if (auto* id = dynamic_cast<IdentExpr*>(u->operand.get())) out.insert(id->name);
    if (auto* lam = dynamic_cast<LambdaExpr*>(e)) { collectAddressTaken(lam->body.get(), out); return; }
    forEachChildExprFlat(e, [&](ExprPtr& c) { collectAddressTaken(c.get(), out); });
}
inline void collectAddressTaken(Stmt* s, std::set<std::string>& out) {
    if (!s) return;
    auto E = [&](const ExprPtr& e) { collectAddressTaken(e.get(), out); };
    auto S = [&](const StmtPtr& st) { collectAddressTaken(st.get(), out); };
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& it : b->items) {
            if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
            if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) E(vd->initializer);
        }
    }
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
        for (auto& c : sw->cases)
            for (auto& it : c.stmts) {
                if (std::holds_alternative<StmtPtr>(it)) { S(std::get<StmtPtr>(it)); continue; }
                if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) E(vd->initializer);
            }
    }
    else if (auto* m = dynamic_cast<MatchStmt*>(s))     { E(m->subject); for (auto& a : m->arms) S(a.body); }
    else if (auto* th = dynamic_cast<ThrowStmt*>(s))    { E(th->value); }
    else if (auto* t = dynamic_cast<TryStmt*>(s))       { S(t->body); for (auto& c : t->catches) S(c.body); S(t->finally); }
    else if (auto* d = dynamic_cast<DeferStmt*>(s))     { S(d->body); }
    else if (auto* es = dynamic_cast<ExprStmt*>(s))     { E(es->expr); }
}

// Whether `e` reads the name `name` (an identifier or `sizeof(name)`; a lambda body
// counts when it spells the name anywhere). Member names are not reads.
inline bool referencesName(Expr* e, const std::string& name) {
    if (!e) return false;
    if (auto* id = dynamic_cast<IdentExpr*>(e)) return id->name == name;
    if (auto* sz = dynamic_cast<SizeofExpr*>(e); sz && !sz->operand) return sz->typeName == name;
    if (auto* lam = dynamic_cast<LambdaExpr*>(e)) {
        std::set<std::string> names;
        collectNames(lam->body.get(), names);
        return names.count(name) > 0;
    }
    bool found = false;
    forEachChildExprFlat(e, [&](ExprPtr& c) { if (!found) found = referencesName(c.get(), name); });
    return found;
}

// Whether `e` names the same place however often it is evaluated: a variable, and a
// field or constant-index element of one (no call, assignment, operator overload or
// variable index). `for (x in E)` over anything else evaluates E once into a local.
inline bool isStablePlace(Expr* e) {
    if (dynamic_cast<IdentExpr*>(e)) return true;
    if (auto* m = dynamic_cast<MemberExpr*>(e)) return isStablePlace(m->base.get());
    if (auto* ix = dynamic_cast<IndexExpr*>(e))
        return ix->opFunc.empty() && isStablePlace(ix->base.get())
            && dynamic_cast<LiteralExpr*>(ix->index.get())
            && (!ix->highIndex || dynamic_cast<LiteralExpr*>(ix->highIndex.get()));
    if (auto* u = dynamic_cast<UnaryExpr*>(e)) return u->opFunc.empty() && u->op == "&" && isStablePlace(u->operand.get());
    return false;
}

// Whether a subtree contains a `throw` or a `try` (lambda bodies included). A program
// with neither never unwinds through Eskiu frames, so codegen emits no cleanup pads.
inline bool containsEH(Stmt* s);
inline bool containsEH(Expr* e) {
    if (!e) return false;
    if (auto* lam = dynamic_cast<LambdaExpr*>(e)) return containsEH(lam->body.get());
    bool found = false;
    forEachChildExprFlat(e, [&](ExprPtr& c) { if (!found) found = containsEH(c.get()); });
    return found;
}
inline bool containsEH(Stmt* s) {
    if (!s) return false;
    if (dynamic_cast<ThrowStmt*>(s) || dynamic_cast<TryStmt*>(s)) return true;
    auto E = [&](const ExprPtr& e) { return containsEH(e.get()); };
    auto S = [&](const StmtPtr& st) { return containsEH(st.get()); };
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& it : b->items) {
            if (std::holds_alternative<StmtPtr>(it)) { if (S(std::get<StmtPtr>(it))) return true; continue; }
            if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get()); vd && E(vd->initializer)) return true;
        }
        return false;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s)) return E(i->condition) || S(i->thenBranch) || S(i->elseBranch);
    if (auto* f = dynamic_cast<ForStmt*>(s)) return S(f->init) || E(f->condition) || E(f->step) || S(f->body);
    if (auto* fi = dynamic_cast<ForInStmt*>(s)) return E(fi->iterable) || S(fi->body);
    if (auto* w = dynamic_cast<WhileStmt*>(s)) return E(w->condition) || S(w->body);
    if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) return S(dw->body) || E(dw->condition);
    if (auto* r = dynamic_cast<ReturnStmt*>(s)) return E(r->value);
    if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        if (E(sw->subject)) return true;
        for (auto& c : sw->cases)
            for (auto& it : c.stmts) {
                if (std::holds_alternative<StmtPtr>(it)) { if (S(std::get<StmtPtr>(it))) return true; continue; }
                if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get()); vd && E(vd->initializer)) return true;
            }
        return false;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(s)) {
        if (E(m->subject)) return true;
        for (auto& a : m->arms) if (S(a.body)) return true;
        return false;
    }
    if (auto* d = dynamic_cast<DeferStmt*>(s)) return S(d->body);
    if (auto* es = dynamic_cast<ExprStmt*>(s)) return E(es->expr);
    return false;
}

// `base` if it is not in `used`, else the first free `base_1`, `base_2`, ...; the
// chosen name is added to `used`.
inline std::string freshName(const std::string& base, std::set<std::string>& used) {
    std::string n = base;
    for (int k = 1; used.count(n); ++k) n = base + "_" + std::to_string(k);
    used.insert(n);
    return n;
}

}  // namespace astwalk
