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

// ============================================================================

// TypeChecker — declaration visitors (functions, structs, enums, unions,
// interfaces, type aliases) + the template capture pass.
// Part of the type_checker.cpp split; all methods are TypeChecker members
// declared in type_checker.h.

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
        if (auto* b = dynamic_cast<BlockStmt*>(s)) {
            scopes.push_back({});
            for (auto& it : b->items) {
                if (std::holds_alternative<DeclPtr>(it)) {
                    if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) {
                        if (vd->initializer) walkExpr(vd->initializer.get());
                        define(vd->name, vd->type);
                    }
                } else walkStmt(std::get<StmtPtr>(it).get());
            }
            scopes.pop_back(); return;
        }
        if (auto* i = dynamic_cast<IfStmt*>(s)) {
            walkExpr(i->condition.get()); walkStmt(i->thenBranch.get()); walkStmt(i->elseBranch.get()); return;
        }
        if (auto* w = dynamic_cast<WhileStmt*>(s)) { walkExpr(w->condition.get()); walkStmt(w->body.get()); return; }
        if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) { walkStmt(dw->body.get()); walkExpr(dw->condition.get()); return; }
        if (auto* f = dynamic_cast<ForStmt*>(s)) {
            scopes.push_back({});
            walkStmt(f->init.get()); walkExpr(f->condition.get()); walkExpr(f->step.get()); walkStmt(f->body.get());
            scopes.pop_back(); return;
        }
        if (auto* fi = dynamic_cast<ForInStmt*>(s)) {
            scopes.push_back({});
            walkExpr(fi->iterable.get()); define(fi->varName, "");
            walkStmt(fi->body.get());
            scopes.pop_back(); return;
        }
        if (auto* r = dynamic_cast<ReturnStmt*>(s)) { walkExpr(r->value.get()); return; }
        if (auto* es = dynamic_cast<ExprStmt*>(s)) { walkExpr(es->expr.get()); return; }
        if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
            walkExpr(sw->subject.get());
            scopes.push_back({});                  // the switch body is one scope
            for (auto& c : sw->cases) {
                walkExpr(c.value.get());
                for (auto& it : c.stmts) {
                    if (std::holds_alternative<DeclPtr>(it)) {
                        if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get())) {
                            if (vd->initializer) walkExpr(vd->initializer.get());
                            define(vd->name, vd->type);
                        }
                    } else walkStmt(std::get<StmtPtr>(it).get());
                }
            }
            scopes.pop_back();
            return;
        }
        if (auto* th = dynamic_cast<ThrowStmt*>(s)) { walkExpr(th->value.get()); return; }
        if (auto* tr = dynamic_cast<TryStmt*>(s)) {
            walkStmt(tr->body.get());
            for (auto& cc : tr->catches) { scopes.push_back({}); define(cc.name, cc.type); walkStmt(cc.body.get()); scopes.pop_back(); }
            walkStmt(tr->finally.get()); return;
        }
        if (auto* tj = dynamic_cast<ThreadJoinStmt*>(s)) { walkExpr(tj->tid.get()); return; }
        if (auto* a = dynamic_cast<AsmStmt*>(s)) {
            for (auto& out : a->outputs) walkExpr(out.second.get());
            for (auto& in : a->inputs) walkExpr(in.second.get());
            return;
        }
        // BreakStmt / ContinueStmt: no children
    }
    void walkExpr(Expr* e) {
        if (!e) return;
        if (auto* id = dynamic_cast<IdentExpr*>(e)) {
            int di = defIndex(id->name);
            if (di >= 0)
                for (auto& L : lambdas)
                    if (di < L.boundary) L.caps[id->name] = scopes[di][id->name];
            return;
        }
        if (auto* lam = dynamic_cast<LambdaExpr*>(e)) {
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

// --- Definite-return analysis --------------------------------------------
// A non-void function must return (or throw, or provably diverge) on every
// path; falling off the end is an error, not an implicit zero return. The
// analysis is "can this statement complete normally?" (Java-style): a function
// whose body can complete normally is missing a return.

bool canCompleteNormally(Stmt* s);

// Is `cond` a literal that is always true? (`while(1)`, `while(true)`, or a
// `for(;;)` whose condition is null.)
bool condAlwaysTrue(Expr* cond) {
    if (!cond) return true;  // for(;;)
    auto* lit = dynamic_cast<LiteralExpr*>(cond);
    if (!lit) return false;
    if (lit->kind == LiteralExpr::Kind::BOOL) return lit->value == "true";
    if (lit->kind == LiteralExpr::Kind::INT)  return lit->value != "0";
    return false;
}

// Does `s` contain a `break` (or, with `wantContinue`, a `continue`) that transfers
// to the loop or switch enclosing `s`, whose label is `label` ("" for none, always
// "" for a switch)? `inner` is set once we are inside a nested loop/switch, where
// an unlabeled jump binds to that construct instead; a labeled jump naming `label`
// reaches us from any depth.
bool jumpsTo(Stmt* s, const std::string& label, bool wantContinue, bool inner) {
    if (!s) return false;
    if (auto* b = dynamic_cast<BreakStmt*>(s)) {
        if (wantContinue) return false;
        return b->label.empty() ? !inner : (!label.empty() && b->label == label);
    }
    if (auto* c = dynamic_cast<ContinueStmt*>(s)) {
        if (!wantContinue) return false;
        return c->label.empty() ? !inner : (!label.empty() && c->label == label);
    }
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it) &&
                jumpsTo(std::get<StmtPtr>(it).get(), label, wantContinue, inner)) return true;
        return false;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s))
        return jumpsTo(i->thenBranch.get(), label, wantContinue, inner) ||
               jumpsTo(i->elseBranch.get(), label, wantContinue, inner);
    if (auto* w = dynamic_cast<WhileStmt*>(s))   return jumpsTo(w->body.get(), label, wantContinue, true);
    if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) return jumpsTo(dw->body.get(), label, wantContinue, true);
    if (auto* f = dynamic_cast<ForStmt*>(s))     return jumpsTo(f->body.get(), label, wantContinue, true);
    if (auto* fi = dynamic_cast<ForInStmt*>(s))  return jumpsTo(fi->body.get(), label, wantContinue, true);
    if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        // A switch captures an unlabeled `break`, but not a `continue`.
        bool in = wantContinue ? inner : true;
        for (auto& c : sw->cases)
            for (auto& it : c.stmts)
                if (std::holds_alternative<StmtPtr>(it) &&
                    jumpsTo(std::get<StmtPtr>(it).get(), label, wantContinue, in)) return true;
        return false;
    }
    if (auto* m = dynamic_cast<MatchStmt*>(s)) {   // match is not a jump target
        for (auto& arm : m->arms)
            if (jumpsTo(arm.body.get(), label, wantContinue, inner)) return true;
        return false;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        if (jumpsTo(t->body.get(), label, wantContinue, inner)) return true;
        for (auto& c : t->catches)
            if (jumpsTo(c.body.get(), label, wantContinue, inner)) return true;
        return jumpsTo(t->finally.get(), label, wantContinue, inner);
    }
    return false;   // a defer body may not jump out of itself (rejected separately)
}

