#include "codegen.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

bool CodeGen::blockTerminated() {
    return builder->GetInsertBlock() && hasTerminator(builder->GetInsertBlock());
}

void CodeGen::runCleanupsToDepth(size_t depth, bool errorPath) {
    // Emit each pending cleanup body, innermost frame first and LIFO within a frame.
    // errdefer bodies run only on the error path (`?`-propagation). A body may itself
    // end the block (a `throw` in a defer): the caller then emits no exit of its own
    // (see blockTerminated at each exit).
    for (size_t i = cleanupScopes.size(); i-- > depth; ) {
        // A copy: emitting a braced body pushes its own frame, which can reallocate
        // cleanupScopes and would leave a reference dangling.
        auto frame = cleanupScopes[i];
        for (size_t j = frame.size(); j-- > 0; ) {
            if (blockTerminated()) return;
            if (frame[j].isErr && !errorPath) continue;
            // Resolve the body against the names visible where it was registered.
            auto names = symbolTable;
            auto types = varTypeStack;
            if (frame[j].names) symbolTable = *frame[j].names;
            if (frame[j].types) varTypeStack = *frame[j].types;
            llvm::BasicBlock* unwind = unwindTarget;
            unwindTarget = frame[j].prevUnwind;
            frame[j].body->accept(this);
            unwindTarget = unwind;
            symbolTable = std::move(names);
            varTypeStack = std::move(types);
        }
    }
}

CodeGen::Cleanup CodeGen::makeCleanup(Stmt* body, bool isErr) {
    return Cleanup{body, isErr,
                   std::make_shared<const std::map<std::string, llvm::Value*>>(symbolTable),
                   std::make_shared<const std::vector<std::map<std::string, std::string>>>(varTypeStack),
                   unwindTarget};
}

void CodeGen::popCleanupFrame() {
    // The calls after a scope's defers unwound to their pads; past the scope they go
    // back to the landingpad that was active before its first defer.
    if (!cleanupScopes.back().empty()) unwindTarget = cleanupScopes.back().front().prevUnwind;
    cleanupScopes.pop_back();
}

void CodeGen::emitDeferPad() {
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    ensureEHRuntime();
    if (!fn->hasPersonalityFn()) fn->setPersonalityFn(module->getFunction(ehPersonalityName()));
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    llvm::BasicBlock* saved = builder->GetInsertBlock();
    llvm::BasicBlock* pad = llvm::BasicBlock::Create(*context, "defer.lpad", fn);
    builder->SetInsertPoint(pad);
    auto* lp = builder->CreateLandingPad(
        llvm::StructType::get(*context, {ptrTy, llvm::Type::getInt32Ty(*context)}), 1, "defer.lp");
    bool inTry = !tryStack.empty();
    if (inTry) lp->addClause(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptrTy)));
    else lp->setCleanup(true);
    llvm::Value* ex = inTry ? builder->CreateExtractValue(lp, {0}, "defer.ex") : nullptr;
    size_t depth = inTry ? tryStack.back().depth : 0;
    runCleanupsToDepth(depth, /*errorPath=*/false);
    if (!blockTerminated()) {
        if (inTry) {
            tryStack.back().incoming.push_back({ex, builder->GetInsertBlock()});
            builder->CreateBr(tryStack.back().dispatch);
        } else {
            builder->CreateResume(lp);
        }
    }
    builder->SetInsertPoint(saved);
    unwindTarget = pad;
}

void CodeGen::visit(BlockStmt* node) {
    // A block is a lexical scope: a `let` inside it shadows (not overwrites) an
    // outer variable of the same name, and the outer binding is back on exit.
    pushScope();
    cleanupScopes.emplace_back();                    // this block's cleanup frame
    for (auto& item : node->items) {
        // Once this block has a terminator (a return/break/continue/throw was
        // emitted), the rest is unreachable. Emitting into a terminated block
        // yields invalid IR ("terminator in the middle of a basic block"), so
        // stop here — the dead code is simply dropped. (An early exit already ran
        // this frame's cleanups via runCleanupsToDepth before terminating.)
        if (blockTerminated())
            break;
        if (std::holds_alternative<DeclPtr>(item)) {
            auto decl = std::get<DeclPtr>(item);
            decl->accept(this);
        } else if (std::holds_alternative<StmtPtr>(item)) {
            auto stmt = std::get<StmtPtr>(item);
            stmt->accept(this);
        }
    }
    // Normal fall-through: run this block's deferred cleanups (LIFO, defers only).
    if (!blockTerminated())
        runCleanupsToDepth(cleanupScopes.size() - 1, /*errorPath=*/false);
    popCleanupFrame();
    popScope();
}

