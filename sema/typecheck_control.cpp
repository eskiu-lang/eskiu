// typecheck_control.cpp: thread_join, throw, try, defer, match and switch statements and constant integers (split from typecheck_stmt.cpp).
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

void TypeChecker::visit(ThreadJoinStmt* node) {
    node->tid->accept(this);
    dropGlobalNarrowings();   // the joined thread may have assigned any global
    std::string t = getExpressionType(node->tid.get());
    std::string n = normalizeType(t);
    if (!n.empty() && n[0] == '?') n = n.substr(1);
    if (t != "unknown" && n != "*void")
        errorAt(node, "thread_join expects a thread handle ('*void' from thread_create), got '" + t + "'");
}

// WebAssembly has C++-style unwinding only through emscripten's runtime.
static bool wasmTarget(const std::string& triple) {
    return (triple.rfind("wasm32", 0) == 0 || triple.rfind("wasm64", 0) == 0) &&
           triple.find("emscripten") == std::string::npos;
}

void TypeChecker::visit(ThrowStmt* node) {
    if (wasmTarget(targetTriple)) errorAt(node, "exceptions are not supported on WebAssembly targets");
    if (node->value) {
        node->value->accept(this);
        if (isVoidValueType(getExpressionType(node->value.get())))
            errorAt(node, "cannot throw a 'void' value");
        if (!inInstance) node->valueType = getExpressionType(node->value.get());
    }
}

void TypeChecker::visit(TryStmt* node) {
    if (wasmTarget(targetTriple)) errorAt(node, "exceptions are not supported on WebAssembly targets");
    if (node->body) node->body->accept(this);
    for (size_t i = 0; i < node->catches.size(); ++i) {
        auto& c = node->catches[i];
        if (c.type.empty() && i + 1 < node->catches.size())
            error(c.line, c.col, "'catch (_)' must be the last catch clause");
        pushScope();
        if (!c.type.empty()) defineSymbol(c.name, c.type, c.line, c.col, false);
        ++catchDepth;
        if (c.body) c.body->accept(this);
        --catchDepth;
        popScope();
    }
    // A `finally` runs on every exit, including an exception unwinding through it, so a
    // `return` there would silently discard the pending exit (the Java/C# footgun).
    if (node->finally) {
        ++finallyDepth;
        node->finally->accept(this);
        --finallyDepth;
    }
}