// Can executing `s` fall through to the following statement? Conservative in the
// safe direction: when unsure it answers "yes", which at worst asks for a return.
bool canCompleteNormally(Stmt* s) {
    if (!s) return true;
    if (dynamic_cast<ReturnStmt*>(s) || dynamic_cast<ThrowStmt*>(s) ||
        dynamic_cast<BreakStmt*>(s) || dynamic_cast<ContinueStmt*>(s)) return false;
    if (auto* b = dynamic_cast<BlockStmt*>(s)) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it) &&
                !canCompleteNormally(std::get<StmtPtr>(it).get())) return false;
        return true;
    }
    if (auto* i = dynamic_cast<IfStmt*>(s))
        return !i->elseBranch || canCompleteNormally(i->thenBranch.get()) ||
               canCompleteNormally(i->elseBranch.get());
    if (auto* w = dynamic_cast<WhileStmt*>(s))
        return !condAlwaysTrue(w->condition.get()) || jumpsTo(w->body.get(), w->label, false, false);
    if (auto* f = dynamic_cast<ForStmt*>(s))
        return !condAlwaysTrue(f->condition.get()) || jumpsTo(f->body.get(), f->label, false, false);
    if (auto* dw = dynamic_cast<DoWhileStmt*>(s)) {
        // The body runs at least once; the condition is reached when the body completes
        // or `continue`s, and the loop exits there unless the condition is always true.
        if (jumpsTo(dw->body.get(), dw->label, false, false)) return true;
        bool reachesCond = canCompleteNormally(dw->body.get()) ||
                           jumpsTo(dw->body.get(), dw->label, true, false);
        return reachesCond && !condAlwaysTrue(dw->condition.get());
    }
    // for-in iterates a possibly-empty collection.
    if (dynamic_cast<ForInStmt*>(s)) return true;
    if (auto* sw = dynamic_cast<SwitchStmt*>(s)) {
        // Cases fall through in order, so without a `break` out of it the switch
        // completes only if there is no default or the last case group completes.
        bool hasDefault = false;
        for (auto& c : sw->cases) if (!c.value) hasDefault = true;
        if (!hasDefault || sw->cases.empty()) return true;
        for (auto& c : sw->cases)
            for (auto& it : c.stmts)
                if (std::holds_alternative<StmtPtr>(it) &&
                    jumpsTo(std::get<StmtPtr>(it).get(), "", false, false)) return true;
        for (auto& it : sw->cases.back().stmts)
            if (std::holds_alternative<StmtPtr>(it) &&
                !canCompleteNormally(std::get<StmtPtr>(it).get())) return false;
        return true;
    }
    // A `match` is verified exhaustive separately; it completes if any arm does.
    if (auto* m = dynamic_cast<MatchStmt*>(s)) {
        if (m->arms.empty()) return true;
        for (auto& arm : m->arms) if (canCompleteNormally(arm.body.get())) return true;
        return false;
    }
    if (auto* t = dynamic_cast<TryStmt*>(s)) {
        if (t->finally && !canCompleteNormally(t->finally.get())) return false;
        if (canCompleteNormally(t->body.get())) return true;
        for (auto& c : t->catches) if (canCompleteNormally(c.body.get())) return true;
        return false;
    }
    return true;
}

bool stmtAlwaysReturns(Stmt* s) { return !canCompleteNormally(s); }
} // namespace

bool stmtCanCompleteNormally(Stmt* s) { return canCompleteNormally(s); }

void TypeChecker::checkTypeParams(ASTNode* at, const std::string& owner, const std::vector<std::string>& tps) {
    std::set<std::string> seen;
    for (const auto& tp : tps)
        if (!seen.insert(tp).second)
            errorAt(at, "duplicate type parameter '" + tp + "' in '" + owner + "'");
}

