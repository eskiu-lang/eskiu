// typecheck_uninit.cpp: -Wall maybe-uninitialized reads (split from typecheck_decl.cpp).
#include "type_checker.h"
#include <cstdint>
#include <functional>
#include <algorithm>
#include <set>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include "../intrinsics.h"
#include "../ast/ast_walk.h"
#include "../ast/type_qual.h"

bool TypeChecker::checkUninitPrefix(BlockStmt* body) {
    if (!body) return false;
    bool reported = false;
    std::set<std::string> uninit;   // scalar locals declared without init, not yet assigned
    std::function<void(Expr*)> scan = [&](Expr* e) {
        if (!e) return;
        // &x initializes x (its address may be written through) — not a read.
        if (auto* u = nodeCast<UnaryExpr>(e); u && u->op == "&")
            if (auto* id = nodeCast<IdentExpr>(u->operand.get())) { uninit.erase(id->name); return; }
        // An assignment (also nested: `x = y = 3`, `let y = (x = 5)`) writes its target
        // after evaluating the value; the target itself is not read.
        if (auto* b = nodeCast<BinaryExpr>(e); b && b->op == "=") {
            scan(b->right.get());
            if (auto* id = nodeCast<IdentExpr>(b->left.get())) uninit.erase(id->name);
            else scan(b->left.get());   // e.g. `arr[x] = …` reads x
            return;
        }
        if (auto* id = nodeCast<IdentExpr>(e)) {
            if (uninit.count(id->name)) {
                errorAt(id, "use of uninitialized variable '" + id->name + "'");
                reported = true;
                uninit.erase(id->name);   // report once
            }
            return;
        }
        astwalk::forEachChildExprFlat(e, [&](ExprPtr& c){ scan(c.get()); });
    };
    for (auto& item : body->items) {
        if (std::holds_alternative<DeclPtr>(item)) {
            if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(item).get())) {
                if (vd->initializer) { scan(vd->initializer.get()); uninit.erase(vd->name); }
                // A `static` local without an initializer is zero (static storage, C).
                else if (vd->isStatic) uninit.erase(vd->name);
                else if (isUninitScalar(vd->type)) uninit.insert(vd->name);
            }
            continue;
        }
        Stmt* s = std::get<StmtPtr>(item).get();
        if (auto* es = nodeCast<ExprStmt>(s)) { scan(es->expr.get()); continue; }
        if (auto* rs = nodeCast<ReturnStmt>(s)) { if (rs->value) scan(rs->value.get()); continue; }
        break;   // control-flow or anything else: stop (stay conservative)
    }
    return reported;
}

// Only genuine scalars: an array (`T[N]`, incl. `*Node[3]`) ends in ']' and is
// excluded (element writes initialize it piecewise), as are structs, unions, slices
// and interface values (field-by-field initialization).
bool TypeChecker::isUninitScalar(const std::string& type) {
    std::string t = normalizeType(type);
    return (!t.empty() && t.back() != ']') &&
           (isNumericType(t) || isPointerType(t) || t == "string" || ty::Type::parse(t).isFn());
}

// --- -Wall: maybe-uninitialized reads --------------------------------------------
// A forward "possibly unassigned" dataflow over a function body. The state is the set
// of tracked locals (scalar locals declared without an initializer, as for the error
// above) that some path to this point has not assigned; paths meet by union, and a read
// of one is a warning. `=` to the bare name and `&x` assign it; only whole-variable
// reads count (aggregates are not tracked, so field-by-field initialization never
// warns). Unreachable code (after return/break/continue/throw) reads nothing. A loop
// body starts from the state before the loop (an assignment only removes names, so the
// back edges add nothing), and the loop exits with its condition-false state joined
// with every `break` to it. `switch` cases fall through in order; a `match` arm and a
// `catch` handler start from the state before the construct, and so does a `finally`
// (it may run after any prefix of the body). A `defer` body is analyzed at each exit
// that runs it (block end, return, break/continue, `?`), and a lambda reads the
// variables it captures when it is created (its body is a function of its own).
// Variables are keyed by declaration, not by name, so shadowing is exact.
namespace {
class MaybeUninit {
public:
    MaybeUninit(std::function<bool(VarDecl*)> tracked,
                std::function<void(IdentExpr*)> report)
        : isTracked(std::move(tracked)), report(std::move(report)) {}

    void function(const std::vector<std::pair<std::string, std::string>>& params, Stmt* body) {
        pushScope();
        for (const auto& p : params) declare(p.second, false);
        block(body);
        popScope();
    }

private:
    struct St { bool live = true; std::set<int> u; };
    struct Frame { bool isLoop; std::string label; size_t deferDepth; St brk; St cont; };
    std::function<bool(VarDecl*)> isTracked;
    std::function<void(IdentExpr*)> report;
    St s;
    int nextId = 0;
    std::map<std::string, std::vector<int>> env;
    std::vector<std::vector<std::string>> scopeNames;
    std::vector<std::vector<Stmt*>> defers;   // per scope, in registration order
    std::vector<Frame> frames;
    std::set<int> warned;
    bool inDefer = false;

