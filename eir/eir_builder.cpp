#include "eir_builder.h"
#include "../ast/type_qual.h"
#include <iostream>
#include <stdexcept>

namespace eir {

EIRBuilder::EIRBuilder() = default;

std::string EIRBuilder::newVar() {
    return "%" + std::to_string(nextVarId++);
}

BasicBlock* EIRBuilder::createBlock(const std::string& prefix) {
    std::string bname = prefix + "." + std::to_string(nextBlockId++);
    return currentFunction->createBlock(bname);
}

void EIRBuilder::setBlock(BasicBlock* bb) {
    currentBlock = bb;
}

void EIRBuilder::emit(Instruction inst) {
    if (!currentBlock) return;
    if (currentBlock->hasTerminator()) return; // Don't emit dead code after terminator
    currentBlock->instructions.push_back(std::make_unique<Instruction>(std::move(inst)));
}

void EIRBuilder::pushScope() {
    scopeStack.emplace_back();
    varTypeStack.emplace_back();
}

void EIRBuilder::popScope() {
    if (!scopeStack.empty()) scopeStack.pop_back();
    if (!varTypeStack.empty()) varTypeStack.pop_back();
}

void EIRBuilder::declareVar(const std::string& name, Operand allocaSlot, TypePtr type) {
    if (!scopeStack.empty()) {
        scopeStack.back()[name] = allocaSlot;
        varTypeStack.back()[name] = type;
    }
}

Operand* EIRBuilder::lookupVar(const std::string& name) {
    for (auto it = scopeStack.rbegin(); it != scopeStack.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

TypePtr EIRBuilder::lookupVarType(const std::string& name) {
    for (auto it = varTypeStack.rbegin(); it != varTypeStack.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return found->second;
    }
    return Type::getI32();
}

TypePtr EIRBuilder::toEirType(const std::string& typeStr) {
    std::string s = tyq::strip(typeStr);
    ty::Type t = ty::Type::parse(s);

    switch (t.kind) {
        case ty::Type::Kind::Void:  return Type::getVoid();
        case ty::Type::Kind::Bool:  return Type::getI1();
        case ty::Type::Kind::Char:  return Type::getU8();
        case ty::Type::Kind::Float:
            return (t.name == "float") ? Type::getF32() : Type::getF64();
        case ty::Type::Kind::Int: {
            if (t.name == "int8")   return Type::getI8();
            if (t.name == "uint8")  return Type::getU8();
            if (t.name == "int16")  return Type::getI16();
            if (t.name == "uint16") return Type::getU16();
            if (t.name == "int64")  return Type::getI64();
            if (t.name == "uint64") return Type::getU64();
            if (t.name == "uint" || t.name == "uint32") return Type::getU32();
            return Type::getI32();
        }
        case ty::Type::Kind::Pointer:
        case ty::Type::Kind::String:
            return Type::getPtr(t.pointee ? toEirType(t.pointee->str()) : Type::getU8());
        case ty::Type::Kind::Slice:
            return Type::getSlice(t.elem ? toEirType(t.elem->str()) : Type::getU8());
        case ty::Type::Kind::Array: {
            uint64_t n = 0;
            if (!t.dim.empty()) {
                try { n = std::stoull(t.dim); } catch (...) { n = 0; }
            }
            return Type::getArray(t.elem ? toEirType(t.elem->str()) : Type::getI32(), n);
        }
        case ty::Type::Kind::Named:
        case ty::Type::Kind::Struct: {
            std::string sname = t.nominalName();
            if (sname.empty()) sname = t.name;
            if (module) {
                auto it = module->structLayouts.find(sname);
                if (it != module->structLayouts.end()) {
                    std::vector<TypePtr> flds;
                    for (const auto& f : it->second.fields) flds.push_back(f.type);
                    return Type::getStruct(sname, flds);
                }
            }
            return Type::getStruct(sname, {});
        }
        case ty::Type::Kind::Interface:
            return Type::getInterface();
        case ty::Type::Kind::Fn:
            return Type::getClosure();
        default:
            return Type::getI32();
    }
}

TypePtr EIRBuilder::getExprEirType(const Expr* expr) {
    if (!expr) return Type::getI32();
    if (resolvedExprTypes) {
        auto it = resolvedExprTypes->find(const_cast<Expr*>(expr));
        if (it != resolvedExprTypes->end()) {
            return toEirType(it->second);
        }
    }
    return Type::getI32();
}

TypePtr EIRBuilder::getExprEirType(const ExprPtr& expr) {
    return getExprEirType(expr.get());
}

Operand EIRBuilder::evaluateExpr(const ExprPtr& expr) {
    if (!expr) return Operand::constInt(0, Type::getI32());
    expr->accept(this);
    return lastValue;
}

Operand EIRBuilder::evaluateLValue(const ExprPtr& expr) {
    if (!expr) return Operand::constNull();
    expr->accept(this);
    return lastLValue;
}

std::unique_ptr<Module> EIRBuilder::build(Program* program) {
    module = std::make_unique<Module>();
    module->name = "main";
    module->targetTriple = targetTriple;

    if (!program) return std::move(module);

    // Phase 1: Structs and Interfaces
    for (auto& decl : program->declarations) {
        if (auto s = dynamic_cast<StructDecl*>(decl.get())) {
            std::vector<TypePtr> fields;
            StructInfo sinfo;
            sinfo.name = s->name;
            size_t currentOffset = 0;
            size_t maxAlign = 1;
            for (const auto& f : s->fields) {
                TypePtr ft = toEirType(f.type);
                fields.push_back(ft);
                size_t sz = getTypeSize(ft);
                size_t al = getTypeAlign(ft);
                if (al > maxAlign) maxAlign = al;
                currentOffset = (currentOffset + al - 1) & ~(al - 1);
                sinfo.fields.push_back({f.name, ft, currentOffset, sz});
                currentOffset += sz;
            }
            sinfo.totalSize = (currentOffset + maxAlign - 1) & ~(maxAlign - 1);
            sinfo.alignment = maxAlign;
            module->structLayouts[s->name] = sinfo;
            module->structTypes.push_back(Type::getStruct(s->name, fields));
        }
    }

    // Phase 2: Function prototypes
    for (auto& decl : program->declarations) {
        if (auto f = dynamic_cast<FunctionDecl*>(decl.get())) {
            std::vector<std::pair<std::string, TypePtr>> params;
            for (const auto& p : f->params) {
                params.emplace_back(p.second, toEirType(p.first));
            }
            module->createFunction(f->name, toEirType(f->returnType), params);
        } else if (auto ext = dynamic_cast<ExternDecl*>(decl.get())) {
            std::vector<std::pair<std::string, TypePtr>> params;
            bool hasVarargs = false;
            for (const auto& p : ext->params) {
                if (p.first == "...") {
                    hasVarargs = true;
                    continue;
                }
                params.emplace_back(p.second, toEirType(p.first));
            }
            auto fn = module->createFunction(ext->name, toEirType(ext->returnType), params);
            fn->isExtern = true;
            fn->isVariadic = hasVarargs;
        } else if (auto s = dynamic_cast<StructDecl*>(decl.get())) {
            for (const auto& m : s->methods) {
                if (auto mf = dynamic_cast<FunctionDecl*>(m.get())) {
                    std::vector<std::pair<std::string, TypePtr>> params;
                    params.emplace_back("self", Type::getPtr(Type::getStruct(s->name, {})));
                    for (const auto& p : mf->params) {
                        params.emplace_back(p.second, toEirType(p.first));
                    }
                    module->createFunction(s->name + "_" + mf->name, toEirType(mf->returnType), params);
                }
            }
        }
    }

    // Phase 3: Function bodies
    program->accept(this);

    return std::move(module);
}

// ============================================================================
// Visitors
// ============================================================================

void EIRBuilder::visit(Program* node) {
    for (auto& decl : node->declarations) {
        decl->accept(this);
    }
}

void EIRBuilder::visit(FunctionDecl* node) {
    currentFunction = module->getFunction(node->name);
    if (!currentFunction) {
        std::vector<std::pair<std::string, TypePtr>> params;
        for (const auto& p : node->params) {
            params.emplace_back(p.second, toEirType(p.first));
        }
        currentFunction = module->createFunction(node->name, toEirType(node->returnType), params);
    }

    if (!node->body) {
        currentFunction->isExtern = true;
        return;
    }

    currentFunction->isExtern = false;

    nextVarId = 0;
    nextBlockId = 0;
    BasicBlock* entryBB = currentFunction->createBlock("entry");
    setBlock(entryBB);

    pushScope();

    // Allocate stack slots for each parameter
    for (size_t i = 0; i < currentFunction->params.size(); ++i) {
        const auto& p = currentFunction->params[i];
        std::string slotName = "%" + p.first + ".addr";
        emit(Instruction{Opcode::Alloca, slotName, p.second, {Operand::constInt(p.second->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var("%" + p.first, p.second), Operand::var(slotName, Type::getPtr(p.second))}});
        declareVar(p.first, Operand::var(slotName, Type::getPtr(p.second)), p.second);
    }

    node->body->accept(this);

    // Ensure terminator
    if (!currentBlock->hasTerminator()) {
        if (currentFunction->returnType->isVoid()) {
            emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
        } else {
            emit(Instruction{Opcode::Unreachable, "", Type::getVoid(), {}});
        }
    }

    popScope();
    currentFunction = nullptr;
    currentBlock = nullptr;
}

void EIRBuilder::visit(VarDecl* node) {
    TypePtr type = toEirType(node->type);
    std::string slotName = "%" + node->name + ".addr";
    emit(Instruction{Opcode::Alloca, slotName, type, {Operand::constInt(type->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
    declareVar(node->name, Operand::var(slotName, Type::getPtr(type)), type);

    if (node->initializer) {
        Operand val = evaluateExpr(node->initializer);
        if (type->isStruct()) {
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(val.str(), type), Operand::var(slotName, Type::getPtr(type))}});
        } else {
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {val, Operand::var(slotName, Type::getPtr(type))}});
        }
    }
}

void EIRBuilder::visit(BlockStmt* node) {
    pushScope();
    for (auto& item : node->items) {
        if (currentBlock->hasTerminator()) break;
        if (std::holds_alternative<DeclPtr>(item)) {
            std::get<DeclPtr>(item)->accept(this);
        } else if (std::holds_alternative<StmtPtr>(item)) {
            std::get<StmtPtr>(item)->accept(this);
        }
    }
    popScope();
}

void EIRBuilder::visit(ReturnStmt* node) {
    if (node->value) {
        Operand val = evaluateExpr(node->value);
        emit(Instruction{Opcode::Ret, "", val.type, {val}});
    } else {
        emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
    }
}

void EIRBuilder::visit(IfStmt* node) {
    Operand cond = evaluateExpr(node->condition);

    if (cond.type->kind != TypeKind::Int || cond.type->bitWidth != 1) {
        std::string cmpDst = newVar();
        emit(Instruction{Opcode::ICmp, cmpDst, Type::getI1(), {cond, Operand::constInt(0, cond.type)}, ICmpCond::Ne});
        cond = Operand::var(cmpDst, Type::getI1());
    }

    BasicBlock* thenBB = createBlock("then");
    BasicBlock* elseBB = node->elseBranch ? createBlock("else") : nullptr;
    BasicBlock* mergeBB = createBlock("merge");

    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(thenBB->name), Operand::block(elseBB ? elseBB->name : mergeBB->name)}});