void CodeGen::emitScopedBody(const StmtPtr& body) {
    // A statement body that is not a block (`if (c) defer f();`, `while (c) stmt;`,
    // an unbraced match arm) is still its own scope: a `defer` in it runs when that
    // body ends, not at the end of the enclosing function or block.
    if (!body) return;
    if (nodeCast<BlockStmt>(body.get())) { body->accept(this); return; }
    pushScope();
    cleanupScopes.emplace_back();
    body->accept(this);
    if (!blockTerminated())
        runCleanupsToDepth(cleanupScopes.size() - 1, /*errorPath=*/false);
    popCleanupFrame();
    popScope();
}

void CodeGen::visit(DeferStmt* node) {
    // Register the body to run at scope exit; emitted by runCleanupsToDepth.
    if (node->body && !cleanupScopes.empty()) {
        cleanupScopes.back().push_back(makeCleanup(node->body.get(), node->isErr));
        if (programUsesEH && !node->isErr) emitDeferPad();
    }
}

void CodeGen::visit(IfStmt* node) {
    // An `else if` chain is emitted with a loop. Each link's else body is a scope of its
    // own (as emitScopedBody would make it); those scopes and the branches to each
    // link's merge block are closed innermost first once the chain ends.
    std::vector<llvm::BasicBlock*> outerMerges;
    for (IfStmt* n = node; n;) {
        // Evaluate condition
        llvm::Value* cond = evaluateExpr(n->condition);

        if (!cond) {
            throw std::runtime_error("If condition evaluation failed");
        }

        // Convert to i1
        cond = emitTruthy(cond);

        // Create blocks
        llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(*context, "then", currentFunction);
        llvm::BasicBlock* elseBlock = nullptr;
        llvm::BasicBlock* mergeBlock = llvm::BasicBlock::Create(*context, "merge", currentFunction);

        if (n->elseBranch) {
            elseBlock = llvm::BasicBlock::Create(*context, "else", currentFunction);
            builder->CreateCondBr(cond, thenBlock, elseBlock);
        } else {
            builder->CreateCondBr(cond, thenBlock, mergeBlock);
        }

        // Then block
        builder->SetInsertPoint(thenBlock);
        emitScopedBody(n->thenBranch);
        if (!hasTerminator(builder->GetInsertBlock())) {
            builder->CreateBr(mergeBlock);
        }

        // Else block
        IfStmt* next = nullptr;
        if (n->elseBranch) {
            builder->SetInsertPoint(elseBlock);
            next = nodeCast<IfStmt>(n->elseBranch.get());
            if (next) {
                pushScope();
                cleanupScopes.emplace_back();
                outerMerges.push_back(mergeBlock);
                n = next;
                continue;
            }
            emitScopedBody(n->elseBranch);
            if (!hasTerminator(builder->GetInsertBlock())) {
                builder->CreateBr(mergeBlock);
            }
        }

        // Merge block
        builder->SetInsertPoint(mergeBlock);
        n = nullptr;
    }
    for (size_t i = outerMerges.size(); i-- > 0;) {
        if (!blockTerminated())
            runCleanupsToDepth(cleanupScopes.size() - 1, /*errorPath=*/false);
        popCleanupFrame();
        popScope();
        if (!hasTerminator(builder->GetInsertBlock())) {
            builder->CreateBr(outerMerges[i]);
        }
        builder->SetInsertPoint(outerMerges[i]);
    }
}

