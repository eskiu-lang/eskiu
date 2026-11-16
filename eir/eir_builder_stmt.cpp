// eir_builder_stmt.cpp: blocks, return, if, loops, jumps, and binary and unary expressions (split from eir_builder.cpp).
#include "eir_builder.h"
#include "../sema/type_checker.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "../template_utils.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <functional>
#include "eir_builder_int.h"

namespace eir {

void EIRBuilder::visit(BlockStmt* node) {
    pushScope();
    cleanupScopes.emplace_back();
    BasicBlock* savedBlockUnwind = currentUnwindTarget;
    size_t startHandlerDepth = currentHandlerDepth;

    for (auto& item : node->items) {
        if (currentBlock && currentBlock->hasTerminator()) break;
        if (std::holds_alternative<DeclPtr>(item)) {
            std::get<DeclPtr>(item)->accept(this);
        } else if (std::holds_alternative<StmtPtr>(item)) {
            std::get<StmtPtr>(item)->accept(this);
        }
    }
    if (currentBlock && !currentBlock->hasTerminator()) {
        runCleanupsToDepth(cleanupScopes.size() - 1, false);
        while (currentHandlerDepth > startHandlerDepth) {
            emit(Instruction{Opcode::PopHandler, "", Type::getVoid(), {}});
            currentHandlerDepth--;
        }
    }
    currentUnwindTarget = savedBlockUnwind;
    cleanupScopes.pop_back();
    popScope();
}

void EIRBuilder::visit(ReturnStmt* node) {
    if (node->value) {
        TypePtr retTy = currentFunction ? currentFunction->returnType : nullptr;
        Operand val = (retTy && !retTy->isVoid()) ? evalForType(node->value, retTy) : evaluateExpr(node->value);
        runCleanupsToDepth(0, false);
        if (currentBlock && !currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Ret, "", val.type, {val}});
        }
    } else {
        runCleanupsToDepth(0, false);
        if (currentBlock && !currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
        }
    }
}

void EIRBuilder::visit(IfStmt* node) {
    Operand cond = evaluateExpr(node->condition);

    cond = truth(cond);

    BasicBlock* thenBB = createBlock("then");
    BasicBlock* elseBB = node->elseBranch ? createBlock("else") : nullptr;
    BasicBlock* mergeBB = createBlock("merge");

    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(thenBB->name), Operand::block(elseBB ? elseBB->name : mergeBB->name)}});

    // Then
    setBlock(thenBB);
    visitBody(node->thenBranch);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});
    }

    // Else
    if (elseBB) {
        setBlock(elseBB);
        visitBody(node->elseBranch);
        if (!currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});
        }
    }

    setBlock(mergeBB);
}

void EIRBuilder::visit(WhileStmt* node) {
    BasicBlock* condBB = createBlock("while.cond");
    BasicBlock* bodyBB = createBlock("while.body");
    BasicBlock* exitBB = createBlock("while.exit");

    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

    setBlock(condBB);
    Operand cond = evaluateExpr(node->condition);
    cond = truth(cond);
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(bodyBB->name), Operand::block(exitBB->name)}});

    setBlock(bodyBB);
    breakStack.push_back(exitBB);
    continueStack.push_back(condBB);
    loopContextStack.push_back({exitBB, condBB, cleanupScopes.size(), currentHandlerDepth, node->label});

    visitBody(node->body);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});
    }

    loopContextStack.pop_back();
    breakStack.pop_back();
    continueStack.pop_back();

    setBlock(exitBB);
}