    // Then
    setBlock(thenBB);
    node->thenBranch->accept(this);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});
    }

    // Else
    if (elseBB) {
        setBlock(elseBB);
        node->elseBranch->accept(this);
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
    if (cond.type->kind != TypeKind::Int || cond.type->bitWidth != 1) {
        std::string cmpDst = newVar();
        emit(Instruction{Opcode::ICmp, cmpDst, Type::getI1(), {cond, Operand::constInt(0, cond.type)}, ICmpCond::Ne});
        cond = Operand::var(cmpDst, Type::getI1());
    }
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(bodyBB->name), Operand::block(exitBB->name)}});

    setBlock(bodyBB);
    breakStack.push_back(exitBB);
    continueStack.push_back(condBB);

    node->body->accept(this);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});
    }

    breakStack.pop_back();
    continueStack.pop_back();

    setBlock(exitBB);
}

void EIRBuilder::visit(ForStmt* node) {
    pushScope();
    if (auto* ib = dynamic_cast<BlockStmt*>(node->init.get())) {
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
        if (cond.type->kind != TypeKind::Int || cond.type->bitWidth != 1) {
            std::string cmpDst = newVar();
            emit(Instruction{Opcode::ICmp, cmpDst, Type::getI1(), {cond, Operand::constInt(0, cond.type)}, ICmpCond::Ne});
            cond = Operand::var(cmpDst, Type::getI1());
        }
        emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(bodyBB->name), Operand::block(exitBB->name)}});
    } else {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(bodyBB->name)}});
    }

    setBlock(bodyBB);
    breakStack.push_back(exitBB);
    continueStack.push_back(stepBB);

    node->body->accept(this);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(stepBB->name)}});
    }

    breakStack.pop_back();
    continueStack.pop_back();

    setBlock(stepBB);
    if (node->step) evaluateExpr(node->step);
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

    setBlock(exitBB);
    popScope();
}