void TypeChecker::visit(DeferStmt* node) {
    // Type-check the deferred body in its own scope.
    pushScope();
    if (node->body) node->body->accept(this);
    popScope();

    // A defer body runs during scope-exit cleanup, so it may not transfer control
    // out of itself: a `return`, or a `break`/`continue` not enclosed by a loop or
    // switch *within* the body, would jump to a target that is no longer valid. A `?`
    // is an early return too (a lambda in the body is its own function).
    std::function<void(Expr*)> checkExpr = [&](Expr* e) {
        if (!e || nodeCast<LambdaExpr>(e)) return;
        if (nodeCast<QuestionExpr>(e))
            errorAt(e, "'?' is not allowed inside a defer body (it would return from the function)");
        astwalk::forEachChildExprFlat(e, [&](ExprPtr& c) { checkExpr(c.get()); });
    };
    std::function<void(Stmt*, int)> check = [&](Stmt* s, int loopDepth) {
        if (!s) return;
        if (auto* es = nodeCast<ExprStmt>(s)) checkExpr(es->expr.get());
        else if (auto* ts = nodeCast<ThrowStmt>(s)) checkExpr(ts->value.get());
        if (nodeCast<ReturnStmt>(s)) {
            errorAt(s, "'return' is not allowed inside a defer body");
        } else if (nodeCast<BreakStmt>(s) || nodeCast<ContinueStmt>(s)) {
            // A bare break/continue is fine if a loop/switch *inside* the defer body encloses
            // it (loopDepth>0). A *labeled* one may target a loop outside the defer body, so it
            // is rejected regardless of depth.
            std::string lbl;
            if (auto* bs = nodeCast<BreakStmt>(s)) lbl = bs->label;
            else if (auto* cs = nodeCast<ContinueStmt>(s)) lbl = cs->label;
            if (loopDepth == 0 || !lbl.empty())
                errorAt(s, "'break'/'continue' inside a defer body may not escape it");
        } else if (auto* b = nodeCast<BlockStmt>(s)) {
            for (auto& it : b->items) {
                if (auto* st = std::get_if<StmtPtr>(&it)) check(st->get(), loopDepth);
                else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) checkExpr(vd->initializer.get());
            }
        } else if (auto* i = nodeCast<IfStmt>(s)) {
            checkExpr(i->condition.get());
            check(i->thenBranch.get(), loopDepth);
            check(i->elseBranch.get(), loopDepth);
        } else if (auto* w = nodeCast<WhileStmt>(s)) {
            checkExpr(w->condition.get());
            check(w->body.get(), loopDepth + 1);
        } else if (auto* dw = nodeCast<DoWhileStmt>(s)) {
            checkExpr(dw->condition.get());
            check(dw->body.get(), loopDepth + 1);
        } else if (auto* f = nodeCast<ForStmt>(s)) {
            check(f->init.get(), loopDepth);
            checkExpr(f->condition.get()); checkExpr(f->step.get());
            check(f->body.get(), loopDepth + 1);
        } else if (auto* fi = nodeCast<ForInStmt>(s)) {
            checkExpr(fi->iterable.get());
            check(fi->body.get(), loopDepth + 1);
        } else if (auto* sw = nodeCast<SwitchStmt>(s)) {
            checkExpr(sw->subject.get());
            for (auto& c : sw->cases)
                for (auto& it : c.stmts) {
                    if (auto* st = std::get_if<StmtPtr>(&it)) check(st->get(), loopDepth + 1);
                    else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) checkExpr(vd->initializer.get());
                }
        } else if (auto* m = nodeCast<MatchStmt>(s)) {
            checkExpr(m->subject.get());
            for (auto& a : m->arms) check(a.body.get(), loopDepth);
        } else if (auto* t = nodeCast<TryStmt>(s)) {
            check(t->body.get(), loopDepth);
            for (auto& c : t->catches) check(c.body.get(), loopDepth);
            check(t->finally.get(), loopDepth);
        } else if (auto* d = nodeCast<DeferStmt>(s)) {
            check(d->body.get(), loopDepth);
        }
    };
    if (node->body) check(node->body.get(), 0);
}