void EIRBuilder::visit(ForStmt* node) {
    pushScope();
    if (auto* ib = nodeCast<BlockStmt>(node->init.get())) {
        // `for (i in A..B)` inside a generic instance: the shared AST carries the type the
        // checker stamped for some instance, so the bounds' common integer type is derived
        // again for this one (as codegen does).
        if (!typeParamOverride.empty() && ib->items.size() == 2 &&
            std::holds_alternative<DeclPtr>(ib->items[0]) && std::holds_alternative<DeclPtr>(ib->items[1])) {
            auto* lo = nodeCast<VarDecl>(std::get<DeclPtr>(ib->items[0]).get());
            auto* hi = nodeCast<VarDecl>(std::get<DeclPtr>(ib->items[1]).get());
            if (lo && hi && lo->rangeBound && hi->rangeBound && lo->initializer && hi->initializer) {
                std::string ct = ty::rangeVarType(intSpelling(getExprEirType(lo->initializer)),
                                                  intSpelling(getExprEirType(hi->initializer)));
                if (!ct.empty()) lo->type = hi->type = ct;
            }
        }
        for (auto& item : ib->items) {
            if (std::holds_alternative<DeclPtr>(item)) std::get<DeclPtr>(item)->accept(this);
            else if (std::holds_alternative<StmtPtr>(item)) std::get<StmtPtr>(item)->accept(this);
        }
    } else if (node->init) {
        node->init->accept(this);
    }

    BasicBlock* condBB = createBlock("for.cond");
    BasicBlock* bodyBB = createBlock("for.body");
    BasicBlock* stepBB = createBlock("for.step");
    BasicBlock* exitBB = createBlock("for.exit");

    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

    setBlock(condBB);
    if (node->condition) {
        Operand cond = evaluateExpr(node->condition);
        cond = truth(cond);
        emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(bodyBB->name), Operand::block(exitBB->name)}});
    } else {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(bodyBB->name)}});
    }

    setBlock(bodyBB);
    breakStack.push_back(exitBB);
    continueStack.push_back(stepBB);
    loopContextStack.push_back({exitBB, stepBB, cleanupScopes.size(), currentHandlerDepth, node->label});

    visitBody(node->body);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(stepBB->name)}});
    }

    loopContextStack.pop_back();
    breakStack.pop_back();
    continueStack.pop_back();

    setBlock(stepBB);
    if (node->step) evaluateExpr(node->step);
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

    setBlock(exitBB);
    popScope();
}

// The loop (or switch, for an unlabeled break) a jump leaves: the innermost, or the loop
// carrying `label`; a continue skips switches.
const EIRBuilder::LoopContext* EIRBuilder::jumpTarget(const std::string& label, bool isContinue) const {
    for (auto it = loopContextStack.rbegin(); it != loopContextStack.rend(); ++it) {
        if (!label.empty() && it->label != label) continue;
        if (isContinue && !it->continueTarget) continue;
        return &*it;
    }
    return nullptr;
}

// Run the cleanups and drop the handlers inside `loop`, then branch to its exit or step.
void EIRBuilder::jumpTo(const LoopContext* loop, bool isContinue) {
    runCleanupsToDepth(loop->cleanupDepth, false);
    for (size_t h = currentHandlerDepth; h > loop->handlerDepth; --h) {
        emit(Instruction{Opcode::PopHandler, "", Type::getVoid(), {}});
    }
    if (currentBlock && !currentBlock->hasTerminator()) {
        BasicBlock* target = isContinue ? loop->continueTarget : loop->breakTarget;
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(target->name)}});
    }
}

void EIRBuilder::visit(BreakStmt* node) {
    if (const LoopContext* loop = jumpTarget(node->label, false)) {
        jumpTo(loop, false);
    } else if (!breakStack.empty()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(breakStack.back()->name)}});
    }
}

void EIRBuilder::visit(ContinueStmt* node) {
    if (const LoopContext* loop = jumpTarget(node->label, true)) {
        jumpTo(loop, true);
    } else if (!continueStack.empty()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(continueStack.back()->name)}});
    }
}

void EIRBuilder::visit(ExprStmt* node) {
    if (node->expr) evaluateExpr(node->expr);
}

