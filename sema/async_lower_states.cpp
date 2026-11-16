// async_lower_states.cpp: the state machinery: states, defer frames, exits and plain statements (see async_lower.h).
#include "async_lower.h"

void AsyncLowering::goTo(int s, int target) { states[s].push_back(assign(fr(frn.st), intlit(target))); }

bool AsyncLowering::cleanupAwaits(const Frames& frs, size_t from, size_t to) {
    for (size_t f = from; f < to && f < frs.size(); ++f)
        for (auto& b : frs[f]) if (stmtHasAwait(b)) return true;
    return false;
}

std::string AsyncLowering::rewriteCatchExText(StmtPtr& root) {
    std::string tf;
    std::function<void(ExprPtr&)> rwE = [&](ExprPtr& e) {
        if (!e || nodeCast<LambdaExpr>(e.get())) return;
        if (auto* call = nodeCast<CallExpr>(e.get()))
            if (auto* id = nodeCast<IdentExpr>(call->callee.get()))
                if (id->name == "exception_text" && call->args.empty()) {
                    if (tf.empty()) {
                        tf = astwalk::freshName("__extxt", used);
                        vars.insert(tf);
                        fields.push_back({"char[512]", tf});
                    }
                    e = std::make_shared<CastExpr>("string",
                        std::make_shared<UnaryExpr>("&", std::make_shared<IndexExpr>(ident(tf), intlit(0))));
                    return;
                }
        astwalk::forEachChildExprFlat(e.get(), [&](ExprPtr& ch) { rwE(ch); });
    };
    std::function<void(StmtPtr&)> rwS = [&](StmtPtr& s) {
        if (!s) return;
        auto items = [&](std::vector<BlockItem>& its) {
            for (auto& it : its) {
                if (std::holds_alternative<StmtPtr>(it)) rwS(std::get<StmtPtr>(it));
                else if (auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get())) rwE(vd->initializer);
            }
        };
        if (auto* b = nodeCast<BlockStmt>(s.get())) items(b->items);
        else if (auto* i = nodeCast<IfStmt>(s.get())) { rwE(i->condition); rwS(i->thenBranch); rwS(i->elseBranch); }
        else if (auto* w = nodeCast<WhileStmt>(s.get())) { rwE(w->condition); rwS(w->body); }
        else if (auto* dw = nodeCast<DoWhileStmt>(s.get())) { rwS(dw->body); rwE(dw->condition); }
        else if (auto* f = nodeCast<ForStmt>(s.get())) { rwS(f->init); rwE(f->condition); rwE(f->step); rwS(f->body); }
        else if (auto* fi = nodeCast<ForInStmt>(s.get())) { rwE(fi->iterable); rwS(fi->body); }
        else if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
            rwE(sw->subject);
            for (auto& c : sw->cases) { rwE(c.value); items(c.stmts); }
        } else if (auto* m = nodeCast<MatchStmt>(s.get())) {
            rwE(m->subject);
            for (auto& arm : m->arms) rwS(arm.body);
        } else if (auto* t = nodeCast<TryStmt>(s.get())) {
            rwS(t->body);
            rwS(t->finally);
        } else if (auto* d = nodeCast<DeferStmt>(s.get())) rwS(d->body);
        else if (auto* r = nodeCast<ReturnStmt>(s.get())) rwE(r->value);
        else if (auto* th = nodeCast<ThrowStmt>(s.get())) rwE(th->value);
        else if (auto* es = nodeCast<ExprStmt>(s.get())) rwE(es->expr);
        else if (auto* tj = nodeCast<ThreadJoinStmt>(s.get())) rwE(tj->tid);
        else if (auto* as = nodeCast<AsmStmt>(s.get())) {
            for (auto& o : as->outputs) rwE(o.second);
            for (auto& in : as->inputs) rwE(in.second);
        }
    };
    rwS(root);
    return tf;
}

