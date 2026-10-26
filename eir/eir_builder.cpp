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
            if (plainEnums.count(sname)) {
                return Type::getI32();
            }
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

int64_t EIRBuilder::foldConstInt(const ExprPtr& expr) {
    if (!expr) return 0;
    if (auto lit = dynamic_cast<LiteralExpr*>(expr.get())) {
        if (lit->kind == LiteralExpr::Kind::INT) {
            try { return std::stoll(lit->value, nullptr, 0); } catch (...) { return 0; }
        }
        if (lit->kind == LiteralExpr::Kind::CHAR) {
            return (!lit->value.empty()) ? static_cast<int64_t>(static_cast<unsigned char>(lit->value[0])) : 0;
        }
        if (lit->kind == LiteralExpr::Kind::BOOL) {
            return (lit->value == "true") ? 1 : 0;
        }
    } else if (auto id = dynamic_cast<IdentExpr*>(expr.get())) {
        auto it = enumConstants.find(id->name);
        if (it != enumConstants.end()) return it->second;
    } else if (auto u = dynamic_cast<UnaryExpr*>(expr.get())) {
        int64_t sub = foldConstInt(u->operand);
        if (u->op == "-") return -sub;
        if (u->op == "~") return ~sub;
        if (u->op == "!") return sub == 0 ? 1 : 0;
    } else if (auto b = dynamic_cast<BinaryExpr*>(expr.get())) {
        int64_t left = foldConstInt(b->left);
        int64_t right = foldConstInt(b->right);
        if (b->op == "+") return left + right;
        if (b->op == "-") return left - right;
        if (b->op == "*") return left * right;
        if (b->op == "/") return right != 0 ? left / right : 0;
        if (b->op == "%") return right != 0 ? left % right : 0;
        if (b->op == "&") return left & right;
        if (b->op == "|") return left | right;
        if (b->op == "^") return left ^ right;
        if (b->op == "<<") return left << right;
        if (b->op == ">>") return left >> right;
        if (b->op == "==") return left == right ? 1 : 0;
        if (b->op == "!=") return left != right ? 1 : 0;
        if (b->op == "<") return left < right ? 1 : 0;
        if (b->op == "<=") return left <= right ? 1 : 0;
        if (b->op == ">") return left > right ? 1 : 0;
        if (b->op == ">=") return left >= right ? 1 : 0;
    } else if (auto c = dynamic_cast<CastExpr*>(expr.get())) {
        return foldConstInt(c->expr);
    }
    return 0;
}

void EIRBuilder::emitArrayInitInto(Operand dest, ArrayLitExpr* lit, TypePtr arrTy) {
    if (!lit || !arrTy || !arrTy->isArray()) return;
    for (size_t i = 0; i < lit->elements.size() && i < arrTy->arraySize; ++i) {
        std::string elemPtr = newVar();
        emit(Instruction{Opcode::Gep, elemPtr, Type::getPtr(arrTy->elemType), {
            dest, Operand::constInt(static_cast<int64_t>(i), Type::getI32())
        }});
        if (arrTy->elemType->isArray()) {
            if (auto subLit = dynamic_cast<ArrayLitExpr*>(lit->elements[i].get())) {
                emitArrayInitInto(Operand::var(elemPtr, Type::getPtr(arrTy->elemType)), subLit, arrTy->elemType);
            } else {
                Operand subVal = evaluateExpr(lit->elements[i]);
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {subVal, Operand::var(elemPtr, Type::getPtr(arrTy->elemType))}});
            }
        } else {
            Operand val = evaluateExpr(lit->elements[i]);
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {val, Operand::var(elemPtr, Type::getPtr(arrTy->elemType))}});
        }
    }
}

