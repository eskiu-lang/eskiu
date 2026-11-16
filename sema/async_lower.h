// async_lower.h: the lowering of one async function (AsyncLowering) and the AST helpers
// it shares (namespace asynclower); internal to sema/async_*.cpp. AsyncTransform::run
// (async_transform.cpp) lowers each async function with an AsyncLowering, in phases:
// setup, collectFrame (async_lower_setup.cpp), lowerBody (async_lower_stmt.cpp, over the
// desugared body of async_lower_desugar.cpp and the state machinery of
// async_lower_states.cpp), buildCleanups and emitDecls (async_lower_frame.cpp).
#pragma once
#include "async_transform.h"
#include "../ast/ast_walk.h"
#include "../template_utils.h"
#include "type.h"
#include <stdexcept>
#include <cctype>
#include <set>
#include <map>
#include <memory>
#include <functional>
#include <algorithm>

// ── Small AST builders and walks (async_lower_util.cpp) ──────────────────────
namespace asynclower {

ExprPtr ident(const std::string& n);

// Names the lowering synthesizes for the async function being lowered: the frame
// pointer and the frame's bookkeeping fields. Each is chosen (AsyncTransform::run) to
// be distinct from every name spelled in the function, so a user local, parameter, or
// field named `__fr` / `st` / `ret` / `awaiting` cannot collide with them.
struct FrameNames { std::string ptr = "__fr", st = "st", ret = "ret", awaiting = "awaiting"; };

extern FrameNames frn;

ExprPtr intlit(long long v);

// fr.<field>
ExprPtr fr(const std::string& field);

ExprPtr binop(ExprPtr l, const std::string& op, ExprPtr r);

StmtPtr exprStmt(ExprPtr e);

StmtPtr assign(ExprPtr lhs, ExprPtr rhs);

StmtPtr ret(ExprPtr v);

// Rename frame-hoisted locals (params + body locals) to `fr.<name>` member accesses
// throughout an expression, in place. Recurses via the shared child enumeration
// (astwalk::forEachChildExpr), so every expression node (struct literals, alloc_with,
// etc.) is covered.
void rewrite(ExprPtr& e, const std::set<std::string>& vars);

// True if an expression contains an AwaitExpr anywhere (used to require `await`
// be bound in a `let` rather than nested inside a larger expression).
bool hasAwait(const ExprPtr& e);

// True if a statement contains an `await` anywhere (decides plain vs state-split).
bool stmtHasAwait(const StmtPtr& s);

// True if a statement contains a labeled `break`/`continue` anywhere. Async bodies are
// always state-split, and a labeled jump can't be threaded through the resume state
// machine (it may target a loop that no longer exists as a real loop), so we reject it.
bool stmtHasLabeledBreak(const StmtPtr& s);

// Every AwaitExpr in a function body (lambda bodies excluded: an await belongs to
// the async function it is spelled in).
void collectAwaits(Expr* e, std::vector<AwaitExpr*>& out);

void collectAwaits(Stmt* s, std::vector<AwaitExpr*>& out);

// Whether evaluating `e` may have a side effect: a call (a user operator counts), an
// assignment, `++`/`--`, an await, `?` (it may return), `alloc_with`, `thread_create` or
// `free_closure`. A lambda body is not evaluated there; `sizeof` never evaluates.
bool isAssignOp(const std::string& op);

bool hasSideEffects(Expr* e);

// Whether a statement contains a `return` (a lambda is an expression: not descended).
bool stmtHasReturn(const StmtPtr& s);

// `t` with every subtree spelled `from` replaced by `to`.
ty::Type replaceSubtree(const ty::Type& t, const std::string& from, const std::string& to);

// A type in a generic function, in terms of the template's type parameters, from its
// value in each checked instance (`recs`: the instance's bindings and the type there): a
// spelling S with substType(S, subs) equal to the type in every instance. Candidates put
// a type parameter in place of each subtree spelled as its argument (every subset of the
// parameters, most first); any consistent one is right for every instance there is. ""
// when none fits.
std::string generalizeType(const AsyncTransform::InstanceTypes& recs, const std::vector<std::string>& tps);

// The awaited type of `aw` in a generic async function, in terms of its type parameters.
std::string genericAwaitType(const AwaitExpr* aw, const std::vector<std::string>& tps);

// The closure  void() { fr.st = <state>; __<name>_resume(fr); }  used as a waker.
// Captures the frame pointer `fr` by value. Because this AST is synthesized after
// the type checker runs, we populate `captures` ourselves (sema would otherwise).
// A call to the resume function; for a generic async function it is itself generic,
// called with the enclosing template's own type parameters.
ExprPtr resumeCall(const std::string& resumeName, const std::vector<std::string>& tps);

ExprPtr resumeWaker(const std::string& resumeName, int state, const std::string& framePtrTy,
                    const std::vector<std::string>& tps);

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
        if (auto* id = nodeCast<IdentExpr>(e)) { uses[id->name]++; return; }
        if (auto* lam = nodeCast<LambdaExpr>(e)) { stmt(lam->body.get(), true); return; }
        if (auto* c = nodeCast<CallExpr>(e); c && !inLambda)
            if (auto* id = nodeCast<IdentExpr>(c->callee.get())) allowed[id->name]++;
        astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { expr(c.get(), inLambda); });
    }
    void decl(VarDecl* vd, bool inLambda, bool forInit) {
        if (!inLambda) {
            if (forInit || (vd->initializer && !nodeCast<LambdaExpr>(vd->initializer.get())))
                bad.insert(vd->name);
            else if (vd->initializer) bindings[vd->name]++;
        }
        expr(vd->initializer.get(), inLambda);
    }
    void items(const std::vector<BlockItem>& its, bool inLambda, bool forInit = false) {
        for (auto& it : its) {
            if (std::holds_alternative<StmtPtr>(it)) { stmt(std::get<StmtPtr>(it).get(), inLambda); continue; }
            if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) decl(vd, inLambda, forInit);
        }
    }
    void stmt(Stmt* s, bool inLambda) {
        if (!s) return;
        auto E = [&](const ExprPtr& e) { expr(e.get(), inLambda); };
        auto S = [&](const StmtPtr& st) { stmt(st.get(), inLambda); };
        if (auto* b = nodeCast<BlockStmt>(s)) items(b->items, inLambda);
        else if (auto* es = nodeCast<ExprStmt>(s)) {
            auto* a = nodeCast<BinaryExpr>(es->expr.get());
            if (a && a->op == "=" && !inLambda && nodeCast<LambdaExpr>(a->right.get()))
                if (auto* id = nodeCast<IdentExpr>(a->left.get())) { allowed[id->name]++; bindings[id->name]++; }
            E(es->expr);
        }
        else if (auto* i = nodeCast<IfStmt>(s)) { E(i->condition); S(i->thenBranch); S(i->elseBranch); }
        else if (auto* f = nodeCast<ForStmt>(s)) {
            if (auto* ib = nodeCast<BlockStmt>(f->init.get())) items(ib->items, inLambda, true);
            else S(f->init);
            E(f->condition); E(f->step); S(f->body);
        }
        else if (auto* fi = nodeCast<ForInStmt>(s))    { E(fi->iterable); S(fi->body); }
        else if (auto* w = nodeCast<WhileStmt>(s))     { E(w->condition); S(w->body); }
        else if (auto* dw = nodeCast<DoWhileStmt>(s))  { S(dw->body); E(dw->condition); }
        else if (auto* r = nodeCast<ReturnStmt>(s))    { E(r->value); }
        else if (auto* sw = nodeCast<SwitchStmt>(s)) {
            E(sw->subject);
            for (auto& c : sw->cases) { E(c.value); items(c.stmts, inLambda); }
        }
        else if (auto* m = nodeCast<MatchStmt>(s))     { E(m->subject); for (auto& a : m->arms) S(a.body); }
        else if (auto* th = nodeCast<ThrowStmt>(s))    { E(th->value); }
        else if (auto* t = nodeCast<TryStmt>(s))       { S(t->body); for (auto& c : t->catches) S(c.body); S(t->finally); }
        else if (auto* d = nodeCast<DeferStmt>(s))     { S(d->body); }
        else if (auto* tj = nodeCast<ThreadJoinStmt>(s)) { E(tj->tid); }
        else if (auto* as = nodeCast<AsmStmt>(s)) {
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
StmtPtr freeClosureOf(const std::string& n);

void freeBeforeRebind(std::vector<BlockItem>& its, const std::set<std::string>& owned);

StmtPtr freeBeforeRebind(const StmtPtr& s, const std::set<std::string>& owned);

void freeBeforeRebind(std::vector<BlockItem>& its, const std::set<std::string>& owned);

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
        if (auto* id = nodeCast<IdentExpr>(e.get())) { id->name = lookup(id->name); return; }
        if (auto* lam = nodeCast<LambdaExpr>(e.get())) {
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
                if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) {
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
        if (auto* b = nodeCast<BlockStmt>(s.get())) { scopes.emplace_back(); items(b->items); scopes.pop_back(); }
        else if (auto* i = nodeCast<IfStmt>(s.get())) { expr(i->condition); scoped(i->thenBranch); scoped(i->elseBranch); }
        else if (auto* w = nodeCast<WhileStmt>(s.get())) { expr(w->condition); scoped(w->body); }
        else if (auto* d = nodeCast<DoWhileStmt>(s.get())) { scoped(d->body); expr(d->condition); }
        else if (auto* f = nodeCast<ForStmt>(s.get())) {
            scopes.emplace_back();
            if (auto* ib = nodeCast<BlockStmt>(f->init.get())) items(ib->items);
            else stmt(f->init);
            expr(f->condition); expr(f->step); scoped(f->body);
            scopes.pop_back();
        } else if (auto* fi = nodeCast<ForInStmt>(s.get())) {
            expr(fi->iterable);
            scopes.emplace_back();
            fi->varName = declare(fi->varName);
            scoped(fi->body);
            scopes.pop_back();
        } else if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
            expr(sw->subject);
            scopes.emplace_back();
            for (auto& c : sw->cases) { expr(c.value); items(c.stmts); }
            scopes.pop_back();
        } else if (auto* m = nodeCast<MatchStmt>(s.get())) {
            expr(m->subject);
            for (auto& arm : m->arms) {
                scopes.emplace_back();
                // A binding gets its own spelling too, so a hoisted local of the same
                // name is not substituted for it.
                for (auto& bn : arm.bindings) if (bn != "_") bn = declare(bn);
                scoped(arm.body);
                scopes.pop_back();
            }
        } else if (auto* t = nodeCast<TryStmt>(s.get())) {
            scoped(t->body);
            for (auto& c : t->catches) {
                scopes.emplace_back();
                if (!c.type.empty() && !c.name.empty()) c.name = declare(c.name);
                scoped(c.body);
                scopes.pop_back();
            }
            scoped(t->finally);
        } else if (auto* r = nodeCast<ReturnStmt>(s.get())) expr(r->value);
        else if (auto* es = nodeCast<ExprStmt>(s.get())) expr(es->expr);
        else if (auto* th = nodeCast<ThrowStmt>(s.get())) expr(th->value);
        else if (auto* df = nodeCast<DeferStmt>(s.get())) scoped(df->body);
        else if (auto* tj = nodeCast<ThreadJoinStmt>(s.get())) expr(tj->tid);
        else if (auto* as = nodeCast<AsmStmt>(s.get())) {
            for (auto& out : as->outputs) expr(out.second);
            for (auto& in : as->inputs) expr(in.second);
        }
    }
};

} // namespace asynclower

