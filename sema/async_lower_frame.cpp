// async_lower_frame.cpp: cancellation, the try regions' handlers and the emitted declarations (see async_lower.h).
#include "async_lower.h"

void AsyncLowering::buildCleanups() {
    // ── Each state of a split try runs inside its regions' handlers (see Region),
    //    innermost first. A handler first runs the defers pending in that state inside
    //    the region, as an exception leaving a block does. When those cleanups await,
    //    a typed handler jumps to states that run them before the handler, and the
    //    unwinding path captures the exception (a catch-all keeping a copy), runs the
    //    cleanups in states of the enclosing region, then throws the copy again.
    cxName = astwalk::freshName("__cx", used);

    for (size_t wrapped = 0, dsDone = 0;;) {
        for (; dsDone < dropSites.size(); ++dsDone) buildDrop(dropSites[dsDone]);
        if (wrapped == states.size()) break;
        for (; wrapped < states.size(); ++wrapped) wrapState(wrapped);
    }
    for (auto& [r, f] : excFields) excFree->items.push_back(freeExc(f));
    for (const auto& n : ownedClosures)
        excFree->items.push_back(exprStmt(std::make_shared<FreeClosureExpr>(fr(n))));
}

std::shared_ptr<CastExpr> AsyncLowering::nullOf(const std::string& t) { return std::make_shared<CastExpr>(t, intlit(0)); }

void AsyncLowering::buildDrop(const std::pair<int, Frames>& site) {
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
}

std::string AsyncLowering::excField(int r) {
    auto it = excFields.find(r);
    if (it != excFields.end()) return it->second;
    std::string f = astwalk::freshName("__exc" + std::to_string(r), used);
    fields.push_back({"*uint8", f});
    excFields[r] = f;
    return f;
}

StmtPtr AsyncLowering::freeExc(const std::string& f) {
    return std::make_shared<IfStmt>(binop(fr(f), "!=", nullOf("*uint8")), blockOf({
        exprStmt(std::make_shared<CallExpr>(ident("__cxa_free_exception"),
            std::vector<ExprPtr>{ std::make_shared<CastExpr>("*void", fr(f)) })),
        assign(fr(f), nullOf("*uint8")) }));
}

void AsyncLowering::wrapState(size_t si) {
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
                hb.push_back(exprStmt(std::make_shared<CallExpr>(ident("__exception_text_into"),
                    std::vector<ExprPtr>{std::make_shared<UnaryExpr>("&",
                        std::make_shared<IndexExpr>(fr(c.txtField), intlit(0)))})));
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
}

void AsyncLowering::emitDecls(std::vector<DeclPtr>& out) {
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