std::unique_ptr<Module> EIRBuilder::build(Program* program) {
    module = std::make_unique<Module>();
    module->name = "main";
    module->targetTriple = targetTriple;

    if (!program) return std::move(module);

    // Phase 1: Structs, Enums, and Interfaces
    for (auto& decl : program->declarations) {
        if (auto e = dynamic_cast<EnumDecl*>(decl.get())) {
            if (e->isADT()) {
                adtEnums[e->name] = e;
                size_t maxPayloadSize = 0;
                for (size_t i = 0; i < e->members.size(); ++i) {
                    const std::string& vname = e->members[i].first;
                    std::vector<TypePtr> payloadTypes;
                    size_t pOffset = 0;
                    size_t pMaxAlign = 1;
                    if (i < e->payloads.size()) {
                        for (const auto& ptStr : e->payloads[i]) {
                            TypePtr pt = toEirType(ptStr);
                            payloadTypes.push_back(pt);
                            size_t al = getTypeAlign(pt);
                            if (al > pMaxAlign) pMaxAlign = al;
                            pOffset = (pOffset + al - 1) & ~(al - 1);
                            pOffset += getTypeSize(pt);
                        }
                    }
                    pOffset = (pOffset + pMaxAlign - 1) & ~(pMaxAlign - 1);
                    if (pOffset > maxPayloadSize) maxPayloadSize = pOffset;
                    adtVariants[vname] = {e->name, static_cast<int>(i), payloadTypes};
                }
                StructInfo sinfo;
                sinfo.name = e->name;
                sinfo.fields.push_back({"tag", Type::getI32(), 0, 4});
                sinfo.totalSize = ((8 + maxPayloadSize) + 7) & ~7;
                sinfo.alignment = 8;
                module->structLayouts[e->name] = sinfo;
                module->structTypes.push_back(Type::getStruct(e->name, {Type::getI32()}));
            } else {
                plainEnums[e->name] = e;
                int64_t nextVal = 0;
                for (size_t i = 0; i < e->members.size(); ++i) {
                    const std::string& mname = e->members[i].first;
                    if (i < e->valueExprs.size() && e->valueExprs[i]) {
                        nextVal = foldConstInt(e->valueExprs[i]);
                    } else if (i < e->members.size()) {
                        nextVal = e->members[i].second;
                    }
                    enumConstants[mname] = nextVal;
                    nextVal++;
                }
            }
        } else if (auto s = dynamic_cast<StructDecl*>(decl.get())) {
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
        } else if (auto v = dynamic_cast<VarDecl*>(decl.get())) {
            TypePtr type = toEirType(v->type);
            Operand initOp = Operand::constInt(0, type);
            if (v->initializer) {
                int64_t cv = foldConstInt(v->initializer);
                if (v->isConst) {
                    enumConstants[v->name] = cv;
                }
                initOp = Operand::constInt(cv, type);
            }
            module->globals.push_back(GlobalVar{v->name, type, initOp, v->isConst});
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
    if (!currentFunction) {
        return; // Globals are processed in Phase 1 of build
    }
    TypePtr type = toEirType(node->type);
    size_t al = getTypeAlign(type);
    if (al < 1) al = 1;
    std::string slotName = "%" + node->name + ".addr";
    emit(Instruction{Opcode::Alloca, slotName, type, {Operand::constInt(static_cast<int64_t>(al), Type::getI32())}});
    declareVar(node->name, Operand::var(slotName, Type::getPtr(type)), type);

    if (node->initializer) {
        if (auto arrLit = dynamic_cast<ArrayLitExpr*>(node->initializer.get())) {
            emitArrayInitInto(Operand::var(slotName, Type::getPtr(type)), arrLit, type);
        } else {
            Operand val = evaluateExpr(node->initializer);
            if (type->isStruct() || type->isArray()) {
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(val.str(), type), Operand::var(slotName, Type::getPtr(type))}});
            } else {
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {val, Operand::var(slotName, Type::getPtr(type))}});
            }
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
    auto ecIt = enumConstants.find(node->name);
    if (ecIt != enumConstants.end()) {
        lastValue = Operand::constInt(ecIt->second, Type::getI32());
        lastLValue = lastValue;
        return;
    }
    auto adtIt = adtVariants.find(node->name);
    if (adtIt != adtVariants.end()) {
        auto& vinfo = adtIt->second;
        TypePtr enumTy = Type::getStruct(vinfo.enumName, {});
        std::string slot = newVar() + ".adt_bare";
        emit(Instruction{Opcode::Alloca, slot, enumTy, {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {
            Operand::constInt(vinfo.variantIndex, Type::getI32()),
            Operand::var(slot, Type::getPtr(Type::getI32()))
        }});
        lastLValue = Operand::var(slot, Type::getPtr(enumTy));
        lastValue = lastLValue;
        return;
    }

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
    // 1. Check if callee is a closure variable or expression
    bool isClosureCall = false;
    Operand closureOp;
    if (auto id = dynamic_cast<IdentExpr*>(node->callee.get())) {
        if (lookupVar(id->name)) {
            TypePtr varTy = lookupVarType(id->name);
            if (varTy && varTy->isClosure()) {
                isClosureCall = true;
                closureOp = evaluateExpr(node->callee);
            }
        }
    } else if (!dynamic_cast<MemberExpr*>(node->callee.get())) {
        TypePtr cTy = getExprEirType(node->callee);
        if (cTy && cTy->isClosure()) {
            isClosureCall = true;
            closureOp = evaluateExpr(node->callee);
        }
    }

    if (isClosureCall) {
        std::vector<Operand> args;
        args.push_back(closureOp);
        for (const auto& arg : node->args) {
            args.push_back(evaluateExpr(arg));
        }
        TypePtr retTy = getExprEirType(node);
        std::string dst = retTy->isVoid() ? "" : newVar();
        emit(Instruction{Opcode::CallIndirect, dst, retTy, args});
        lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
        return;
    }

    // 2. Check if callee is an ADT variant constructor (e.g. Lit(7), Add(1, 2))
    if (auto id = dynamic_cast<IdentExpr*>(node->callee.get())) {
        auto it = adtVariants.find(id->name);
        if (it != adtVariants.end()) {
            auto& vinfo = it->second;
            TypePtr enumTy = Type::getStruct(vinfo.enumName, {});
            std::string slot = newVar() + ".adt";
            emit(Instruction{Opcode::Alloca, slot, enumTy, {Operand::constInt(8, Type::getI32())}});
            // Store tag at offset 0
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {
                Operand::constInt(vinfo.variantIndex, Type::getI32()),
                Operand::var(slot, Type::getPtr(Type::getI32()))
            }});

            size_t payOffset = 8;
            for (size_t p = 0; p < node->args.size() && p < vinfo.payloadTypes.size(); ++p) {
                TypePtr pt = vinfo.payloadTypes[p];
                size_t pAlign = getTypeAlign(pt);
                payOffset = (payOffset + pAlign - 1) & ~(pAlign - 1);
                std::string payPtr = newVar() + ".pay";
                emit(Instruction{Opcode::OffsetPtr, payPtr, Type::getPtr(pt), {
                    Operand::var(slot, Type::getPtr()), Operand::constInt(static_cast<int64_t>(payOffset), Type::getI64())
                }});
                Operand argVal = evaluateExpr(node->args[p]);
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {argVal, Operand::var(payPtr, Type::getPtr(pt))}});
                payOffset += getTypeSize(pt);
            }
            lastLValue = Operand::var(slot, Type::getPtr(enumTy));
            lastValue = lastLValue;
            return;
        }
    }

    // 3. Named or Member Function Call
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
void EIRBuilder::visit(ForInStmt* node) {
    pushScope();
    TypePtr itTy = getExprEirType(node->iterable);
    Operand itVal = evaluateExpr(node->iterable);

    if (itTy && itTy->isArray()) {
        TypePtr elemTy = itTy->elemType ? itTy->elemType : Type::getI32();
        uint64_t totalCount = itTy->arraySize;

        std::string iterSlot = newVar() + ".forin_arr";
        emit(Instruction{Opcode::Alloca, iterSlot, itTy, {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {itVal, Operand::var(iterSlot, Type::getPtr(itTy))}});

        std::string varSlot = "%" + node->varName + ".addr";
        emit(Instruction{Opcode::Alloca, varSlot, elemTy, {Operand::constInt(static_cast<int64_t>(getTypeAlign(elemTy)), Type::getI32())}});
        declareVar(node->varName, Operand::var(varSlot, Type::getPtr(elemTy)), elemTy);

        std::string idxSlot = newVar() + ".idx";
        emit(Instruction{Opcode::Alloca, idxSlot, Type::getI64(), {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getI64()), Operand::var(idxSlot, Type::getPtr(Type::getI64()))}});

        BasicBlock* condBB = createBlock("forin.cond");
        BasicBlock* bodyBB = createBlock("forin.body");
        BasicBlock* stepBB = createBlock("forin.step");
        BasicBlock* exitBB = createBlock("forin.exit");

        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

        setBlock(condBB);
        std::string curIdx = newVar();
        emit(Instruction{Opcode::Load, curIdx, Type::getI64(), {Operand::var(idxSlot, Type::getPtr(Type::getI64()))}});
        std::string cmp = newVar();
        emit(Instruction{Opcode::ICmp, cmp, Type::getI1(), {Operand::var(curIdx, Type::getI64()), Operand::constInt(static_cast<int64_t>(totalCount), Type::getI64())}, ICmpCond::Slt});
        emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(cmp, Type::getI1()), Operand::block(bodyBB->name), Operand::block(exitBB->name)}});

        setBlock(bodyBB);
        breakStack.push_back(exitBB);
        continueStack.push_back(stepBB);

        std::string elemPtr = newVar();
        emit(Instruction{Opcode::Gep, elemPtr, Type::getPtr(elemTy), {Operand::var(iterSlot, Type::getPtr(itTy)), Operand::var(curIdx, Type::getI64())}});
        std::string loadedElem = newVar();
        emit(Instruction{Opcode::Load, loadedElem, elemTy, {Operand::var(elemPtr, Type::getPtr(elemTy))}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(loadedElem, elemTy), Operand::var(varSlot, Type::getPtr(elemTy))}});

        node->body->accept(this);

        if (!currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(stepBB->name)}});
        }

        breakStack.pop_back();
        continueStack.pop_back();

        setBlock(stepBB);
        std::string nextIdx = newVar();
        emit(Instruction{Opcode::Add, nextIdx, Type::getI64(), {Operand::var(curIdx, Type::getI64()), Operand::constInt(1, Type::getI64())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(nextIdx, Type::getI64()), Operand::var(idxSlot, Type::getPtr(Type::getI64()))}});
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

        setBlock(exitBB);
    } else if (itTy && itTy->isSlice()) {
        TypePtr elemTy = itTy->elemType ? itTy->elemType : Type::getI32();

        std::string dataPtr = newVar() + ".slice_data";
        emit(Instruction{Opcode::ExtractFat, dataPtr, Type::getPtr(elemTy), {itVal, Operand::constInt(0, Type::getI32())}});
        std::string lenVal = newVar() + ".slice_len";
        emit(Instruction{Opcode::ExtractFat, lenVal, Type::getI64(), {itVal, Operand::constInt(1, Type::getI32())}});

        std::string varSlot = "%" + node->varName + ".addr";
        emit(Instruction{Opcode::Alloca, varSlot, elemTy, {Operand::constInt(static_cast<int64_t>(getTypeAlign(elemTy)), Type::getI32())}});
        declareVar(node->varName, Operand::var(varSlot, Type::getPtr(elemTy)), elemTy);

        std::string idxSlot = newVar() + ".idx";
        emit(Instruction{Opcode::Alloca, idxSlot, Type::getI64(), {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getI64()), Operand::var(idxSlot, Type::getPtr(Type::getI64()))}});

        BasicBlock* condBB = createBlock("forin.cond");
        BasicBlock* bodyBB = createBlock("forin.body");
        BasicBlock* stepBB = createBlock("forin.step");
        BasicBlock* exitBB = createBlock("forin.exit");

        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

        setBlock(condBB);
        std::string curIdx = newVar();
        emit(Instruction{Opcode::Load, curIdx, Type::getI64(), {Operand::var(idxSlot, Type::getPtr(Type::getI64()))}});
        std::string cmp = newVar();
        emit(Instruction{Opcode::ICmp, cmp, Type::getI1(), {Operand::var(curIdx, Type::getI64()), Operand::var(lenVal, Type::getI64())}, ICmpCond::Slt});
        emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(cmp, Type::getI1()), Operand::block(bodyBB->name), Operand::block(exitBB->name)}});

        setBlock(bodyBB);
        breakStack.push_back(exitBB);
        continueStack.push_back(stepBB);

        std::string elemPtr = newVar();
        emit(Instruction{Opcode::Gep, elemPtr, Type::getPtr(elemTy), {Operand::var(dataPtr, Type::getPtr(elemTy)), Operand::var(curIdx, Type::getI64())}});
        std::string loadedElem = newVar();
        emit(Instruction{Opcode::Load, loadedElem, elemTy, {Operand::var(elemPtr, Type::getPtr(elemTy))}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(loadedElem, elemTy), Operand::var(varSlot, Type::getPtr(elemTy))}});

        node->body->accept(this);

        if (!currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(stepBB->name)}});
        }

        breakStack.pop_back();
        continueStack.pop_back();

        setBlock(stepBB);
        std::string nextIdx = newVar();
        emit(Instruction{Opcode::Add, nextIdx, Type::getI64(), {Operand::var(curIdx, Type::getI64()), Operand::constInt(1, Type::getI64())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(nextIdx, Type::getI64()), Operand::var(idxSlot, Type::getPtr(Type::getI64()))}});
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});

        setBlock(exitBB);
    }
    popScope();
}