using namespace asynclower;

class AsyncLowering {
public:
    AsyncLowering(const AsyncTransform& xf, FunctionDecl* fn, const std::set<std::string>& topNames)
        : xf(xf), exprTypes(xf.exprTypes), instanceExprTypes(xf.instanceExprTypes), fn(fn),
          topNames(topNames), tps(fn->typeParams) {}
    // Names the frame, resume function and constructor, and renames shadowing locals;
    // false when there is nothing to lower (a generic function never instantiated).
    bool setup();
    // Desugars the body (every await the initializer of a let) and collects the frame's
    // fields: the parameters, every local and each await's future.
    void collectFrame();
    // Lowers the body into the state graph.
    void lowerBody();
    // The cancellation states, the try regions' handlers and what the frame's end frees.
    void buildCleanups();
    // Snapshots of the frame fields a lambda in a state captures.
    void snapshotCaptures();
    // The frame struct, the resume function and the constructor, in place of the function.
    void emitDecls(std::vector<DeclPtr>& out);

private:
    const AsyncTransform& xf;
    // The type checker's expression types (AsyncTransform's), for typeOf.
    const std::map<Expr*, std::string>* exprTypes;
    const std::map<Expr*, AsyncTransform::InstanceTypes>* instanceExprTypes;
    std::string declType(const std::string& checked) const { return xf.declType(checked); }