void CodeGen::visit(WhileStmt* node) {
    llvm::BasicBlock* loopBlock = llvm::BasicBlock::Create(*context, "while", currentFunction);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(*context, "while_body", currentFunction);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(*context, "while_exit", currentFunction);

    builder->CreateBr(loopBlock);

    builder->SetInsertPoint(loopBlock);
    llvm::Value* cond = evaluateExpr(node->condition);
    cond = emitTruthy(cond);
    builder->CreateCondBr(cond, bodyBlock, exitBlock);

    builder->SetInsertPoint(bodyBlock);
    { LoopContext lc(this, exitBlock, loopBlock, node->label); emitScopedBody(node->body); }
    if (!hasTerminator(builder->GetInsertBlock()))
        builder->CreateBr(loopBlock);

    builder->SetInsertPoint(exitBlock);
}

void CodeGen::visit(DoWhileStmt* node) {
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(*context, "do_body", currentFunction);
    llvm::BasicBlock* condBlock = llvm::BasicBlock::Create(*context, "do_cond", currentFunction);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(*context, "do_exit", currentFunction);

    builder->CreateBr(bodyBlock);
    builder->SetInsertPoint(bodyBlock);

    // `continue` re-tests the condition (jumps to condBlock)
    { LoopContext lc(this, exitBlock, condBlock, node->label); emitScopedBody(node->body); }
    if (!hasTerminator(builder->GetInsertBlock()))
        builder->CreateBr(condBlock);

    builder->SetInsertPoint(condBlock);
    llvm::Value* cond = evaluateExpr(node->condition);
    cond = emitTruthy(cond);
    builder->CreateCondBr(cond, bodyBlock, exitBlock);

    builder->SetInsertPoint(exitBlock);
}

void CodeGen::visit(ForStmt* node) {
    llvm::BasicBlock* loopBlock = llvm::BasicBlock::Create(*context, "for", currentFunction);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(*context, "for_body", currentFunction);
    llvm::BasicBlock* stepBlock = llvm::BasicBlock::Create(*context, "for_step", currentFunction);
    llvm::BasicBlock* exitBlock = llvm::BasicBlock::Create(*context, "for_exit", currentFunction);

    // The init declaration is scoped to the loop (C semantics). The parser wraps it
    // in a BlockStmt; emit its items in the loop scope itself so the loop variable
    // stays visible to the condition, step, and body.
    pushScope();
    if (auto* ib = nodeCast<BlockStmt>(node->init.get())) {
        // `for (i in A..B)` inside a generic instance: the shared AST carries the type the
        // checker stamped for SOME instance, so re-derive the bounds' common integer type
        // for this one (outside a generic the checker's stamp is exact).
        if (!typeParamOverride.empty() && ib->items.size() == 2 &&
            std::holds_alternative<DeclPtr>(ib->items[0]) && std::holds_alternative<DeclPtr>(ib->items[1])) {
            auto* lo = nodeCast<VarDecl>(std::get<DeclPtr>(ib->items[0]).get());
            auto* hi = nodeCast<VarDecl>(std::get<DeclPtr>(ib->items[1]).get());
            if (lo && hi && lo->rangeBound && hi->rangeBound && lo->initializer && hi->initializer) {
                std::string ct = ty::rangeVarType(expandAlias(getExprEskiuType(lo->initializer)),
                                                  expandAlias(getExprEskiuType(hi->initializer)));
                if (!ct.empty()) lo->type = hi->type = ct;
            }
        }
        for (auto& item : ib->items) {
            if (std::holds_alternative<DeclPtr>(item)) std::get<DeclPtr>(item)->accept(this);
            else std::get<StmtPtr>(item)->accept(this);
        }
    } else if (node->init) {
        node->init->accept(this);
    }
    builder->CreateBr(loopBlock);

    // Condition
    builder->SetInsertPoint(loopBlock);
    if (node->condition) {
        llvm::Value* cond = evaluateExpr(node->condition);
        cond = emitTruthy(cond);
        builder->CreateCondBr(cond, bodyBlock, exitBlock);
    } else {
        builder->CreateBr(bodyBlock);
    }

    // Body
    builder->SetInsertPoint(bodyBlock);
    // continue jumps to the step block
    { LoopContext lc(this, exitBlock, stepBlock, node->label); emitScopedBody(node->body); }
    if (!hasTerminator(builder->GetInsertBlock()))
        builder->CreateBr(stepBlock);

    // Step
    builder->SetInsertPoint(stepBlock);
    if (node->step) {
        evaluateExpr(node->step);
    }
    builder->CreateBr(loopBlock);
    popScope();

    // Exit
    builder->SetInsertPoint(exitBlock);
}