void EIRBuilder::visit(DoWhileStmt* node) {
    BasicBlock* bodyBB = createBlock("dowhile.body");
    BasicBlock* condBB = createBlock("dowhile.cond");
    BasicBlock* exitBB = createBlock("dowhile.exit");

    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(bodyBB->name)}});

    setBlock(bodyBB);
    breakStack.push_back(exitBB);
    continueStack.push_back(condBB);

    node->body->accept(this);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});
    }

    breakStack.pop_back();
    continueStack.pop_back();

    setBlock(condBB);
    Operand cond = evaluateExpr(node->condition);
    if (cond.type->kind != TypeKind::Int || cond.type->bitWidth != 1) {
        std::string cmpDst = newVar();
        emit(Instruction{Opcode::ICmp, cmpDst, Type::getI1(), {cond, Operand::constInt(0, cond.type)}, ICmpCond::Ne});
        cond = Operand::var(cmpDst, Type::getI1());
    }
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(bodyBB->name), Operand::block(exitBB->name)}});

    setBlock(exitBB);
}

void EIRBuilder::visit(SwitchStmt* node) {
    Operand subj = evaluateExpr(node->subject);
    BasicBlock* endBB = createBlock("switch.end");

    std::vector<BasicBlock*> caseBBs;
    std::vector<int64_t> caseVals;
    BasicBlock* defaultBB = endBB;

    for (size_t i = 0; i < node->cases.size(); ++i) {
        std::string lbl = node->cases[i].value ? "switch.case." + std::to_string(i) : "switch.default";
        BasicBlock* bb = createBlock(lbl);
        caseBBs.push_back(bb);
        if (!node->cases[i].value) {
            defaultBB = bb;
            caseVals.push_back(0);
        } else {
            int64_t v = foldConstInt(node->cases[i].value);
            caseVals.push_back(v);
        }
    }

    Instruction swInst{Opcode::Switch, "", Type::getVoid(), {subj}};
    swInst.defaultLabel = defaultBB->name;
    for (size_t i = 0; i < node->cases.size(); ++i) {
        if (node->cases[i].value) {
            swInst.switchCases.emplace_back(caseVals[i], caseBBs[i]->name);
        }
    }
    emit(std::move(swInst));

    breakStack.push_back(endBB);
    pushScope();

    for (size_t i = 0; i < node->cases.size(); ++i) {
        setBlock(caseBBs[i]);
        for (auto& item : node->cases[i].stmts) {
            if (currentBlock->hasTerminator()) break;
            if (std::holds_alternative<DeclPtr>(item)) std::get<DeclPtr>(item)->accept(this);
            else if (std::holds_alternative<StmtPtr>(item)) std::get<StmtPtr>(item)->accept(this);
        }
        if (!currentBlock->hasTerminator()) {
            BasicBlock* next = (i + 1 < caseBBs.size()) ? caseBBs[i + 1] : endBB;
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(next->name)}});
        }
    }

    popScope();
    breakStack.pop_back();

    setBlock(endBB);
}