    static void join(St& into, const St& other) {
        if (!other.live) return;
        if (!into.live) { into = other; return; }
        into.u.insert(other.u.begin(), other.u.end());
    }
    static St dead() { St d; d.live = false; return d; }
    void pushScope() { scopeNames.emplace_back(); defers.emplace_back(); }
    void popScope() {
        for (const auto& n : scopeNames.back()) {
            auto it = env.find(n);
            s.u.erase(it->second.back());
            it->second.pop_back();
            if (it->second.empty()) env.erase(it);
        }
        scopeNames.pop_back();
        defers.pop_back();
    }
    void declare(const std::string& name, bool tracked) {
        int id = nextId++;
        env[name].push_back(id);
        scopeNames.back().push_back(name);
        if (tracked) s.u.insert(id);
    }
    int lookup(const std::string& name) const {
        auto it = env.find(name);
        return it == env.end() ? -1 : it->second.back();
    }
    void assign(const std::string& name) { int id = lookup(name); if (id >= 0) s.u.erase(id); }
    void read(IdentExpr* id) {
        int v = lookup(id->name);
        if (v < 0 || !s.live || !s.u.count(v)) return;
        if (warned.insert(v).second) report(id);
    }

    // Run the pending defer bodies of scopes [from, end), innermost first, on a copy
    // of the state (they run on the way out, so nothing after them sees their writes).
    // A defer body is not re-entered (an invalid jump out of one would loop).
    void runDefers(size_t from) {
        if (!s.live || inDefer) return;
        St saved = s;
        inDefer = true;
        for (size_t d = defers.size(); d-- > from;) {
            auto ds = defers[d];
            for (size_t i = ds.size(); i-- > 0;) stmt(ds[i]);
        }
        inDefer = false;
        s = saved;
    }
    // The defers of the innermost scope, at its normal end.
    void runScopeDefers() {
        if (!s.live || inDefer) return;
        inDefer = true;
        auto ds = defers.back();
        for (size_t i = ds.size(); i-- > 0;) stmt(ds[i]);
        inDefer = false;
    }

    static bool logical(const std::string& op) { return op == "&&" || op == "||"; }

    // Evaluate condition `e`, leaving in `onTrue` / `onFalse` the state on the paths where
    // it is true / false: the right operand of `a && b` runs only when `a` is true, so what
    // it assigns is assigned where the whole is true (`||` and `!` likewise). A chain of
    // one operator is walked by a loop down its left spine.
    void cond(Expr* e, St& onTrue, St& onFalse) {
        if (auto* u = nodeCast<UnaryExpr>(e); u && u->op == "!") {
            cond(u->operand.get(), onFalse, onTrue);
            return;
        }
        auto* b = nodeCast<BinaryExpr>(e);
        if (!b || !logical(b->op)) { expr(e); onTrue = s; onFalse = s; return; }
        bool isAnd = b->op == "&&";
        std::vector<BinaryExpr*> spine{b};
        while (auto* l = nodeCast<BinaryExpr>(spine.back()->left.get())) {
            if (l->op != b->op) break;
            spine.push_back(l);
        }
        St t, f;
        cond(spine.back()->left.get(), t, f);
        for (size_t i = spine.size(); i-- > 0;) {
            s = isAnd ? t : f;
            St rt, rf;
            cond(spine[i]->right.get(), rt, rf);
            if (isAnd) { t = std::move(rt); join(f, rf); }
            else { join(t, rt); f = std::move(rf); }
        }
        onTrue = std::move(t);
        onFalse = std::move(f);
        s = onTrue;
        join(s, onFalse);
    }