void CodeGen::visit(ForInStmt* node) {
    // Desugar `for (x in it)` into a counted for-loop. We lower to a real
    // ForStmt (not a while) so `continue` lands on the index increment.
    static int counter = 0;
    std::string idxName = "__forin_i_" + std::to_string(counter++);
    auto idx = [&]() -> ExprPtr { return std::make_shared<IdentExpr>(idxName); };
    auto intLit = [&](const std::string& v) {
        return std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, v);
    };

    std::string itType = getExprEskiuType(node->iterable);
    std::string elemType;
    ExprPtr lengthExpr, elemExpr;
    ExprPtr iterable = node->iterable;
    std::shared_ptr<VarDecl> listPtrDecl;
    // The iterable is evaluated once. Anything but a stable place (a call, a variable
    // index) is held in a local: a list-like value that has an address by a pointer to
    // it, anything else (an array, a slice, a pointer, a returned value) by value.
    if (!astwalk::isStablePlace(node->iterable.get())) {
        std::function<bool(Expr*)> addressable = [&](Expr* e) -> bool {
            if (nodeCast<IdentExpr>(e)) return true;
            if (auto* u = nodeCast<UnaryExpr>(e)) return u->op == "*" && u->opFunc.empty();
            if (auto* ix = nodeCast<IndexExpr>(e)) return ix->opFunc.empty() && !ix->highIndex;
            if (auto* m = nodeCast<MemberExpr>(e))
                return ty::Type::parse(getExprEskiuType(m->base)).isPointer() || addressable(m->base.get());
            return false;
        };
        ty::Type k = ty::Type::parse(itType);
        bool byRef = k.kind != ty::Type::Kind::Array && k.kind != ty::Type::Kind::Slice && !k.isPointer()
                     && addressable(node->iterable.get());
        ExprPtr init = byRef ? ExprPtr(std::make_shared<UnaryExpr>("&", node->iterable)) : node->iterable;
        listPtrDecl = std::make_shared<VarDecl>(idxName + "_v", byRef ? "*" + itType : itType, init);
        iterable = std::make_shared<IdentExpr>(idxName + "_v");
    }

    ty::Type itT = ty::Type::parse(itType);
    if (itT.kind == ty::Type::Kind::Array) {
        // Fixed-size array T[N] — resolve the outer dimension N (literal, enum, const int).
        elemType   = itT.elem->str();
        uint64_t len = 0;
        resolveArrayDim(itT.dim, len);
        lengthExpr = intLit(std::to_string(len));
        elemExpr   = std::make_shared<IndexExpr>(iterable, idx());
    } else if (itT.kind == ty::Type::Kind::Slice) {
        // Slice T[] — length is the fat pointer's `.len` field.
        elemType   = itT.elem->str();
        lengthExpr = std::make_shared<MemberExpr>(iterable, "len");
        elemExpr   = std::make_shared<IndexExpr>(iterable, idx());
    } else {
        // List-like struct: needs `data` (pointer) and `size` (int) fields.
        // Strip the pointer/struct: decoration to the bare registry key (the resolved
        // type arrives normalized as e.g. "struct:List_int", or "*struct:List_int" for
        // an iterable like `&li`).
        ty::Type base = ty::Type::parse(expandAlias(itType));
        while (base.isPointer() && base.pointee) { ty::Type p = *base.pointee; base = p; }
        std::string s = base.isTemplate() ? mangleTemplate(base.str()) : base.nominalName();
        // A pointer iterable (`&li`) is evaluated once into a local the loop reads.
        if (itT.isPointer() && !listPtrDecl) {
            listPtrDecl = std::make_shared<VarDecl>(idxName + "_p", itType, node->iterable);
            iterable = std::make_shared<IdentExpr>(idxName + "_p");
        }
        auto it = structFields.find(s);
        std::string dataType;
        bool hasSize = false;
        if (it != structFields.end())
            for (const auto& f : it->second) {
                if (f.name == "data") dataType = f.type;
                if (f.name == "size") hasSize = true;
            }
        if (dataType.empty() || !hasSize)
            throw std::runtime_error("for-in over unsupported type: " + itType);
        while (!dataType.empty() && dataType.front() == '*') dataType = dataType.substr(1);
        while (!dataType.empty() && dataType.back()  == '*') dataType.pop_back();
        elemType   = dataType;
        lengthExpr = std::make_shared<MemberExpr>(iterable, "size");
        elemExpr   = std::make_shared<IndexExpr>(
            std::make_shared<MemberExpr>(iterable, "data"), idx());
    }

    auto idxDecl = std::make_shared<VarDecl>(idxName, "int", intLit("0"));
    std::vector<BlockItem> initItems;
    if (listPtrDecl) initItems.push_back(DeclPtr(listPtrDecl));
    initItems.push_back(DeclPtr(idxDecl));
    StmtPtr init = std::make_shared<BlockStmt>(initItems);
    ExprPtr cond = std::make_shared<BinaryExpr>(idx(), "<", lengthExpr);
    ExprPtr step = std::make_shared<BinaryExpr>(idx(), "=",
                       std::make_shared<BinaryExpr>(idx(), "+", intLit("1")));

    auto elemDecl = std::make_shared<VarDecl>(node->varName, elemType, elemExpr);
    StmtPtr body = std::make_shared<BlockStmt>(std::vector<BlockItem>{
        DeclPtr(elemDecl), StmtPtr(node->body)});

    ForStmt loop(init, cond, step, body);
    loop.label = node->label;   // carry the for-in's label onto the desugared loop
    visit(&loop);
}