void TypeChecker::visit(FunctionDecl* node) {
    localAddrHolders.clear();
    localAddrFn = node->name;
    if (!inInstance) {
        checkTypeParams(node, fnDisplay(node->name), node->typeParams);
        // `must_use` asks the caller to use a result; a void function has none.
        if (node->mustUse && tyq::strip(node->returnType) == "void")
            errorAt(node, "'must_use' on '" + fnDisplay(node->name) + "', which returns void (there is no result to use)");
    }
    if (!node->typeParams.empty()) {
        // Template body: type-checking is deferred to instantiation, but lambda
        // captures must be resolved now (codegen has no equivalent pass). See
        // TemplateCapturePass — purely additive, type-independent.
        if (node->body) { TemplateCapturePass p; p.run(node); }
        // The definite-return check is structural (control-flow only), so it
        // applies to a template body too: a generic function declared to return
        // a value must return on every path regardless of the type argument.
        // (Instantiation reuses this body and never re-runs visit(FunctionDecl),
        // so this is the only place the template is checked.)
        if (node->body && !node->isAsync && node->returnType != "void" &&
            !stmtAlwaysReturns(node->body.get())) {
            errorAt(node, "missing return in non-void function '" + node->name +
                          "' (control can reach the end without returning a " +
                          node->returnType + ")");
        }
        return;
    }

    // `main` is the program entry point: its return value is the process exit code,
    // so it must return `int`. A `void main()` leaves the exit code as whatever garbage
    // is in the return register (undefined, and platform-dependent).
    if (node->name == "main" && normalizeType(node->returnType) != "int" &&
        normalizeType(node->returnType) != "int32")
        errorAt(node, "'main' must return int (its return value is the process exit code); "
                      "got '" + node->returnType + "'");

    // An operator overload extends an operator to a user type: one operand must be a
    // struct/union, a sum type or an interface value (operators on built-in operands,
    // pointers included, keep their built-in meaning).
    if (!node->operatorSym.empty() && !inInstance) {
        bool userOperand = false;
        for (const auto& p : node->params) {
            std::string t = normalizeType(p.first);
            if (!isPointerType(t) && (t.rfind("struct:", 0) == 0 || adtEnums.count(t) || interfaceDecls.count(t)))
                userOperand = true;
        }
        if (!userOperand)
            errorAt(node, "'" + fnDisplay(node->name) + "' does not overload an operator for a user type: "
                          "an operand must be a struct, union, sum type or interface (built-in operands keep "
                          "their built-in meaning)");
        // Operand count: `!`, `~` and unary `-` take one, every other operator two.
        const std::string& op = node->operatorSym;
        size_t want = (op == "u-" || op == "!" || op == "~") ? 1 : 2;
        if (node->params.size() != want) {
            std::string shown = op == "u-" ? "-" : op;
            errorAt(node, "operator '" + shown + "' takes " +
                          (op == "-" || op == "u-" ? std::string("1 or 2 parameters") :
                           want == 1 ? std::string("1 parameter") : std::string("2 parameters")) +
                          ", got " + std::to_string(node->params.size()));
        }
    }

    // The C runtime calls `main` and uses its return value at once; an async main would
    // return a future nobody runs.
    if (node->name == "main" && node->isAsync)
        errorAt(node, "'main' cannot be async: the program's entry point must return int directly "
                      "(run an async function from main with an executor)");
    // `main` is called by the C runtime as `main()` or `main(argc, argv[, envp])`.
    if (node->name == "main" && !node->params.empty()) {
        auto isArgv = [&](const std::string& t) {
            std::string s = normalizeType(tyq::strip(t));
            if (pointerDepth(s) == 1) {
                std::string base = s.front() == '*' ? s.substr(1) : s.substr(0, s.size() - 1);
                return base == "string";
            }
            if (pointerDepth(s) == 2) {
                std::string base = s;
                while (!base.empty() && base.front() == '*') base.erase(0, 1);
                while (!base.empty() && base.back() == '*') base.pop_back();
                return base == "char" || base == "uint8" || base == "int8";
            }
            return false;
        };
        std::string a0 = normalizeType(tyq::strip(node->params[0].first));
        bool ok = (node->params.size() == 2 || node->params.size() == 3) && (a0 == "int" || a0 == "int32");
        for (size_t i = 1; ok && i < node->params.size(); ++i) ok = isArgv(node->params[i].first);
        if (!ok)
            errorAt(node, "'main' must take no parameters or (int argc, string* argv): the C runtime calls it that way");
    }

    // Parameter and return types must name known types.
    std::set<std::string> paramNames;
    for (const auto& param : node->params) {
        if (param.first == "...") continue;
        if (!param.second.empty() && !paramNames.insert(param.second).second)
            errorAt(node, "duplicate parameter '" + param.second + "' in function '" + fnDisplay(node->name) + "'");
        validateStructType(normalizeType(param.first), node);
        if (std::string vm = voidTypeError(param.first); !vm.empty())
            errorAt(node, "parameter '" + param.second + "' " + vm);
    }
    validateStructType(normalizeType(node->returnType), node);

    // Record definition location
    if (!inInstance) definitionLocations[node->name] = {node->line, node->col, diagFile()};
    // -Wall: track top-level functions for unused-function reporting (skip main).
    if (node->name != "main" && !inInstance && !node->fromImport) definedFns[node->name] = {node->line, node->col};

    currentFunctionReturnType = node->returnType;   // inner T (async body returns T)
    currentRawReturnType = inInstance && !instRawReturnType.empty() ? instRawReturnType : node->returnType;
    bool prevInAsync = inAsyncFn;
    inAsyncFn = node->isAsync;
    bool prevVariadic = inVariadicFn;
    inVariadicFn = !node->params.empty() && node->params.back().first == "...";
    bool prevAwaitSeen = awaitSeenInFn;
    awaitSeenInFn = false;
    pushScope();

    // Define parameters (preserving a pointee-const qualifier so writing through
    // a `const T*` parameter is caught; normalization otherwise strips const).
    for (size_t pi = 0; pi < node->params.size(); ++pi) {
        const auto& param = node->params[pi];
        std::string pt = normalizeType(param.first);
        if (pt == "int" && plainEnumDecls.count(tyq::strip(dealiasOperand(param.first))))
            pt = tyq::strip(dealiasOperand(param.first));   // keep enum name for `match`
        if (tyq::baseConst(param.first) && tyq::isPtr(param.first)) pt = "const " + pt;
        // The parameter's own position when the parser recorded it (else the function's).
        int pl = node->line, pc = node->col;
        if (pi < node->paramPositions.size()) { pl = node->paramPositions[pi].first; pc = node->paramPositions[pi].second; }
        defineSymbol(param.second, pt, pl, pc, /*isParam=*/true);
        // A parameter's own position: its name hovers and resolves like a local's.
        if (pi < node->paramPositions.size() && pl > 0 && inPrimaryFile())
            hoverSyms.push_back({pl, pc, (int)param.second.size(),
                                 pt.rfind("struct:", 0) == 0 ? pt.substr(7) : pt});
        if (!node->body) scopes.back()[param.second].used = true;   // a prototype's names are documentation
        // `const int x` (or `T*const p`): the parameter itself may not be reassigned.
        if (tyq::bindingConst(param.first)) scopes.back()[param.second].isConst = true;
    }

    // Escape-soundness: a non-`escaping` closure parameter may only be *called*.
    // Any other use (returned, stored, passed as an argument, captured) lets the
    // closure outlive the call, which is unsound unless its env is heap-allocated
    // — so it must be marked `escaping`. Track such params and verify after the body.
    std::set<std::string> prevWatch = nonEscapingFnParams;
    std::set<std::string> prevEscaped = escapedFnParams;
    auto prevCaptures = std::move(watchedCaptures);
    auto prevLambdaLocal = std::move(lambdaLocal);
    auto prevLambdaLocals = std::move(lambdaLocals), prevEscapedLocals = std::move(escapedLambdaLocals);
    auto prevLocalCaptures = std::move(localCaptures);
    auto prevAsyncLocals = std::move(asyncLocalLambdas);
    nonEscapingFnParams.clear();
    escapedFnParams.clear();
    watchedCaptures.clear();
    lambdaLocal.clear();
    lambdaLocals.clear();
    escapedLambdaLocals.clear();
    localCaptures.clear();
    asyncLocalLambdas.clear();
    for (size_t i = 0; i < node->params.size(); ++i) {
        const std::string& pty = node->params[i].first;
        bool isFn = pty.size() > 3 && pty.substr(0, 3) == "fn(";
        bool marked = i < node->paramEscaping.size() && node->paramEscaping[i];
        if (isFn && !marked) nonEscapingFnParams.insert(node->params[i].second);
    }

    // Type check body
    if (node->body) {
        node->body->accept(this);
    }

    // Flag reads of uninitialized scalar locals (conservative straight-line scan); under
    // -Wall, warn about the reads that are uninitialized on some path. An async body is
    // skipped: its locals become frame fields of the lowered state machine.
    if (node->body && !node->isAsync &&
        !checkUninitPrefix(dynamic_cast<BlockStmt*>(node->body.get())) && warnAll && !inInstance)
        warnMaybeUninit(node);

    // A non-void function must return on every path; falling off the end is an
    // error (there is no implicit zero return). `void` may fall off; `async`
    // functions complete their future implicitly and are exempt.
    if (node->body && !inInstance && !node->isAsync && node->returnType != "void" &&
        !stmtAlwaysReturns(node->body.get())) {
        errorAt(node, "missing return in non-void function '" + node->name +
                      "' (control can reach the end without returning a " +
                      node->returnType + ")");
    }

    // An `async fn` must contain at least one `await` — the state-machine
    // transform needs a suspend point. Report here, where the location is known.
    if (node->isAsync && !awaitSeenInFn) {
        errorAt(node, "async function '" + node->name + "' has no `await`; "
                      "remove `async` or add an `await`");
    }

    // Escape soundness is enforced on non-generic functions only (as before per-instance
    // checking existed): it counts passing a closure param down to another call as an
    // escape, which generic helpers such as sort<T> rely on.
    // A closure param captured by a lambda that outlives the call (any lambda not passed
    // straight to a non-`escaping` param, nor bound to a local that is only called)
    // escapes with it.
    // A lambda bound to a local that is only called gets its env on the stack. The local
    // escapes when it is used beyond a call, or captured by a lambda that outlives the
    // call. A local of an async body keeps a heap env: it lives in the frame across
    // suspensions, and the async lowering frees it with the frame.
    auto outlives = [&](LambdaExpr* l) {
        // In an instance a lambda's env is not decided yet (finalizeGenericEscapes), and a
        // stamp left by an earlier check is not this instance's: it outlives the call.
        if (inInstance) return true;
        if (!l->escapes) return false;
        auto b = lambdaLocal.find(l);
        return b == lambdaLocal.end() || escapedLambdaLocals.count(b->second) || asyncLocalLambdas.count(l);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [lam, name] : localCaptures)
            if (!escapedLambdaLocals.count(name) && outlives(lam)) {
                escapedLambdaLocals.insert(name);
                changed = true;
            }
    }
    for (const auto& [lam, name] : lambdaLocal)
        if (!escapedLambdaLocals.count(name) && !asyncLocalLambdas.count(lam)) lam->escapes = false;
    for (const auto& [lam, name] : watchedCaptures) {
        if (!lam->escapes && !inInstance) continue;
        auto bound = lambdaLocal.find(lam);
        if (bound != lambdaLocal.end() && !escapedLambdaLocals.count(bound->second)) continue;
        escapedFnParams.insert(name);
    }
    // An instance of a free generic records, per closure param, whether it escaped here.
    if (inInstance && curTemplate && node->params.size() == curTemplate->params.size()) {
        for (size_t i = 0; i < node->params.size(); ++i) {
            const std::string& pty = node->params[i].first;
            if (pty.size() <= 3 || pty.substr(0, 3) != "fn(") continue;
            bool marked = i < node->paramEscaping.size() && node->paramEscaping[i];
            bool& e = genericParamEscapes[{curTemplate, i}];
            e = e || marked || node->isAsync || escapedFnParams.count(node->params[i].second);
        }
    }
    for (size_t i = 0; i < node->params.size() && !inInstance; ++i) {
        if (escapedFnParams.count(node->params[i].second)) {
            errorAt(node, "closure parameter '" + node->params[i].second +
                "' escapes (used beyond a direct call); mark it `escaping`");
        }
    }
    nonEscapingFnParams = prevWatch;
    escapedFnParams = prevEscaped;
    watchedCaptures = std::move(prevCaptures);
    lambdaLocal = std::move(prevLambdaLocal);
    lambdaLocals = std::move(prevLambdaLocals);
    escapedLambdaLocals = std::move(prevEscapedLocals);
    localCaptures = std::move(prevLocalCaptures);
    asyncLocalLambdas = std::move(prevAsyncLocals);

    popScope();
    currentFunctionReturnType = "";
    inAsyncFn = prevInAsync;
    inVariadicFn = prevVariadic;
    awaitSeenInFn = prevAwaitSeen;
}