void EIRBuilder::visit(BreakStmt*) {
    if (!breakStack.empty()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(breakStack.back()->name)}});
    }
}

void EIRBuilder::visit(ContinueStmt*) {
    if (!continueStack.empty()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(continueStack.back()->name)}});
    }
}

void EIRBuilder::visit(ExprStmt* node) {
    if (node->expr) evaluateExpr(node->expr);
}

void EIRBuilder::visit(BinaryExpr* node) {
    // Assignment: =
    if (node->op == "=") {
        Operand rhs = evaluateExpr(node->right);
        Operand lhsAddr = evaluateLValue(node->left);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {rhs, lhsAddr}});
        lastValue = rhs;
        return;
    }

    // Compound assignment: +=, -=, *=, /=, %=, &=, |=, ^=, <<=, >>=
    if (node->op == "+=" || node->op == "-=" || node->op == "*=" ||
        node->op == "/=" || node->op == "%=" || node->op == "&=" ||
        node->op == "|=" || node->op == "^=" || node->op == "<<=" || node->op == ">>=") {
        std::string baseOp = node->op.substr(0, node->op.find('='));
        Operand lhsAddr = evaluateLValue(node->left);
        TypePtr ty = getExprEirType(node->left);

        std::string curVal = newVar();
        emit(Instruction{Opcode::Load, curVal, ty, {lhsAddr}});

        Operand rhs = evaluateExpr(node->right);
        std::string resVar = newVar();

        Opcode binOp = Opcode::Add;
        if (baseOp == "-") binOp = Opcode::Sub;
        else if (baseOp == "*") binOp = Opcode::Mul;
        else if (baseOp == "/") binOp = ty->isFloat() ? Opcode::FDiv : (ty->isSigned ? Opcode::SDiv : Opcode::UDiv);
        else if (baseOp == "%") binOp = ty->isSigned ? Opcode::SRem : Opcode::URem;
        else if (baseOp == "&") binOp = Opcode::And;
        else if (baseOp == "|") binOp = Opcode::Or;
        else if (baseOp == "^") binOp = Opcode::Xor;
        else if (baseOp == "<<") binOp = Opcode::Shl;
        else if (baseOp == ">>") binOp = ty->isSigned ? Opcode::AShr : Opcode::LShr;

        emit(Instruction{binOp, resVar, ty, {Operand::var(curVal, ty), rhs}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(resVar, ty), lhsAddr}});
        lastValue = Operand::var(resVar, ty);
        return;
    }

    Operand lhs = evaluateExpr(node->left);
    Operand rhs = evaluateExpr(node->right);
    TypePtr resTy = getExprEirType(node);
    if (!resTy || (resTy->isInt() && (lhs.type->isFloat() || rhs.type->isFloat()))) {
        resTy = lhs.type->isFloat() ? lhs.type : rhs.type;
    }

    std::string dst = newVar();

    // Arithmetic
    if (node->op == "+") {
        emit(Instruction{resTy->isFloat() ? Opcode::FAdd : Opcode::Add, dst, resTy, {lhs, rhs}});
    } else if (node->op == "-") {
        emit(Instruction{resTy->isFloat() ? Opcode::FSub : Opcode::Sub, dst, resTy, {lhs, rhs}});
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
        ICmpCond icond = ICmpCond::Eq;
        if (node->op == "!=") icond = ICmpCond::Ne;
        else if (node->op == "<")  icond = lhs.type->isSigned ? ICmpCond::Slt : ICmpCond::Ult;
        else if (node->op == "<=") icond = lhs.type->isSigned ? ICmpCond::Sle : ICmpCond::Ule;
        else if (node->op == ">")  icond = lhs.type->isSigned ? ICmpCond::Sgt : ICmpCond::Ugt;
        else if (node->op == ">=") icond = lhs.type->isSigned ? ICmpCond::Sge : ICmpCond::Uge;

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
    if (node->op == "&") {
        lastValue = evaluateLValue(node->operand);
        return;
    }

    if (node->op == "*") {
        Operand ptr = evaluateExpr(node->operand);
        TypePtr elemTy = getExprEirType(node);
        std::string dst = newVar();
        emit(Instruction{Opcode::Load, dst, elemTy, {ptr}});
        lastValue = Operand::var(dst, elemTy);
        lastLValue = ptr;
        return;
    }

    Operand val = evaluateExpr(node->operand);
    std::string dst = newVar();

    if (node->op == "-") {
        emit(Instruction{val.type->isFloat() ? Opcode::FSub : Opcode::Sub, dst, val.type, {Operand::constInt(0, val.type), val}});
    } else if (node->op == "!") {
        emit(Instruction{Opcode::ICmp, dst, Type::getI1(), {val, Operand::constInt(0, val.type)}, ICmpCond::Eq});
        lastValue = Operand::var(dst, Type::getI1());
        return;
    } else if (node->op == "~") {
        emit(Instruction{Opcode::Not, dst, val.type, {val}});
    }

    lastValue = Operand::var(dst, val.type);
}