void CodeGen::visit(ReturnStmt* node) {
    // Coerce return value to the declared function return type
    auto coerceRetVal = [&](llvm::Value* v) -> llvm::Value* {
        if (!currentFunction) return v;
        llvm::Type* ft = currentFunction->getReturnType();
        // A bool/comparison result (i1) or an unsigned source zero-extends — e.g.
        // `return a < b;` from an int function is 1, not -1.
        bool uns = v->getType()->isIntegerTy(1) ||
                   (node->value && eskiuUnsigned(getExprEskiuType(node->value)));
        return coerceValue(v, ft, uns);
    };

    std::string retEsk;
    if (currentFunction) {
        auto rit = funcEskiuReturnType.find(currentFunction->getName().str());
        if (rit != funcEskiuReturnType.end()) retEsk = rit->second;
        // In a generic instance the declared return type names its type parameters
        // (`T` with T = an interface: `return null;` is the empty interface value).
        if (!retEsk.empty() && !typeParamOverride.empty()) retEsk = substType(retEsk, typeParamOverride);
    }
    if (currentSretParam != nullptr) {
        // sret function: store result to hidden pointer, return void
        if (node->value) {
            llvm::Value* retValue = evalForType(node->value, retEsk);
            builder->CreateStore(retValue, currentSretParam);
        }
        runCleanupsToDepth(0, /*errorPath=*/false);          // run pending defers/finally before leaving
        if (!blockTerminated()) builder->CreateRetVoid();
    } else if (node->value && currentFunction && currentFunction->getReturnType()->isVoidTy()) {
        // `return f();` with f void, in a void function: evaluate the call, return nothing.
        evaluateExpr(node->value);
        runCleanupsToDepth(0, /*errorPath=*/false);
        if (!blockTerminated()) builder->CreateRetVoid();
    } else if (node->value) {
        // Evaluate the return value first, THEN run cleanups (C defer order), then ret.
        llvm::Value* retValue = coerceRetVal(evalForType(node->value, retEsk));
        runCleanupsToDepth(0, /*errorPath=*/false);
        if (!blockTerminated()) builder->CreateRet(retValue);
    } else {
        runCleanupsToDepth(0, /*errorPath=*/false);
        if (!blockTerminated()) builder->CreateRetVoid();
    }
}

void CodeGen::visit(BreakStmt* node) {
    if (!node->label.empty()) {
        // `break label` targets a named enclosing loop. Scan the loop frames from the
        // innermost out, unwind the body defers between here and that loop, then branch.
        for (size_t i = loopStack.size(); i-- > 0; ) {
            if (loopStack[i].label == node->label) {
                runCleanupsToDepth(loopStack[i].cleanupDepth, false);
                if (!blockTerminated()) builder->CreateBr(loopStack[i].breakBlock);
                return;
            }
        }
        throw std::runtime_error("break: no enclosing loop labeled '" + node->label + "'");
    }
    if (!breakTarget)
        throw std::runtime_error("break used outside of a loop");
    runCleanupsToDepth(breakCleanupDepth, false);   // defers inside the loop body run
    if (!blockTerminated()) builder->CreateBr(breakTarget);
}

