// async_lower_stmt.cpp: the lowering of statements into states (see async_lower.h).
#include "async_lower.h"

void AsyncLowering::lowerBody() {
    // Releases the exceptions captured for an awaiting cleanup that never rethrew them
    // (filled in once every capture is known), run when the frame is finished.
    excFree = std::make_shared<BlockStmt>(std::vector<BlockItem>{});

    int entry = newState();                 // state 0
    int exit  = lowerSeq(items, entry);
    // Fall off the end of a reachable exit state: void completes with unit 0;
    // a non-void fn that falls off is a user error, but emit a bare return.
    if (exit != -1) {
        if (isVoid) completeInto(states[exit], intlit(0));
        else        states[exit].push_back(ret(nullptr));
    }
}

int AsyncLowering::lowerItem(BlockItem& it, int cur) {
    if (std::holds_alternative<DeclPtr>(it)) {
        auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get());
        if (!vd) return cur;
        if (!vd->initializer) { states[cur].push_back(initField(vd)); return cur; }
        if (auto* aw = nodeCast<AwaitExpr>(vd->initializer.get())) {
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
}

int AsyncLowering::lowerSeq(const std::vector<BlockItem>& its, int entry) {
    int cur = entry;
    deferFrames.push_back({});           // this split block's defers
    for (auto& it : its) {
        if (cur == -1) break;            // rest is unreachable
        if (std::holds_alternative<StmtPtr>(it))
            if (auto* ds = nodeCast<DeferStmt>(std::get<StmtPtr>(it).get())) {
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
}

int AsyncLowering::lowerStmt(const StmtPtr& s, int cur) {
    // return E  -> complete the future and terminate this path.
    if (auto* rs = nodeCast<ReturnStmt>(s.get())) return lowerReturn(rs, cur);
    // break/continue bind to the enclosing split loop -> state transition.
    if (nodeCast<BreakStmt>(s.get())) {
        if (brkTargets.empty())
            throw std::runtime_error("async function '" + name + "': `break` outside a loop");
        emitExit(cur, brkDeferDepth.back(), brkTargets.back(), false);
        return -1;
    }
    if (nodeCast<ContinueStmt>(s.get())) {
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
    if (auto* i = nodeCast<IfStmt>(s.get())) return lowerIf(i, cur);
    if (auto* w = nodeCast<WhileStmt>(s.get())) return lowerWhile(w, cur);
    if (auto* dw = nodeCast<DoWhileStmt>(s.get())) return lowerDoWhile(dw, cur);
    if (auto* f = nodeCast<ForStmt>(s.get())) return lowerFor(f, cur);
    if (auto* sw = nodeCast<SwitchStmt>(s.get())) return lowerSwitch(sw, cur);
    if (auto* m = nodeCast<MatchStmt>(s.get())) return lowerMatch(m, s, cur);
    if (auto* t = nodeCast<TryStmt>(s.get())) return lowerTry(t, cur);
    if (auto* b = nodeCast<BlockStmt>(s.get())) return lowerSeq(b->items, cur);
    throw locError(s.get(), "'await' is not supported inside this statement");
}

int AsyncLowering::lowerReturn(ReturnStmt* rs, int cur) {
    if (hasAwait(rs->value))
        throw std::runtime_error("async function '" + name + "': `return await ...` "
            "must be bound first (`let r = await ...; return r;`)");
    ExprPtr v = rs->value ? rs->value : intlit(0); rewrite(v, vars);
    states[cur].push_back(assign(std::make_shared<MemberExpr>(fr(frn.ret), "value"), v));
    emitExit(cur, 0, 0, true);
    return -1;
}

int AsyncLowering::lowerIf(IfStmt* i, int cur) {
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

int AsyncLowering::lowerWhile(WhileStmt* w, int cur) {
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

int AsyncLowering::lowerDoWhile(DoWhileStmt* dw, int cur) {
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

int AsyncLowering::lowerFor(ForStmt* f, int cur) {
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

int AsyncLowering::lowerSwitch(SwitchStmt* sw, int cur) {
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

int AsyncLowering::lowerMatch(MatchStmt* m, const StmtPtr& s, int cur) {
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

int AsyncLowering::lowerTry(TryStmt* t, int cur) {
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