void EIRBuilder::visit(MatchStmt* node) {
    std::string enumName = node->enumName;
    if (enumName.empty()) {
        TypePtr subjTy = getExprEirType(node->subject);
        if (subjTy) enumName = subjTy->nominalName();
    }

    if (plainEnums.count(enumName)) {
        EnumDecl* ped = plainEnums[enumName];
        auto valueOf = [&](const std::string& v) -> int64_t {
            auto it = enumConstants.find(v);
            if (it != enumConstants.end()) return it->second;
            for (const auto& m : ped->members) {
                if (m.first == v) return m.second;
            }
            return 0;
        };

        Operand subj = evaluateExpr(node->subject);
        BasicBlock* endBB = createBlock("match.end");
        std::vector<BasicBlock*> armBBs(node->arms.size());
        BasicBlock* defaultBB = endBB;
        for (size_t i = 0; i < node->arms.size(); ++i) {
            armBBs[i] = createBlock("match.arm");
            if (node->arms[i].variant.empty() || node->arms[i].variant == "_") {
                defaultBB = armBBs[i];
            }
        }

        Instruction swInst{Opcode::Switch, "", Type::getVoid(), {subj}};
        swInst.defaultLabel = defaultBB->name;
        for (size_t i = 0; i < node->arms.size(); ++i) {
            if (!node->arms[i].variant.empty() && node->arms[i].variant != "_") {
                swInst.switchCases.emplace_back(valueOf(node->arms[i].variant), armBBs[i]->name);
            }
        }
        emit(std::move(swInst));

        for (size_t i = 0; i < node->arms.size(); ++i) {
            setBlock(armBBs[i]);
            pushScope();
            node->arms[i].body->accept(this);
            popScope();
            if (!currentBlock->hasTerminator()) {
                emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(endBB->name)}});
            }
        }
        setBlock(endBB);
        return;
    }

    // ADT Enum Match
    Operand subjVal = evaluateExpr(node->subject);
    Operand subjAddr = subjVal;
    TypePtr enumTy = Type::getStruct(enumName, {});
    if (!subjVal.type || !subjVal.type->isPointer()) {
        std::string tempSubj = newVar() + ".match_subj";
        emit(Instruction{Opcode::Alloca, tempSubj, enumTy, {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {subjVal, Operand::var(tempSubj, Type::getPtr(enumTy))}});
        subjAddr = Operand::var(tempSubj, Type::getPtr(enumTy));
    }

    // Load tag from offset 0
    std::string tagVal = newVar() + ".tag";
    emit(Instruction{Opcode::Load, tagVal, Type::getI32(), {subjAddr}});
    Operand tagOp = Operand::var(tagVal, Type::getI32());

    BasicBlock* endBB = createBlock("match.end");
    std::vector<BasicBlock*> armBBs(node->arms.size());
    BasicBlock* defaultBB = endBB;
    for (size_t i = 0; i < node->arms.size(); ++i) {
        armBBs[i] = createBlock("match.arm");
        if (node->arms[i].variant.empty() || node->arms[i].variant == "_") {
            defaultBB = armBBs[i];
        }
    }

    Instruction swInst{Opcode::Switch, "", Type::getVoid(), {tagOp}};
    swInst.defaultLabel = defaultBB->name;
    for (size_t i = 0; i < node->arms.size(); ++i) {
        if (!node->arms[i].variant.empty() && node->arms[i].variant != "_") {
            int vidx = 0;
            auto it = adtVariants.find(node->arms[i].variant);
            if (it != adtVariants.end()) vidx = it->second.variantIndex;
            swInst.switchCases.emplace_back(vidx, armBBs[i]->name);
        }
    }
    emit(std::move(swInst));

    for (size_t i = 0; i < node->arms.size(); ++i) {
        auto& arm = node->arms[i];
        setBlock(armBBs[i]);
        pushScope();

        if (!arm.variant.empty() && arm.variant != "_" && !arm.bindings.empty()) {
            auto it = adtVariants.find(arm.variant);
            if (it != adtVariants.end()) {
                auto& vinfo = it->second;
                size_t payOffset = 8;
                for (size_t b = 0; b < arm.bindings.size() && b < vinfo.payloadTypes.size(); ++b) {
                    TypePtr bTy = vinfo.payloadTypes[b];
                    size_t bAlign = getTypeAlign(bTy);
                    payOffset = (payOffset + bAlign - 1) & ~(bAlign - 1);
                    std::string fPtr = newVar() + "." + arm.bindings[b] + ".ptr";
                    emit(Instruction{Opcode::OffsetPtr, fPtr, Type::getPtr(bTy), {
                        subjAddr, Operand::constInt(static_cast<int64_t>(payOffset), Type::getI64())
                    }});
                    std::string loadedVal = newVar() + "." + arm.bindings[b];
                    emit(Instruction{Opcode::Load, loadedVal, bTy, {Operand::var(fPtr, Type::getPtr(bTy))}});

                    std::string slot = "%" + arm.bindings[b] + ".addr";
                    emit(Instruction{Opcode::Alloca, slot, bTy, {Operand::constInt(static_cast<int64_t>(getTypeAlign(bTy)), Type::getI32())}});
                    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(loadedVal, bTy), Operand::var(slot, Type::getPtr(bTy))}});
                    declareVar(arm.bindings[b], Operand::var(slot, Type::getPtr(bTy)), bTy);

                    payOffset += getTypeSize(bTy);
                }
            }
        }

        arm.body->accept(this);
        popScope();

        if (!currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(endBB->name)}});
        }
    }

    setBlock(endBB);
}