void EIRBuilder::visit(IncDecExpr* node) {
    Operand addr = evaluateLValue(node->operand);
    TypePtr ty = getExprEirType(node->operand);

    std::string cur = newVar();
    emit(Instruction{Opcode::Load, cur, ty, {addr}});

    std::string next = newVar();
    emit(Instruction{node->decrement ? Opcode::Sub : Opcode::Add, next, ty, {Operand::var(cur, ty), Operand::constInt(1, ty)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(next, ty), addr}});

    lastValue = node->prefix ? Operand::var(next, ty) : Operand::var(cur, ty);
}

void EIRBuilder::visit(IdentExpr* node) {
    Operand* slot = lookupVar(node->name);
    if (!slot) {
        lastValue = Operand::global(node->name, Type::getPtr());
        lastLValue = lastValue;
        return;
    }
    TypePtr ty = lookupVarType(node->name);
    std::string dst = newVar();
    emit(Instruction{Opcode::Load, dst, ty, {*slot}});
    lastValue = Operand::var(dst, ty);
    lastLValue = *slot;
}

void EIRBuilder::visit(LiteralExpr* node) {
    TypePtr ty = getExprEirType(node);
    if (node->kind == LiteralExpr::Kind::FLOAT && ty->isInt()) {
        ty = Type::getF32();
    }
    switch (node->kind) {
        case LiteralExpr::Kind::INT: {
            int64_t v = 0;
            try { v = std::stoll(node->value, nullptr, 0); } catch (...) { v = 0; }
            lastValue = Operand::constInt(v, ty);
            break;
        }
        case LiteralExpr::Kind::FLOAT: {
            double v = 0.0;
            try { v = std::stod(node->value); } catch (...) { v = 0.0; }
            lastValue = Operand::constFloat(v, ty);
            break;
        }
        case LiteralExpr::Kind::STRING:
            lastValue = Operand::constString(node->value);
            break;
        case LiteralExpr::Kind::CHAR: {
            int64_t v = (!node->value.empty()) ? (int64_t)(unsigned char)node->value[0] : 0;
            lastValue = Operand::constInt(v, Type::getU8());
            break;
        }
        case LiteralExpr::Kind::BOOL:
            lastValue = Operand::constInt((node->value == "true") ? 1 : 0, Type::getI1());
            break;
        case LiteralExpr::Kind::NULL_VAL:
            lastValue = Operand::constNull();
            break;
    }
}