void CodeGen::visit(ExprStmt* node) {
    evaluateExpr(node->expr);
}

void CodeGen::visit(ContinueStmt* node) {
    if (!node->label.empty()) {
        for (size_t i = loopStack.size(); i-- > 0; ) {
            if (loopStack[i].label == node->label) {
                runCleanupsToDepth(loopStack[i].cleanupDepth, false);
                if (!blockTerminated()) builder->CreateBr(loopStack[i].continueBlock);
                return;
            }
        }
        throw std::runtime_error("continue: no enclosing loop labeled '" + node->label + "'");
    }
    if (!continueTarget)
        throw std::runtime_error("continue used outside of a loop");
    runCleanupsToDepth(continueCleanupDepth, false);
    if (!blockTerminated()) builder->CreateBr(continueTarget);
}

void CodeGen::visit(MatchStmt* node) {
    llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
    // The type checker stamps node->enumName, except inside template-function
    // bodies (which it skips) — there we derive it from the subject's type, with
    // the active typeParamOverride applied, and ensure the instance is built.
    std::string enumName = node->enumName;
    if (enumName.empty() || !structTypes.count(enumName)) {
        std::string st = getExprEskiuType(node->subject);
        // getExprEskiuType's deref only strips a leading '*'; for a trailing-star
        // pointer (`Option<int>*`) it returns "" — fall back to the operand's type.
        if (st.empty())
            if (auto* u = nodeCast<UnaryExpr>(node->subject.get()); u && u->op == "*")
                st = getExprEskiuType(u->operand);
        if (!typeParamOverride.empty()) st = substType(st, typeParamOverride);  // T -> int
        while (!st.empty() && st.front() == '*') st = st.substr(1);
        while (!st.empty() && st.back()  == '*') st.pop_back();
        if (st.rfind("struct:", 0) == 0) st = st.substr(7);
        if (st.find('<') != std::string::npos) {
            auto [b, a] = splitTemplateType(st);
            enumName = genericEnumDecls.count(b) ? ensureEnumInst(b, a) : mangleTemplate(st);
        } else if (!st.empty()) {
            enumName = stripToStructKey(st);
        }
    }
    // Classic int enum: the subject IS the enum's int value (no tag / no payload). Lower to
    // a switch on that value, one case per variant's constant. `_` is the default arm.
    if (plainEnumDecls.count(enumName)) {
        EnumDecl* ped = plainEnumDecls[enumName];
        auto valueOf = [&](const std::string& v) -> long long {
            for (auto& m : ped->members) if (m.first == v) return m.second;
            return 0;
        };
        llvm::Value* subj = evaluateExpr(node->subject);
        if (subj->getType() != i32) subj = builder->CreateIntCast(subj, i32, true);
        llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(*context, "match.end", currentFunction);
        std::vector<llvm::BasicBlock*> armBlocks(node->arms.size());
        llvm::BasicBlock* defaultBlock = endBlock;
        for (size_t i = 0; i < node->arms.size(); ++i) {
            armBlocks[i] = llvm::BasicBlock::Create(*context, "match.arm", currentFunction);
            if (node->arms[i].variant.empty()) defaultBlock = armBlocks[i];
        }
        llvm::SwitchInst* sw = builder->CreateSwitch(subj, defaultBlock);
        for (size_t i = 0; i < node->arms.size(); ++i)
            if (!node->arms[i].variant.empty())
                sw->addCase(llvm::cast<llvm::ConstantInt>(constIntBits(i32, (uint64_t)(int64_t)valueOf(node->arms[i].variant))), armBlocks[i]);
        for (size_t i = 0; i < node->arms.size(); ++i) {
            builder->SetInsertPoint(armBlocks[i]);
            emitScopedBody(node->arms[i].body);
            if (!hasTerminator(builder->GetInsertBlock()))
                builder->CreateBr(endBlock);
        }
        builder->SetInsertPoint(endBlock);
        return;
    }

    // Resolve the enum decl + (for a generic instance) the type-arg substitutions,
    // so each variant's payload field types come out concrete.
    EnumDecl* ed = nullptr;
    std::map<std::string, std::string> subs;
    if (adtEnumDecls.count(enumName)) {
        ed = adtEnumDecls[enumName];                         // concrete ADT enum
    } else if (enumInstanceArgs.count(enumName)) {           // generic instance
        auto& inst = enumInstanceArgs[enumName];
        ed = genericEnumDecls[inst.first];
        for (size_t i = 0; i < ed->typeParams.size() && i < inst.second.size(); ++i)
            subs[ed->typeParams[i]] = inst.second[i];
    }
    if (!ed)
        throw std::runtime_error("match: could not resolve the algebraic enum for subject "
                                 "type '" + enumName + "' (the subject must be an enum value)");
    auto variantIndex = [&](const std::string& v) -> int {
        for (size_t i = 0; i < ed->members.size(); ++i)
            if (ed->members[i].first == v) return (int)i;
        return -1;
    };
    // Concrete payload LLVM field types of a variant (after substitution).
    auto payloadTypes = [&](int idx) {
        std::vector<llvm::Type*> v;
        for (const auto& ft : ed->payloads[idx]) v.push_back(getTypeFromString(substType(ft, subs)));
        return v;
    };

    llvm::StructType* et = structTypes[enumName];

    // Materialize the subject so tag + payload can be read by pointer.
    llvm::Value* sv = evaluateExpr(node->subject);
    llvm::Value* sptr = entryAlloca(et, nullptr, "match.subj");
    builder->CreateStore(sv, sptr);
    llvm::Value* tag = builder->CreateLoad(i32, builder->CreateStructGEP(et, sptr, 0), "match.tag");

    llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(*context, "match.end", currentFunction);
    std::vector<llvm::BasicBlock*> armBlocks(node->arms.size());
    llvm::BasicBlock* defaultBlock = endBlock;
    for (size_t i = 0; i < node->arms.size(); ++i) {
        armBlocks[i] = llvm::BasicBlock::Create(*context, "match.arm", currentFunction);
        if (node->arms[i].variant.empty()) defaultBlock = armBlocks[i];
    }
    llvm::SwitchInst* sw = builder->CreateSwitch(tag, defaultBlock);
    for (size_t i = 0; i < node->arms.size(); ++i)
        if (!node->arms[i].variant.empty())
            sw->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(i32, variantIndex(node->arms[i].variant))), armBlocks[i]);

    for (size_t i = 0; i < node->arms.size(); ++i) {
        auto& arm = node->arms[i];
        builder->SetInsertPoint(armBlocks[i]);
        pushScope();
        if (!arm.variant.empty() && !arm.bindings.empty()) {
            int vi = variantIndex(arm.variant);
            std::vector<llvm::Type*> vfields = payloadTypes(vi);
            llvm::StructType* vt = llvm::StructType::get(*context, vfields);
            llvm::Value* pay = builder->CreateStructGEP(et, sptr, 1);
            for (size_t b = 0; b < arm.bindings.size() && b < vfields.size(); ++b) {
                llvm::Value* fp = builder->CreateStructGEP(vt, pay, b);
                llvm::Value* val = builder->CreateLoad(vfields[b], fp, arm.bindings[b]);
                llvm::Value* slot = entryAlloca(vfields[b], nullptr, arm.bindings[b]);
                builder->CreateStore(val, slot);
                defineSymbol(arm.bindings[b], slot);
                defineVarType(arm.bindings[b], substType(ed->payloads[vi][b], subs));
            }
        }
        emitScopedBody(arm.body);
        popScope();
        if (!hasTerminator(builder->GetInsertBlock()))
            builder->CreateBr(endBlock);
    }
    builder->SetInsertPoint(endBlock);
}

