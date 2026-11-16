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
#include "template_capture_pass.h"

// ============================================================================

// TypeChecker — declaration visitors (functions, structs, enums, unions,
// interfaces, type aliases) + the template capture pass.
// Part of the type_checker.cpp split; all methods are TypeChecker members
// declared in type_checker.h.

namespace {
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
    auto* lit = nodeCast<LiteralExpr>(cond);
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
    if (auto* b = nodeCast<BreakStmt>(s)) {
        if (wantContinue) return false;
        return b->label.empty() ? !inner : (!label.empty() && b->label == label);
    }
    if (auto* c = nodeCast<ContinueStmt>(s)) {
        if (!wantContinue) return false;
        return c->label.empty() ? !inner : (!label.empty() && c->label == label);
    }
    if (auto* b = nodeCast<BlockStmt>(s)) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it) &&
                jumpsTo(std::get<StmtPtr>(it).get(), label, wantContinue, inner)) return true;
        return false;
    }
    if (auto* i = nodeCast<IfStmt>(s))
        return jumpsTo(i->thenBranch.get(), label, wantContinue, inner) ||
               jumpsTo(i->elseBranch.get(), label, wantContinue, inner);
    if (auto* w = nodeCast<WhileStmt>(s))   return jumpsTo(w->body.get(), label, wantContinue, true);
    if (auto* dw = nodeCast<DoWhileStmt>(s)) return jumpsTo(dw->body.get(), label, wantContinue, true);
    if (auto* f = nodeCast<ForStmt>(s))     return jumpsTo(f->body.get(), label, wantContinue, true);
    if (auto* fi = nodeCast<ForInStmt>(s))  return jumpsTo(fi->body.get(), label, wantContinue, true);
    if (auto* sw = nodeCast<SwitchStmt>(s)) {
        // A switch captures an unlabeled `break`, but not a `continue`.
        bool in = wantContinue ? inner : true;
        for (auto& c : sw->cases)
            for (auto& it : c.stmts)
                if (std::holds_alternative<StmtPtr>(it) &&
                    jumpsTo(std::get<StmtPtr>(it).get(), label, wantContinue, in)) return true;
        return false;
    }
    if (auto* m = nodeCast<MatchStmt>(s)) {   // match is not a jump target
        for (auto& arm : m->arms)
            if (jumpsTo(arm.body.get(), label, wantContinue, inner)) return true;
        return false;
    }
    if (auto* t = nodeCast<TryStmt>(s)) {
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
    if (nodeCast<ReturnStmt>(s) || nodeCast<ThrowStmt>(s) ||
        nodeCast<BreakStmt>(s) || nodeCast<ContinueStmt>(s)) return false;
    if (auto* b = nodeCast<BlockStmt>(s)) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it) &&
                !canCompleteNormally(std::get<StmtPtr>(it).get())) return false;
        return true;
    }
    if (auto* i = nodeCast<IfStmt>(s))
        return !i->elseBranch || canCompleteNormally(i->thenBranch.get()) ||
               canCompleteNormally(i->elseBranch.get());
    if (auto* w = nodeCast<WhileStmt>(s))
        return !condAlwaysTrue(w->condition.get()) || jumpsTo(w->body.get(), w->label, false, false);
    if (auto* f = nodeCast<ForStmt>(s))
        return !condAlwaysTrue(f->condition.get()) || jumpsTo(f->body.get(), f->label, false, false);
    if (auto* dw = nodeCast<DoWhileStmt>(s)) {
        // The body runs at least once; the condition is reached when the body completes
        // or `continue`s, and the loop exits there unless the condition is always true.
        if (jumpsTo(dw->body.get(), dw->label, false, false)) return true;
        bool reachesCond = canCompleteNormally(dw->body.get()) ||
                           jumpsTo(dw->body.get(), dw->label, true, false);
        return reachesCond && !condAlwaysTrue(dw->condition.get());
    }
    // for-in iterates a possibly-empty collection.
    if (nodeCast<ForInStmt>(s)) return true;
    if (auto* sw = nodeCast<SwitchStmt>(s)) {
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
    if (auto* m = nodeCast<MatchStmt>(s)) {
        if (m->arms.empty()) return true;
        for (auto& arm : m->arms) if (canCompleteNormally(arm.body.get())) return true;
        return false;
    }
    if (auto* t = nodeCast<TryStmt>(s)) {
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

// C has no closures, interfaces, slices or sum types and passes no array by value, and a
// generic instance has no name a C header can keep.
std::string TypeChecker::exportTypeError(const std::string& type) {
    std::string t = normalizeType(tyq::strip(type));
    if (isPointerType(t)) return "";
    ty::Type ty = ty::Type::parse(t);
    std::string name = ty.nominalName();
    switch (ty.kind) {
        case ty::Type::Kind::Fn: return "is a closure, which C cannot call";
        case ty::Type::Kind::Slice: return "is a slice (pass a pointer and a length)";
        case ty::Type::Kind::Array: return "is an array, which C does not pass by value";
        case ty::Type::Kind::Interface: return "is an interface value";
        case ty::Type::Kind::VaList: return "is a va_list";
        case ty::Type::Kind::Template: return "is a generic instance, which has no stable C name (wrap it in a named struct)";
        default: break;
    }
    if (adtEnums.count(name) || adtEnums.count(t)) return "is a sum type, which has no C layout";
    if (interfaceDecls.count(name)) return "is an interface value";
    if (templateInstanceArgs.count(name))
        return "is a generic instance, which has no stable C name (wrap it in a named struct)";
    return "";
}

// `export T f(...)`: a C entry point, so a function C can call by its name with its
// own calling convention.
void TypeChecker::checkExport(FunctionDecl* node) {
    std::string fn = "exported function '" + node->name + "'";
    if (!node->body) errorAt(node, "'export' needs a definition: put it on the body of '" + node->name + "'");
    if (!node->typeParams.empty()) errorAt(node, fn + " cannot be generic (C calls one instance by its name)");
    if (node->isAsync) errorAt(node, fn + " cannot be async (C cannot run the future it returns)");
    if (!node->operatorSym.empty()) errorAt(node, "an operator overload cannot be exported (it has no C name)");
    if (!node->params.empty() && node->params.back().first == "...")
        errorAt(node, fn + " cannot be variadic");
    if (!node->typeParams.empty()) return;
    for (const auto& p : node->params) {
        if (p.first == "...") continue;
        if (std::string why = exportTypeError(p.first); !why.empty())
            errorAt(node, "parameter '" + p.second + "' of " + fn + " " + why);
    }
    if (std::string why = exportTypeError(node->returnType); !why.empty())
        errorAt(node, "the result of " + fn + " " + why);
}

void TypeChecker::visit(FunctionDecl* node) {
    localAddrHolders.clear();
    localAddrFn = node->name;
    if (!inInstance) {
        checkTypeParams(node, fnDisplay(node->name), node->typeParams);
        // `must_use` asks the caller to use a result; a void function has none.
        if (node->mustUse && tyq::strip(node->returnType) == "void")
            errorAt(node, "'must_use' on '" + fnDisplay(node->name) + "', which returns void (there is no result to use)");
        if (node->isExport) checkExport(node);
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
        !checkUninitPrefix(nodeCast<BlockStmt>(node->body.get())) && warnAll && !inInstance)
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