void EIRBuilder::visit(CallExpr* node) {
    std::string fnName;
    std::vector<Operand> args;

    if (auto id = dynamic_cast<IdentExpr*>(node->callee.get())) {
        fnName = id->name;
        args.push_back(Operand::global(fnName, Type::getPtr()));
        for (const auto& arg : node->args) {
            args.push_back(evaluateExpr(arg));
        }
    } else if (auto mem = dynamic_cast<MemberExpr*>(node->callee.get())) {
        std::string baseTypeName;
        TypePtr baseTy = getExprEirType(mem->base);
        if (baseTy && baseTy->isStruct()) {
            baseTypeName = baseTy->nominalName();
        } else if (baseTy && baseTy->isPointer() && baseTy->elemType && baseTy->elemType->isStruct()) {
            baseTypeName = baseTy->elemType->nominalName();
        } else if (resolvedExprTypes) {
            auto it = resolvedExprTypes->find(mem->base.get());
            if (it != resolvedExprTypes->end()) {
                baseTypeName = ty::Type::parse(tyq::strip(it->second)).nominalName();
            }
        }
        fnName = baseTypeName + "_" + mem->member;
        args.push_back(Operand::global(fnName, Type::getPtr()));

        // Receiver argument (self)
        if (baseTy && baseTy->isPointer()) {
            args.push_back(evaluateExpr(mem->base));
        } else {
            args.push_back(evaluateLValue(mem->base));
        }

        for (const auto& arg : node->args) {
            args.push_back(evaluateExpr(arg));
        }
    } else {
        Operand calleeOp = evaluateExpr(node->callee);
        fnName = calleeOp.str();
        args.push_back(calleeOp);
        for (const auto& arg : node->args) {
            args.push_back(evaluateExpr(arg));
        }
    }

    TypePtr retTy = getExprEirType(node);
    std::string dst = retTy->isVoid() ? "" : newVar();

    emit(Instruction{Opcode::Call, dst, retTy, args});
    lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
}