void CodeGen::visit(SwitchStmt* node) {
    // Pre-evaluate case values (must be ConstantInt) before creating the switch
    std::vector<llvm::ConstantInt*> caseVals;
    for (auto& c : node->cases) {
        if (!c.value) { caseVals.push_back(nullptr); continue; }
        // Fold first: a `const int K` (or `K + 1`) is a compile-time case label even
        // though evaluating it as an expression would load the global.
        llvm::Value* v = evaluateConstantExpr(c.value);
        if (!v) v = evaluateExpr(c.value);
        auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v);
        if (!ci) throw std::runtime_error("switch case value must be a constant integer");
        caseVals.push_back(ci);
    }

    llvm::Value* subj = evaluateExpr(node->subject);
    if (!subj->getType()->isIntegerTy())
        throw std::runtime_error("switch subject must be integer");

    // LLVM requires the switch value and every case constant to share one integer
    // type. Widen to the widest of the subject and the cases (C promotes the
    // controlling expression; the cases may be wider, e.g. an int64 switch), so a
    // `switch (aChar)` (i8 subject, i32 case constants) is well-formed.
    unsigned swW = subj->getType()->getIntegerBitWidth();
    for (auto* ci : caseVals) if (ci) swW = std::max(swW, ci->getType()->getIntegerBitWidth());
    llvm::Type* swTy = llvm::Type::getIntNTy(*context, swW);
    if (subj->getType() != swTy)
        subj = coerceInt(subj, swTy, eskiuUnsigned(getExprEskiuType(node->subject)));
    for (auto& ci : caseVals)
        if (ci && ci->getType() != swTy)
            ci = llvm::ConstantInt::get(*context, ci->getValue().sextOrTrunc(swW));

    llvm::BasicBlock* endBlock = llvm::BasicBlock::Create(*context, "switch.end", currentFunction);

    std::vector<llvm::BasicBlock*> caseBlocks(node->cases.size());
    for (size_t i = 0; i < node->cases.size(); ++i) {
        std::string lbl = node->cases[i].value ? "case" + std::to_string(i) : "default";
        caseBlocks[i] = llvm::BasicBlock::Create(*context, lbl, currentFunction);
    }

    llvm::BasicBlock* defaultBlock = endBlock;
    for (size_t i = 0; i < node->cases.size(); ++i) {
        if (!node->cases[i].value) { defaultBlock = caseBlocks[i]; break; }
    }

    llvm::SwitchInst* sw = builder->CreateSwitch(subj, defaultBlock);
    for (size_t i = 0; i < node->cases.size(); ++i) {
        if (caseVals[i]) sw->addCase(caseVals[i], caseBlocks[i]);
    }

    llvm::BasicBlock* prevBreak = breakTarget;
    breakTarget = endBlock;
    // A `switch` pushes no cleanup frame, so a `break` inside a case must unwind only to
    // the switch's own depth. Without saving/restoring breakCleanupDepth, the break would
    // run the enclosing loop's/function's defers early (and again on normal exit).
    size_t prevBreakCleanupDepth = breakCleanupDepth;
    breakCleanupDepth = cleanupScopes.size();

    // The switch body is one variable scope (C): a declaration in a case is visible in
    // the cases after it (its storage is an entry-block alloca, so a jump past the
    // initialization still names valid storage).
    pushScope();
    for (size_t i = 0; i < node->cases.size(); ++i) {
        builder->SetInsertPoint(caseBlocks[i]);
        // Each case's statements are a cleanup scope: a `defer` there runs when the case
        // body is left (by `break`, or by falling through into the next case).
        cleanupScopes.emplace_back();
        for (auto& item : node->cases[i].stmts) {
            if (auto* st = std::get_if<StmtPtr>(&item)) (*st)->accept(this);
            else std::get<DeclPtr>(item)->accept(this);
            if (hasTerminator(builder->GetInsertBlock())) break;
        }
        if (!blockTerminated())
            runCleanupsToDepth(cleanupScopes.size() - 1, /*errorPath=*/false);
        popCleanupFrame();
        if (!hasTerminator(builder->GetInsertBlock())) {
            llvm::BasicBlock* next = (i + 1 < caseBlocks.size()) ? caseBlocks[i+1] : endBlock;
            builder->CreateBr(next);
        }
    }

    popScope();
    breakTarget = prevBreak;
    breakCleanupDepth = prevBreakCleanupDepth;
    builder->SetInsertPoint(endBlock);
}