void EIRBuilder::visit(BinaryExpr* node) {
    if (node->op != "=" && !node->opFunc.empty()) {
        auto call = std::make_shared<CallExpr>(
            std::make_shared<IdentExpr>(node->opFunc),
            std::vector<ExprPtr>{node->left, node->right});
        call->accept(this);
        return;
    }

    // Assignment: =
    if (node->op == "=") {
        // `lv op= v` is parsed as `lv = lv op v` sharing the lvalue node: evaluate the
        // lvalue once (a[f()] += 1 calls f once).
        if (auto* rhsOp = nodeCast<BinaryExpr>(node->right.get());
            rhsOp && rhsOp->left.get() == node->left.get() && rhsOp->op != "=") {
            emitCompoundAssign(node, rhsOp);
            return;
        }
        if (auto* mem = nodeCast<MemberExpr>(node->left.get())) {
            std::string baseType = getExprEirType(mem->base)->nominalName();
            auto it = module->structLayouts.find(baseType);
            if (it != module->structLayouts.end()) {
                const StructFieldInfo* fi = it->second.getFieldInfo(mem->member);
                if (fi && fi->isBitfield) {
                    // The assignment's value is the field's (wrapped) value, read as the
                    // field is.
                    Operand base = bitfieldBase(mem);
                    Operand rhs = coerceNumeric(evaluateExpr(node->right), fi->type);
                    storeBitfieldAt(base, fi, rhs, volatileRooted(mem));
                    lastValue = coerceNumeric(rhs, bitfieldReadType(fi));
                    return;
                }
            }
        }
        // The target's address first, then the value (as compound assignment does).
        TypePtr lhsTy = getExprEirType(node->left);
        Operand lhsAddr = evaluateLValue(node->left);
        Operand rhs = evalForType(node->right, lhsTy);
        Instruction st{Opcode::Store, "", Type::getVoid(), {rhs, lhsAddr}};
        st.isVolatile = volatileRooted(node->left.get());
        emit(st);
        lastValue = rhs;
        return;
    }

    // Compound assignment: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=
    if (node->op == "+=" || node->op == "-=" || node->op == "*=" ||
        node->op == "/=" || node->op == "%=" || node->op == "&=" ||
        node->op == "|=" || node->op == "^=" || node->op == "<<=" || node->op == ">>=") {
        std::string baseOp = node->op.substr(0, node->op.find('='));

        if (auto* mem = nodeCast<MemberExpr>(node->left.get())) {
            std::string baseType = getExprEirType(mem->base)->nominalName();
            auto it = module->structLayouts.find(baseType);
            if (it != module->structLayouts.end()) {
                const StructFieldInfo* fi = it->second.getFieldInfo(mem->member);
                if (fi && fi->isBitfield) {
                    Operand base = bitfieldBase(mem);
                    Operand cur = loadBitfieldAt(base, fi, volatileRooted(mem));
                    Operand rhs = evaluateExpr(node->right);
                    Operand res = compoundOp(baseOp, cur, rhs, fi->type);
                    storeBitfieldAt(base, fi, res, volatileRooted(mem));
                    lastValue = coerceNumeric(res, cur.type);
                    return;
                }
            }
        }

        Operand lhsAddr = evaluateLValue(node->left);
        TypePtr ty = getExprEirType(node->left);

        std::string curVal = newVar();
        Instruction ld{Opcode::Load, curVal, ty, {lhsAddr}};
        ld.isVolatile = volatileRooted(node->left.get());
        emit(ld);

        Operand rhs = evaluateExpr(node->right);
        Operand res = compoundOp(baseOp, Operand::var(curVal, ty), rhs, ty);
        Instruction st{Opcode::Store, "", Type::getVoid(), {res, lhsAddr}};
        st.isVolatile = volatileRooted(node->left.get());
        emit(st);
        lastValue = res;
        return;
    }

    if (node->op == "&&" || node->op == "||") {
        Operand l = evaluateExpr(node->left);
        Operand lVal = l;
        lVal = truth(l);

        std::string resVar = newVar();
        emit(Instruction{Opcode::Alloca, resVar, Type::getPtr(Type::getI1()), {}});
        int scVal = (node->op == "||") ? 1 : 0;
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(scVal, Type::getI1()), Operand::var(resVar, Type::getPtr(Type::getI1()))}});

        BasicBlock* rhsBB = createBlock("sc.rhs");
        BasicBlock* contBB = createBlock("sc.cont");

        if (node->op == "&&") {
            emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {lVal, Operand::block(rhsBB->name), Operand::block(contBB->name)}});
        } else {
            emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {lVal, Operand::block(contBB->name), Operand::block(rhsBB->name)}});
        }

        setBlock(rhsBB);
        Operand r = evaluateExpr(node->right);
        Operand rVal = r;
        rVal = truth(r);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {rVal, Operand::var(resVar, Type::getPtr(Type::getI1()))}});
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(contBB->name)}});

        setBlock(contBB);
        std::string finalVal = newVar();
        emit(Instruction{Opcode::Load, finalVal, Type::getI1(), {Operand::var(resVar, Type::getPtr(Type::getI1()))}});
        lastValue = Operand::var(finalVal, Type::getI1());
        return;
    }

    Operand lhs = evaluateExpr(node->left);
    Operand rhs = evaluateExpr(node->right);
    const std::string& op = node->op;
    if (std::string f = instanceOperator(op, {lhs, rhs}); !f.empty()) {
        lastValue = callOperator(f, {lhs, rhs});
        return;
    }
    bool isCmp = op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
    auto isNum = [](const Operand& o) {
        return o.kind != OperandKind::ConstantNull && o.type && (o.type->isInt() || o.type->isFloat());
    };

    // Numbers follow C: narrow integers promote to int, then both operands meet at their
    // common type, the operation's type; a shift has its promoted left operand's type.
    if (isNum(lhs) && isNum(rhs) && op != "&&" && op != "||") {
        std::string dst = newVar();
        if (op == "<<" || op == ">>") {
            lhs = promoteInt(lhs);
            TypePtr t = lhs.type;
            rhs = coerceNumeric(promoteInt(rhs), t);
            emit(Instruction{op == "<<" ? Opcode::Shl : (t->isSigned ? Opcode::AShr : Opcode::LShr), dst, t, {lhs, rhs}});
            lastValue = Operand::var(dst, t);
            return;
        }
        lhs = promoteInt(lhs);
        rhs = promoteInt(rhs);
        TypePtr t = commonType(lhs.type, rhs.type);
        lhs = coerceNumeric(lhs, t);
        rhs = coerceNumeric(rhs, t);
        if (isCmp) {
            if (t->isFloat()) {
                FCmpCond fc = op == "==" ? FCmpCond::Oeq : op == "!=" ? FCmpCond::One : op == "<" ? FCmpCond::Olt
                            : op == "<=" ? FCmpCond::Ole : op == ">" ? FCmpCond::Ogt : FCmpCond::Oge;
                emit(Instruction{Opcode::FCmp, dst, Type::getI1(), {lhs, rhs}, ICmpCond::Eq, fc});
            } else {
                bool sg = t->isSigned;
                ICmpCond ic = op == "==" ? ICmpCond::Eq : op == "!=" ? ICmpCond::Ne
                            : op == "<" ? (sg ? ICmpCond::Slt : ICmpCond::Ult) : op == "<=" ? (sg ? ICmpCond::Sle : ICmpCond::Ule)
                            : op == ">" ? (sg ? ICmpCond::Sgt : ICmpCond::Ugt) : (sg ? ICmpCond::Sge : ICmpCond::Uge);
                emit(Instruction{Opcode::ICmp, dst, Type::getI1(), {lhs, rhs}, ic});
            }
            lastValue = Operand::var(dst, Type::getI1());
            return;
        }
        bool f = t->isFloat(), sg = t->isSigned;
        Opcode opc = Opcode::Add;
        if (op == "+") opc = f ? Opcode::FAdd : Opcode::Add;
        else if (op == "-") opc = f ? Opcode::FSub : Opcode::Sub;
        else if (op == "*") opc = f ? Opcode::FMul : Opcode::Mul;
        else if (op == "/") opc = f ? Opcode::FDiv : (sg ? Opcode::SDiv : Opcode::UDiv);
        else if (op == "%") opc = sg ? Opcode::SRem : Opcode::URem;
        else if (op == "&") opc = Opcode::And;
        else if (op == "|") opc = Opcode::Or;
        else if (op == "^") opc = Opcode::Xor;
        if (f && op == "%") throw std::runtime_error("EIR: `%` on floating-point operands is not supported");
        emit(Instruction{opc, dst, t, {lhs, rhs}});
        lastValue = Operand::var(dst, t);
        return;
    }

    // Pointers (and the rest): pointer arithmetic steps by elements with a 64-bit index.
    if (lhs.type && lhs.type->isPointer() && isNum(rhs)) rhs = coerceNumeric(rhs, Type::getI64());
    if (rhs.type && rhs.type->isPointer() && isNum(lhs)) lhs = coerceNumeric(lhs, Type::getI64());
    TypePtr resTy = getExprEirType(node);
    if (!resTy || (resTy->isInt() && (lhs.type->isFloat() || rhs.type->isFloat()))) {
        resTy = lhs.type->isFloat() ? lhs.type : rhs.type;
    }

    std::string dst = newVar();

    // Arithmetic
    if (node->op == "+") {
        if (resTy->isPointer()) {
            if (lhs.type->isPointer()) {
                emit(Instruction{Opcode::Gep, dst, resTy, {lhs, rhs}});
            } else {
                emit(Instruction{Opcode::Gep, dst, resTy, {rhs, lhs}});
            }
        } else {
            emit(Instruction{resTy->isFloat() ? Opcode::FAdd : Opcode::Add, dst, resTy, {lhs, rhs}});
        }
    } else if (node->op == "-") {
        if (resTy->isPointer() && lhs.type->isPointer() && !rhs.type->isPointer()) {
            std::string negIdx = newVar();
            emit(Instruction{Opcode::Sub, negIdx, rhs.type, {Operand::constInt(0, rhs.type), rhs}});
            emit(Instruction{Opcode::Gep, dst, resTy, {lhs, Operand::var(negIdx, rhs.type)}});
        } else if (lhs.type->isPointer() && rhs.type->isPointer()) {
            std::string rawDiff = newVar();
            emit(Instruction{Opcode::Sub, rawDiff, Type::getI64(), {lhs, rhs}});
            TypePtr elemTy = lhs.type->elemType ? lhs.type->elemType : Type::getI8();
            size_t elemSz = getTypeSize(elemTy);
            if (elemSz > 1) {
                emit(Instruction{Opcode::SDiv, dst, resTy, {
                    Operand::var(rawDiff, Type::getI64()),
                    Operand::constInt(static_cast<int64_t>(elemSz), Type::getI64())
                }});
            } else {
                dst = rawDiff;
            }
        } else {
            emit(Instruction{resTy->isFloat() ? Opcode::FSub : Opcode::Sub, dst, resTy, {lhs, rhs}});
        }
    } else if (node->op == "*") {
        emit(Instruction{resTy->isFloat() ? Opcode::FMul : Opcode::Mul, dst, resTy, {lhs, rhs}});
    } else if (node->op == "/") {
        Opcode opc = resTy->isFloat() ? Opcode::FDiv : (lhs.type->isSigned ? Opcode::SDiv : Opcode::UDiv);
        emit(Instruction{opc, dst, resTy, {lhs, rhs}});
    } else if (node->op == "%") {
        Opcode opc = lhs.type->isSigned ? Opcode::SRem : Opcode::URem;
        emit(Instruction{opc, dst, resTy, {lhs, rhs}});
    }
    // Bitwise
    else if (node->op == "&")  emit(Instruction{Opcode::And, dst, resTy, {lhs, rhs}});
    else if (node->op == "|")  emit(Instruction{Opcode::Or,  dst, resTy, {lhs, rhs}});
    else if (node->op == "^")  emit(Instruction{Opcode::Xor, dst, resTy, {lhs, rhs}});
    else if (node->op == "<<") emit(Instruction{Opcode::Shl, dst, resTy, {lhs, rhs}});
    else if (node->op == ">>") emit(Instruction{lhs.type->isSigned ? Opcode::AShr : Opcode::LShr, dst, resTy, {lhs, rhs}});
    // Comparison
    else if (node->op == "==" || node->op == "!=" || node->op == "<" || node->op == "<=" || node->op == ">" || node->op == ">=") {
        if (lhs.type->isFloat() || rhs.type->isFloat()) {
            FCmpCond fcond = FCmpCond::Oeq;
            if (node->op == "!=") fcond = FCmpCond::One;
            else if (node->op == "<")  fcond = FCmpCond::Olt;
            else if (node->op == "<=") fcond = FCmpCond::Ole;
            else if (node->op == ">")  fcond = FCmpCond::Ogt;
            else if (node->op == ">=") fcond = FCmpCond::Oge;

            emit(Instruction{Opcode::FCmp, dst, Type::getI1(), {lhs, rhs}, ICmpCond::Eq, fcond});
            lastValue = Operand::var(dst, Type::getI1());
            return;
        }

        ICmpCond icond = ICmpCond::Eq;
        if (node->op == "!=") icond = ICmpCond::Ne;
        else if (node->op == "<")  icond = ICmpCond::Ult;
        else if (node->op == "<=") icond = ICmpCond::Ule;
        else if (node->op == ">")  icond = ICmpCond::Ugt;
        else if (node->op == ">=") icond = ICmpCond::Uge;

        emit(Instruction{Opcode::ICmp, dst, Type::getI1(), {lhs, rhs}, icond});
        lastValue = Operand::var(dst, Type::getI1());
        return;
    }
    // Logical
    else if (node->op == "&&") {
        emit(Instruction{Opcode::And, dst, Type::getI1(), {lhs, rhs}});
        lastValue = Operand::var(dst, Type::getI1());
        return;
    } else if (node->op == "||") {
        emit(Instruction{Opcode::Or, dst, Type::getI1(), {lhs, rhs}});
        lastValue = Operand::var(dst, Type::getI1());
        return;
    }

    lastValue = Operand::var(dst, resTy);
}