void EIRBuilder::visit(CastExpr* node) {
    Operand val = evaluateExpr(node->expr);
    TypePtr toTy = toEirType(node->targetType);
    std::string dst = newVar();

    if (val.type->isInt() && toTy->isInt()) {
        if (toTy->bitWidth < val.type->bitWidth) {
            emit(Instruction{Opcode::Trunc, dst, toTy, {val}});
        } else if (toTy->bitWidth > val.type->bitWidth) {
            emit(Instruction{val.type->isSigned ? Opcode::SExt : Opcode::ZExt, dst, toTy, {val}});
        } else {
            dst = val.str();
        }
    } else if (val.type->isFloat() && toTy->isInt()) {
        emit(Instruction{Opcode::FPToSI, dst, toTy, {val}});
    } else if (val.type->isInt() && toTy->isFloat()) {
        emit(Instruction{Opcode::SIToFP, dst, toTy, {val}});
    } else if (val.type->isPointer() && toTy->isPointer()) {
        emit(Instruction{Opcode::Bitcast, dst, toTy, {val}});
    } else {
        dst = val.str();
    }

    lastValue = Operand::var(dst, toTy);
}

void EIRBuilder::visit(TernaryExpr* node) {
    Operand cond = evaluateExpr(node->condition);
    BasicBlock* thenBB = createBlock("ternary.then");
    BasicBlock* elseBB = createBlock("ternary.else");
    BasicBlock* mergeBB = createBlock("ternary.merge");

    TypePtr resTy = getExprEirType(node);
    std::string resultSlot = newVar() + ".ternary";
    emit(Instruction{Opcode::Alloca, resultSlot, resTy, {Operand::constInt(resTy->bitWidth <= 32 ? 4 : 8, Type::getI32())}});

    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(thenBB->name), Operand::block(elseBB->name)}});

    setBlock(thenBB);
    Operand trueVal = evaluateExpr(node->thenExpr);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {trueVal, Operand::var(resultSlot, Type::getPtr(resTy))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});

    setBlock(elseBB);
    Operand falseVal = evaluateExpr(node->elseExpr);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {falseVal, Operand::var(resultSlot, Type::getPtr(resTy))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});

    setBlock(mergeBB);
    std::string dst = newVar();
    emit(Instruction{Opcode::Load, dst, resTy, {Operand::var(resultSlot, Type::getPtr(resTy))}});
    lastValue = Operand::var(dst, resTy);
}