int AsyncLowering::newStateIn(int region, size_t depth) {
    states.push_back({});
    stateRegion.push_back(region);
    stateCancel.push_back(cancelCleanup);
    stateFrames.emplace_back(deferFrames.begin(),
        deferFrames.begin() + (long)std::min(depth, deferFrames.size()));
    return (int)states.size() - 1;
}

int AsyncLowering::newState() { return newStateIn(curRegion, deferFrames.size()); }

void AsyncLowering::enterLoop(int brk, int cont) {
    brkTargets.push_back(brk); contTargets.push_back(cont);
    brkDeferDepth.push_back(deferFrames.size()); contDeferDepth.push_back(deferFrames.size());
}

void AsyncLowering::leaveLoop() {
    brkTargets.pop_back(); contTargets.pop_back();
    brkDeferDepth.pop_back(); contDeferDepth.pop_back();
}

void AsyncLowering::emitDefers(std::vector<BlockItem>& st, size_t downTo) {
    if (cleanupAwaits(deferFrames, downTo, deferFrames.size()))
        throw std::runtime_error("async function '" + name + "': internal error: an awaiting "
            "cleanup emitted in place");
    for (size_t f = deferFrames.size(); f-- > downTo;)
        for (auto it = deferFrames[f].rbegin(); it != deferFrames[f].rend(); ++it)
            st.push_back(*it);
}

bool AsyncLowering::escapesIn(const StmtPtr& s, bool contOnly) {
    if (!s) return false;
    if (nodeCast<ContinueStmt>(s.get())) return true;
    if (nodeCast<BreakStmt>(s.get())) return !contOnly;
    auto items = [&](const std::vector<BlockItem>& its, bool co) {
        for (auto& it : its)
            if (std::holds_alternative<StmtPtr>(it) && escapesIn(std::get<StmtPtr>(it), co)) return true;
        return false;
    };
    if (auto* b = nodeCast<BlockStmt>(s.get())) return items(b->items, contOnly);
    if (auto* i = nodeCast<IfStmt>(s.get()))
        return escapesIn(i->thenBranch, contOnly) || escapesIn(i->elseBranch, contOnly);
    if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
        for (auto& c : sw->cases) if (items(c.stmts, true)) return true;
        return false;
    }
    if (auto* m = nodeCast<MatchStmt>(s.get())) {
        for (auto& a : m->arms) if (escapesIn(a.body, contOnly)) return true;
        return false;
    }
    if (auto* t = nodeCast<TryStmt>(s.get())) {
        if (escapesIn(t->body, contOnly) || escapesIn(t->finally, contOnly)) return true;
        for (auto& c : t->catches) if (escapesIn(c.body, contOnly)) return true;
        return false;
    }
    return false;
}

bool AsyncLowering::loopEscapes(const StmtPtr& s) { return escapesIn(s, false); }

bool AsyncLowering::stmtTerminates(const StmtPtr& s) {
    if (!s) return false;
    if (nodeCast<ReturnStmt>(s.get()) || nodeCast<ThrowStmt>(s.get())) return true;
    if (auto* b = nodeCast<BlockStmt>(s.get())) {
        for (auto& it : b->items)
            if (std::holds_alternative<StmtPtr>(it) && stmtTerminates(std::get<StmtPtr>(it)))
                return true;
        return false;
    }
    if (auto* i = nodeCast<IfStmt>(s.get()))
        return i->elseBranch && stmtTerminates(i->thenBranch) && stmtTerminates(i->elseBranch);
    return false;
}

void AsyncLowering::publish(std::vector<BlockItem>& st) {
    st.push_back(StmtPtr(excFree));
    ExprPtr swap = std::make_shared<CallExpr>(ident("atomic_swap"),
        std::vector<ExprPtr>{ std::make_shared<UnaryExpr>("&",
            std::make_shared<MemberExpr>(fr(frn.ret), "state")), intlit(2) });
    std::vector<BlockItem> wk;
    wk.push_back(exprStmt(std::make_shared<CallExpr>(
        std::make_shared<MemberExpr>(fr(frn.ret), "waker"), std::vector<ExprPtr>{})));
    st.push_back(std::make_shared<IfStmt>(binop(swap, "==", intlit(1)),
        std::make_shared<BlockStmt>(wk)));
    st.push_back(ret(nullptr));
}

