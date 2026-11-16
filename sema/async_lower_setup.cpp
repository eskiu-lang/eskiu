// async_lower_setup.cpp: setup, the frame's fields, typing helpers and the capture snapshots (see async_lower.h).
#include "async_lower.h"

bool AsyncLowering::setup() {
    name = fn->name;
    // A generic async function is lowered once, generically: the frame struct, the
    // resume function, and the constructor are templates over its type parameters,
    // instantiated per use like any generic. The awaited types come from the type
    // checker's per-instance records (genericAwaitType). Never instantiated: nothing
    // to lower (and nothing codegen will emit).
    generic = !tps.empty();
    if (generic) {
        std::vector<AwaitExpr*> aws;
        collectAwaits(fn->body.get(), aws);
        if (aws.empty() || aws.front()->instanceTypes.empty()) return false;
        for (auto* aw : aws) {
            std::string t = genericAwaitType(aw, tps);
            if (t.empty())
                throw std::runtime_error("async function '" + name + "': cannot express an awaited "
                    "type in terms of its type parameters");
            genAwTy[aw] = t;
        }
    }
    // Every synthesized name avoids the names already spelled in the function (and
    // the program's top-level names, for the frame type and resume function).
    used = topNames;
    astwalk::collectNames(fn->body.get(), used);
    for (const auto& p : fn->params) used.insert(p.second);
    frameName = astwalk::freshName("__" + name + "_frame", used);
    frameT = frameName;
    if (generic) {
        frameT += "<";
        for (size_t i = 0; i < tps.size(); ++i) frameT += (i ? "," : "") + tps[i];
        frameT += ">";
    }
    resumeN = astwalk::freshName("__" + name + "_resume", used);
    frn.ptr = astwalk::freshName("__fr", used);
    frn.st = astwalk::freshName("st", used);
    frn.ret = astwalk::freshName("ret", used);
    frn.awaiting = astwalk::freshName("awaiting", used);
    T = fn->returnType;
    isVoid = (T == "void");
    // `async void` uses a 1-byte unit (uint8) as the Future's value type.
    Tret = isVoid ? "uint8" : T;

    block = nodeCast<BlockStmt>(fn->body.get());
    if (!block)
        throw std::runtime_error("async function '" + name + "': missing body");
    if (stmtHasLabeledBreak(fn->body))
        throw std::runtime_error("async function '" + name + "': labeled 'break'/'continue' "
            "is not supported inside an async function");
    {
        ShadowRenamer sr;
        sr.used = &used;
        sr.scopes.emplace_back();
        for (const auto& p : fn->params) { sr.seen.insert(p.second); sr.scopes.back()[p.second] = p.second; }
        sr.items(block->items);
        statics = std::move(sr.statics);
    }
    return true;
}

void AsyncLowering::collectFrame() {
    items = desugarItems(block->items);
    fields.push_back({"Future<" + Tret + ">", frn.ret});
    fields.push_back({"int", frn.st});
    fields.push_back({"FutureHdr*", frn.awaiting});
    for (const auto& p : fn->params) { vars.insert(p.second); fields.push_back({p.first, p.second}); }

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
    {
        OwnedClosureScan oc;
        oc.items(items, false);
        ownedClosures = oc.owned();
        if (!ownedClosures.empty()) freeBeforeRebind(items, ownedClosures);
    }
}

void AsyncLowering::snapshotCaptures() {
    // ── Lambdas capturing a frame-hoisted local. The resume function has no local of
    //    that name (it lives in `fr.<name>`, and rewrite() stops at a lambda), so each
    //    statement whose own expressions create such a lambda is wrapped as
    //    `{ T x = fr.x; <stmt> }`: a by-value snapshot at the creation point, which is
    //    exactly what the closure's capture takes.
    for (auto& f : fields) fieldTy[f.name] = f.type;
    for (auto& st : states)
        for (auto& it : st)
            if (std::holds_alternative<StmtPtr>(it)) it = BlockItem(wrapCaps(std::get<StmtPtr>(it)));
}

std::string AsyncLowering::awType(AwaitExpr* aw) {
    return generic ? genAwTy[aw] : declType(aw->resolvedType);
}

std::runtime_error AsyncLowering::locError(ASTNode* at, const std::string& msg) {
    int ln = (at && at->line) ? at->line : fn->line, cl = (at && at->line) ? at->col : fn->col;
    return std::runtime_error(fn->sourceFile + ":" + std::to_string(ln) + ":" +
        std::to_string(cl) + ": async function '" + name + "': " + msg);
}

bool AsyncLowering::knownType(const std::string& t) {
    return !(t.empty() || t == "unknown" || t == "null" || t == "void");
}

std::string AsyncLowering::generalized(const AsyncTransform::InstanceTypes& recs) {
    AsyncTransform::InstanceTypes dr;
    for (const auto& [subs, t] : recs) {
        if (!knownType(t)) return "";
        std::map<std::string, std::string> ds;
        for (const auto& [k, v] : subs) ds[k] = declType(v);
        dr.push_back({ds, declType(t)});
    }
    return generalizeType(dr, tps);
}

std::string AsyncLowering::typeOf(Expr* e) {
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
}

std::shared_ptr<LiteralExpr> AsyncLowering::boolLit(bool v) { return std::make_shared<LiteralExpr>(LiteralExpr::Kind::BOOL, v ? "true" : "false"); }

StmtPtr AsyncLowering::blockOf(std::vector<BlockItem> its) { return std::make_shared<BlockStmt>(std::move(its)); }

std::string AsyncLowering::fresh(const std::string& base) { return astwalk::freshName(base + std::to_string(tmpN++), used); }