void TypeChecker::visit(MatchStmt* node) {
    node->subject->accept(this);
    std::string rawSt = getExpressionType(node->subject.get());
    std::string st = normalizeType(rawSt);
    // An alias of a classic enum (`type C = Col`) names that enum's variant set.
    if (plainEnumDecls.count(tyq::strip(dealiasOperand(rawSt)))) rawSt = tyq::strip(dealiasOperand(rawSt));
    // A classic (payload-less) enum normalizes to `int`, so keep its declared name for a
    // `match`: that is the only place the variant set (for exhaustiveness) is recoverable.
    if (!enumDecls.count(st) && !templateInstanceArgs.count(st) && plainEnumDecls.count(rawSt))
        st = rawSt;
    // Resolve the enum decl + (for a generic instance like Option_int) the type-
    // parameter substitutions, so payload types come out concrete.
    EnumDecl* ed = nullptr;
    std::map<std::string, std::string> subs;
    if (enumDecls.count(st)) {
        ed = enumDecls[st];                                  // concrete ADT enum
    } else if (templateInstanceArgs.count(st)) {             // generic instance
        auto& inst = templateInstanceArgs[st];
        auto git = genericEnumDecls.find(inst.first);
        if (git != genericEnumDecls.end()) {
            ed = git->second;
            for (size_t i = 0; i < ed->typeParams.size() && i < inst.second.size(); ++i)
                subs[ed->typeParams[i]] = inst.second[i];
        }
    } else if (plainEnumDecls.count(st)) {                   // classic int enum (no payloads)
        ed = plainEnumDecls[st];
    }
    if (!ed && st != "unknown")
        errorAt(node, "match subject must be an enum, got " + st);
    if (!inInstance) node->enumName = st;
    // Index of a variant within `ed` by name (-1 if absent).
    auto variantIndex = [&](const std::string& v) -> int {
        if (!ed) return -1;
        for (size_t i = 0; i < ed->members.size(); ++i)
            if (ed->members[i].first == v) return (int)i;
        return -1;
    };
    bool hasDefault = false;
    std::set<std::string> covered;
    // A classic enum may give two members the same value (legal, as in C); a `match` is a
    // switch on the value, so two arms for equal values would be one duplicate case.
    bool plain = ed && plainEnumDecls.count(st) && plainEnumDecls[st] == ed;
    std::map<long long, std::string> valueArm;
    auto memberValue = [&](const std::string& v, long long& out) {
        for (const auto& m : ed->members) if (m.first == v) { out = m.second; return true; }
        return false;
    };
    for (size_t ai = 0; ai < node->arms.size(); ++ai) {
        auto& arm = node->arms[ai];
        if (arm.variant.empty()) {
            if (hasDefault) errorAt(node, "duplicate match arm for variant '_'");
            hasDefault = true;
            if (ai + 1 < node->arms.size())   // arms after `_` can never match
                warning(node->line, node->col, "match arms after the `_` default are unreachable");
        }
        else if (!covered.insert(arm.variant).second)
            errorAt(node, "duplicate match arm for variant '" + arm.variant + "'");
        else if (long long val = 0; plain && memberValue(arm.variant, val)) {
            auto [it, fresh] = valueArm.insert({val, arm.variant});
            if (!fresh)
                errorAt(node, "duplicate match value: '" + arm.variant + "' has the same value (" +
                              std::to_string(val) + ") as '" + it->second + "'");
        }
        pushScope();
        if (!arm.variant.empty() && ed) {
            int vi = variantIndex(arm.variant);
            if (vi < 0) {
                errorAt(node, "'" + arm.variant + "' is not a variant of " + st);
            } else {
                static const std::vector<std::string> noPayload;   // classic enums carry none
                const auto& payload = (vi < (int)ed->payloads.size()) ? ed->payloads[vi] : noPayload;
                if (arm.bindings.size() != payload.size())
                    errorAt(node, "variant '" + arm.variant + "' binds " +
                        std::to_string(payload.size()) + " field(s), got " +
                        std::to_string(arm.bindings.size()));
                std::set<std::string> bound;
                for (const auto& b : arm.bindings)
                    if (b != "_" && !bound.insert(b).second)
                        errorAt(node, "duplicate binding '" + b + "' in match arm '" + arm.variant + "'");
                // The async lowering keeps a binding that lives across an await in a frame
                // field of this type (a generic body's nodes are shared: not stamped).
                if (!inInstance) arm.bindingTypes.clear();
                std::vector<std::string> instBts;
                for (size_t i = 0; i < arm.bindings.size() && i < payload.size(); ++i) {
                    std::string bt = normalizeType(substType(payload[i], subs));
                    defineSymbol(arm.bindings[i], bt, node->line, node->col, /*isParam=*/false);
                    if (!inInstance) arm.bindingTypes.push_back(bt);
                    else instBts.push_back(bt);
                }
                if (inInstance) arm.instanceBindingTypes.push_back({instSubs, instBts});
            }
        }
        if (arm.body) arm.body->accept(this);
        popScope();
    }
    // Exhaustiveness: without a `_` default, every variant must be covered.
    if (ed && !hasDefault) {
        std::string missing;
        for (const auto& m : ed->members)
            if (!covered.count(m.first) && !(plain && valueArm.count(m.second)))   // an equal-valued arm covers it
                missing += (missing.empty() ? "'" : ", '") + m.first + "'";
        if (!missing.empty())
            errorAt(node, "non-exhaustive match on '" + st + "': missing " + missing +
                          " (add those arms or a `_` default)");
    }
}