    // The function being lowered and the names the lowering gives its parts.
    FunctionDecl* fn;
    const std::set<std::string>& topNames;
    std::string name;
    const std::vector<std::string>& tps;
    bool generic = false;
    std::map<AwaitExpr*, std::string> genAwTy;
    std::set<std::string> used;
    std::string frameName;
    std::string frameT;          // the frame type's spelling (`__f_frame<T>` if generic)
    std::string resumeN;
    std::string T;               // declared return type
    bool isVoid = false;
    std::string Tret;            // the Future's value type (`async void` uses uint8)
    BlockStmt* block = nullptr;
    std::vector<DeclPtr> statics;
    int tmpN = 0;
    int forinSeq = 0;

    // The desugared body, its awaits and the frame's fields.
    std::vector<BlockItem> items;
    struct AwaitSite { VarDecl* var; AwaitExpr* expr; };
    std::vector<AwaitSite> awaits;
    std::map<AwaitExpr*, int> awIdx;
    std::vector<std::string> awNames;   // frame field of each await's future
    std::set<std::string> vars;
    std::vector<StructDecl::Field> fields;
    std::set<std::string> ownedClosures;

    // The state graph (see lowerStmt).
    std::vector<std::vector<BlockItem>> states;
    std::vector<int> brkTargets, contTargets;
    using Frames = std::vector<std::vector<StmtPtr>>;
    Frames deferFrames;
    bool cancelCleanup = false;
    std::vector<char> stateCancel;
    std::vector<size_t> brkDeferDepth, contDeferDepth;
    struct CatchInfo { std::string type, var, txtField; int state; };
    struct Region { int parent; size_t startDepth, base; std::vector<CatchInfo> catches; StmtPtr fin; };
    std::vector<Region> regions;
    int curRegion = -1;
    std::vector<int> stateRegion;
    std::vector<std::vector<std::vector<StmtPtr>>> stateFrames;   // defer frames pending in each state
    std::vector<std::pair<int, Frames>> dropSites;
    std::shared_ptr<BlockStmt> excFree;
    int initTmp = 0;