void EIRBuilder::visit(UnaryExpr* node) {
    if (!node->opFunc.empty()) {
        auto call = std::make_shared<CallExpr>(
            std::make_shared<IdentExpr>(node->opFunc),
            std::vector<ExprPtr>{node->operand});
        call->accept(this);
        return;
    }

    if (node->op == "&") {
        lastValue = evaluateLValue(node->operand);
        return;
    }

    if (node->op == "*") {
        Operand ptr = evaluateExpr(node->operand);
        TypePtr elemTy = getExprEirType(node);
        std::string dst = newVar();
        Instruction ld{Opcode::Load, dst, elemTy, {ptr}};
        ld.isVolatile = volatileRooted(node);
        emit(ld);
        lastValue = Operand::var(dst, elemTy);
        lastLValue = ptr;
        return;
    }

    Operand val = evaluateExpr(node->operand);

    if (node->op == "!") {
        Operand b = coerceNumeric(val, Type::getI1());
        if (b.type && b.type->isInt() && b.type->bitWidth == 1 && b.kind != OperandKind::ConstantNull) {
            std::string dst = newVar();
            emit(Instruction{Opcode::Xor, dst, Type::getI1(), {b, Operand::constInt(1, Type::getI1())}});
            lastValue = Operand::var(dst, Type::getI1());
        } else {
            std::string dst = newVar();
            emit(Instruction{Opcode::ICmp, dst, Type::getI1(), {val, Operand::constNull()}, ICmpCond::Eq});
            lastValue = Operand::var(dst, Type::getI1());
        }
        return;
    }

    if (std::string f = instanceOperator(node->op == "-" ? "u-" : node->op, {val}); !f.empty()) {
        lastValue = callOperator(f, {val});
        return;
    }
    // `+`, `-` and `~` work on the promoted operand (C), so `-u8` is an int.
    val = promoteInt(val);
    if (node->op == "+") {
        lastValue = val;
        return;
    }
    if (val.kind == OperandKind::ConstantInt) {
        int64_t r = node->op == "-" ? static_cast<int64_t>(0ULL - static_cast<uint64_t>(val.intVal)) : ~val.intVal;
        lastValue = Operand::constInt(truncConst(r, val.type), val.type);
        return;
    }
    if (val.kind == OperandKind::ConstantFloat && node->op == "-") {
        lastValue = Operand::constFloat(-val.floatVal, val.type);
        return;
    }
    std::string dst = newVar();
    if (node->op == "-") {
        if (val.type->isFloat()) emit(Instruction{Opcode::FSub, dst, val.type, {Operand::constFloat(-0.0, val.type), val}});
        else emit(Instruction{Opcode::Sub, dst, val.type, {Operand::constInt(0, val.type), val}});
    } else if (node->op == "~") {
        emit(Instruction{Opcode::Not, dst, val.type, {val}});
    }

    lastValue = Operand::var(dst, val.type);
}

} // namespace eir