// Does the folded case value `v` lie in the range of integer type `t`? A 64-bit subject
// holds every folded value (it is folded in 64 bits).
static bool caseValueFits(const std::string& raw, long long v) {
    std::string t = tyq::strip(raw);
    if (t == "bool") return v >= 0 && v <= 1;
    if (t == "char" || t == "uint8") return v >= 0 && v <= 255;
    if (t == "int8") return v >= -128 && v <= 127;
    if (t == "uint16") return v >= 0 && v <= 65535;
    if (t == "int16") return v >= -32768 && v <= 32767;
    if (t == "uint" || t == "uint32") return v >= 0 && v <= 4294967295LL;
    if (t == "int" || t == "int32") return v >= INT_MIN && v <= INT_MAX;
    return true;
}

void TypeChecker::visit(SwitchStmt* node) {
    node->subject->accept(this);
    std::string subjType = getExpressionType(node->subject.get());
    if (subjType != "unknown" && !isIntType(normalizeType(subjType)))   // a classic enum counts as int
        errorAt(node,"switch subject must be integer type, got " + subjType);
    std::set<long long> seenCases;   // detect duplicate case values (else codegen
                                     // emits a switch the IR verifier rejects)
    bool seenDefault = false;
    pushScope();                       // the switch body is one scope (C): a case's
                                       // declaration is visible in the cases after it
    for (auto& c : node->cases) {
        if (!c.value) {
            if (seenDefault) errorAt(node, "multiple 'default' labels in one switch");
            seenDefault = true;
        }
        if (c.value) {
            c.value->accept(this);
            std::string caseType = getExpressionType(c.value.get());
            if (caseType != "unknown" && subjType != "unknown") {
                if (!isValidAssignment(subjType, caseType) &&
                    !(isIntType(subjType) && isIntType(caseType))) {
                    errorAt(c.value.get(),
                        "case value type '" + caseType +
                        "' is incompatible with switch subject type '" + subjType + "'");
                }
            }
            if (!isConstIntExpr(c.value.get()))
                errorAt(c.value.get(), "switch case value must be a constant integer expression");
            // Fold the label to its value so two spellings of one value (`7` and
            // `(4 * 2) - 1`) are caught as duplicates, as in C.
            long long cv = 0;
            bool haveCv = foldConstInt(c.value.get(), cv);
            // A label the subject's type cannot hold never matches (and, truncated to that
            // type, could collide with another label): reject it.
            if (haveCv && subjType != "unknown" && !caseValueFits(normalizeType(subjType), cv)) {
                errorAt(c.value.get(), "case value " + std::to_string(cv) +
                                       " is out of range for switch subject type '" + subjType + "'");
                haveCv = false;
            }
            if (haveCv) {
                if (seenCases.count(cv))
                    errorAt(c.value.get(), "duplicate case value in switch");
                else
                    seenCases.insert(cv);
            }
        }
        ++switchDepth;
        for (auto& it : c.stmts) {
            if (auto* st = std::get_if<StmtPtr>(&it)) (*st)->accept(this);
            else std::get<DeclPtr>(it)->accept(this);
        }
        --switchDepth;
    }
    popScope();
}

bool TypeChecker::isLvalueExpr(Expr* e) {
    if (auto* id = nodeCast<IdentExpr>(e))
        return !lookupSymbol(id->name).empty() || !functionSignatures.count(id->name);
    if (auto* u = nodeCast<UnaryExpr>(e)) return u->op == "*";
    // A field or an array element is storage only when its struct or array is: through a
    // pointer (or a slice), or itself stored (`mk().x` and `arr()[0]` are temporaries).
    auto storedBase = [&](Expr* base) {
        ty::Type bt = ty::Type::parse(normalizeType(dealiasOperand(getExpressionType(base))));
        if (bt.kind != ty::Type::Kind::Array && bt.kind != ty::Type::Kind::Struct &&
            bt.kind != ty::Type::Kind::Template && bt.kind != ty::Type::Kind::Named)
            return true;
        return isLvalueExpr(base);
    };
    if (auto* m = nodeCast<MemberExpr>(e)) return !isSliceLen(e) && storedBase(m->base.get());
    if (auto* ix = nodeCast<IndexExpr>(e))
        return !ix->highIndex && ix->opFunc.empty() && storedBase(ix->base.get());
    return false;
}