void AsyncLowering::completeInto(std::vector<BlockItem>& st, ExprPtr v) {
    st.push_back(assign(std::make_shared<MemberExpr>(fr(frn.ret), "value"), v));
    emitDefers(st, 0);
    publish(st);
}

int AsyncLowering::runFrames(int st, size_t downTo) {
    int reg = stateRegion[st];
    for (size_t f = deferFrames.size(); f-- > downTo;) {
        while (reg != -1 && f < regions[reg].base) {
            int p = regions[reg].parent;
            int s2 = newStateIn(p, f);
            goTo(st, s2);
            st = s2; reg = p;
        }
        const std::vector<StmtPtr> frame = deferFrames[f];
        for (size_t k = frame.size(); k-- > 0;) {
            if (!stmtHasAwait(frame[k])) { states[st].push_back(frame[k]); continue; }
            Frames saved = deferFrames;
            int savedRegion = curRegion;
            deferFrames.resize(f + 1);
            deferFrames[f].resize(k);
            curRegion = reg;
            int n = newState();
            goTo(st, n);
            int e = lowerStmt(frame[k], n);
            deferFrames = std::move(saved);
            curRegion = savedRegion;
            if (e == -1) return -1;
            st = e;
        }
    }
    return st;
}

void AsyncLowering::emitExit(int st, size_t downTo, int target, bool complete) {
    st = runFrames(st, downTo);
    if (st == -1) return;
    if (!complete) { goTo(st, target); return; }
    for (int reg = stateRegion[st]; reg != -1; reg = regions[reg].parent) {
        int s2 = newStateIn(regions[reg].parent, downTo);
        goTo(st, s2);
        st = s2;
    }
    publish(states[st]);
}

StmtPtr AsyncLowering::initField(VarDecl* vd) {
    // `T x;` resets its field to zero at each declaration (a loop iteration), as a
    // local does: a fresh local of the type (zeroed by codegen) is copied in.
    if (!vd->initializer) {
        std::string tn = astwalk::freshName("__zero_t" + std::to_string(initTmp++), used);
        std::vector<BlockItem> blk;
        blk.push_back(DeclPtr(std::make_shared<VarDecl>(tn, vd->type, nullptr)));
        blk.push_back(assign(fr(vd->name), ident(tn)));
        return std::make_shared<BlockStmt>(blk);
    }
    rewrite(vd->initializer, vars);
    if (!nodeCast<ArrayLitExpr>(vd->initializer.get()))
        return assign(fr(vd->name), vd->initializer);
    std::string tn = astwalk::freshName("__init_t" + std::to_string(initTmp++), used);
    std::vector<BlockItem> blk;
    blk.push_back(DeclPtr(std::make_shared<VarDecl>(tn, vd->type, vd->initializer)));
    blk.push_back(assign(fr(vd->name), ident(tn)));
    return std::make_shared<BlockStmt>(blk);
}