    // Cancellation and the regions' handlers (buildCleanups).
    struct DropState { int parked, cleanup; bool detached; };
    std::vector<DropState> dropStates;
    std::string cxName;
    std::map<int, std::string> excFields;   // region -> its captured exception's frame field
    std::map<std::string, std::string> fieldTy;


    // Typing, names and frame scans (async_lower_setup.cpp).
    std::string awType(AwaitExpr* aw);
    std::runtime_error locError(ASTNode* at, const std::string& msg);
    static bool knownType(const std::string& t);
    std::string generalized(const AsyncTransform::InstanceTypes& recs);
    std::string typeOf(Expr* e);
    static std::shared_ptr<LiteralExpr> boolLit(bool v);
    static StmtPtr blockOf(std::vector<BlockItem> its);
    std::string fresh(const std::string& base);
    void letTmp(std::vector<BlockItem>& pre, const std::string& tn, const std::string& ty, ExprPtr init);
    void scanB(const std::vector<BlockItem>& its);
    void scanS(const StmtPtr& s);
    void lambdaCaps(const ExprPtr& e, std::vector<std::string>& caps);
    StmtPtr wrapCaps(const StmtPtr& s);

    // Desugaring: every await the initializer of a let (async_lower_desugar.cpp).
    void spill(ExprPtr& c, std::vector<BlockItem>& pre);
    void rval(ExprPtr& c, std::vector<BlockItem>& pre);
    static bool placeShaped(Expr* x);
    void place(ExprPtr& e, std::vector<BlockItem>& pre, bool spillParts);
    void hoist(ExprPtr& e, std::vector<BlockItem>& pre);
    std::vector<BlockItem> desugarItems(const std::vector<BlockItem>& its);
    StmtPtr breakUnless(ExprPtr c);
    StmtPtr withPre(std::vector<BlockItem>& pre, StmtPtr st);
    StmtPtr desugarStmt(const StmtPtr& s);
    StmtPtr desugarExpr(ExprStmt* es, const StmtPtr& s, std::vector<BlockItem>& pre);
    StmtPtr desugarReturn(ReturnStmt* rs, const StmtPtr& s, std::vector<BlockItem>& pre);
    StmtPtr desugarThrow(ThrowStmt* ts, const StmtPtr& s, std::vector<BlockItem>& pre);
    StmtPtr desugarIf(IfStmt* i, std::vector<BlockItem>& pre);
    StmtPtr desugarWhile(WhileStmt* w);
    StmtPtr desugarDoWhile(DoWhileStmt* dw);
    StmtPtr desugarFor(ForStmt* f);
    StmtPtr desugarForIn(ForInStmt* fi, std::vector<BlockItem>& pre);
    StmtPtr desugarSwitch(SwitchStmt* sw, std::vector<BlockItem>& pre);
    StmtPtr desugarMatch(MatchStmt* m, std::vector<BlockItem>& pre);
    StmtPtr desugarTry(TryStmt* t);
    StmtPtr desugarDefer(DeferStmt* d, const StmtPtr& s);