// `s.len` of a slice: a read-only view of the fat pointer's length.
bool TypeChecker::isSliceLen(Expr* e) {
    auto* m = nodeCast<MemberExpr>(e);
    if (!m || m->member != "len") return false;
    std::string bt = getExpressionType(m->base.get());
    if (!bt.empty() && bt[0] == '?') bt = bt.substr(1);
    return ty::Type::parse(bt).kind == ty::Type::Kind::Slice;
}

// Value of an integer constant expression built from literals, enum members, unary
// and binary operators and casts. Returns false when the value is not known here
// (`sizeof`, a non-constant name, division by zero), so callers never guess.
bool TypeChecker::foldConstInt(Expr* e, long long& out) {
    ty::CInt c;
    if (!foldConstIntT(e, c)) return false;
    out = c.v;
    return true;
}

// The typed fold behind foldConstInt: the value keeps its C type (after promotion), so
// the operators follow C's usual arithmetic conversions (ty::cintBinary).
bool TypeChecker::foldConstIntT(Expr* e, ty::CInt& out) {
    if (auto* l = nodeCast<LiteralExpr>(e)) {
        if (l->kind == LiteralExpr::Kind::INT) {
            // A negative literal (`-2147483648`) is one token, typed by its value as the
            // program sees it at run time: `int` when the value fits, else `int64`.
            bool neg = !l->value.empty() && l->value[0] == '-';
            unsigned long long m = 0;
            try { m = std::stoull(neg ? l->value.substr(1) : l->value, nullptr, 0); }
            catch (...) { return false; }
            out = neg ? ty::cintNegLiteral(m) : ty::cintLiteral(m);
            return true;
        }
        if (l->kind == LiteralExpr::Kind::CHAR) { out = ty::cintMake(l->value.empty() ? 0 : (unsigned char)l->value[0], 32, false); return true; }
        if (l->kind == LiteralExpr::Kind::BOOL) { out = ty::cintMake(l->value == "true" ? 1 : 0, 32, false); return true; }
        return false;
    }
    if (auto* id = nodeCast<IdentExpr>(e)) {
        if (const Symbol* sym = findSymbol(id->name)) {
            // A `const` integer folds through its initializer (bounded, so a
            // self-referential const can't recurse forever), converted to its type.
            std::string st = normalizeType(sym->type);
            if (!sym->isConst || !sym->constInit || !isIntType(st)) return false;
            if (foldDepth > 64) return false;
            ++foldDepth;
            ty::CInt v;
            bool ok = foldConstIntT(sym->constInit, v);
            --foldDepth;
            if (!ok) return false;
            if (!ty::cintCast(tyq::strip(st), v, out)) out = v;
            return true;
        }
        auto it = enumConstants.find(id->name);
        if (it == enumConstants.end()) return false;
        out = it->second >= INT32_MIN && it->second <= INT32_MAX ? ty::cintMake(it->second, 32, false)
                                                                 : ty::cintMake(it->second, 64, false);
        return true;
    }
    if (auto* u = nodeCast<UnaryExpr>(e)) {
        ty::CInt v;
        if (!foldConstIntT(u->operand.get(), v)) return false;
        return ty::cintUnary(u->op, v, out);
    }
    if (auto* b = nodeCast<BinaryExpr>(e)) {
        // A left-leaning chain (`A + B + C ...`) is folded along its spine with a loop.
        std::vector<BinaryExpr*> spine{b};
        while (auto* l = nodeCast<BinaryExpr>(spine.back()->left.get())) spine.push_back(l);
        ty::CInt x;
        if (!foldConstIntT(spine.back()->left.get(), x)) return false;
        for (size_t i = spine.size(); i-- > 0;) {
            const std::string& op = spine[i]->op;
            // `&&` / `||` short-circuit: an unevaluated right operand need not fold.
            if (op == "&&" || op == "||") {
                if ((op == "&&") == (x.v == 0)) { x = ty::cintMake(op == "||", 32, false); continue; }
                ty::CInt y;
                if (!foldConstIntT(spine[i]->right.get(), y)) return false;
                x = ty::cintMake(y.v != 0, 32, false);
                continue;
            }
            ty::CInt y;
            if (!foldConstIntT(spine[i]->right.get(), y)) return false;
            if (!ty::cintBinary(op, x, y, x)) return false;
        }
        out = x;
        return true;
    }
    if (auto* t = nodeCast<TernaryExpr>(e)) {
        ty::CInt c, a, other;
        if (!foldConstIntT(t->condition.get(), c)) return false;
        if (!foldConstIntT(c.v != 0 ? t->thenExpr.get() : t->elseExpr.get(), a)) return false;
        // The arms meet at their common type (when the other one folds too).
        out = a;
        if (foldConstIntT(c.v != 0 ? t->elseExpr.get() : t->thenExpr.get(), other)) {
            int r = a.rank > other.rank ? a.rank : other.rank;
            bool un = a.rank == other.rank ? (a.uns || other.uns) : (a.rank > other.rank ? a.uns : other.uns);
            out = ty::cintMake(a.v, r, un);
        }
        return true;
    }
    if (auto* c = nodeCast<CastExpr>(e)) {
        std::string to = tyq::strip(normalizeType(c->targetType));
        bool isInt = true; ty::CInt i; double d = 0;
        if (!foldConstNumT(c->expr.get(), isInt, i, d)) return false;
        if (!isInt) {
            // A floating value converts toward zero; out of range it has no value (C).
            if (!isIntType(to) || (to != "bool" && !floatConstFitsInt(d, to))) return false;
            long long iv;
            if (to == "bool") iv = d != 0;
            else iv = d >= 9223372036854775808.0 ? (long long)(unsigned long long)d : (long long)d;
            i = ty::cintMake(iv, 64, to == "uint64");
        }
        if (!ty::cintCast(to, i, out)) out = i;
        return true;
    }
    if (auto* z = nodeCast<SizeofExpr>(e)) {
        // A type whose target layout is known here (scalars, pointers, arrays, structs
        // and unions of those) folds; the rest is left to codegen. A size is unsigned
        // 64-bit (C's size_t).
        if (inInstance || z->operand) return false;
        std::string zt = z->typeName;
        if (!isPrimitiveType(zt) && !structs.count(zt) && !typeAliases.count(zt) && !enumTypes.count(zt)) {
            std::string vt = lookupSymbol(zt);
            if (!vt.empty() && vt != "unknown" && vt != "struct:" + zt) zt = tyq::strip(vt);
        }
        long long sz = constSizeof(zt);
        out = ty::cintMake(sz, 64, true);
        return sz != 0;
    }
    return false;
}