void AsyncLowering::letTmp(std::vector<BlockItem>& pre, const std::string& tn, const std::string& ty, ExprPtr init) {
    pre.push_back(DeclPtr(std::make_shared<VarDecl>(tn, ty, std::move(init))));
}

void AsyncLowering::scanB(const std::vector<BlockItem>& its) {
    for (auto& it : its) {
        if (std::holds_alternative<DeclPtr>(it)) {
            auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get());
            if (!vd) continue;
            if (!vars.count(vd->name)) { vars.insert(vd->name); fields.push_back({vd->type, vd->name}); }
            if (vd->initializer)
                if (auto* aw = nodeCast<AwaitExpr>(vd->initializer.get())) {
                    awIdx[aw] = (int)awaits.size();
                    awNames.push_back(astwalk::freshName("__aw" + std::to_string(awaits.size()), used));
                    fields.push_back({"*Future<" + awType(aw) + ">", awNames.back()});
                    awaits.push_back({vd, aw});
                }
        } else scanS(std::get<StmtPtr>(it));
    }
}

void AsyncLowering::scanS(const StmtPtr& s) {
    if (!s) return;
    if (auto* b = nodeCast<BlockStmt>(s.get())) scanB(b->items);
    else if (auto* i = nodeCast<IfStmt>(s.get())) { scanS(i->thenBranch); scanS(i->elseBranch); }
    else if (auto* w = nodeCast<WhileStmt>(s.get())) scanS(w->body);
    else if (auto* dw = nodeCast<DoWhileStmt>(s.get())) scanS(dw->body);
    else if (auto* ds = nodeCast<DeferStmt>(s.get())) scanS(ds->body);
    else if (auto* f = nodeCast<ForStmt>(s.get())) { scanS(f->init); scanS(f->body); }
    else if (auto* fi = nodeCast<ForInStmt>(s.get())) scanS(fi->body);
    else if (auto* sw = nodeCast<SwitchStmt>(s.get()))
        for (auto& c : sw->cases) scanB(c.stmts);
    else if (auto* m = nodeCast<MatchStmt>(s.get()))
        for (auto& arm : m->arms) scanS(arm.body);
    else if (auto* t = nodeCast<TryStmt>(s.get())) {
        scanS(t->body);
        for (auto& c : t->catches) scanS(c.body);
        scanS(t->finally);
    }
}

void AsyncLowering::lambdaCaps(const ExprPtr& e, std::vector<std::string>& caps) {
    if (!e) return;
    if (auto* lam = nodeCast<LambdaExpr>(e.get())) {
        for (auto& c : lam->captures)
            if (vars.count(c.first) && std::find(caps.begin(), caps.end(), c.first) == caps.end())
                caps.push_back(c.first);
        return;
    }
    astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& ch) { lambdaCaps(ch, caps); });
}

StmtPtr AsyncLowering::wrapCaps(const StmtPtr& s) {
    if (!s) return s;
    std::vector<std::string> caps;
    if (auto* b = nodeCast<BlockStmt>(s.get())) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it)) it = BlockItem(wrapCaps(std::get<StmtPtr>(it)));
        return s;
    }
    if (auto* i = nodeCast<IfStmt>(s.get())) {
        lambdaCaps(i->condition, caps);
        i->thenBranch = wrapCaps(i->thenBranch); i->elseBranch = wrapCaps(i->elseBranch);
    } else if (auto* w = nodeCast<WhileStmt>(s.get())) {
        lambdaCaps(w->condition, caps); w->body = wrapCaps(w->body);
    } else if (auto* dw = nodeCast<DoWhileStmt>(s.get())) {
        lambdaCaps(dw->condition, caps); dw->body = wrapCaps(dw->body);
    } else if (auto* f = nodeCast<ForStmt>(s.get())) {
        f->init = wrapCaps(f->init);
        lambdaCaps(f->condition, caps); lambdaCaps(f->step, caps);
        f->body = wrapCaps(f->body);
    } else if (auto* fi = nodeCast<ForInStmt>(s.get())) {
        lambdaCaps(fi->iterable, caps); fi->body = wrapCaps(fi->body);
    } else if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
        lambdaCaps(sw->subject, caps);
        for (auto& c : sw->cases) {
            lambdaCaps(c.value, caps);
            for (auto& it : c.stmts)
                if (std::holds_alternative<StmtPtr>(it)) it = BlockItem(wrapCaps(std::get<StmtPtr>(it)));
        }
    } else if (auto* m = nodeCast<MatchStmt>(s.get())) {
        lambdaCaps(m->subject, caps);
        for (auto& arm : m->arms) arm.body = wrapCaps(arm.body);
    } else if (auto* ds = nodeCast<DeferStmt>(s.get())) {
        ds->body = wrapCaps(ds->body);
    } else if (auto* t = nodeCast<TryStmt>(s.get())) {
        t->body = wrapCaps(t->body);
        for (auto& c : t->catches) c.body = wrapCaps(c.body);
        t->finally = wrapCaps(t->finally);
    } else if (auto* rs = nodeCast<ReturnStmt>(s.get())) {
        lambdaCaps(rs->value, caps);
    } else if (auto* es = nodeCast<ExprStmt>(s.get())) {
        lambdaCaps(es->expr, caps);
    } else if (auto* ts = nodeCast<ThrowStmt>(s.get())) {
        lambdaCaps(ts->value, caps);
    }
    if (caps.empty()) return s;
    std::vector<BlockItem> blk;
    for (auto& n : caps) blk.push_back(DeclPtr(std::make_shared<VarDecl>(n, fieldTy[n], fr(n))));
    blk.push_back(s);
    return std::make_shared<BlockStmt>(blk);
}