void EIRBuilder::visit(MemberExpr* node) {
    TypePtr objTy = getExprEirType(node->base);
    std::string structName;
    if (objTy && objTy->isStruct()) {
        structName = objTy->nominalName();
    } else if (objTy && objTy->isPointer() && objTy->elemType && objTy->elemType->isStruct()) {
        structName = objTy->elemType->nominalName();
    } else if (resolvedExprTypes) {
        auto it = resolvedExprTypes->find(node->base.get());
        if (it != resolvedExprTypes->end()) {
            structName = ty::Type::parse(tyq::strip(it->second)).nominalName();
        }
    }

    Operand basePtr;
    if (objTy && objTy->isPointer()) {
        basePtr = evaluateExpr(node->base);
    } else {
        basePtr = evaluateLValue(node->base);
    }

    TypePtr memberTy = nullptr;
    if (module && !structName.empty()) {
        auto it = module->structLayouts.find(structName);
        if (it != module->structLayouts.end()) {
            for (const auto& f : it->second.fields) {
                if (f.name == node->member) {
                    memberTy = f.type;
                    break;
                }
            }
        }
    }
    if (!memberTy) memberTy = getExprEirType(node);

    std::string fptr = newVar();
    emit(Instruction{Opcode::FieldPtr, fptr, Type::getPtr(memberTy), {Operand::var(structName, nullptr), basePtr, Operand::constString(node->member)}});

    std::string loaded = newVar();
    emit(Instruction{Opcode::Load, loaded, memberTy, {Operand::var(fptr, Type::getPtr(memberTy))}});
    lastValue = Operand::var(loaded, memberTy);
    lastLValue = Operand::var(fptr, Type::getPtr(memberTy));
}

void EIRBuilder::visit(IndexExpr* node) {
    Operand basePtr = evaluateLValue(node->base);
    Operand idx = evaluateExpr(node->index);
    TypePtr elemTy = getExprEirType(node);

    std::string gepDst = newVar();
    emit(Instruction{Opcode::Gep, gepDst, Type::getPtr(elemTy), {basePtr, idx}});

    std::string loaded = newVar();
    emit(Instruction{Opcode::Load, loaded, elemTy, {Operand::var(gepDst, Type::getPtr(elemTy))}});
    lastValue = Operand::var(loaded, elemTy);
    lastLValue = Operand::var(gepDst, Type::getPtr(elemTy));
}