    // States, defer frames, exits and plain statements (async_lower_states.cpp).
    void goTo(int s, int target);
    bool cleanupAwaits(const Frames& frs, size_t from, size_t to);
    std::string rewriteCatchExText(StmtPtr& root);
    int newStateIn(int region, size_t depth);
    int newState();
    void enterLoop(int brk, int cont);
    void leaveLoop();
    void emitDefers(std::vector<BlockItem>& st, size_t downTo);
    bool escapesIn(const StmtPtr& s, bool contOnly);
    bool loopEscapes(const StmtPtr& s);
    bool stmtTerminates(const StmtPtr& s);
    void publish(std::vector<BlockItem>& st);
    void completeInto(std::vector<BlockItem>& st, ExprPtr v);
    int runFrames(int st, size_t downTo);
    void emitExit(int st, size_t downTo, int target, bool complete);
    StmtPtr initField(VarDecl* vd);
    StmtPtr rewritePlain(const StmtPtr& s);

    // Statements into states (async_lower_stmt.cpp).
    int lowerItem(BlockItem& it, int cur);
    int lowerSeq(const std::vector<BlockItem>& its, int entry);
    int lowerStmt(const StmtPtr& s, int cur);
    int lowerReturn(ReturnStmt* rs, int cur);
    int lowerIf(IfStmt* i, int cur);
    int lowerWhile(WhileStmt* w, int cur);
    int lowerDoWhile(DoWhileStmt* dw, int cur);
    int lowerFor(ForStmt* f, int cur);
    int lowerSwitch(SwitchStmt* sw, int cur);
    int lowerMatch(MatchStmt* m, const StmtPtr& s, int cur);
    int lowerTry(TryStmt* t, int cur);

    // Cancellation and the try regions' handlers (async_lower_frame.cpp).
    static std::shared_ptr<CastExpr> nullOf(const std::string& t);
    void buildDrop(const std::pair<int, Frames>& site);
    std::string excField(int r);
    StmtPtr freeExc(const std::string& f);
    void wrapState(size_t si);
};