void EIRBuilder::visit(QuestionExpr* node) {
    Operand resVal = evaluateExpr(node->operand);
    TypePtr st = resVal.type;
    std::string sname = st ? st->nominalName() : "";
    std::string tempSlot = newVar() + ".q_tmp";
    emit(Instruction{Opcode::Alloca, tempSlot, st, {Operand::constInt(8, Type::getI32())}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {resVal, Operand::var(tempSlot, Type::getPtr(st))}});

    std::string okPtr = newVar() + ".q_ok_ptr";
    emit(Instruction{Opcode::FieldPtr, okPtr, Type::getPtr(Type::getI1()), {Operand::var(sname, nullptr), Operand::var(tempSlot, Type::getPtr(st)), Operand::constString("ok")}});
    std::string okVal = newVar() + ".q_ok";
    emit(Instruction{Opcode::Load, okVal, Type::getI1(), {Operand::var(okPtr, Type::getPtr(Type::getI1()))}});

    std::string isErr = newVar() + ".q_iserr";
    emit(Instruction{Opcode::ICmp, isErr, Type::getI1(), {Operand::var(okVal, Type::getI1()), Operand::constInt(0, Type::getI1())}, ICmpCond::Eq});

    BasicBlock* errBB = createBlock("q.err");
    BasicBlock* contBB = createBlock("q.cont");
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(isErr, Type::getI1()), Operand::block(errBB->name), Operand::block(contBB->name)}});

    setBlock(errBB);
    emit(Instruction{Opcode::Ret, "", st, {resVal}});

    setBlock(contBB);
    TypePtr valTy = getExprEirType(node);
    std::string valPtr = newVar() + ".q_val_ptr";
    emit(Instruction{Opcode::FieldPtr, valPtr, Type::getPtr(valTy), {Operand::var(sname, nullptr), Operand::var(tempSlot, Type::getPtr(st)), Operand::constString("value")}});
    std::string unwrapped = newVar() + ".q_unwrapped";
    emit(Instruction{Opcode::Load, unwrapped, valTy, {Operand::var(valPtr, Type::getPtr(valTy))}});
    lastValue = Operand::var(unwrapped, valTy);
}

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