void EIRBuilder::visit(StructDecl* node) {
    for (const auto& m : node->methods) {
        if (auto mf = dynamic_cast<FunctionDecl*>(m.get())) {
            std::vector<std::pair<std::string, std::string>> params;
            params.push_back({"*" + node->name, "self"});
            for (const auto& p : mf->params) params.push_back(p);
            auto mangled = std::make_shared<FunctionDecl>(
                node->name + "_" + mf->name,
                mf->returnType, params, mf->body);
            mangled->accept(this);
        }
    }
}
void EIRBuilder::visit(ExternDecl*) {}
void EIRBuilder::visit(IntrinsicDecl*) {}
void EIRBuilder::visit(InterfaceDecl*) {}
void EIRBuilder::visit(EnumDecl*) {}
void EIRBuilder::visit(TypeAliasDecl*) {}
void EIRBuilder::visit(ForInStmt*) {}
void EIRBuilder::visit(DoWhileStmt*) {}
void EIRBuilder::visit(SwitchStmt*) {}
void EIRBuilder::visit(MatchStmt*) {}
void EIRBuilder::visit(QuestionExpr*) {}

void EIRBuilder::visit(StructInitExpr* node) {
    std::string sname = node->structName;
    TypePtr stype = Type::getStruct(sname, {});
    std::string tempSlot = newVar();
    emit(Instruction{Opcode::Alloca, tempSlot, stype, {Operand::constInt(8, Type::getI32())}});

    for (size_t i = 0; i < node->fieldInits.size(); ++i) {
        std::string fname = node->fieldInits[i].first;
        ExprPtr fval = node->fieldInits[i].second;
        if (!fval) continue;

        TypePtr ftype = nullptr;
        if (module) {
            auto it = module->structLayouts.find(sname);
            if (it != module->structLayouts.end()) {
                if (fname.empty() && i < it->second.fields.size()) {
                    fname = it->second.fields[i].name;
                }
                for (const auto& f : it->second.fields) {
                    if (f.name == fname) {
                        ftype = f.type;
                        break;
                    }
                }
            }
        }
        if (!ftype) ftype = getExprEirType(fval);

        std::string fptr = newVar();
        emit(Instruction{Opcode::FieldPtr, fptr, Type::getPtr(ftype), {
            Operand::var(sname, nullptr),
            Operand::var(tempSlot, Type::getPtr(stype)),
            Operand::constString(fname)
        }});

        Operand val = evaluateExpr(fval);
        if (ftype->isFloat() && val.type->isFloat() && val.type->bitWidth != ftype->bitWidth) {
            val.type = ftype;
        }
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {val, Operand::var(fptr, Type::getPtr(ftype))}});
    }

    lastLValue = Operand::var(tempSlot, Type::getPtr(stype));
    lastValue = lastLValue;
}
void EIRBuilder::visit(ArrayLitExpr*) {}
void EIRBuilder::visit(AllocWithExpr*) {}
void EIRBuilder::visit(TemplateCallExpr*) {}
void EIRBuilder::visit(LambdaExpr*) {}
void EIRBuilder::visit(AsmStmt*) {}
void EIRBuilder::visit(UnionDecl*) {}
void EIRBuilder::visit(SizeofExpr*) {}
void EIRBuilder::visit(ThreadCreateExpr*) {}
void EIRBuilder::visit(FreeClosureExpr*) {}
void EIRBuilder::visit(AwaitExpr*) {}
void EIRBuilder::visit(ThreadJoinStmt*) {}
void EIRBuilder::visit(ThrowStmt*) {}
void EIRBuilder::visit(TryStmt*) {}
void EIRBuilder::visit(DeferStmt*) {}

} // namespace eir