bool TypeChecker::checkUninitPrefix(BlockStmt* body) {
    if (!body) return false;
    bool reported = false;
    std::set<std::string> uninit;   // scalar locals declared without init, not yet assigned
    std::function<void(Expr*)> scan = [&](Expr* e) {
        if (!e) return;
        // &x initializes x (its address may be written through) — not a read.
        if (auto* u = dynamic_cast<UnaryExpr*>(e); u && u->op == "&")
            if (auto* id = dynamic_cast<IdentExpr*>(u->operand.get())) { uninit.erase(id->name); return; }
        // An assignment (also nested: `x = y = 3`, `let y = (x = 5)`) writes its target
        // after evaluating the value; the target itself is not read.
        if (auto* b = dynamic_cast<BinaryExpr*>(e); b && b->op == "=") {
            scan(b->right.get());
            if (auto* id = dynamic_cast<IdentExpr*>(b->left.get())) uninit.erase(id->name);
            else scan(b->left.get());   // e.g. `arr[x] = …` reads x
            return;
        }
        if (auto* id = dynamic_cast<IdentExpr*>(e)) {
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
            if (auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(item).get())) {
                if (vd->initializer) { scan(vd->initializer.get()); uninit.erase(vd->name); }
                // A `static` local without an initializer is zero (static storage, C).
                else if (vd->isStatic) uninit.erase(vd->name);
                else if (isUninitScalar(vd->type)) uninit.insert(vd->name);
            }
            continue;
        }
        Stmt* s = std::get<StmtPtr>(item).get();
        if (auto* es = dynamic_cast<ExprStmt*>(s)) { scan(es->expr.get()); continue; }
        if (auto* rs = dynamic_cast<ReturnStmt*>(s)) { if (rs->value) scan(rs->value.get()); continue; }
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
        if (auto* u = dynamic_cast<UnaryExpr*>(e); u && u->op == "!") {
            cond(u->operand.get(), onFalse, onTrue);
            return;
        }
        auto* b = dynamic_cast<BinaryExpr*>(e);
        if (!b || !logical(b->op)) { expr(e); onTrue = s; onFalse = s; return; }
        bool isAnd = b->op == "&&";
        std::vector<BinaryExpr*> spine{b};
        while (auto* l = dynamic_cast<BinaryExpr*>(spine.back()->left.get())) {
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
        if (auto* id = dynamic_cast<IdentExpr*>(e)) { read(id); return; }
        if (auto* u = dynamic_cast<UnaryExpr*>(e); u && u->op == "&")
            if (auto* id = dynamic_cast<IdentExpr*>(u->operand.get())) { assign(id->name); return; }
        if (dynamic_cast<SizeofExpr*>(e)) return;   // not evaluated
        if (auto* b = dynamic_cast<BinaryExpr*>(e)) {
            if (b->op == "=") {
                expr(b->right.get());
                if (auto* id = dynamic_cast<IdentExpr*>(b->left.get())) assign(id->name);
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
            while (auto* l = dynamic_cast<BinaryExpr*>(spine.back()->left.get())) {
                if (l->op == "=" || logical(l->op) != isLogical) break;
                spine.push_back(l);
            }
            expr(spine.back()->left.get());
            St after = s;
            for (size_t i = spine.size(); i-- > 0;) expr(spine[i]->right.get());
            if (isLogical) s = after;
            return;
        }
        if (auto* t = dynamic_cast<TernaryExpr*>(e)) {
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
        if (auto* q = dynamic_cast<QuestionExpr*>(e)) {
            expr(q->operand.get());
            runDefers(0);   // `?` may return from here
            return;
        }
        if (auto* lam = dynamic_cast<LambdaExpr*>(e)) {
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
        auto* vd = dynamic_cast<VarDecl*>(std::get<DeclPtr>(it).get());
        if (!vd) return;
        expr(vd->initializer.get());
        declare(vd->name, !vd->initializer && !vd->isStatic && !vd->isExtern && isTracked(vd));
    }

    // A statement in its own scope (a block, or an unbraced body).
    void block(Stmt* body) {
        pushScope();
        if (auto* b = dynamic_cast<BlockStmt*>(body)) for (auto& it : b->items) item(it);
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
        auto* lit = dynamic_cast<LiteralExpr*>(c);
        if (!lit) return false;
        if (lit->kind == LiteralExpr::Kind::BOOL) return lit->value == "true";
        if (lit->kind == LiteralExpr::Kind::INT)  return lit->value != "0";
        return false;
    }

    void stmt(Stmt* st) {
        if (!st) return;
        if (dynamic_cast<BlockStmt*>(st)) { block(st); return; }
        if (auto* es = dynamic_cast<ExprStmt*>(st)) { expr(es->expr.get()); return; }
        if (auto* r = dynamic_cast<ReturnStmt*>(st)) { expr(r->value.get()); runDefers(0); s.live = false; return; }
        if (auto* th = dynamic_cast<ThrowStmt*>(st)) { expr(th->value.get()); s.live = false; return; }
        if (auto* br = dynamic_cast<BreakStmt*>(st)) { jump(br->label, false); return; }
        if (auto* co = dynamic_cast<ContinueStmt*>(st)) { jump(co->label, true); return; }
        if (auto* d = dynamic_cast<DeferStmt*>(st)) { if (s.live) defers.back().push_back(d->body.get()); return; }
        if (auto* a = dynamic_cast<AsmStmt*>(st)) { for (auto& in : a->inputs) expr(in.second.get()); return; }
        if (auto* tj = dynamic_cast<ThreadJoinStmt*>(st)) { expr(tj->tid.get()); return; }
        if (auto* i = dynamic_cast<IfStmt*>(st)) {
            // An `else if` chain is walked with a loop.
            St out = dead();
            for (IfStmt* n = i; n;) {
                St thenSt, elseSt;
                cond(n->condition.get(), thenSt, elseSt);
                s = std::move(thenSt);
                block(n->thenBranch.get());
                join(out, s);
                s = std::move(elseSt);
                auto* next = dynamic_cast<IfStmt*>(n->elseBranch.get());
                if (!next) {
                    if (n->elseBranch) block(n->elseBranch.get());
                    join(out, s);
                }
                n = next;
            }
            s = std::move(out);
            return;
        }
        if (auto* w = dynamic_cast<WhileStmt*>(st)) {
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
        if (auto* dw = dynamic_cast<DoWhileStmt*>(st)) {
            pushFrame(true, dw->label);
            block(dw->body.get());
            join(s, frames.back().cont);
            expr(dw->condition.get());
            Frame f = popFrame();
            if (alwaysTrue(dw->condition.get())) s.live = false;
            join(s, f.brk);
            return;
        }
        if (auto* f = dynamic_cast<ForStmt*>(st)) {
            pushScope();
            // The init's declarations belong to the loop.
            if (auto* ib = dynamic_cast<BlockStmt*>(f->init.get())) { for (auto& it : ib->items) item(it); }
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
        if (auto* fi = dynamic_cast<ForInStmt*>(st)) {
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
        if (auto* sw = dynamic_cast<SwitchStmt*>(st)) {
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
        if (auto* m = dynamic_cast<MatchStmt*>(st)) {
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
        if (auto* t = dynamic_cast<TryStmt*>(st)) {
            St before = s;
            block(t->body.get());
            St out = s;
            for (auto& c : t->catches) {
                s = before;
                pushScope();
                declare(c.name, false);
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

// Is `e` a compile-time constant initializer codegen can fold? A literal, `sizeof`, an
// enum member, a top-level `const`, the address of a global, a non-capturing lambda, and
// unary/binary/ternary/cast combinations of those, or an array/struct literal built from
// constants. A call or a read of a non-const variable is not constant (as in C), since
// it would need code to run before `main`.
bool TypeChecker::isConstInit(const ExprPtr& e) const {
    if (!e) return true;
    if (dynamic_cast<LiteralExpr*>(e.get())) return true;
    if (dynamic_cast<SizeofExpr*>(e.get())) return true;
    if (auto* lam = dynamic_cast<LambdaExpr*>(e.get())) return lam->captures.empty();
    if (auto* id = dynamic_cast<IdentExpr*>(e.get())) {
        if (enumConstants.count(id->name)) return true;
        // Only a global `const` (scopes[0]) folds; a local const may be a runtime value.
        int si = scopeOf(id->name);
        // A top-level function name decays to a constant closure (`Op g = add;`).
        if (si < 0) return functionSignatures.count(id->name) > 0;
        return si == 0 && scopes[0].find(id->name)->second.isConst;
    }
    if (auto* u = dynamic_cast<UnaryExpr*>(e.get())) {
        if (u->op == "&") {
            // The address of a global variable is a link-time constant.
            auto* id = dynamic_cast<IdentExpr*>(u->operand.get());
            if (!id || functionSignatures.count(id->name)) return false;
            return scopeOf(id->name) == 0;
        }
        return (u->op == "-" || u->op == "~" || u->op == "!" || u->op == "+") && isConstInit(u->operand);
    }
    if (auto* b = dynamic_cast<BinaryExpr*>(e.get())) {
        static const std::set<std::string> ops = {"+","-","*","/","%","&","|","^","<<",">>",
            "==","!=","<",">","<=",">=","&&","||"};
        // Down the left spine with a loop (a long `A + B + C ...` is as deep as it is long).
        for (;;) {
            if (!ops.count(b->op) || b->opFunc.size() || !isConstInit(b->right)) return false;
            auto* l = dynamic_cast<BinaryExpr*>(b->left.get());
            if (!l) return isConstInit(b->left);
            b = l;
        }
    }
    if (auto* t = dynamic_cast<TernaryExpr*>(e.get()))
        return isConstInit(t->condition) && isConstInit(t->thenExpr) && isConstInit(t->elseExpr);
    if (auto* c = dynamic_cast<CastExpr*>(e.get())) return isConstInit(c->expr);
    if (auto* a = dynamic_cast<ArrayLitExpr*>(e.get())) {
        for (auto& el : a->elements) if (!isConstInit(el)) return false;
        return true;
    }
    if (auto* s = dynamic_cast<StructInitExpr*>(e.get())) {
        for (auto& fi : s->fieldInits) if (!isConstInit(fi.second)) return false;
        return true;
    }
    return false;
}

// The type a bare integer-literal range bound takes: the first of int, int64, uint64
// that holds its value (C's rule for an unsuffixed literal, extended to uint64).
static std::string rangeLiteralType(const std::string& v) {
    bool neg = !v.empty() && v[0] == '-';
    unsigned long long mag = 0;
    try { mag = std::stoull(neg ? v.substr(1) : v, nullptr, 0); }
    catch (...) { return "int"; }
    if (mag <= (neg ? 2147483648ULL : 2147483647ULL)) return "int";
    if (neg || mag <= 9223372036854775807ULL) return "int64";
    return "uint64";
}

// Checks a (possibly nested) array literal against a (possibly
// multi-dimensional) array type: `int[2][3]` expects two rows, each itself a
// `{...}` of up to three ints; a short list zero-fills. Through a type alias
// (`type A3 = int[3]; A3 a = {...}`) the target is the array, at any level.
// Errors are reported at `at`.
void TypeChecker::checkArrayLiteral(const std::string& arrT0, ArrayLitExpr* a, ASTNode* at) {
    auto dealiasArr = [&](std::string t) {
        for (int hops = 0; hops < 32; ++hops) {
            auto al = typeAliases.find(tyq::strip(t));
            if (al == typeAliases.end()) break;
            t = al->second;
        }
        return t;
    };
    std::string arrT = dealiasArr(arrT0);
    ty::Type ty = ty::Type::parse(arrT);
    if (ty.kind != ty::Type::Kind::Array) {
        errorAt(at, "an array literal '{...}' can only initialize an array type, not '" + arrT + "'");
        return;
    }
    std::string elemT = ty.elem->str();
    const std::string& dim = ty.dim;
    bool dimNum = !dim.empty() &&
        std::all_of(dim.begin(), dim.end(), [](unsigned char c){ return std::isdigit(c); });
    if (dimNum && a->elements.size() > (size_t)std::stoull(dim))
        errorAt(at, "array literal has " + std::to_string(a->elements.size()) +
                    " elements but '" + arrT + "' holds " + dim);
    bool elemIsArray = ty::Type::parse(dealiasArr(elemT)).kind == ty::Type::Kind::Array;
    for (auto& el : a->elements) {
        if (elemIsArray) {
            if (auto* sub = dynamic_cast<ArrayLitExpr*>(el.get()))
                checkArrayLiteral(elemT, sub, at);
            else
                errorAt(at, "nested array initializer expected '{...}' for element type '" + elemT + "'");
        } else {
            std::string et = getExpressionType(el.get());
            std::string e = assignabilityError(elemT, et, el.get());
            if (!e.empty()) errorAt(at, "array element: " + e);
        }
    }
}

void TypeChecker::visit(VarDecl* node) {
    // `extern <type> <name>;` names a variable defined in another translation unit
    // (a C global). It lives at top level and carries no initializer.
    ASTNode* savedCtx = posCtx;
    if (node->line > 0) posCtx = node;
    struct CtxRestore { ASTNode*& p; ASTNode* v; ~CtxRestore() { p = v; } } ctxRestore{posCtx, savedCtx};
    if (node->isExtern) {
        if (scopes.size() > 1)
            errorAt(node, "'extern' is only allowed on a top-level variable");
        if (node->initializer)
            errorAt(node, "an 'extern' variable cannot have an initializer");
    }

    // `static` is a local-only qualifier whose initializer must be a compile-time
    // constant (it runs once, at load time), as in C. Accept every form codegen's
    // constant folder can emit — a literal, a numeric unary/cast of one, or an
    // array/struct literal (or sizeof) built from those — not just a bare literal.
    if (node->isStatic) {
        if (scopes.size() <= 1)
            errorAt(node, "'static' is only allowed on a local variable");
        if (node->initializer && !isConstInit(node->initializer))
            errorAt(node, "a 'static' local's initializer must be a constant");
    }

    // Record definition location
    if (node->line > 0 && !inInstance)
        definitionLocations[node->name] = {node->line, node->col, diagFile()};
    // A `fn(...)` local declared without a value can still be a lambda local: lambdas
    // assigned to it later (`f = <lambda>`) are bound to it.
    if (!node->initializer && scopes.size() > 1 && !node->isStatic && !inInstance &&
        ty::Type::parse(node->type).isFn())
        lambdaLocals.insert(node->name);
    if (node->initializer) {
        // Reconcile a lambda initializer's return type with a declared fn(...)->R
        // target BEFORE checking its body. A mismatched lambda header (e.g. an
        // `int(int x)` used as `fn(int)->float`) would otherwise emit a function
        // whose return type disagrees with the closure's call ABI — correct at -O0
        // by luck, a silent miscompile (0.0) under -O2. Setting the lambda's return
        // type to R lets its `return` coerce to R through the normal path.
        if (auto* lam = dynamic_cast<LambdaExpr*>(node->initializer.get())) {
            ty::Type dt = ty::Type::parse(node->type);
            if (dt.isFn() && dt.ret && dt.ret->str() != lam->returnType)
                lam->returnType = dt.ret->str();
        }
        hintIfaceTarget(node->initializer.get(), node->type);
        inferVariantTarget(node->initializer, node->type, true);
        node->initializer->accept(this);
        // A lambda bound to a local that is only ever called does not outlive the call.
        if (auto* lam = dynamic_cast<LambdaExpr*>(node->initializer.get());
            lam && scopes.size() > 1 && !node->isStatic && !inInstance) {
            lambdaLocal[lam] = node->name;
            lambdaLocals.insert(node->name);
            if (inAsyncFn) asyncLocalLambdas.insert(lam);
        }
        // A `for (i in A..B)` bound decl takes its bound's integer type (promoted to at
        // least `int`); visit(ForStmt) then widens both decls to their common type.
        bool badRangeBound = false;
        if (node->rangeBound) {
            std::string bt = getExpressionType(node->initializer.get());
            if (auto* lit = dynamic_cast<LiteralExpr*>(node->initializer.get()))
                if (lit->kind == LiteralExpr::Kind::INT) bt = rangeLiteralType(lit->value);
            if (bt != "unknown") {
                std::string nt = tyq::strip(normalizeType(tyq::strip(bt)));
                std::string rt = ty::rangeVarType(nt, "int");
                if (rt.empty()) {
                    errorAt(node->initializer.get(), "range bound must be an integer, got '" + nt + "'");
                    badRangeBound = true;
                } else {
                    node->type = rt;
                }
            }
        }
        // A global's initializer is evaluated at compile time (there is no code before
        // `main` to run it), so it must be a constant expression, as in C.
        if (scopes.size() <= 1 && !node->isStatic && !node->scriptInit && !isConstInit(node->initializer))
            errorAt(node, "initializer of global '" + node->name + "' is not a compile-time constant");
        // Array literal `= {..}`: the target must be a fixed-size array; check the
        // element count (fewer than the size zero-fill, C-style) and element types.
        if (auto* arr = dynamic_cast<ArrayLitExpr*>(node->initializer.get())) {
            // Recursively check a (possibly nested) array literal against a
            // (possibly multi-dimensional) array type: `int[2][3]` expects two rows,
            // each itself a `{...}` of up to three ints. A short list zero-fills.
            // Through a type alias (`type A3 = int[3]; A3 a = {...}`) the target is the array,
            // at any level (`A3[2] m = {{..}, {..}}`).
            checkArrayLiteral(node->type, arr, node);
        } else if (!badRangeBound) {
            std::string initType = getExpressionType(node->initializer.get());
            if (initType != "unknown") {
                if (dropsConstQual(node->type, initType))
                    errorAt(node, "cannot initialize '" + node->type + "' from '" + initType +
                                  "': conversion discards a const qualifier");
                else {
                    std::string e = assignabilityError(node->type, initType, node->initializer.get());
                    if (!e.empty()) errorAt(node, "initializing '" + node->name + "': " + e);
                }
            }
        }
    }
    // A const must be initialized — there is no later point to assign it. An
    // `extern const` is the exception: its definition (and value) lives elsewhere.
    if (node->isConst && !node->initializer && !node->isExtern) {
        errorAt(node, "const '" + node->name + "' must be initialized");
    }

    // Normalize the type (e.g., "Point" -> "struct:Point")
    std::string normalizedType = normalizeType(node->type);
    // A classic (payload-less) enum collapses to `int` in normalizeType, but keep its
    // nominal name on the variable so `match` can recover the variant set. It still behaves
    // as an int everywhere else (every other check runs the type back through normalizeType).
    if (normalizedType == "int" && plainEnumDecls.count(tyq::strip(dealiasOperand(node->type))))
        normalizedType = tyq::strip(dealiasOperand(node->type));

    // Validate that struct types exist before use
    validateStructType(normalizedType, node);
    if (std::string vm = voidTypeError(node->type); !vm.empty())
        errorAt(node, "variable '" + node->name + "' " + vm);

    // Preserve a pointee-const qualifier through normalization so the symbol
    // remembers it's read-only (const checks read it back; everything else strips).
    std::string storedType = normalizedType;
    if (tyq::baseConst(node->type) && tyq::isPtr(node->type))
        storedType = "const " + normalizedType;

    // Two locals of the same name in one scope (`let x; let x;`). Globals are checked
    // once for the whole program (checkTopLevelNames); an inner block may shadow.
    // A function's (or lambda's) outermost block shares the parameters' scope, as in C,
    // so a local there may not reuse a parameter's name.
    auto redeclaresParam = [&]() {
        if (scopes.size() < 2) return false;
        auto& outer = scopes[scopes.size() - 2];
        auto it = outer.find(node->name);
        return it != outer.end() && it->second.isParam;
    };
    if (scopes.size() > 1 && (scopes.back().count(node->name) || redeclaresParam()))
        errorAt(node, "redefinition of '" + node->name + "' in the same scope");
    defineSymbol(node->name, storedType, node->line, node->col, /*isParam=*/false);
    if (node->isConst && !scopes.empty()) {
        scopes.back()[node->name].isConst = true;
        scopes.back()[node->name].constInit = node->initializer.get();
    }
    if (node->isStatic && !scopes.empty()) scopes.back()[node->name].isStatic = true;
    if (scopes.size() > 1 && !node->isStatic && !node->isExtern) {
        localAddrHolders.erase(localAddrKey(node->name));
        if (node->initializer) trackLocalAddr(node->name, node->initializer.get());
        // A local this large overflows a thread's stack (an async function's locals
        // live in its heap frame).
        unsigned long long size = 0, align = 0;
        if (!inAsyncFn && constLayout(normalizedType, size, align) && size > kLargeLocalBytes)
            noteWarning("frame-larger-than", node->line, node->col,
                        "local '" + node->name + "' takes " + std::to_string(size) +
                        " bytes of stack; a thread's stack is 512 KiB to 8 MiB, so allocate it with alloc<T> or make it static");
    }
}

void TypeChecker::visit(StructDecl* node) {
    defineSymbol(node->name, "struct:" + node->name);
    checkTypeParams(node, node->name, node->typeParams);
    // A field default (`int t = 30;`) is a constant of the field's type, as a global's
    // initializer; it is what an omitted or unset field of the struct holds.
    for (auto& f : node->fields) {
        if (!f.defaultValue) continue;
        Expr* e = f.defaultValue.get();
        ASTNode* at = e->line > 0 ? static_cast<ASTNode*>(e) : node;
        if (!node->typeParams.empty()) {
            errorAt(at, "field '" + f.name + "' of generic struct '" + node->name + "' cannot have a default value");
            continue;
        }
        if (f.bitWidth != 0) { errorAt(at, "bitfield '" + f.name + "' cannot have a default value"); continue; }
        inferVariantTarget(f.defaultValue, f.type, true);
        f.defaultValue->accept(this);
        if (!isConstInit(f.defaultValue)) {
            errorAt(at, "default value of field '" + f.name + "' is not a compile-time constant");
            continue;
        }
        if (auto* arr = dynamic_cast<ArrayLitExpr*>(e)) {
            checkArrayLiteral(f.type, arr, at);
            continue;
        }
        std::string err = assignabilityError(f.type, getExpressionType(e), e);
        if (!err.empty())
            errorAt(at, "default value of field '" + f.name + "': " + err);
    }
    {
        std::set<std::string> names;
        for (const auto& f : node->fields)
            if (!names.insert(f.name).second)
                errorAt(node, "duplicate field '" + f.name + "' in struct '" + node->name + "'");
        std::set<std::string> methods;
        for (const auto& m : node->methods)
            if (auto* mf = dynamic_cast<FunctionDecl*>(m.get())) {
                if (names.count(mf->name))
                    errorAt(mf, "method '" + mf->name + "' of struct '" + node->name + "' has the same name as a field");
                if (!methods.insert(mf->name).second)
                    errorAt(mf, "duplicate method '" + mf->name + "' in struct '" + node->name + "'");
                for (const auto& p : mf->params)
                    if (p.second == "self")
                        errorAt(mf, "method '" + mf->name + "' of struct '" + node->name +
                                    "' cannot have a parameter named 'self': it is the implicit receiver");
            }
    }
    checkUnnamedBitfields(node, node->name, node->pads);
    // Field types must name known types (a template's fields mention its type params and
    // are checked per instantiation through the instance's type arguments instead).
    if (node->typeParams.empty()) {
        for (const auto& f : node->fields) {
            validateStructType(normalizeType(f.type), node);
            if (std::string vm = voidTypeError(f.type); !vm.empty())
                errorAt(node, "field '" + f.name + "' of '" + node->name + "' " + vm);
            checkBitfield(node, node->name, f);
        }
        for (const auto& method : node->methods)
            if (auto func = dynamic_cast<FunctionDecl*>(method.get())) {
                for (const auto& p : func->params) {
                    validateStructType(normalizeType(p.first), func);
                    if (std::string vm = voidTypeError(p.first); !vm.empty())
                        errorAt(func, "parameter '" + p.second + "' " + vm);
                }
                validateStructType(normalizeType(func->returnType), func);
            }
    }
    // A generic struct's methods mention its type params: each body is checked per
    // struct instance (queued when `Box<int>` is instantiated). Lambda captures are
    // resolved here, type-independently, as for a generic function.
    if (!node->typeParams.empty()) {
        std::string selfT = "*" + node->name + "<";
        for (size_t i = 0; i < node->typeParams.size(); ++i) selfT += (i ? "," : "") + node->typeParams[i];
        selfT += ">";
        for (const auto& method : node->methods)
            if (auto* func = dynamic_cast<FunctionDecl*>(method.get()); func && func->body) {
                TemplateCapturePass p;
                p.run(func, selfT);
            }
        return;
    }
    // Type-check method bodies
    for (const auto& method : node->methods) {
        if (auto func = dynamic_cast<FunctionDecl*>(method.get())) {
            std::string savedReturn = currentFunctionReturnType, savedRaw = currentRawReturnType;
            currentFunctionReturnType = func->returnType;
            currentRawReturnType = func->returnType;
            bool savedVariadic = inVariadicFn;
            inVariadicFn = !func->params.empty() && func->params.back().first == "...";
            pushScope();
            defineSymbol("self", "*" + node->name);
            for (const auto& p : func->params) {
                defineSymbol(p.second, normalizeType(p.first));
                if (tyq::bindingConst(p.first)) scopes.back()[p.second].isConst = true;
            }
            if (func->body) func->body->accept(this);
            popScope();
            currentFunctionReturnType = savedReturn;
            currentRawReturnType = savedRaw;
            inVariadicFn = savedVariadic;
        }
    }
}

void TypeChecker::visit(ExternDecl* node) {
    // Extern functions are already registered in first pass; verify the signature's types.
    for (const auto& param : node->params) {
        if (param.first == "...") continue;
        validateStructType(normalizeType(param.first), node);
        if (std::string vm = voidTypeError(param.first); !vm.empty())
            errorAt(node, "parameter '" + param.second + "' " + vm);
    }
    validateStructType(normalizeType(node->returnType), node);
}

void TypeChecker::visit(IntrinsicDecl* node) {
    // `intrinsic` is a compiler-provided mechanism, not a user extension point:
    // a name with no codegen lowering must be rejected here, not blow up later.
    if (!isSupportedIntrinsic(node->name)) {
        errorAt(node, "unknown intrinsic '" + node->name +
            "': the compiler provides no lowering for it. `intrinsic` cannot "
            "declare new operations — use `extern` for an external C symbol.");
    }
}

void TypeChecker::foldEnumValues(EnumDecl* ed, bool report) {
    bool any = false;
    for (const auto& v : ed->valueExprs) if (v) any = true;
    if (!any) return;
    bool known = true;
    bool seenExpr = false;
    long long prev = 0;
    for (size_t i = 0; i < ed->members.size(); ++i) {
        auto& m = ed->members[i];
        Expr* ve = i < ed->valueExprs.size() ? ed->valueExprs[i].get() : nullptr;
        long long v = m.second;
        if (ve) {
            seenExpr = true;
            size_t before = errors.size();
            if (report) ve->accept(this);
            long long r = 0;
            known = errors.size() == before && foldConstInt(ve, r);
            if (known) v = r;
            else if (report && errors.size() == before)
                errorAt(ve, "value of enum member '" + m.first + "' of '" + ed->name +
                            "' is not an integer constant expression");
        } else if (seenExpr) {
            v = prev + 1;
        }
        if (known || !seenExpr) m.second = v;
        prev = m.second;
        enumConstants[m.first] = m.second;
    }
}

void TypeChecker::visit(EnumDecl* node) {
    // Members and the enum type were registered in the first pass.
    definitionLocations[node->name] = {node->line, node->col, diagFile()};
    checkTypeParams(node, node->name, node->typeParams);
    if (!node->isADT() && node->typeParams.empty()) foldEnumValues(node, /*report=*/true);
    // A classic enum is an `int`: every member value (explicit, or the previous one + 1)
    // must fit it, instead of wrapping or being truncated.
    if (!node->isADT())
        for (const auto& m : node->members)
            if (m.second < INT32_MIN || m.second > INT32_MAX)
                errorAt(node, "enum member '" + m.first + "' of '" + node->name + "' has value " +
                              std::to_string(m.second) + ", out of range for 'int'");
}

void TypeChecker::visit(TypeAliasDecl* node) {
    // The alias was registered in the first pass; validate the underlying type.
    validateStructType(normalizeType(node->aliased), node);
}

void TypeChecker::visit(InterfaceDecl* node) {
    // Interface registered in first pass; no body to type-check. A method is required once.
    std::set<std::string> names;
    for (const auto& m : node->methods)
        if (!names.insert(m.name).second)
            errorAt(node, "duplicate method '" + m.name + "' in interface '" + node->name + "'");
}

void TypeChecker::visit(UnionDecl* node) {
    // Registered as a struct in the first pass (so field access works and a signature
    // declared before it may name it); here only its field types are validated.
    std::set<std::string> names;
    for (const auto& f : node->fields)
        if (!names.insert(f.name).second)
            errorAt(node, "duplicate field '" + f.name + "' in union '" + node->name + "'");
    checkUnnamedBitfields(node, node->name, node->pads);
    for (const auto& f : node->fields) {
        validateStructType(normalizeType(f.type), node);
        if (std::string vm = voidTypeError(f.type); !vm.empty())
            errorAt(node, "field '" + f.name + "' of '" + node->name + "' " + vm);
    }
}

// The bit width of an integer type a bitfield may have (0: not one).
static int bitfieldTypeWidth(const std::string& normalized) {
    static const std::map<std::string, int> widths = {
        {"bool", 1}, {"char", 8}, {"int8", 8}, {"uint8", 8}, {"int16", 16}, {"uint16", 16},
        {"int", 32}, {"int32", 32}, {"uint", 32}, {"uint32", 32}, {"int64", 64}, {"uint64", 64}};
    auto w = widths.find(normalized);
    return w == widths.end() ? 0 : w->second;
}

void TypeChecker::checkBitfield(ASTNode* at, const std::string& owner, const StructDecl::Field& f) {
    // `at` is null for a generic struct's instance: report where the type is used.
    auto errorAt = [&](ASTNode* node, const std::string& m) {
        if (node) this->errorAt(node, m); else errorAtCtx(m);
    };
    if (f.bitWidth < 0) {
        errorAt(at, "bitfield '" + f.name + "' of '" + owner + "' has zero width (only an unnamed bitfield may be zero-width)");
        return;
    }
    if (f.bitWidth == 0) return;
    int w = bitfieldTypeWidth(normalizeType(f.type));
    if (w == 0) {
        errorAt(at, "bitfield '" + f.name + "' of '" + owner + "' must have an integer type, got '" + f.type + "'");
        return;
    }
    if (f.bitWidth > w)
        errorAt(at, "bitfield '" + f.name + "' of '" + owner + "' is " + std::to_string(f.bitWidth) +
                    " bits wide, more than its type '" + f.type + "' holds (" + std::to_string(w) + ")");
}

void TypeChecker::checkUnnamedBitfields(ASTNode* at, const std::string& owner,
                                        const std::vector<StructDecl::Pad>& pads) {
    for (const auto& p : pads) {
        int w = bitfieldTypeWidth(normalizeType(p.type));
        if (w == 0)
            errorAt(at, "unnamed bitfield of '" + owner + "' must have an integer type, got '" + p.type + "'");
        else if (p.bitWidth > w)
            errorAt(at, "unnamed bitfield of '" + owner + "' is " + std::to_string(p.bitWidth) +
                        " bits wide, more than its type '" + p.type + "' holds (" + std::to_string(w) + ")");
    }
}