void EIRBuilder::visit(ArrayLitExpr* node) {
    TypePtr arrTy = getExprEirType(node);
    size_t al = getTypeAlign(arrTy);
    if (al < 1) al = 1;
    std::string tempSlot = newVar() + ".arrlit";
    emit(Instruction{Opcode::Alloca, tempSlot, arrTy, {Operand::constInt(static_cast<int64_t>(al), Type::getI32())}});
    emitArrayInitInto(Operand::var(tempSlot, Type::getPtr(arrTy)), node, arrTy);
    lastLValue = Operand::var(tempSlot, Type::getPtr(arrTy));
    lastValue = lastLValue;
}

void EIRBuilder::visit(AllocWithExpr*) {}
void EIRBuilder::visit(TemplateCallExpr*) {}

void EIRBuilder::visit(LambdaExpr* node) {
    std::string lambdaName = "__lambda_" + std::to_string(nextLambdaId++);

    std::vector<TypePtr> capTypes;
    std::vector<size_t> capOffsets;
    size_t envOffset = 0;
    size_t maxAlign = 1;
    for (const auto& cap : node->captures) {
        TypePtr ct = toEirType(cap.second);
        size_t al = getTypeAlign(ct);
        if (al > maxAlign) maxAlign = al;
        envOffset = (envOffset + al - 1) & ~(al - 1);
        capOffsets.push_back(envOffset);
        envOffset += getTypeSize(ct);
        capTypes.push_back(ct);
    }
    size_t totalEnvSize = (envOffset + maxAlign - 1) & ~(maxAlign - 1);
    if (totalEnvSize == 0) totalEnvSize = 8;

    Operand envPtr = Operand::constNull();
    if (!node->captures.empty()) {
        std::string envSlot = newVar() + ".env";
        emit(Instruction{Opcode::Alloca, envSlot, Type::getPtr(), {Operand::constInt(static_cast<int64_t>(totalEnvSize), Type::getI32())}});
        for (size_t c = 0; c < node->captures.size(); ++c) {
            Operand* srcSlot = lookupVar(node->captures[c].first);
            if (srcSlot) {
                std::string valVar = newVar() + ".cap";
                emit(Instruction{Opcode::Load, valVar, capTypes[c], {*srcSlot}});
                std::string capFieldPtr = newVar() + ".cap_field";
                emit(Instruction{Opcode::OffsetPtr, capFieldPtr, Type::getPtr(capTypes[c]), {
                    Operand::var(envSlot, Type::getPtr()), Operand::constInt(static_cast<int64_t>(capOffsets[c]), Type::getI64())
                }});
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {
                    Operand::var(valVar, capTypes[c]), Operand::var(capFieldPtr, Type::getPtr(capTypes[c]))
                }});
            }
        }
        envPtr = Operand::var(envSlot, Type::getPtr());
    }

    std::string closureSlot = newVar() + ".closure";
    emit(Instruction{Opcode::MakeClosure, closureSlot, Type::getClosure(), {
        Operand::global(lambdaName, Type::getPtr()), envPtr
    }});
    Operand callerClosure = Operand::var(closureSlot, Type::getClosure());

    // Compile lambda function body
    Function* prevFn = currentFunction;
    BasicBlock* prevBB = currentBlock;
    int prevVarId = nextVarId;
    int prevBlockId = nextBlockId;
    auto prevBreakStack = breakStack;
    auto prevContinueStack = continueStack;

    std::vector<std::pair<std::string, TypePtr>> fnParams;
    fnParams.emplace_back("env", Type::getPtr());
    for (const auto& p : node->params) {
        fnParams.emplace_back(p.second, toEirType(p.first));
    }
    Function* lambdaFn = module->createFunction(lambdaName, toEirType(node->returnType), fnParams);
    currentFunction = lambdaFn;
    nextVarId = 0;
    nextBlockId = 0;
    breakStack.clear();
    continueStack.clear();

    BasicBlock* entryBB = lambdaFn->createBlock("entry");
    setBlock(entryBB);
    pushScope();

    for (size_t i = 0; i < lambdaFn->params.size(); ++i) {
        const auto& p = lambdaFn->params[i];
        std::string slotName = "%" + p.first + ".addr";
        emit(Instruction{Opcode::Alloca, slotName, p.second, {Operand::constInt(p.second->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var("%" + p.first, p.second), Operand::var(slotName, Type::getPtr(p.second))}});
        declareVar(p.first, Operand::var(slotName, Type::getPtr(p.second)), p.second);
    }

    if (!node->captures.empty()) {
        for (size_t c = 0; c < node->captures.size(); ++c) {
            std::string cname = node->captures[c].first;
            TypePtr cty = capTypes[c];
            std::string capFieldPtr = newVar() + "." + cname + ".ptr";
            emit(Instruction{Opcode::OffsetPtr, capFieldPtr, Type::getPtr(cty), {
                Operand::var("%env", Type::getPtr()), Operand::constInt(static_cast<int64_t>(capOffsets[c]), Type::getI64())
            }});
            std::string loadedVal = newVar() + "." + cname + ".val";
            emit(Instruction{Opcode::Load, loadedVal, cty, {Operand::var(capFieldPtr, Type::getPtr(cty))}});
            std::string localSlot = "%" + cname + ".addr";
            emit(Instruction{Opcode::Alloca, localSlot, cty, {Operand::constInt(cty->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(loadedVal, cty), Operand::var(localSlot, Type::getPtr(cty))}});
            declareVar(cname, Operand::var(localSlot, Type::getPtr(cty)), cty);
        }
    }

    node->body->accept(this);

    if (!currentBlock->hasTerminator()) {
        if (lambdaFn->returnType->isVoid()) {
            emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
        } else {
            emit(Instruction{Opcode::Unreachable, "", Type::getVoid(), {}});
        }
    }

    popScope();

    currentFunction = prevFn;
    setBlock(prevBB);
    nextVarId = prevVarId;
    nextBlockId = prevBlockId;
    breakStack = prevBreakStack;
    continueStack = prevContinueStack;

    lastValue = callerClosure;
    lastLValue = callerClosure;
}

void EIRBuilder::visit(AsmStmt*) {}
void EIRBuilder::visit(UnionDecl*) {}

void EIRBuilder::visit(SizeofExpr* node) {
    TypePtr ty = nullptr;
    if (!node->typeName.empty()) {
        ty = toEirType(node->typeName);
    } else if (node->operand) {
        ty = getExprEirType(node->operand);
    }
    size_t sz = 0;
    if (ty) {
        sz = getTypeSize(ty);
        if (sz == 0 && ty->isStruct() && module) {
            sz = module->getStructSize(ty->nominalName());
        }
    }
    if (sz == 0) sz = 8;
    lastValue = Operand::constInt(static_cast<int64_t>(sz), Type::getI64());
    lastLValue = lastValue;
}

void EIRBuilder::visit(ThreadCreateExpr*) {}

void EIRBuilder::visit(FreeClosureExpr* node) {
    Operand closureOp = evaluateExpr(node->closure);
    std::string envPtr = newVar() + ".env";
    emit(Instruction{Opcode::ExtractFat, envPtr, Type::getPtr(), {closureOp, Operand::constInt(1, Type::getI32())}});
    emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("free", Type::getPtr()), Operand::var(envPtr, Type::getPtr())}});
    lastValue = Operand::var("", Type::getVoid());
}

void EIRBuilder::visit(AwaitExpr*) {}
void EIRBuilder::visit(ThreadJoinStmt*) {}
void EIRBuilder::visit(ThrowStmt*) {}
void EIRBuilder::visit(TryStmt*) {}
void EIRBuilder::visit(DeferStmt*) {}

} // namespace eir