    void expr(Expr* e) {
        if (!e) return;
        if (auto* id = nodeCast<IdentExpr>(e)) { read(id); return; }
        if (auto* u = nodeCast<UnaryExpr>(e); u && u->op == "&")
            if (auto* id = nodeCast<IdentExpr>(u->operand.get())) { assign(id->name); return; }
        if (nodeCast<SizeofExpr>(e)) return;   // not evaluated
        if (auto* b = nodeCast<BinaryExpr>(e)) {
            if (b->op == "=") {
                expr(b->right.get());
                if (auto* id = nodeCast<IdentExpr>(b->left.get())) assign(id->name);
                else expr(b->left.get());
                return;
            }
            // A `&&`/`||` value: what follows sees the join of its true and false paths.
            if (logical(b->op)) {
                St t, f;
                cond(b, t, f);
                s = std::move(t);
                join(s, f);
                return;
            }
            // Operator chains are walked by a loop down the left spine.
            bool isLogical = false;
            std::vector<BinaryExpr*> spine{b};
            while (auto* l = nodeCast<BinaryExpr>(spine.back()->left.get())) {
                if (l->op == "=" || logical(l->op) != isLogical) break;
                spine.push_back(l);
            }
            expr(spine.back()->left.get());
            St after = s;
            for (size_t i = spine.size(); i-- > 0;) expr(spine[i]->right.get());
            if (isLogical) s = after;
            return;
        }
        if (auto* t = nodeCast<TernaryExpr>(e)) {
            St before, elseSt;
            cond(t->condition.get(), before, elseSt);
            s = before;
            expr(t->thenExpr.get());
            St a = s;
            s = elseSt;
            expr(t->elseExpr.get());
            join(s, a);
            return;
        }
        if (auto* q = nodeCast<QuestionExpr>(e)) {
            expr(q->operand.get());
            runDefers(0);   // `?` may return from here
            return;
        }
        if (auto* lam = nodeCast<LambdaExpr>(e)) {
            for (const auto& c : lam->captures) {
                IdentExpr at(c.first);
                at.line = lam->line; at.col = lam->col;
                read(&at);
            }
            lambda(lam);
            return;
        }
        astwalk::forEachChildExpr(e, [&](ExprPtr& c) { expr(c.get()); });
    }

    void lambda(LambdaExpr* lam) {
        St savedS = std::move(s);
        auto savedEnv = std::move(env);
        auto savedScopes = std::move(scopeNames);
        auto savedDefers = std::move(defers);
        auto savedFrames = std::move(frames);
        env.clear(); scopeNames.clear(); defers.clear(); frames.clear();
        s = St{};
        function(lam->params, lam->body.get());
        env = std::move(savedEnv);
        scopeNames = std::move(savedScopes);
        defers = std::move(savedDefers);
        frames = std::move(savedFrames);
        s = std::move(savedS);
    }

    void item(BlockItem& it) {
        if (std::holds_alternative<StmtPtr>(it)) { stmt(std::get<StmtPtr>(it).get()); return; }
        auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get());
        if (!vd) return;
        expr(vd->initializer.get());
        declare(vd->name, !vd->initializer && !vd->isStatic && !vd->isExtern && isTracked(vd));
    }

    // A statement in its own scope (a block, or an unbraced body).
    void block(Stmt* body) {
        pushScope();
        if (auto* b = nodeCast<BlockStmt>(body)) for (auto& it : b->items) item(it);
        else stmt(body);
        runScopeDefers();
        popScope();
    }

    void jump(const std::string& label, bool isContinue) {
        for (size_t i = frames.size(); i-- > 0;) {
            Frame& f = frames[i];
            if (!label.empty() ? !(f.isLoop && f.label == label) : (isContinue && !f.isLoop)) continue;
            runDefers(f.deferDepth);
            join(isContinue ? f.cont : f.brk, s);
            break;
        }
        s.live = false;
    }
    void pushFrame(bool isLoop, const std::string& label) {
        frames.push_back(Frame{isLoop, label, defers.size(), dead(), dead()});
    }
    Frame popFrame() { Frame f = std::move(frames.back()); frames.pop_back(); return f; }

    static bool alwaysTrue(Expr* c) {
        if (!c) return true;
        auto* lit = nodeCast<LiteralExpr>(c);
        if (!lit) return false;
        if (lit->kind == LiteralExpr::Kind::BOOL) return lit->value == "true";
        if (lit->kind == LiteralExpr::Kind::INT)  return lit->value != "0";
        return false;
    }

