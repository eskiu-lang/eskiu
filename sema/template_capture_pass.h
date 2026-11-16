// template_capture_pass.h: the capture pass for template function bodies, shared by
// typecheck_decl.cpp and typecheck_decl_data.cpp (internal to the type checker).
#pragma once
#include "type_checker.h"
#include <functional>
#include <set>
#include "../ast/ast_walk.h"

namespace {
// Type-independent capture analysis for TEMPLATE function bodies.
//
// The normal capture detection (visit(IdentExpr)/visit(LambdaExpr)) runs as part
// of type-checking, which skips template bodies (their expressions mention the
// unresolved type parameter T). So a lambda inside a generic function would get
// an empty capture list and miscompile ("Referring to an argument in another
// function"). This pass fills that gap: a pure lexical scope + free-variable walk
// (no types resolved, no diagnostics) that records, for each lambda, the
// enclosing-scope names it references — with their SOURCE-form types (T intact).
// Codegen's getTypeFromString already substitutes typeParamOverride, so a capture
// typed `*Future<T>` becomes `*Future<int>` automatically per instantiation.
//
// Purely additive: it only writes LambdaExpr::captures, which were previously
// empty for template-body lambdas, so it cannot affect non-template code.
struct TemplateCapturePass {
    std::vector<std::map<std::string, std::string>> scopes;  // name -> source type
    struct Active { int boundary; std::map<std::string, std::string> caps; };
    std::vector<Active> lambdas;

    void define(const std::string& name, const std::string& srcType) {
        if (!scopes.empty()) scopes.back()[name] = srcType;
    }
    int defIndex(const std::string& name) {
        for (int i = (int)scopes.size() - 1; i >= 0; --i)
            if (scopes[i].count(name)) return i;
        return -1;   // not a tracked local -> a global or top-level fn (not captured)
    }
    void run(FunctionDecl* fn, const std::string& selfType = "") {
        scopes.push_back({});
        if (!selfType.empty()) define("self", selfType);
        for (auto& p : fn->params) define(p.second, p.first);  // params: (type, name)
        walkStmt(fn->body.get());
        scopes.pop_back();
    }
    void walkStmt(Stmt* s) {
        if (!s) return;
        if (auto* b = nodeCast<BlockStmt>(s)) {
            scopes.push_back({});
            for (auto& it : b->items) {
                if (std::holds_alternative<DeclPtr>(it)) {
                    if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) {
                        if (vd->initializer) walkExpr(vd->initializer.get());
                        define(vd->name, vd->type);
                    }
                } else walkStmt(std::get<StmtPtr>(it).get());
            }
            scopes.pop_back(); return;
        }
        if (auto* i = nodeCast<IfStmt>(s)) {
            walkExpr(i->condition.get()); walkStmt(i->thenBranch.get()); walkStmt(i->elseBranch.get()); return;
        }
        if (auto* w = nodeCast<WhileStmt>(s)) { walkExpr(w->condition.get()); walkStmt(w->body.get()); return; }
        if (auto* dw = nodeCast<DoWhileStmt>(s)) { walkStmt(dw->body.get()); walkExpr(dw->condition.get()); return; }
        if (auto* f = nodeCast<ForStmt>(s)) {
            scopes.push_back({});
            walkStmt(f->init.get()); walkExpr(f->condition.get()); walkExpr(f->step.get()); walkStmt(f->body.get());
            scopes.pop_back(); return;
        }
        if (auto* fi = nodeCast<ForInStmt>(s)) {
            scopes.push_back({});
            walkExpr(fi->iterable.get()); define(fi->varName, "");
            walkStmt(fi->body.get());
            scopes.pop_back(); return;
        }
        if (auto* r = nodeCast<ReturnStmt>(s)) { walkExpr(r->value.get()); return; }
        if (auto* es = nodeCast<ExprStmt>(s)) { walkExpr(es->expr.get()); return; }
        if (auto* sw = nodeCast<SwitchStmt>(s)) {
            walkExpr(sw->subject.get());
            scopes.push_back({});                  // the switch body is one scope
            for (auto& c : sw->cases) {
                walkExpr(c.value.get());
                for (auto& it : c.stmts) {
                    if (std::holds_alternative<DeclPtr>(it)) {
                        if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) {
                            if (vd->initializer) walkExpr(vd->initializer.get());
                            define(vd->name, vd->type);
                        }
                    } else walkStmt(std::get<StmtPtr>(it).get());
                }
            }
            scopes.pop_back();
            return;
        }
        if (auto* th = nodeCast<ThrowStmt>(s)) { walkExpr(th->value.get()); return; }
        if (auto* tr = nodeCast<TryStmt>(s)) {
            walkStmt(tr->body.get());
            for (auto& cc : tr->catches) { scopes.push_back({}); if (!cc.type.empty()) define(cc.name, cc.type); walkStmt(cc.body.get()); scopes.pop_back(); }
            walkStmt(tr->finally.get()); return;
        }
        if (auto* tj = nodeCast<ThreadJoinStmt>(s)) { walkExpr(tj->tid.get()); return; }
        if (auto* a = nodeCast<AsmStmt>(s)) {
            for (auto& out : a->outputs) walkExpr(out.second.get());
            for (auto& in : a->inputs) walkExpr(in.second.get());
            return;
        }
        // BreakStmt / ContinueStmt: no children
    }
    void walkExpr(Expr* e) {
        if (!e) return;
        if (auto* id = nodeCast<IdentExpr>(e)) {
            int di = defIndex(id->name);
            if (di >= 0)
                for (auto& L : lambdas)
                    if (di < L.boundary) L.caps[id->name] = scopes[di][id->name];
            return;
        }
        if (auto* lam = nodeCast<LambdaExpr>(e)) {
            lambdas.push_back({(int)scopes.size(), {}});
            scopes.push_back({});
            std::set<std::string> params;
            for (auto& p : lam->params) { define(p.second, p.first); params.insert(p.second); }
            walkStmt(lam->body.get());
            scopes.pop_back();
            Active fin = lambdas.back(); lambdas.pop_back();
            lam->captures.clear();
            for (auto& [n, t] : fin.caps)
                if (!params.count(n)) lam->captures.push_back({n, t});
            return;
        }
        // IdentExpr and LambdaExpr are handled above (capture recording / scope
        // boundary); every other expression just recurses into its children via
        // the shared enumeration, so this pass can never miss a node type.
        astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { walkExpr(c.get()); });
    }
};
}  // namespace