bool TypeChecker::foldConstNum(Expr* e, bool& isInt, long long& i, double& d) {
    ty::CInt c;
    if (!foldConstNumT(e, isInt, c, d)) return false;
    i = c.v;
    return true;
}

bool TypeChecker::foldConstNumT(Expr* e, bool& isInt, ty::CInt& i, double& d) {
    auto toFloat = [](const std::string& t, double v) { return t == "float" ? (double)(float)v : v; };
    auto asDouble = [](const ty::CInt& c) { return c.uns ? (double)(unsigned long long)c.v : (double)c.v; };
    if (auto* l = nodeCast<LiteralExpr>(e); l && l->kind == LiteralExpr::Kind::FLOAT) {
        isInt = false; d = std::strtod(l->value.c_str(), nullptr);
        return true;
    }
    if (auto* id = nodeCast<IdentExpr>(e)) {
        const Symbol* sym = findSymbol(id->name);
        std::string t = sym ? tyq::strip(normalizeType(sym->type)) : "";
        if (sym && sym->isConst && sym->constInit && (t == "float" || t == "double")) {
            if (foldDepth > 64) return false;
            ++foldDepth;
            bool ok = foldConstNumT(sym->constInit, isInt, i, d);
            --foldDepth;
            if (!ok) return false;
            if (isInt) d = asDouble(i);
            isInt = false; d = toFloat(t, d);
            return true;
        }
    }
    if (auto* u = nodeCast<UnaryExpr>(e); u && u->op == "-") {
        if (!foldConstNumT(u->operand.get(), isInt, i, d)) return false;
        if (isInt) return ty::cintUnary("-", i, i);
        d = -d;
        return true;
    }
    if (auto* b = nodeCast<BinaryExpr>(e)) {
        std::vector<BinaryExpr*> spine{b};
        while (auto* l = nodeCast<BinaryExpr>(spine.back()->left.get())) spine.push_back(l);
        if (!foldConstNumT(spine.back()->left.get(), isInt, i, d)) return false;
        for (size_t k = spine.size(); k-- > 0;) {
            bool yInt = true; ty::CInt yi; double yd = 0;
            const std::string& op = spine[k]->op;
            if (!foldConstNumT(spine[k]->right.get(), yInt, yi, yd)) return false;
            if (isInt && yInt) {
                if (!ty::cintBinary(op, i, yi, i)) return false;
                continue;
            }
            double x = isInt ? asDouble(i) : d, y = yInt ? asDouble(yi) : yd;
            if (op == "+") d = x + y;
            else if (op == "-") d = x - y;
            else if (op == "*") d = x * y;
            else if (op == "/") d = x / y;
            else return false;
            isInt = false;
        }
        return true;
    }
    if (auto* c = nodeCast<CastExpr>(e)) {
        std::string to = tyq::strip(normalizeType(c->targetType));
        if (to == "float" || to == "double") {
            if (!foldConstNumT(c->expr.get(), isInt, i, d)) return false;
            if (isInt) d = asDouble(i);
            isInt = false; d = toFloat(to, d);
            return true;
        }
    }
    isInt = true;
    return foldConstIntT(e, i);
}