    void stmt(Stmt* st) {
        if (!st) return;
        if (nodeCast<BlockStmt>(st)) { block(st); return; }
        if (auto* es = nodeCast<ExprStmt>(st)) { expr(es->expr.get()); return; }
        if (auto* r = nodeCast<ReturnStmt>(st)) { expr(r->value.get()); runDefers(0); s.live = false; return; }
        if (auto* th = nodeCast<ThrowStmt>(st)) { expr(th->value.get()); s.live = false; return; }
        if (auto* br = nodeCast<BreakStmt>(st)) { jump(br->label, false); return; }
        if (auto* co = nodeCast<ContinueStmt>(st)) { jump(co->label, true); return; }
        if (auto* d = nodeCast<DeferStmt>(st)) { if (s.live) defers.back().push_back(d->body.get()); return; }
        if (auto* a = nodeCast<AsmStmt>(st)) { for (auto& in : a->inputs) expr(in.second.get()); return; }
        if (auto* tj = nodeCast<ThreadJoinStmt>(st)) { expr(tj->tid.get()); return; }
        if (auto* i = nodeCast<IfStmt>(st)) {
            // An `else if` chain is walked with a loop.
            St out = dead();
            for (IfStmt* n = i; n;) {
                St thenSt, elseSt;
                cond(n->condition.get(), thenSt, elseSt);
                s = std::move(thenSt);
                block(n->thenBranch.get());
                join(out, s);
                s = std::move(elseSt);
                auto* next = nodeCast<IfStmt>(n->elseBranch.get());
                if (!next) {
                    if (n->elseBranch) block(n->elseBranch.get());
                    join(out, s);
                }
                n = next;
            }
            s = std::move(out);
            return;
        }
        if (auto* w = nodeCast<WhileStmt>(st)) {
            St bodySt, exitSt;
            cond(w->condition.get(), bodySt, exitSt);
            s = std::move(bodySt);
            St exit = alwaysTrue(w->condition.get()) ? dead() : exitSt;
            pushFrame(true, w->label);
            block(w->body.get());
            Frame f = popFrame();
            s = std::move(exit);
            join(s, f.brk);
            return;
        }
        if (auto* dw = nodeCast<DoWhileStmt>(st)) {
            pushFrame(true, dw->label);
            block(dw->body.get());
            join(s, frames.back().cont);
            expr(dw->condition.get());
            Frame f = popFrame();
            if (alwaysTrue(dw->condition.get())) s.live = false;
            join(s, f.brk);
            return;
        }
        if (auto* f = nodeCast<ForStmt>(st)) {
            pushScope();
            // The init's declarations belong to the loop.
            if (auto* ib = nodeCast<BlockStmt>(f->init.get())) { for (auto& it : ib->items) item(it); }
            else stmt(f->init.get());
            St bodySt, exitSt;
            if (f->condition) cond(f->condition.get(), bodySt, exitSt);
            else { bodySt = s; exitSt = s; }
            s = std::move(bodySt);
            St exit = alwaysTrue(f->condition.get()) ? dead() : exitSt;
            pushFrame(true, f->label);
            block(f->body.get());
            join(s, frames.back().cont);
            expr(f->step.get());
            Frame fr = popFrame();
            s = std::move(exit);
            join(s, fr.brk);
            popScope();
            return;
        }
        if (auto* fi = nodeCast<ForInStmt>(st)) {
            expr(fi->iterable.get());
            St exit = s;
            pushScope();
            declare(fi->varName, false);
            pushFrame(true, fi->label);
            block(fi->body.get());
            Frame fr = popFrame();
            popScope();
            s = std::move(exit);
            join(s, fr.brk);
            return;
        }
        if (auto* sw = nodeCast<SwitchStmt>(st)) {
            expr(sw->subject.get());
            St entry = s, cur = dead();
            bool hasDefault = false;
            pushScope();   // the switch body is one scope
            pushFrame(false, "");
            for (auto& c : sw->cases) {
                if (!c.value) hasDefault = true;
                s = entry;
                join(s, cur);   // fall-through from the case above
                for (auto& it : c.stmts) item(it);
                cur = s;
            }
            Frame fr = popFrame();
            s = sw->cases.empty() ? entry : cur;
            if (!hasDefault) join(s, entry);
            join(s, fr.brk);
            runScopeDefers();
            popScope();
            return;
        }
        if (auto* m = nodeCast<MatchStmt>(st)) {
            // The arms are exhaustive (checked separately).
            expr(m->subject.get());
            if (m->arms.empty()) return;
            St entry = s, out = dead();
            for (auto& arm : m->arms) {
                s = entry;
                pushScope();
                for (const auto& bn : arm.bindings) declare(bn, false);
                block(arm.body.get());
                popScope();
                join(out, s);
            }
            s = std::move(out);
            return;
        }
        if (auto* t = nodeCast<TryStmt>(st)) {
            St before = s;
            block(t->body.get());
            St out = s;
            for (auto& c : t->catches) {
                s = before;
                pushScope();
                if (!c.type.empty()) declare(c.name, false);
                block(c.body.get());
                popScope();
                join(out, s);
            }
            if (t->finally) {
                s = before;
                block(t->finally.get());
                if (!s.live) return;
                // What the finally assigns on every path through it is assigned after.
                for (int v : before.u) if (!s.u.count(v)) out.u.erase(v);
            }
            s = std::move(out);
            return;
        }
    }
};
} // namespace

void TypeChecker::warnMaybeUninit(FunctionDecl* node) {
    auto tracked = [&](VarDecl* vd) { return isUninitScalar(vd->type); };
    auto report = [&](IdentExpr* id) {
        std::string key = diagFile() + ":" + std::to_string(id->line) + ":" +
                          std::to_string(id->col) + ":" + id->name;
        if (!maybeUninitWarned.insert(key).second) return;
        warning(id->line, id->col, "variable '" + id->name + "' may be used uninitialized");
    };
    MaybeUninit(tracked, report).function(node->params, node->body.get());
}