StmtPtr AsyncLowering::rewritePlain(const StmtPtr& s) {
    if (auto* b = nodeCast<BlockStmt>(s.get())) {
        std::vector<BlockItem> out2;
        for (auto& it : b->items) {
            if (std::holds_alternative<DeclPtr>(it)) {
                auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get());
                if (vd) out2.push_back(initField(vd));
            } else out2.push_back(rewritePlain(std::get<StmtPtr>(it)));
        }
        return std::make_shared<BlockStmt>(out2);
    }
    if (auto* t = nodeCast<TryStmt>(s.get())) {
        std::vector<TryStmt::CatchClause> cs;
        for (auto& c : t->catches) {
            TryStmt::CatchClause nc = c;
            nc.body = c.body ? rewritePlain(c.body) : nullptr;
            cs.push_back(nc);
        }
        StmtPtr body = rewritePlain(t->body);   // in this order on every host compiler
        StmtPtr fin = t->finally ? rewritePlain(t->finally) : nullptr;
        return std::make_shared<TryStmt>(body, cs, fin);
    }
    if (auto* rs = nodeCast<ReturnStmt>(s.get())) {
        ExprPtr v = rs->value ? rs->value : intlit(0); rewrite(v, vars);
        std::vector<BlockItem> cb; completeInto(cb, v);
        return std::make_shared<BlockStmt>(cb);
    }
    if (auto* es = nodeCast<ExprStmt>(s.get())) { rewrite(es->expr, vars); return s; }
    // An asm operand naming a local reads or writes its frame field.
    if (auto* as = nodeCast<AsmStmt>(s.get())) {
        for (auto& o : as->outputs) rewrite(o.second, vars);
        for (auto& in : as->inputs) rewrite(in.second, vars);
        return s;
    }
    if (auto* i = nodeCast<IfStmt>(s.get())) {
        rewrite(i->condition, vars);
        return std::make_shared<IfStmt>(i->condition, rewritePlain(i->thenBranch),
            i->elseBranch ? rewritePlain(i->elseBranch) : nullptr);
    }
    if (auto* w = nodeCast<WhileStmt>(s.get())) {
        rewrite(w->condition, vars);
        return std::make_shared<WhileStmt>(w->condition, rewritePlain(w->body));
    }
    if (auto* dw = nodeCast<DoWhileStmt>(s.get())) {
        rewrite(dw->condition, vars);
        return std::make_shared<DoWhileStmt>(rewritePlain(dw->body), dw->condition);
    }
    if (auto* ds = nodeCast<DeferStmt>(s.get()))
        return std::make_shared<DeferStmt>(rewritePlain(ds->body), ds->isErr);
    if (auto* ts = nodeCast<ThrowStmt>(s.get())) { rewrite(ts->value, vars); return s; }
    if (auto* f = nodeCast<ForStmt>(s.get())) {
        StmtPtr in2 = f->init ? rewritePlain(f->init) : nullptr;
        rewrite(f->condition, vars); rewrite(f->step, vars);
        return std::make_shared<ForStmt>(in2, f->condition, f->step, rewritePlain(f->body));
    }
    if (auto* fi = nodeCast<ForInStmt>(s.get())) {
        rewrite(fi->iterable, vars);
        return std::make_shared<ForInStmt>(fi->varName, fi->iterable, rewritePlain(fi->body));
    }
    if (auto* sw = nodeCast<SwitchStmt>(s.get())) {
        rewrite(sw->subject, vars);
        auto out2 = std::make_shared<SwitchStmt>(sw->subject, std::vector<SwitchStmt::Case>{});
        for (auto& c : sw->cases) {
            SwitchStmt::Case nc; nc.value = c.value;
            for (auto& it : c.stmts) {
                if (std::holds_alternative<DeclPtr>(it)) {
                    auto* vd = nodeCast<VarDecl>(std::get<DeclPtr>(it).get());
                    if (vd) nc.stmts.push_back(initField(vd));
                } else nc.stmts.push_back(rewritePlain(std::get<StmtPtr>(it)));
            }
            out2->cases.push_back(nc);
        }
        return out2;
    }
    if (auto* m = nodeCast<MatchStmt>(s.get())) {
        rewrite(m->subject, vars);
        auto out2 = std::make_shared<MatchStmt>(m->subject, std::vector<MatchStmt::Arm>{});
        out2->enumName = m->enumName;   // stamped by the type checker
        for (auto& arm : m->arms) {
            MatchStmt::Arm na; na.variant = arm.variant; na.bindings = arm.bindings;
            na.body = arm.body ? rewritePlain(arm.body) : nullptr;
            out2->arms.push_back(na);
        }
        return out2;
    }
    return s;   // break/continue/etc.
}