// `v` converted to the integer type `raw` (C: truncate, then sign- or zero-extend);
// a non-integer or 64-bit type leaves it unchanged.
long long TypeChecker::truncConstInt(const std::string& raw, long long v) {
    std::string t = tyq::strip(raw);
    if (t == "bool") return v != 0;
    if (t == "char" || t == "uint8") return (long long)(uint8_t)v;
    if (t == "int8") return (long long)(int8_t)v;
    if (t == "uint16") return (long long)(uint16_t)v;
    if (t == "int16") return (long long)(int16_t)v;
    if (t == "uint" || t == "uint32") return (long long)(uint32_t)v;
    if (t == "int" || t == "int32") return (long long)(int32_t)v;
    return v;
}

bool TypeChecker::isConstIntExpr(Expr* e) {
    if (auto* l = nodeCast<LiteralExpr>(e))
        return l->kind == LiteralExpr::Kind::INT || l->kind == LiteralExpr::Kind::CHAR ||
               l->kind == LiteralExpr::Kind::BOOL;
    if (auto* id = nodeCast<IdentExpr>(e)) {
        std::string t = lookupSymbol(id->name);
        if (t.empty()) return enumConstants.count(id->name) > 0;
        const Symbol* sym = findSymbol(id->name);   // `const int K = 7` folds (a const parameter does not)
        return sym && sym->isConst && sym->constInit && isIntType(normalizeType(t));
    }
    if (auto* u = nodeCast<UnaryExpr>(e))
        return (u->op == "-" || u->op == "~" || u->op == "!") && isConstIntExpr(u->operand.get());
    if (auto* b = nodeCast<BinaryExpr>(e)) {
        // Down the left spine with a loop (a long `A + B + C ...` is as deep as it is long).
        for (;;) {
            if (b->op == "=" || b->op == "&&" || b->op == "||") return false;
            if (!isConstIntExpr(b->right.get())) return false;
            auto* l = nodeCast<BinaryExpr>(b->left.get());
            if (!l) return isConstIntExpr(b->left.get());
            b = l;
        }
    }
    if (auto* c = nodeCast<CastExpr>(e)) return isConstIntExpr(c->expr.get());
    if (nodeCast<SizeofExpr>(e)) return true;
    return false;
}
