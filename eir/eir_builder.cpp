#include "eir_builder.h"
#include "../ast/type_qual.h"
#include "../template_utils.h"
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
    if (module) {
        for (const auto& g : module->globals) {
            if (g.name == name) return g.type;
        }
    }
    return Type::getI32();
}

TypePtr EIRBuilder::toEirType(const std::string& typeStr) {
    std::string s = tyq::strip(typeParamOverride.empty() ? typeStr : substType(typeStr, typeParamOverride));
    if (s == "closure" || s == "{ ptr, ptr }") {
        return Type::getClosure();
    }
    if (!typeAliases.empty()) {
        s = ty::dealiasSpelling(s, typeAliases);
    } else if (!templateTypeAliases().empty()) {
        s = ty::dealiasSpelling(s, templateTypeAliases());
    }
    if (s == "closure" || s == "{ ptr, ptr }") {
        return Type::getClosure();
    }
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
        case ty::Type::Kind::Template:
        case ty::Type::Kind::Named:
        case ty::Type::Kind::Struct: {
            std::string sname = (t.kind == ty::Type::Kind::Template || s.find('<') != std::string::npos) ? mangleTemplate(s) : t.nominalName();
            if (sname.empty()) sname = t.name;
            if (plainEnums.count(sname)) {
                return Type::getI32();
            }
            if (interfaces.count(sname)) {
                return Type::getInterface(sname);
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
        case ty::Type::Kind::Interface: {
            std::string sname = t.nominalName();
            if (sname.empty()) sname = t.name;
            return Type::getInterface(sname);
        }
        case ty::Type::Kind::VaList:
            return Type::getStruct("__va_list", {
                Type::getPtr(), Type::getPtr(), Type::getPtr(), Type::getI32(), Type::getI32()
            });
        case ty::Type::Kind::Fn:
            return Type::getClosure(t.ret ? toEirType(t.ret->str()) : Type::getVoid());
        default:
            return Type::getI32();
    }
}

TypePtr EIRBuilder::getExprEirType(const Expr* expr) {
    if (!expr) return Type::getI32();
    if (auto id = dynamic_cast<const IdentExpr*>(expr)) {
        TypePtr vt = lookupVarType(id->name);
        if (vt && !vt->isVoid()) return vt;
    }
    if (auto c = dynamic_cast<const CastExpr*>(expr)) {
        std::string targetTy = c->targetType;
        if (!typeParamOverride.empty()) targetTy = substType(targetTy, typeParamOverride);
        return toEirType(targetTy);
    }
    if (auto mem = dynamic_cast<const MemberExpr*>(expr)) {
        TypePtr bty = getExprEirType(mem->base);
        if (bty && (bty->isSlice() || (bty->isPointer() && bty->elemType && bty->elemType->isSlice()))) {
            if (mem->member == "len") return Type::getI64();
            if (mem->member == "ptr" || mem->member == "data") {
                TypePtr elem = bty->isSlice() ? bty->elemType : bty->elemType->elemType;
                return Type::getPtr(elem ? elem : Type::getU8());
            }
        }
        std::string sname;
        if (bty && bty->isStruct()) sname = bty->nominalName();
        else if (bty && bty->isPointer() && bty->elemType && bty->elemType->isStruct()) sname = bty->elemType->nominalName();
        if (!sname.empty() && module) {
            auto it = module->structLayouts.find(sname);
            if (it != module->structLayouts.end()) {
                const StructFieldInfo* fi = it->second.getFieldInfo(mem->member);
                if (fi) return fi->type;
            }
        }
    }
    if (auto idx = dynamic_cast<const IndexExpr*>(expr)) {
        TypePtr bty = getExprEirType(idx->base);
        if (idx->isSlice()) {
            TypePtr elemTy = nullptr;
            if (bty && (bty->isPointer() || bty->isArray() || bty->isSlice()) && bty->elemType) {
                elemTy = bty->elemType;
            }
            if (!elemTy && resolvedExprTypes) {
                auto it = resolvedExprTypes->find(const_cast<Expr*>(expr));
                if (it != resolvedExprTypes->end()) {
                    std::string et = it->second;
                    if (!typeParamOverride.empty()) et = substType(et, typeParamOverride);
                    if (et.size() >= 2 && et.substr(et.size() - 2) == "[]") {
                        elemTy = toEirType(et.substr(0, et.size() - 2));
                    }
                }
            }
            if (!elemTy) elemTy = Type::getI32();
            return Type::getSlice(elemTy);
        }
        if (bty && (bty->isPointer() || bty->isArray() || bty->isSlice()) && bty->elemType) {
            return bty->elemType;
        }
    }
    if (auto u = dynamic_cast<const UnaryExpr*>(expr)) {
        if (u->op == "*") {
            TypePtr opty = getExprEirType(u->operand);
            if (opty && opty->isPointer() && opty->elemType) return opty->elemType;
        } else if (u->op == "&") {
            TypePtr opty = getExprEirType(u->operand);
            if (opty) return Type::getPtr(opty);
        }
    }
    if (auto tc = dynamic_cast<const TemplateCallExpr*>(expr)) {
        std::string fnName = tc->templateName;
        std::vector<std::string> substitutedArgs;
        for (const auto& a : tc->typeArgs) {
            substitutedArgs.push_back(typeParamOverride.empty() ? a : substType(a, typeParamOverride));
        }
        std::string mangledName = tc->templateName;
        for (const auto& t : substitutedArgs) mangledName += "_" + mangleTemplate(t);
        if (module) {
            if (Function* fn = module->getFunction(mangledName)) return fn->returnType;
            if (Function* fn = module->getFunction(fnName)) return fn->returnType;
        }
        auto templ = funcTemplateDecls.find(tc->templateName);
        if (templ != funcTemplateDecls.end()) {
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < templ->second->typeParams.size() && i < substitutedArgs.size(); ++i) {
                subs[templ->second->typeParams[i]] = substitutedArgs[i];
            }
            std::string subRet = substType(templ->second->returnType, subs);
            return toEirType(subRet);
        }
    }
    if (auto ce = dynamic_cast<const CallExpr*>(expr)) {
        TypePtr cTy = getExprEirType(ce->callee.get());
        if (cTy && cTy->isClosure() && cTy->returnType) {
            return cTy->returnType;
        }
        if (auto id = dynamic_cast<const IdentExpr*>(ce->callee.get())) {
            if (module) {
                if (Function* fn = module->getFunction(id->name)) return fn->returnType;
            }
        }
        if (auto mem = dynamic_cast<const MemberExpr*>(ce->callee.get())) {
            TypePtr bty = getExprEirType(mem->base);
            std::string sname;
            if (bty && bty->isStruct()) sname = bty->nominalName();
            else if (bty && bty->isPointer() && bty->elemType && bty->elemType->isStruct()) sname = bty->elemType->nominalName();
            if (!sname.empty() && module) {
                if (Function* fn = module->getFunction(sname + "_" + mem->member)) return fn->returnType;
            }
        }
    }
    if (auto b = dynamic_cast<const BinaryExpr*>(expr)) {
        if (b->op == "==" || b->op == "!=" || b->op == "<" || b->op == "<=" || b->op == ">" || b->op == ">=" ||
            b->op == "&&" || b->op == "||") {
            return Type::getI1();
        }
        TypePtr lty = getExprEirType(b->left);
        TypePtr rty = getExprEirType(b->right);
        if (lty && lty->isFloat()) return lty;
        if (rty && rty->isFloat()) return rty;
        if (lty && lty->bitWidth > 32) return lty;
        if (rty && rty->bitWidth > 32) return rty;
    }
    if (resolvedExprTypes) {
        auto it = resolvedExprTypes->find(const_cast<Expr*>(expr));
        if (it != resolvedExprTypes->end()) {
            std::string tyStr = it->second;
            if (!typeParamOverride.empty()) tyStr = substType(tyStr, typeParamOverride);
            return toEirType(tyStr);
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
    lastLValue = Operand::constNull();
    expr->accept(this);
    if (lastLValue.kind == OperandKind::ConstantNull && lastValue.type &&
        (lastValue.type->isStruct() || lastValue.type->isArray() || lastValue.type->isInterface() || lastValue.type->isClosure() || lastValue.type->isSlice())) {
        lastLValue = Operand::var(lastValue.text, Type::getPtr(lastValue.type));
    }
    return lastLValue;
}

int64_t EIRBuilder::foldConstInt(const ExprPtr& expr) {
    if (!expr) return 0;
    if (auto lit = dynamic_cast<LiteralExpr*>(expr.get())) {
        if (lit->kind == LiteralExpr::Kind::INT) {
            try {
                if (!lit->value.empty() && lit->value[0] == '-') {
                    return std::stoll(lit->value, nullptr, 0);
                } else {
                    return static_cast<int64_t>(std::stoull(lit->value, nullptr, 0));
                }
            } catch (...) { return 0; }
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
            Operand val = evalForType(lit->elements[i], arrTy->elemType);
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {val, Operand::var(elemPtr, Type::getPtr(arrTy->elemType))}});
        }
    }
}

bool EIRBuilder::volatileRooted(const Expr* e) const {
    if (volatileVars.empty()) return false;
    while (e) {
        if (auto* id = dynamic_cast<const IdentExpr*>(e)) {
            return volatileVars.count(id->name) > 0;
        }
        if (auto* u = dynamic_cast<const UnaryExpr*>(e)) {
            if (u->op != "*" || !u->opFunc.empty()) break;
            e = u->operand.get();
        } else if (auto* ix = dynamic_cast<const IndexExpr*>(e)) {
            if (!ix->opFunc.empty()) break;
            e = ix->base.get();
        } else if (auto* m = dynamic_cast<const MemberExpr*>(e)) {
            e = m->base.get();
        } else {
            break;
        }
    }
    return false;
}

void EIRBuilder::runCleanupsToDepth(size_t depth, bool errorPath) {
    for (size_t i = cleanupScopes.size(); i-- > depth; ) {
        auto frame = cleanupScopes[i];
        for (size_t j = frame.size(); j-- > 0; ) {
            if (currentBlock && currentBlock->hasTerminator()) return;
            if (frame[j].isErr && !errorPath) continue;
            frame[j].body->accept(this);
        }
    }
}

void EIRBuilder::layoutBitfieldsForStruct(StructDecl* s, StructInfo& sinfo, std::vector<TypePtr>& fields) {
    uint64_t bitpos = 0;
    size_t structAlign = 1;
    for (const auto& f : s->fields) {
        TypePtr ty = toEirType(f.type);
        fields.push_back(ty);
        size_t size = getTypeSize(ty, module.get());
        if (size < 1) size = 1;
        size_t align = getTypeAlign(ty, module.get());
        if (align < 1) align = 1;
        if (align > structAlign) structAlign = align;

        if (f.bitWidth > 0) {
            uint64_t w = static_cast<uint64_t>(f.bitWidth);
            uint64_t unitBits = size * 8;
            if (bitpos / unitBits != (bitpos + w - 1) / unitBits) {
                bitpos = (bitpos + unitBits - 1) / unitBits * unitBits;
            }
            size_t byteOffset = (bitpos / unitBits) * size;
            unsigned bitOffset = static_cast<unsigned>(bitpos - byteOffset * 8);
            StructFieldInfo sfield;
            sfield.name = f.name;
            sfield.type = ty;
            sfield.offset = byteOffset;
            sfield.size = size;
            sfield.isBitfield = true;
            sfield.bitOffset = bitOffset;
            sfield.bitWidth = static_cast<unsigned>(w);
            sfield.isSigned = ty->isSigned;
            sfield.storageType = ty;
            sfield.storageOffset = byteOffset;
            sinfo.fields.push_back(sfield);
            bitpos += w;
        } else {
            size_t off = ((bitpos + 7) / 8 + align - 1) / align * align;
            StructFieldInfo sfield;
            sfield.name = f.name;
            sfield.type = ty;
            sfield.offset = off;
            sfield.size = size;
            sfield.isBitfield = false;
            sinfo.fields.push_back(sfield);
            bitpos = (off + size) * 8;
        }
    }
    sinfo.totalSize = ((bitpos + 7) / 8 + structAlign - 1) / structAlign * structAlign;
    sinfo.alignment = structAlign;
}

Operand EIRBuilder::loadBitfield(MemberExpr* m, bool isVolatile) {
    std::string baseType = getExprEirType(m->base)->nominalName();
    auto it = module->structLayouts.find(baseType);
    if (it == module->structLayouts.end()) return Operand::constInt(0, Type::getI32());
    const StructFieldInfo* fi = it->second.getFieldInfo(m->member);
    if (!fi || !fi->isBitfield) return Operand::constInt(0, Type::getI32());

    Operand basePtr;
    TypePtr baseTy = getExprEirType(m->base);
    if (baseTy->isPointer()) {
        basePtr = evaluateExpr(m->base);
    } else {
        basePtr = evaluateLValue(m->base);
    }

    std::string wordPtr = newVar();
    emit(Instruction{Opcode::OffsetPtr, wordPtr, Type::getPtr(fi->storageType),
                     {basePtr, Operand::constInt(static_cast<int64_t>(fi->storageOffset), Type::getI64())}});

    std::string wordVal = newVar();
    Instruction ld{Opcode::Load, wordVal, fi->storageType, {Operand::var(wordPtr, Type::getPtr(fi->storageType))}};
    ld.isVolatile = isVolatile;
    emit(ld);

    Operand cur = Operand::var(wordVal, fi->storageType);
    if (fi->bitOffset > 0) {
        std::string shifted = newVar();
        emit(Instruction{Opcode::LShr, shifted, fi->storageType, {cur, Operand::constInt(static_cast<int64_t>(fi->bitOffset), fi->storageType)}});
        cur = Operand::var(shifted, fi->storageType);
    }
    uint64_t mask = (fi->bitWidth >= 64) ? ~0ULL : ((1ULL << fi->bitWidth) - 1);
    std::string masked = newVar();
    emit(Instruction{Opcode::And, masked, fi->storageType, {cur, Operand::constInt(static_cast<int64_t>(mask), fi->storageType)}});
    cur = Operand::var(masked, fi->storageType);

    if (fi->isSigned && fi->bitWidth < static_cast<unsigned>(fi->storageType->bitWidth)) {
        unsigned sh = static_cast<unsigned>(fi->storageType->bitWidth) - fi->bitWidth;
        std::string shl = newVar();
        emit(Instruction{Opcode::Shl, shl, fi->storageType, {cur, Operand::constInt(static_cast<int64_t>(sh), fi->storageType)}});
        std::string ashr = newVar();
        emit(Instruction{Opcode::AShr, ashr, fi->storageType, {Operand::var(shl, fi->storageType), Operand::constInt(static_cast<int64_t>(sh), fi->storageType)}});
        cur = Operand::var(ashr, fi->storageType);
    }
    return cur;
}

void EIRBuilder::storeBitfield(MemberExpr* m, Operand val, bool isVolatile) {
    std::string baseType = getExprEirType(m->base)->nominalName();
    auto it = module->structLayouts.find(baseType);
    if (it == module->structLayouts.end()) return;
    const StructFieldInfo* fi = it->second.getFieldInfo(m->member);
    if (!fi || !fi->isBitfield) return;

    Operand basePtr;
    TypePtr baseTy = getExprEirType(m->base);
    if (baseTy->isPointer()) {
        basePtr = evaluateExpr(m->base);
    } else {
        basePtr = evaluateLValue(m->base);
    }

    std::string wordPtr = newVar();
    emit(Instruction{Opcode::OffsetPtr, wordPtr, Type::getPtr(fi->storageType),
                     {basePtr, Operand::constInt(static_cast<int64_t>(fi->storageOffset), Type::getI64())}});

    std::string wordVal = newVar();
    Instruction ld{Opcode::Load, wordVal, fi->storageType, {Operand::var(wordPtr, Type::getPtr(fi->storageType))}};
    ld.isVolatile = isVolatile;
    emit(ld);

    uint64_t mask = (fi->bitWidth >= 64) ? ~0ULL : ((1ULL << fi->bitWidth) - 1);
    uint64_t fieldMask = mask << fi->bitOffset;
    std::string cleared = newVar();
    emit(Instruction{Opcode::And, cleared, fi->storageType, {Operand::var(wordVal, fi->storageType), Operand::constInt(static_cast<int64_t>(~fieldMask), fi->storageType)}});

    Operand valToStore = val;
    if (val.type->bitWidth != fi->storageType->bitWidth) {
        std::string conv = newVar();
        if (val.type->bitWidth > fi->storageType->bitWidth) {
            emit(Instruction{Opcode::Trunc, conv, fi->storageType, {val}});
        } else {
            emit(Instruction{fi->isSigned ? Opcode::SExt : Opcode::ZExt, conv, fi->storageType, {val}});
        }
        valToStore = Operand::var(conv, fi->storageType);
    }

    std::string vMasked = newVar();
    emit(Instruction{Opcode::And, vMasked, fi->storageType, {valToStore, Operand::constInt(static_cast<int64_t>(mask), fi->storageType)}});
    Operand vShifted = Operand::var(vMasked, fi->storageType);
    if (fi->bitOffset > 0) {
        std::string s = newVar();
        emit(Instruction{Opcode::Shl, s, fi->storageType, {vShifted, Operand::constInt(static_cast<int64_t>(fi->bitOffset), fi->storageType)}});
        vShifted = Operand::var(s, fi->storageType);
    }

    std::string newWord = newVar();
    emit(Instruction{Opcode::Or, newWord, fi->storageType, {Operand::var(cleared, fi->storageType), vShifted}});

    Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(newWord, fi->storageType), Operand::var(wordPtr, Type::getPtr(fi->storageType))}};
    st.isVolatile = isVolatile;
    emit(st);
}

static bool isConstIntExpr(const ExprPtr& expr, const std::unordered_map<std::string, int64_t>& enumConstants) {
    if (!expr) return false;
    if (auto lit = dynamic_cast<LiteralExpr*>(expr.get())) {
        return lit->kind == LiteralExpr::Kind::INT ||
               lit->kind == LiteralExpr::Kind::CHAR ||
               lit->kind == LiteralExpr::Kind::BOOL;
    }
    if (auto id = dynamic_cast<IdentExpr*>(expr.get())) {
        return enumConstants.count(id->name) > 0;
    }
    if (auto u = dynamic_cast<UnaryExpr*>(expr.get())) {
        return (u->op == "-" || u->op == "~" || u->op == "!") && isConstIntExpr(u->operand, enumConstants);
    }
    if (auto b = dynamic_cast<BinaryExpr*>(expr.get())) {
        return isConstIntExpr(b->left, enumConstants) && isConstIntExpr(b->right, enumConstants);
    }
    if (auto c = dynamic_cast<CastExpr*>(expr.get())) {
        return isConstIntExpr(c->expr, enumConstants);
    }
    return false;
}

std::unique_ptr<Module> EIRBuilder::build(Program* program) {
    volatileVars.clear();
    cleanupScopes.clear();
    loopContextStack.clear();
    templateDecls.clear();
    funcTemplateDecls.clear();
    templateInstanceArgs.clear();
    typeParamOverride.clear();
    if (semaInstanceArgs) {
        for (const auto& kv : *semaInstanceArgs) {
            templateInstanceArgs.emplace(kv.first, kv.second);
        }
    }
    module = std::make_unique<Module>();
    module->name = "main";
    module->targetTriple = targetTriple;

    if (!program) return std::move(module);

    // Phase 0: Type Aliases
    for (auto& decl : program->declarations) {
        if (auto ta = dynamic_cast<TypeAliasDecl*>(decl.get())) {
            typeAliases[ta->name] = ta->aliased;
            templateTypeAliases()[ta->name] = ta->aliased;
        }
    }

    // Phase 1: Structs, Enums, and Interfaces
    std::unordered_map<std::string, StructDecl*> plainStructDecls;
    for (auto& decl : program->declarations) {
        if (auto iface = dynamic_cast<InterfaceDecl*>(decl.get())) {
            InterfaceInfo info;
            info.name = iface->name;
            for (const auto& m : iface->methods) {
                info.methodNames.push_back(m.name);
                info.methodReturnTypes.push_back(m.returnType);
                std::vector<std::string> pts;
                for (const auto& p : m.params) pts.push_back(p.first);
                info.methodParamTypes.push_back(pts);
            }
            interfaces[iface->name] = info;
        } else if (auto e = dynamic_cast<EnumDecl*>(decl.get())) {
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
                if (maxPayloadSize < 24) maxPayloadSize = 24;
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
            if (!s->typeParams.empty()) {
                templateDecls[s->name] = s;
            } else {
                plainStructDecls[s->name] = s;
            }
        } else if (auto f = dynamic_cast<FunctionDecl*>(decl.get())) {
            if (!f->typeParams.empty()) {
                funcTemplateDecls[f->name] = f;
            }
        } else if (auto v = dynamic_cast<VarDecl*>(decl.get())) {
            if (v->isVolatile) {
                volatileVars.insert(v->name);
            }
            TypePtr type = toEirType(v->type);
            Operand initOp = Operand::constInt(0, type);
            if (v->initializer) {
                if (auto u = dynamic_cast<UnaryExpr*>(v->initializer.get())) {
                    if (u->op == "&") {
                        if (auto id = dynamic_cast<IdentExpr*>(u->operand.get())) {
                            initOp = Operand::global(id->name, type);
                        }
                    }
                }
                if (initOp.kind != OperandKind::GlobalRef) {
                    if (type && type->isInt()) {
                        if (isConstIntExpr(v->initializer, enumConstants)) {
                            int64_t cv = foldConstInt(v->initializer);
                            if (v->isConst) {
                                enumConstants[v->name] = cv;
                            }
                            initOp = Operand::constInt(cv, type);
                        } else {
                            initOp = Operand::constInt(0, type);
                        }
                    } else if (type && type->isFloat()) {
                        double fv = 0.0;
                        if (auto lit = dynamic_cast<LiteralExpr*>(v->initializer.get())) {
                            try { fv = std::stod(lit->value); } catch (...) {}
                        }
                        initOp = Operand::constFloat(fv, type);
                    } else {
                        initOp = Operand::constNull();
                    }
                }
            }
            module->globals.push_back(GlobalVar{v->name, type, initOp, v->isConst});
        }
    }

    std::function<void(const std::string&)> ensureStructLayout = [&](const std::string& name) {
        if (name.empty() || module->structLayouts.count(name)) return;

        auto tit = templateInstanceArgs.find(name);
        if (tit != templateInstanceArgs.end()) {
            const std::string& tname = tit->second.first;
            const std::vector<std::string>& targs = tit->second.second;
            auto tdIt = templateDecls.find(tname);
            if (tdIt != templateDecls.end()) {
                StructDecl* td = tdIt->second;
                std::map<std::string, std::string> subs;
                for (size_t i = 0; i < td->typeParams.size() && i < targs.size(); ++i) {
                    subs[td->typeParams[i]] = targs[i];
                }
                std::vector<TypePtr> fields;
                StructInfo sinfo;
                sinfo.name = name;
                size_t currentOffset = 0;
                size_t maxAlign = 1;
                for (const auto& f : td->fields) {
                    std::string subType = substType(f.type, subs);
                    TypePtr ft = toEirType(subType);
                    fields.push_back(ft);
                    if (ft && ft->isStruct()) ensureStructLayout(ft->nominalName());
                    size_t sz = getTypeSize(ft, module.get());
                    size_t al = getTypeAlign(ft, module.get());
                    if (al > maxAlign) maxAlign = al;
                    currentOffset = (currentOffset + al - 1) & ~(al - 1);
                    sinfo.fields.push_back({f.name, ft, currentOffset, sz});
                    currentOffset += sz;
                }
                sinfo.totalSize = (currentOffset + maxAlign - 1) & ~(maxAlign - 1);
                sinfo.alignment = maxAlign;
                module->structLayouts[name] = sinfo;
                module->structTypes.push_back(Type::getStruct(name, fields));
                return;
            }
        }

        auto sit = plainStructDecls.find(name);
        if (sit != plainStructDecls.end()) {
            StructDecl* s = sit->second;
            std::vector<TypePtr> fields;
            StructInfo sinfo;
            sinfo.name = s->name;
            bool hasBitfield = false;
            for (const auto& f : s->fields) {
                if (f.bitWidth > 0) { hasBitfield = true; break; }
            }
            if (hasBitfield) {
                layoutBitfieldsForStruct(s, sinfo, fields);
            } else {
                size_t currentOffset = 0;
                size_t maxAlign = 1;
                for (const auto& f : s->fields) {
                    TypePtr ft = toEirType(f.type);
                    fields.push_back(ft);
                    if (ft && ft->isStruct()) ensureStructLayout(ft->nominalName());
                    size_t sz = getTypeSize(ft, module.get());
                    size_t al = getTypeAlign(ft, module.get());
                    if (al > maxAlign) maxAlign = al;
                    currentOffset = (currentOffset + al - 1) & ~(al - 1);
                    sinfo.fields.push_back({f.name, ft, currentOffset, sz});
                    currentOffset += sz;
                }
                sinfo.totalSize = (currentOffset + maxAlign - 1) & ~(maxAlign - 1);
                sinfo.alignment = maxAlign;
            }
            module->structLayouts[s->name] = sinfo;
            module->structTypes.push_back(Type::getStruct(s->name, fields));
            return;
        }
    };

    for (const auto& kv : plainStructDecls) {
        ensureStructLayout(kv.first);
    }
    for (const auto& kv : templateInstanceArgs) {
        ensureStructLayout(kv.first);
    }

    // Phase 2: Function prototypes
    for (auto& decl : program->declarations) {
        if (auto f = dynamic_cast<FunctionDecl*>(decl.get())) {
            if (!f->typeParams.empty()) continue;
            std::vector<std::pair<std::string, TypePtr>> params;
            bool hasVarargs = false;
            for (const auto& p : f->params) {
                if (p.first == "...") {
                    hasVarargs = true;
                    break;
                }
                params.emplace_back(p.second, toEirType(p.first));
            }
            auto fn = module->createFunction(f->name, toEirType(f->returnType), params);
            fn->isVariadic = hasVarargs;
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
            if (!s->typeParams.empty()) continue;
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

    bool hasGlobalInits = false;
    for (const auto& decl : program->declarations) {
        if (auto v = dynamic_cast<VarDecl*>(decl.get())) {
            if (v->initializer) {
                hasGlobalInits = true;
                break;
            }
        }
    }
    if (hasGlobalInits) {
        module->createFunction("__eir_global_init", Type::getVoid(), {});
        module->globals.push_back(GlobalVar{"__eir_inited", Type::getI1(), Operand::constInt(0, Type::getI1()), false});
    }

    // Phase 3: Function bodies
    program->accept(this);

    // Phase 4: Emit global initializers
    if (hasGlobalInits) {
        Function* initFn = module->getFunction("__eir_global_init");
        currentFunction = initFn;
        nextVarId = 0;
        nextBlockId = 0;
        BasicBlock* entryBB = initFn->createBlock("entry");
        setBlock(entryBB);

        std::string flagVal = newVar();
        emit(Instruction{Opcode::Load, flagVal, Type::getI1(), {Operand::global("__eir_inited", Type::getPtr(Type::getI1()))}});
        BasicBlock* doInitBB = initFn->createBlock("do_init");
        BasicBlock* exitBB = initFn->createBlock("already_inited");
        emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(flagVal, Type::getI1()), Operand::block(exitBB->name), Operand::block(doInitBB->name)}});

        setBlock(doInitBB);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(1, Type::getI1()), Operand::global("__eir_inited", Type::getPtr(Type::getI1()))}});

        for (const auto& decl : program->declarations) {
            if (auto v = dynamic_cast<VarDecl*>(decl.get())) {
                if (v->initializer) {
                    TypePtr type = toEirType(v->type);
                    Operand target = Operand::global(v->name, Type::getPtr(type));
                    if (auto arrLit = dynamic_cast<ArrayLitExpr*>(v->initializer.get())) {
                        emitArrayInitInto(target, arrLit, type);
                    } else {
                        Operand val = evalForType(v->initializer, v->type);
                        Instruction st{Opcode::Store, "", Type::getVoid(),
                            {(type->isStruct() || type->isArray()) ? Operand::var(val.str(), type) : val,
                             target}};
                        st.isVolatile = v->isVolatile;
                        emit(st);
                    }
                }
            }
        }
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(exitBB->name)}});

        setBlock(exitBB);
        emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
        currentFunction = nullptr;
        currentBlock = nullptr;
    }

    return std::move(module);
}

// ============================================================================
// Visitors
// ============================================================================

void EIRBuilder::visit(Program* node) {
    for (auto& decl : node->declarations) {
        if (auto iface = dynamic_cast<InterfaceDecl*>(decl.get())) {
            iface->accept(this);
        } else if (auto en = dynamic_cast<EnumDecl*>(decl.get())) {
            en->accept(this);
        }
    }
    for (auto& decl : node->declarations) {
        decl->accept(this);
    }
}

void EIRBuilder::visit(FunctionDecl* node) {
    if (!node->typeParams.empty()) {
        funcTemplateDecls[node->name] = node;
        return;
    }

    currentFunction = module->getFunction(node->name);
    if (!currentFunction) {
        std::vector<std::pair<std::string, TypePtr>> params;
        bool hasVarargs = false;
        for (const auto& p : node->params) {
            if (p.first == "...") {
                hasVarargs = true;
                break;
            }
            params.emplace_back(p.second, toEirType(p.first));
        }
        currentFunction = module->createFunction(node->name, toEirType(node->returnType), params);
        currentFunction->isVariadic = hasVarargs;
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

    auto savedCleanups = std::move(cleanupScopes);
    auto savedLoops = std::move(loopContextStack);
    cleanupScopes.clear();
    cleanupScopes.emplace_back(); // root function cleanup frame
    loopContextStack.clear();

    pushScope();

    // Allocate stack slots for each parameter
    for (size_t i = 0; i < currentFunction->params.size(); ++i) {
        const auto& p = currentFunction->params[i];
        std::string slotName = "%" + p.first + ".addr";
        emit(Instruction{Opcode::Alloca, slotName, p.second, {Operand::constInt(p.second->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var("%" + p.first, p.second), Operand::var(slotName, Type::getPtr(p.second))}});
        declareVar(p.first, Operand::var(slotName, Type::getPtr(p.second)), p.second);
    }

    if (node->name == "main") {
        if (module->getFunction("__eir_global_init")) {
            emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("__eir_global_init", Type::getVoid())}});
        }
    }

    node->body->accept(this);

    // Ensure terminator and run any remaining cleanups
    if (!currentBlock->hasTerminator()) {
        runCleanupsToDepth(0, false);
        if (!currentBlock->hasTerminator()) {
            if (currentFunction->returnType->isVoid()) {
                emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
            } else {
                emit(Instruction{Opcode::Unreachable, "", Type::getVoid(), {}});
            }
        }
    }

    cleanupScopes = std::move(savedCleanups);
    loopContextStack = std::move(savedLoops);
    popScope();
    currentFunction = nullptr;
    currentBlock = nullptr;
}

void EIRBuilder::visit(VarDecl* node) {
    if (!currentFunction) {
        return; // Globals are processed in Phase 1 of build
    }
    if (node->isVolatile) {
        volatileVars.insert(node->name);
    }
    TypePtr type = toEirType(node->type);

    if (node->isStatic) {
        std::string gname = (currentFunction ? currentFunction->name : "global") + "." + node->name;
        std::string guardName = gname + ".inited";
        bool found = false;
        for (const auto& g : module->globals) {
            if (g.name == gname) { found = true; break; }
        }
        if (!found) {
            module->globals.push_back(GlobalVar{gname, type, Operand::constInt(0, type), false});
            module->globals.push_back(GlobalVar{guardName, Type::getI1(), Operand::constInt(0, Type::getI1()), false});
        }
        declareVar(node->name, Operand::global(gname, Type::getPtr(type)), type);

        if (node->initializer) {
            std::string guardVal = newVar();
            emit(Instruction{Opcode::Load, guardVal, Type::getI1(), {Operand::global(guardName, Type::getPtr(Type::getI1()))}});
            BasicBlock* initBB = createBlock("static.init");
            BasicBlock* contBB = createBlock("static.cont");
            emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(guardVal, Type::getI1()), Operand::block(contBB->name), Operand::block(initBB->name)}});

            setBlock(initBB);
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(1, Type::getI1()), Operand::global(guardName, Type::getPtr(Type::getI1()))}});
            if (auto arrLit = dynamic_cast<ArrayLitExpr*>(node->initializer.get())) {
                emitArrayInitInto(Operand::global(gname, Type::getPtr(type)), arrLit, type);
            } else {
                Operand val = evalForType(node->initializer, node->type);
                Instruction st{Opcode::Store, "", Type::getVoid(),
                    {(type->isStruct() || type->isArray()) ? Operand::var(val.str(), type) : val,
                     Operand::global(gname, Type::getPtr(type))}};
                emit(st);
            }
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(contBB->name)}});
            setBlock(contBB);
        }
        return;
    }

    size_t al = getTypeAlign(type);
    if (al < 1) al = 1;
    std::string slotName = "%" + node->name + ".addr";
    emit(Instruction{Opcode::Alloca, slotName, type, {Operand::constInt(static_cast<int64_t>(al), Type::getI32())}});
    declareVar(node->name, Operand::var(slotName, Type::getPtr(type)), type);

    if (node->initializer) {
        if (auto arrLit = dynamic_cast<ArrayLitExpr*>(node->initializer.get())) {
            emitArrayInitInto(Operand::var(slotName, Type::getPtr(type)), arrLit, type);
        } else {
            Operand val = evalForType(node->initializer, node->type);
            Instruction st{Opcode::Store, "", Type::getVoid(),
                {(type->isStruct() || type->isArray()) ? Operand::var(val.str(), type) : val,
                 Operand::var(slotName, Type::getPtr(type))}};
            st.isVolatile = node->isVolatile;
            emit(st);
        }
    }
}

void EIRBuilder::visit(BlockStmt* node) {
    pushScope();
    cleanupScopes.emplace_back();
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
    }
    cleanupScopes.pop_back();
    popScope();
}

void EIRBuilder::visit(ReturnStmt* node) {
    if (node->value) {
        std::string retTyName = currentFunction && currentFunction->returnType ? currentFunction->returnType->nominalName() : "";
        Operand val = evalForType(node->value, retTyName);
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
    loopContextStack.push_back({exitBB, condBB, cleanupScopes.size()});

    node->body->accept(this);
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
    loopContextStack.push_back({exitBB, stepBB, cleanupScopes.size()});

    node->body->accept(this);
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

void EIRBuilder::visit(BreakStmt*) {
    if (!loopContextStack.empty()) {
        runCleanupsToDepth(loopContextStack.back().cleanupDepth, false);
        if (currentBlock && !currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopContextStack.back().breakTarget->name)}});
        }
    } else if (!breakStack.empty()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(breakStack.back()->name)}});
    }
}

void EIRBuilder::visit(ContinueStmt*) {
    if (!loopContextStack.empty()) {
        runCleanupsToDepth(loopContextStack.back().cleanupDepth, false);
        if (currentBlock && !currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopContextStack.back().continueTarget->name)}});
        }
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
        if (auto* mem = dynamic_cast<MemberExpr*>(node->left.get())) {
            std::string baseType = getExprEirType(mem->base)->nominalName();
            auto it = module->structLayouts.find(baseType);
            if (it != module->structLayouts.end()) {
                const StructFieldInfo* fi = it->second.getFieldInfo(mem->member);
                if (fi && fi->isBitfield) {
                    Operand rhs = evaluateExpr(node->right);
                    storeBitfield(mem, rhs, volatileRooted(mem));
                    lastValue = rhs;
                    return;
                }
            }
        }
        TypePtr lhsTy = getExprEirType(node->left);
        std::string targetTy = lhsTy ? lhsTy->nominalName() : "";
        Operand rhs = evalForType(node->right, targetTy);
        Operand lhsAddr = evaluateLValue(node->left);
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

        if (auto* mem = dynamic_cast<MemberExpr*>(node->left.get())) {
            std::string baseType = getExprEirType(mem->base)->nominalName();
            auto it = module->structLayouts.find(baseType);
            if (it != module->structLayouts.end()) {
                const StructFieldInfo* fi = it->second.getFieldInfo(mem->member);
                if (fi && fi->isBitfield) {
                    Operand cur = loadBitfield(mem, volatileRooted(mem));
                    Operand rhs = evaluateExpr(node->right);
                    std::string resVar = newVar();

                    Opcode binOp = Opcode::Add;
                    if (baseOp == "-") binOp = Opcode::Sub;
                    else if (baseOp == "*") binOp = Opcode::Mul;
                    else if (baseOp == "/") binOp = cur.type->isFloat() ? Opcode::FDiv : (cur.type->isSigned ? Opcode::SDiv : Opcode::UDiv);
                    else if (baseOp == "%") binOp = cur.type->isSigned ? Opcode::SRem : Opcode::URem;
                    else if (baseOp == "&") binOp = Opcode::And;
                    else if (baseOp == "|") binOp = Opcode::Or;
                    else if (baseOp == "^") binOp = Opcode::Xor;
                    else if (baseOp == "<<") binOp = Opcode::Shl;
                    else if (baseOp == ">>") binOp = cur.type->isSigned ? Opcode::AShr : Opcode::LShr;

                    emit(Instruction{binOp, resVar, cur.type, {cur, rhs}});
                    storeBitfield(mem, Operand::var(resVar, cur.type), volatileRooted(mem));
                    lastValue = Operand::var(resVar, cur.type);
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
        Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(resVar, ty), lhsAddr}};
        st.isVolatile = volatileRooted(node->left.get());
        emit(st);
        lastValue = Operand::var(resVar, ty);
        return;
    }

    if (node->op == "&&" || node->op == "||") {
        Operand l = evaluateExpr(node->left);
        Operand lVal = l;
        if (!l.type || !l.type->isInt() || l.type->bitWidth != 1) {
            std::string lCond = newVar();
            if (l.type && l.type->isPointer()) {
                emit(Instruction{Opcode::ICmp, lCond, Type::getI1(), {l, Operand::constNull()}, ICmpCond::Ne});
            } else {
                TypePtr cmpTy = l.type ? l.type : Type::getI32();
                emit(Instruction{Opcode::ICmp, lCond, Type::getI1(), {l, Operand::constInt(0, cmpTy)}, ICmpCond::Ne});
            }
            lVal = Operand::var(lCond, Type::getI1());
        }

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
        if (!r.type || !r.type->isInt() || r.type->bitWidth != 1) {
            std::string rCond = newVar();
            if (r.type && r.type->isPointer()) {
                emit(Instruction{Opcode::ICmp, rCond, Type::getI1(), {r, Operand::constNull()}, ICmpCond::Ne});
            } else {
                TypePtr cmpTy = r.type ? r.type : Type::getI32();
                emit(Instruction{Opcode::ICmp, rCond, Type::getI1(), {r, Operand::constInt(0, cmpTy)}, ICmpCond::Ne});
            }
            rVal = Operand::var(rCond, Type::getI1());
        }
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
    if (auto* mem = dynamic_cast<MemberExpr*>(node->operand.get())) {
        std::string baseType = getExprEirType(mem->base)->nominalName();
        auto it = module->structLayouts.find(baseType);
        if (it != module->structLayouts.end()) {
            const StructFieldInfo* fi = it->second.getFieldInfo(mem->member);
            if (fi && fi->isBitfield) {
                Operand cur = loadBitfield(mem, volatileRooted(mem));
                std::string next = newVar();
                emit(Instruction{node->decrement ? Opcode::Sub : Opcode::Add, next, cur.type, {cur, Operand::constInt(1, cur.type)}});
                storeBitfield(mem, Operand::var(next, cur.type), volatileRooted(mem));
                lastValue = node->prefix ? Operand::var(next, cur.type) : cur;
                return;
            }
        }
    }

    Operand addr = evaluateLValue(node->operand);
    TypePtr ty = getExprEirType(node->operand);

    std::string cur = newVar();
    Instruction ld{Opcode::Load, cur, ty, {addr}};
    ld.isVolatile = volatileRooted(node->operand.get());
    emit(ld);

    std::string next = newVar();
    emit(Instruction{node->decrement ? Opcode::Sub : Opcode::Add, next, ty, {Operand::var(cur, ty), Operand::constInt(1, ty)}});
    Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(next, ty), addr}};
    st.isVolatile = volatileRooted(node->operand.get());
    emit(st);

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
    if (slot) {
        TypePtr ty = lookupVarType(node->name);
        std::string dst = newVar();
        Instruction ld{Opcode::Load, dst, ty, {*slot}};
        ld.isVolatile = volatileVars.count(node->name) > 0;
        emit(ld);
        lastValue = Operand::var(dst, ty);
        lastLValue = *slot;
        return;
    }

    if (module && module->getFunction(node->name)) {
        TypePtr ty = getExprEirType(node);
        if (ty && ty->isClosure()) {
            std::string dst = newVar() + ".fn_closure";
            emit(Instruction{Opcode::MakeClosure, dst, ty, {Operand::global(node->name, Type::getPtr()), Operand::constNull()}});
            lastValue = Operand::var(dst, ty);
            lastLValue = lastValue;
            return;
        }
        lastLValue = Operand::global(node->name, Type::getPtr());
        lastValue = lastLValue;
        return;
    }

    lastLValue = Operand::global(node->name, Type::getPtr());
    TypePtr ty = getExprEirType(node);
    if (ty && !ty->isArray() && !ty->isFunction()) {
        std::string dst = newVar();
        Instruction ld{Opcode::Load, dst, ty, {lastLValue}};
        ld.isVolatile = volatileVars.count(node->name) > 0;
        emit(ld);
        lastValue = Operand::var(dst, ty);
    } else {
        lastValue = lastLValue;
    }
}

void EIRBuilder::visit(LiteralExpr* node) {
    TypePtr ty = getExprEirType(node);
    if (node->kind == LiteralExpr::Kind::FLOAT && ty->isInt()) {
        ty = Type::getF32();
    }
    switch (node->kind) {
        case LiteralExpr::Kind::INT: {
            int64_t v = 0;
            try {
                if (!node->value.empty() && node->value[0] == '-') {
                    v = std::stoll(node->value, nullptr, 0);
                } else {
                    v = static_cast<int64_t>(std::stoull(node->value, nullptr, 0));
                }
            } catch (...) { v = 0; }
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
    if (auto id = dynamic_cast<IdentExpr*>(node->callee.get())) {
        if ((id->name == "va_start" || id->name == "va_end") && node->args.size() == 1) {
            Operand ap = evaluateLValue(node->args[0]);
            emit(Instruction{Opcode::Call, "", Type::getVoid(), {
                Operand::global(id->name, Type::getPtr()),
                ap
            }});
            lastValue = Operand::constInt(0, Type::getVoid());
            return;
        }
    }

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
        std::vector<std::string> paramNominals;
        if (resolvedExprTypes) {
            auto it = resolvedExprTypes->find(node->callee.get());
            if (it != resolvedExprTypes->end()) {
                auto parsed = ty::Type::parse(it->second);
                if (parsed.kind == ty::Type::Kind::Fn) {
                    for (const auto& p : parsed.params) {
                        paramNominals.push_back(p.nominalName());
                    }
                }
            }
        }
        std::vector<Operand> args;
        args.push_back(closureOp);
        for (size_t i = 0; i < node->args.size(); ++i) {
            std::string pName = (i < paramNominals.size()) ? paramNominals[i] : "";
            args.push_back(evalForType(node->args[i], pName));
        }
        TypePtr retTy = nullptr;
        TypePtr cTy = getExprEirType(node->callee);
        if (cTy && cTy->isClosure() && cTy->returnType) {
            retTy = cTy->returnType;
        }
        if (!retTy) retTy = getExprEirType(node);
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
                std::string targetNominal = pt ? pt->nominalName() : "";
                Operand argVal = evalForType(node->args[p], targetNominal);
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {argVal, Operand::var(payPtr, Type::getPtr(pt))}});
                payOffset += getTypeSize(pt);
            }
            lastLValue = Operand::var(slot, Type::getPtr(enumTy));
            lastValue = Operand::var(slot, enumTy);
            return;
        }
    }

    // 2.5 Inferred template function call (e.g. fill(&big, 5000000000, 3))
    if (auto id = dynamic_cast<IdentExpr*>(node->callee.get())) {
        auto tIt = funcTemplateDecls.find(id->name);
        if (tIt != funcTemplateDecls.end()) {
            FunctionDecl* fd = tIt->second;
            std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
            std::map<std::string, std::string> subs;
            auto getArgTypeStr = [&](const ExprPtr& arg) -> std::string {
                if (resolvedExprTypes) {
                    auto it = resolvedExprTypes->find(arg.get());
                    if (it != resolvedExprTypes->end()) return it->second;
                }
                return getExprEirType(arg)->nominalName();
            };
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                if (!tps.count(tyq::strip(fd->params[j].first))) {
                    unifyTypeParam(fd->params[j].first, getArgTypeStr(node->args[j]), tps, subs);
                }
            }
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                std::string tp = tyq::strip(fd->params[j].first);
                if (!tps.count(tp) || subs.count(tp)) continue;
                std::string at = getArgTypeStr(node->args[j]);
                if (at.empty() || at == "null") continue;
                unifyTypeParam(fd->params[j].first, at, tps, subs);
            }
            std::vector<std::string> typeArgs;
            for (const auto& tpName : fd->typeParams) {
                auto sIt = subs.find(tpName);
                if (sIt == subs.end()) break;
                typeArgs.push_back(sIt->second);
            }
            if (typeArgs.size() == fd->typeParams.size()) {
                TemplateCallExpr tc(id->name, typeArgs, node->args);
                visit(&tc);
                return;
            }
        }
    }

    // 3. Named or Member Function Call
    std::string fnName;
    std::vector<Operand> args;

    if (auto id = dynamic_cast<IdentExpr*>(node->callee.get())) {
        fnName = id->name;
        args.push_back(Operand::global(fnName, Type::getPtr()));
        Function* targetFn = module ? module->getFunction(fnName) : nullptr;
        for (size_t i = 0; i < node->args.size(); ++i) {
            TypePtr paramType = (targetFn && i < targetFn->params.size()) ? targetFn->params[i].second : nullptr;
            args.push_back(evalForType(node->args[i], paramType));
        }
    } else if (auto mem = dynamic_cast<MemberExpr*>(node->callee.get())) {
        std::string baseTypeName;
        TypePtr baseTy = getExprEirType(mem->base);
        if (baseTy && baseTy->isInterface()) {
            baseTypeName = baseTy->nominalName();
        } else if (baseTy && baseTy->isStruct()) {
            baseTypeName = baseTy->nominalName();
        } else if (baseTy && baseTy->isPointer() && baseTy->elemType && baseTy->elemType->isStruct()) {
            baseTypeName = baseTy->elemType->nominalName();
        } else if (baseTy && baseTy->isPointer() && baseTy->elemType && baseTy->elemType->isInterface()) {
            baseTypeName = baseTy->elemType->nominalName();
        } else if (resolvedExprTypes) {
            auto it = resolvedExprTypes->find(mem->base.get());
            if (it != resolvedExprTypes->end()) {
                baseTypeName = ty::Type::parse(tyq::strip(it->second)).nominalName();
            }
        }

        if (interfaces.count(baseTypeName)) {
            // Dynamic dispatch through interface vtable!
            Operand fat = evaluateExpr(mem->base);
            if (baseTy && baseTy->isPointer()) {
                std::string loadedFat = newVar();
                emit(Instruction{Opcode::Load, loadedFat, Type::getInterface(baseTypeName), {fat}});
                fat = Operand::var(loadedFat, Type::getInterface(baseTypeName));
            }
            std::string dataVar = newVar() + ".data";
            std::string vtVar = newVar() + ".vt";
            emit(Instruction{Opcode::ExtractFat, dataVar, Type::getPtr(), {fat, Operand::constInt(0, Type::getI32())}});
            emit(Instruction{Opcode::ExtractFat, vtVar, Type::getPtr(), {fat, Operand::constInt(1, Type::getI32())}});

            auto& iface = interfaces[baseTypeName];
            int methodIdx = 0;
            for (size_t m = 0; m < iface.methodNames.size(); ++m) {
                if (iface.methodNames[m] == mem->member) {
                    methodIdx = static_cast<int>(m);
                    break;
                }
            }

            std::string fnPtrSlot = newVar() + ".fn_ptr_slot";
            emit(Instruction{Opcode::Gep, fnPtrSlot, Type::getPtr(), {Operand::var(vtVar, Type::getPtr()), Operand::constInt(methodIdx, Type::getI32())}});
            std::string fnPtr = newVar() + ".fn_ptr";
            emit(Instruction{Opcode::Load, fnPtr, Type::getPtr(), {Operand::var(fnPtrSlot, Type::getPtr())}});

            std::vector<Operand> callArgs;
            callArgs.push_back(Operand::var(fnPtr, Type::getPtr()));
            callArgs.push_back(Operand::var(dataVar, Type::getPtr()));
            for (const auto& arg : node->args) {
                callArgs.push_back(evaluateExpr(arg));
            }

            TypePtr retTy = getExprEirType(node);
            std::string dst = retTy->isVoid() ? "" : newVar();
            emit(Instruction{Opcode::CallIndirect, dst, retTy, callArgs});
            lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
            return;
        }

        std::string mangled = baseTypeName + "_" + mem->member;
        Function* mfunc = module ? module->getFunction(mangled) : nullptr;
        if (!mfunc) mfunc = instantiateGenericMethod(baseTypeName, mem->member);
        if (mfunc) {
            fnName = mfunc->name;
            args.push_back(Operand::global(fnName, Type::getPtr()));
            if (baseTy && baseTy->isPointer()) {
                args.push_back(evaluateExpr(mem->base));
            } else {
                args.push_back(evaluateLValue(mem->base));
            }
            for (size_t i = 0; i < node->args.size(); ++i) {
                TypePtr paramType = (i + 1 < mfunc->params.size()) ? mfunc->params[i + 1].second : nullptr;
                args.push_back(evalForType(node->args[i], paramType));
            }
            TypePtr retTy = mfunc->returnType;
            std::string dst = retTy->isVoid() ? "" : newVar();
            emit(Instruction{Opcode::Call, dst, retTy, args});
            lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
            lastLValue = (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isInterface() || retTy->isClosure() || retTy->isSlice())) ? Operand::var(dst, Type::getPtr(retTy)) : lastValue;
            return;
        }

        if (FunctionDecl* gf = genericFreeMethod(baseTypeName, mem->member)) {
            std::set<std::string> tps(gf->typeParams.begin(), gf->typeParams.end());
            std::map<std::string, std::string> subs;
            bool baseIsPtr = baseTy && baseTy->isPointer();
            std::string recvAsSelf = baseIsPtr ? ("*" + baseTypeName) : baseTypeName;
            if (tyq::isPtr(gf->params[0].first) && !baseIsPtr) recvAsSelf = "*" + baseTypeName;
            unifyTypeParam(gf->params[0].first, recvAsSelf, tps, subs);
            for (size_t j = 1; j < gf->params.size() && j - 1 < node->args.size(); ++j) {
                std::string at;
                if (resolvedExprTypes) {
                    auto it = resolvedExprTypes->find(node->args[j - 1].get());
                    if (it != resolvedExprTypes->end()) at = it->second;
                }
                if (at.empty()) {
                    TypePtr et = getExprEirType(node->args[j - 1]);
                    if (et) {
                        at = et->nominalName();
                        if (et->isPointer()) at = "*" + at;
                    }
                }
                if (!at.empty()) unifyTypeParam(gf->params[j].first, at, tps, subs);
            }
            std::string gname = gf->name;
            for (const auto& tpn : gf->typeParams) gname += "_" + mangleTemplate(subs[tpn]);
            Function* gfunc = instantiateFnTemplate(gf, gname, subs);
            if (gfunc) {
                fnName = gfunc->name;
                args.push_back(Operand::global(fnName, Type::getPtr()));
                if (tyq::isPtr(gf->params[0].first)) {
                    if (baseIsPtr) args.push_back(evaluateExpr(mem->base));
                    else args.push_back(evaluateLValue(mem->base));
                } else {
                    args.push_back(evaluateExpr(mem->base));
                }
                for (size_t i = 0; i < node->args.size(); ++i) {
                    TypePtr paramType = (i + 1 < gfunc->params.size()) ? gfunc->params[i + 1].second : nullptr;
                    args.push_back(evalForType(node->args[i], paramType));
                }
                TypePtr retTy = gfunc->returnType;
                std::string dst = retTy->isVoid() ? "" : newVar();
                emit(Instruction{Opcode::Call, dst, retTy, args});
                lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
                lastLValue = (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isInterface() || retTy->isClosure() || retTy->isSlice())) ? Operand::var(dst, Type::getPtr(retTy)) : lastValue;
                return;
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

        Function* targetFn = module ? module->getFunction(fnName) : nullptr;
        for (size_t i = 0; i < node->args.size(); ++i) {
            TypePtr paramType = (targetFn && i + 1 < targetFn->params.size()) ? targetFn->params[i + 1].second : nullptr;
            args.push_back(evalForType(node->args[i], paramType));
        }
    } else {
        Operand calleeOp = evaluateExpr(node->callee);
        fnName = calleeOp.str();
        args.push_back(calleeOp);
        for (const auto& arg : node->args) {
            args.push_back(evaluateExpr(arg));
        }
    }

    TypePtr retTy = nullptr;
    if (module && !fnName.empty()) {
        std::string lookupName = fnName;
        if (lookupName[0] == '@') lookupName = lookupName.substr(1);
        if (Function* fn = module->getFunction(lookupName)) {
            retTy = fn->returnType;
        }
    }
    if (!retTy) retTy = getExprEirType(node);
    std::string dst = retTy->isVoid() ? "" : newVar();

    emit(Instruction{Opcode::Call, dst, retTy, args});
    lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
    lastLValue = (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isInterface() || retTy->isClosure() || retTy->isSlice())) ? Operand::var(dst, Type::getPtr(retTy)) : lastValue;
}

void EIRBuilder::visit(CastExpr* node) {
    Operand val = evaluateExpr(node->expr);
    TypePtr toTy = toEirType(node->targetType);

    if (val.kind == OperandKind::ConstantInt) {
        if (toTy->isInt()) {
            int64_t v = val.intVal;
            if (toTy->bitWidth == 1) v = v ? 1 : 0;
            else if (toTy->bitWidth == 8) v = toTy->isSigned ? static_cast<int8_t>(v) : static_cast<uint8_t>(v);
            else if (toTy->bitWidth == 16) v = toTy->isSigned ? static_cast<int16_t>(v) : static_cast<uint16_t>(v);
            else if (toTy->bitWidth == 32) v = toTy->isSigned ? static_cast<int32_t>(v) : static_cast<uint32_t>(v);
            lastValue = Operand::constInt(v, toTy);
            return;
        } else if (toTy->isFloat()) {
            lastValue = Operand::constFloat(static_cast<double>(val.intVal), toTy);
            return;
        }
    } else if (val.kind == OperandKind::ConstantFloat) {
        if (toTy->isInt()) {
            lastValue = Operand::constInt(static_cast<int64_t>(val.floatVal), toTy);
            return;
        } else if (toTy->isFloat()) {
            lastValue = Operand::constFloat(val.floatVal, toTy);
            return;
        }
    } else if (val.kind == OperandKind::ConstantNull) {
        lastValue = Operand::constNull();
        lastValue.type = toTy;
        return;
    }

    std::string dst = newVar();

    if (val.type->isInt() && toTy->isInt()) {
        if (toTy->bitWidth < val.type->bitWidth) {
            emit(Instruction{Opcode::Trunc, dst, toTy, {val}});
        } else if (toTy->bitWidth > val.type->bitWidth) {
            emit(Instruction{val.type->isSigned ? Opcode::SExt : Opcode::ZExt, dst, toTy, {val}});
        } else {
            lastValue = val;
            lastValue.type = toTy;
            return;
        }
    } else if (val.type->isFloat() && toTy->isInt()) {
        emit(Instruction{Opcode::FPToSI, dst, toTy, {val}});
    } else if (val.type->isInt() && toTy->isFloat()) {
        emit(Instruction{Opcode::SIToFP, dst, toTy, {val}});
    } else if (val.type->isPointer() && toTy->isPointer()) {
        emit(Instruction{Opcode::Bitcast, dst, toTy, {val}});
    } else {
        lastValue = val;
        lastValue.type = toTy;
        return;
    }

    lastValue = Operand::var(dst, toTy);
}

void EIRBuilder::visit(TernaryExpr* node) {
    Operand cond = evaluateExpr(node->condition);
    if (cond.type->kind != TypeKind::Int || cond.type->bitWidth != 1) {
        std::string cmpDst = newVar();
        emit(Instruction{Opcode::ICmp, cmpDst, Type::getI1(), {cond, Operand::constInt(0, cond.type)}, ICmpCond::Ne});
        cond = Operand::var(cmpDst, Type::getI1());
    }

    BasicBlock* thenBB = createBlock("ternary.then");
    BasicBlock* elseBB = createBlock("ternary.else");
    BasicBlock* mergeBB = createBlock("ternary.merge");

    TypePtr resTy = getExprEirType(node);
    std::string resultSlot = newVar() + ".ternary";
    size_t al = (resTy->bitWidth <= 32 && !resTy->isInterface() && !resTy->isClosure() && !resTy->isSlice() && !resTy->isStruct()) ? 4 : 8;
    emit(Instruction{Opcode::Alloca, resultSlot, resTy, {Operand::constInt(al, Type::getI32())}});

    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(thenBB->name), Operand::block(elseBB->name)}});

    std::string targetNominal = resTy ? resTy->nominalName() : "";
    setBlock(thenBB);
    Operand trueVal = evalForType(node->thenExpr, targetNominal);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {trueVal, Operand::var(resultSlot, Type::getPtr(resTy))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});

    setBlock(elseBB);
    Operand falseVal = evalForType(node->elseExpr, targetNominal);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {falseVal, Operand::var(resultSlot, Type::getPtr(resTy))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});

    setBlock(mergeBB);
    std::string dst = newVar();
    emit(Instruction{Opcode::Load, dst, resTy, {Operand::var(resultSlot, Type::getPtr(resTy))}});
    lastValue = Operand::var(dst, resTy);
}

void EIRBuilder::visit(MemberExpr* node) {
    TypePtr objTy = getExprEirType(node->base);
    if (!objTy) {
        if (auto id = dynamic_cast<IdentExpr*>(node->base.get())) {
            objTy = lookupVarType(id->name);
        }
    }
    if (objTy && (objTy->isSlice() || (objTy->isPointer() && objTy->elemType && objTy->elemType->isSlice()))) {
        Operand sliceFat = evaluateExpr(node->base);
        if (node->member == "len") {
            std::string lenVar = newVar() + ".len";
            emit(Instruction{Opcode::ExtractFat, lenVar, Type::getI64(), {sliceFat, Operand::constInt(1, Type::getI32())}});
            lastValue = Operand::var(lenVar, Type::getI64());
            std::string lenPtr = newVar() + ".len_ptr";
            emit(Instruction{Opcode::OffsetPtr, lenPtr, Type::getPtr(Type::getI64()), {sliceFat, Operand::constInt(8, Type::getI64())}});
            lastLValue = Operand::var(lenPtr, Type::getPtr(Type::getI64()));
            return;
        } else if (node->member == "ptr" || node->member == "data") {
            TypePtr elemTy = objTy->isSlice() ? objTy->elemType : objTy->elemType->elemType;
            if (!elemTy) elemTy = Type::getU8();
            std::string ptrVar = newVar() + ".ptr";
            emit(Instruction{Opcode::ExtractFat, ptrVar, Type::getPtr(elemTy), {sliceFat, Operand::constInt(0, Type::getI32())}});
            lastValue = Operand::var(ptrVar, Type::getPtr(elemTy));
            lastLValue = sliceFat;
            return;
        }
    }

    std::string structName;
    if (objTy && objTy->isStruct()) {
        structName = objTy->nominalName();
    } else if (objTy && objTy->isPointer() && objTy->elemType && objTy->elemType->isStruct()) {
        structName = objTy->elemType->nominalName();
    } else if (resolvedExprTypes) {
        auto it = resolvedExprTypes->find(node->base.get());
        if (it != resolvedExprTypes->end()) {
            std::string st = tyq::strip(it->second);
            if (!typeParamOverride.empty()) st = substType(st, typeParamOverride);
            while (!st.empty() && st.front() == '*') st = st.substr(1);
            while (!st.empty() && st.back() == '*') st.pop_back();
            structName = (st.find('<') != std::string::npos) ? mangleTemplate(st) : ty::Type::parse(st).nominalName();
        }
    }
    if (structName.empty()) {
        if (auto id = dynamic_cast<IdentExpr*>(node->base.get())) {
            TypePtr vt = lookupVarType(id->name);
            if (vt && vt->isPointer() && vt->elemType && vt->elemType->isStruct()) {
                structName = vt->elemType->nominalName();
            } else if (vt && vt->isStruct()) {
                structName = vt->nominalName();
            }
        }
    }

    if (module && !structName.empty()) {
        auto it = module->structLayouts.find(structName);
        if (it != module->structLayouts.end()) {
            const StructFieldInfo* fi = it->second.getFieldInfo(node->member);
            if (fi && fi->isBitfield) {
                lastValue = loadBitfield(node, volatileRooted(node));
                lastLValue = lastValue;
                return;
            }
        }
    }

    bool isPtr = (objTy && objTy->isPointer());
    if (!isPtr) {
        if (auto id = dynamic_cast<IdentExpr*>(node->base.get())) {
            TypePtr vt = lookupVarType(id->name);
            if (vt && vt->isPointer()) isPtr = true;
        }
    }
    Operand basePtr = isPtr ? evaluateExpr(node->base) : evaluateLValue(node->base);

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
    Instruction ld{Opcode::Load, loaded, memberTy, {Operand::var(fptr, Type::getPtr(memberTy))}};
    ld.isVolatile = volatileRooted(node);
    emit(ld);
    lastValue = Operand::var(loaded, memberTy);
    lastLValue = Operand::var(fptr, Type::getPtr(memberTy));
}

void EIRBuilder::visit(IndexExpr* node) {
    if (!node->opFunc.empty()) {
        auto call = std::make_shared<CallExpr>(
            std::make_shared<IdentExpr>(node->opFunc),
            std::vector<ExprPtr>{node->base, node->index});
        call->accept(this);
        return;
    }

    TypePtr baseTy = getExprEirType(node->base);
    TypePtr elemTy = nullptr;
    if (baseTy && (baseTy->isPointer() || baseTy->isArray() || baseTy->isSlice()) && baseTy->elemType) {
        elemTy = baseTy->elemType;
    }
    if (!elemTy) {
        TypePtr selfTy = getExprEirType(node);
        if (node->isSlice() && selfTy && selfTy->isSlice()) {
            elemTy = selfTy->elemType;
        } else if (!node->isSlice() && selfTy) {
            elemTy = selfTy;
        }
    }
    if (!elemTy) elemTy = Type::getI32();

    if (node->isSlice()) {
        Operand lo = evaluateExpr(node->index);
        Operand hi = evaluateExpr(node->highIndex);

        std::string lenVar = newVar() + ".slice_len";
        emit(Instruction{Opcode::Sub, lenVar, Type::getI64(), {hi, lo}});

        Operand sliceStart;
        if (baseTy && baseTy->isSlice()) {
            Operand baseFat = evaluateExpr(node->base);
            std::string baseData = newVar() + ".base_data";
            emit(Instruction{Opcode::ExtractFat, baseData, Type::getPtr(elemTy), {baseFat, Operand::constInt(0, Type::getI32())}});
            std::string startPtr = newVar() + ".slice_start";
            emit(Instruction{Opcode::Gep, startPtr, Type::getPtr(elemTy), {Operand::var(baseData, Type::getPtr(elemTy)), lo}});
            sliceStart = Operand::var(startPtr, Type::getPtr(elemTy));
        } else if (baseTy && baseTy->isPointer()) {
            Operand basePtr = evaluateExpr(node->base);
            std::string startPtr = newVar() + ".slice_start";
            emit(Instruction{Opcode::Gep, startPtr, Type::getPtr(elemTy), {basePtr, lo}});
            sliceStart = Operand::var(startPtr, Type::getPtr(elemTy));
        } else {
            Operand basePtr = evaluateLValue(node->base);
            std::string startPtr = newVar() + ".slice_start";
            emit(Instruction{Opcode::Gep, startPtr, Type::getPtr(elemTy), {basePtr, lo}});
            sliceStart = Operand::var(startPtr, Type::getPtr(elemTy));
        }

        std::string sliceSlot = newVar() + ".slice";
        TypePtr sliceTy = Type::getSlice(elemTy);
        emit(Instruction{Opcode::MakeSlice, sliceSlot, sliceTy, {sliceStart, Operand::var(lenVar, Type::getI64())}});
        lastValue = Operand::var(sliceSlot, sliceTy);
        lastLValue = lastValue;
        return;
    }

    Operand basePtr;
    if (baseTy && baseTy->isSlice()) {
        Operand sliceFat = evaluateExpr(node->base);
        std::string dataPtr = newVar() + ".slice_data";
        emit(Instruction{Opcode::ExtractFat, dataPtr, Type::getPtr(elemTy), {sliceFat, Operand::constInt(0, Type::getI32())}});
        basePtr = Operand::var(dataPtr, Type::getPtr(elemTy));
    } else if (baseTy && baseTy->isPointer()) {
        basePtr = evaluateExpr(node->base);
    } else {
        basePtr = evaluateLValue(node->base);
    }
    Operand idx = evaluateExpr(node->index);

    std::string gepDst = newVar();
    emit(Instruction{Opcode::Gep, gepDst, Type::getPtr(elemTy), {basePtr, idx}});

    std::string loaded = newVar();
    Instruction ld{Opcode::Load, loaded, elemTy, {Operand::var(gepDst, Type::getPtr(elemTy))}};
    ld.isVolatile = volatileRooted(node);
    emit(ld);
    lastValue = Operand::var(loaded, elemTy);
    lastLValue = Operand::var(gepDst, Type::getPtr(elemTy));
}

void EIRBuilder::visit(StructDecl* node) {
    if (!node->typeParams.empty()) {
        templateDecls[node->name] = node;
        return;
    }
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

void EIRBuilder::unifyTypeParam(std::string pattern, std::string concrete,
                                const std::set<std::string>& tps,
                                std::map<std::string, std::string>& subs) const {
    auto stripStruct = [](std::string s) {
        return s.rfind("struct:", 0) == 0 ? s.substr(7) : s;
    };
    auto canon = [](std::string t) {
        int stars = 0;
        while (!t.empty() && t.back()  == '*') { t.pop_back();    stars++; }
        while (!t.empty() && t.front() == '*') { t = t.substr(1); stars++; }
        return std::string(stars, '*') + t;
    };
    pattern  = canon(stripStruct(tyq::strip(pattern)));
    concrete = canon(stripStruct(tyq::strip(concrete)));
    size_t pi = 0, ci = 0;
    while (pi < pattern.size() && pattern[pi] == '*' &&
           ci < concrete.size() && concrete[ci] == '*') { pi++; ci++; }
    pattern = pattern.substr(pi);
    concrete = stripStruct(concrete.substr(ci));
    if (pattern.empty() || concrete.empty()) return;

    if (tps.count(pattern)) {
        if (!subs.count(pattern)) subs[pattern] = concrete;
        return;
    }
    auto isSlice = [](const std::string& t) {
        return t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0;
    };
    if (isSlice(pattern)) {
        if (isSlice(concrete))
            unifyTypeParam(pattern.substr(0, pattern.size() - 2),
                           concrete.substr(0, concrete.size() - 2), tps, subs);
        return;
    }
    if (pattern.rfind("fn(", 0) == 0 && concrete.rfind("fn(", 0) == 0) {
        auto pArrow = pattern.find(")->");
        auto cArrow = concrete.find(")->");
        if (pArrow != std::string::npos && cArrow != std::string::npos) {
            std::string pRet = pattern.substr(pArrow + 3);
            std::string cRet = concrete.substr(cArrow + 3);
            unifyTypeParam(pRet, cRet, tps, subs);
            std::string pParams = pattern.substr(3, pArrow - 3);
            std::string cParams = concrete.substr(3, cArrow - 3);
            unifyTypeParam(pParams, cParams, tps, subs);
        }
        return;
    }
    size_t lt = pattern.find('<');
    if (lt == std::string::npos) return;
    auto [pbase, pargs] = splitTemplateType(pattern);
    std::string cbase; std::vector<std::string> cargs;
    if (concrete.find('<') != std::string::npos) {
        auto pr = splitTemplateType(concrete); cbase = pr.first; cargs = pr.second;
    } else {
        auto it = templateInstanceArgs.find(concrete);
        if (it != templateInstanceArgs.end()) { cbase = it->second.first; cargs = it->second.second; }
    }
    if (cbase != pbase) return;
    for (size_t i = 0; i < pargs.size() && i < cargs.size(); ++i)
        unifyTypeParam(pargs[i], cargs[i], tps, subs);
}

FunctionDecl* EIRBuilder::genericFreeMethod(const std::string& instName, const std::string& method) const {
    auto ia = templateInstanceArgs.find(instName);
    if (ia == templateInstanceArgs.end()) return nullptr;
    auto ft = funcTemplateDecls.find(ia->second.first + "_" + method);
    if (ft == funcTemplateDecls.end() || ft->second->params.empty()) return nullptr;
    return ft->second;
}

Function* EIRBuilder::instantiateGenericMethod(const std::string& instName, const std::string& method) {
    std::string mangled = instName + "_" + method;
    if (module) {
        if (Function* f = module->getFunction(mangled)) return f;
    }
    std::map<std::string, std::string> subs;
    auto ia = templateInstanceArgs.find(instName);
    if (ia == templateInstanceArgs.end()) return nullptr;
    auto tdIt = templateDecls.find(ia->second.first);
    if (tdIt == templateDecls.end()) return nullptr;
    StructDecl* td = tdIt->second;
    FunctionDecl* mf = nullptr;
    for (const auto& m : td->methods) {
        auto* f = dynamic_cast<FunctionDecl*>(m.get());
        if (f && f->name == method) {
            mf = f;
            break;
        }
    }
    if (!mf || !mf->body) return nullptr;

    const auto& tp = td->typeParams;
    for (size_t i = 0; i < tp.size() && i < ia->second.second.size(); ++i)
        subs[tp[i]] = ia->second.second[i];

    std::vector<std::pair<std::string, std::string>> params{{"*" + instName, "self"}};
    for (const auto& p : mf->params) params.push_back({substType(p.first, subs), p.second});

    BasicBlock* savedBlock = currentBlock;
    Function* savedFunc = currentFunction;
    int savedVarId = nextVarId;
    int savedBlockId = nextBlockId;
    auto savedOverride = typeParamOverride;

    typeParamOverride = subs;
    std::string retType = substType(mf->returnType, subs);
    std::vector<std::pair<std::string, TypePtr>> eirParams;
    for (const auto& p : params) {
        eirParams.emplace_back(p.second, toEirType(p.first));
    }
    module->createFunction(mangled, toEirType(retType), eirParams);

    auto inst = std::make_shared<FunctionDecl>(mangled, retType, params, mf->body);
    inst->accept(this);

    typeParamOverride = savedOverride;
    currentFunction = savedFunc;
    currentBlock = savedBlock;
    nextVarId = savedVarId;
    nextBlockId = savedBlockId;

    return module->getFunction(mangled);
}

Function* EIRBuilder::instantiateFnTemplate(FunctionDecl* fd, const std::string& mangledName,
                                            const std::map<std::string, std::string>& subs) {
    if (module) {
        if (Function* f = module->getFunction(mangledName)) return f;
    }

    BasicBlock* savedBlock = currentBlock;
    Function* savedFunc = currentFunction;
    int savedVarId = nextVarId;
    int savedBlockId = nextBlockId;
    auto savedOverride = typeParamOverride;

    typeParamOverride = subs;
    std::string retType = substType(fd->returnType, subs);
    std::vector<std::pair<std::string, std::string>> params;
    std::vector<std::pair<std::string, TypePtr>> eirParams;
    for (const auto& p : fd->params) {
        std::string subPType = substType(p.first, subs);
        params.emplace_back(subPType, p.second);
        eirParams.emplace_back(p.second, toEirType(subPType));
    }
    module->createFunction(mangledName, toEirType(retType), eirParams);

    auto inst = std::make_shared<FunctionDecl>(mangledName, retType, params, fd->body);
    inst->accept(this);

    typeParamOverride = savedOverride;
    currentFunction = savedFunc;
    currentBlock = savedBlock;
    nextVarId = savedVarId;
    nextBlockId = savedBlockId;

    return module->getFunction(mangledName);
}

Operand EIRBuilder::boxAsInterface(const std::string& ifaceName, const std::string& structName, const Operand& structPtr) {
    auto ifIt = interfaces.find(ifaceName);
    if (ifIt == interfaces.end()) return structPtr;

    std::string vtGlobName = ifaceName + "_vtable_" + structName;
    if (module) {
        bool found = false;
        for (const auto& g : module->globals) {
            if (g.name == vtGlobName) {
                found = true;
                break;
            }
        }
        if (!found) {
            GlobalVar g;
            g.name = vtGlobName;
            g.type = Type::getArray(Type::getPtr(), ifIt->second.methodNames.size());
            for (size_t mi = 0; mi < ifIt->second.methodNames.size(); ++mi) {
                const auto& mname = ifIt->second.methodNames[mi];
                std::string mangled = structName + "_" + mname;
                Function* fn = module->getFunction(mangled);
                if (!fn) fn = instantiateGenericMethod(structName, mname);
                if (!fn) {
                    if (FunctionDecl* gf = genericFreeMethod(structName, mname)) {
                        std::set<std::string> tps(gf->typeParams.begin(), gf->typeParams.end());
                        std::map<std::string, std::string> subs;
                        unifyTypeParam(gf->params[0].first, "*" + structName, tps, subs);
                        if (mi < ifIt->second.methodParamTypes.size()) {
                            const auto& ipts = ifIt->second.methodParamTypes[mi];
                            for (size_t j = 1; j < gf->params.size() && j - 1 < ipts.size(); ++j)
                                unifyTypeParam(gf->params[j].first, ipts[j - 1], tps, subs);
                        }
                        std::string gname = gf->name;
                        for (const auto& tpn : gf->typeParams) gname += "_" + mangleTemplate(subs[tpn]);
                        fn = instantiateFnTemplate(gf, gname, subs);
                    }
                }
                std::string actualName = fn ? fn->name : mangled;
                g.elements.push_back(Operand::global(actualName, Type::getPtr()));
            }
            module->globals.push_back(std::move(g));
        }
    }

    std::string dst = newVar() + ".iface";
    emit(Instruction{Opcode::MakeInterface, dst, Type::getInterface(ifaceName), {structPtr, Operand::global(vtGlobName, Type::getPtr())}});
    return Operand::var(dst, Type::getInterface(ifaceName));
}

Operand EIRBuilder::evalForType(const ExprPtr& e, TypePtr targetType) {
    if (!e) return Operand::constNull();
    if (!targetType) return evaluateExpr(e);

    if (targetType->isInterface()) {
        std::string iface = targetType->nominalName();
        TypePtr srcTy = getExprEirType(e);
        if (srcTy && srcTy->isInterface()) return evaluateExpr(e);

        if (auto* lit = dynamic_cast<LiteralExpr*>(e.get()); lit && lit->kind == LiteralExpr::Kind::NULL_VAL) {
            std::string dst = newVar() + ".iface_null";
            emit(Instruction{Opcode::MakeInterface, dst, targetType, {Operand::constNull(), Operand::constNull()}});
            return Operand::var(dst, targetType);
        }

        std::string sname;
        if (resolvedExprTypes) {
            auto it = resolvedExprTypes->find(e.get());
            if (it != resolvedExprTypes->end()) {
                std::string st = tyq::strip(it->second);
                while (!st.empty() && st.front() == '*') st = st.substr(1);
                while (!st.empty() && st.back() == '*') st.pop_back();
                sname = ty::Type::parse(st).nominalName();
            }
        }
        if (sname.empty() && srcTy && srcTy->elemType) {
            sname = srcTy->elemType->nominalName();
        }
        return boxAsInterface(iface, sname, evaluateExpr(e));
    }

    if (targetType->isClosure()) {
        Operand val = evaluateExpr(e);
        if (val.type && val.type->isClosure()) return val;
        std::string dst = newVar() + ".fn_closure";
        emit(Instruction{Opcode::MakeClosure, dst, targetType, {val, Operand::constNull()}});
        return Operand::var(dst, targetType);
    }

    return evaluateExpr(e);
}

Operand EIRBuilder::evalForType(const ExprPtr& e, const std::string& targetType) {
    if (!e) return Operand::constNull();
    if (targetType.empty()) return evaluateExpr(e);
    std::string t = targetType;
    if (t.rfind("interface:", 0) == 0) t = t.substr(10);
    if (interfaces.count(t)) {
        return evalForType(e, Type::getInterface(t));
    }
    return evalForType(e, toEirType(targetType));
}

void EIRBuilder::visit(InterfaceDecl* node) {
    InterfaceInfo info;
    info.name = node->name;
    for (const auto& m : node->methods) {
        info.methodNames.push_back(m.name);
    }
    interfaces[node->name] = info;
}

void EIRBuilder::visit(EnumDecl*) {}
void EIRBuilder::visit(TypeAliasDecl* node) {
    if (node) {
        typeAliases[node->name] = node->aliased;
        templateTypeAliases()[node->name] = node->aliased;
    }
}
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
    loopContextStack.push_back({exitBB, condBB, cleanupScopes.size()});

    node->body->accept(this);
    if (!currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(condBB->name)}});
    }

    loopContextStack.pop_back();
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
    loopContextStack.push_back({endBB, nullptr, cleanupScopes.size()});
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
    loopContextStack.pop_back();
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
                std::vector<TypePtr> actualPayloadTypes = vinfo.payloadTypes;
                std::string subTypeName;
                if (resolvedExprTypes) {
                    auto sit = resolvedExprTypes->find(node->subject.get());
                    if (sit != resolvedExprTypes->end()) {
                        subTypeName = tyq::strip(sit->second);
                    }
                }
                if (subTypeName.empty() && node->subject) {
                    TypePtr st = getExprEirType(node->subject);
                    if (st) subTypeName = st->nominalName();
                }

                if (!subTypeName.empty()) {
                    auto parsed = ty::Type::parse(subTypeName);
                    if (parsed.kind == ty::Type::Kind::Template && !parsed.args.empty()) {
                        for (size_t a = 0; a < parsed.args.size() && a < actualPayloadTypes.size(); ++a) {
                            actualPayloadTypes[a] = toEirType(parsed.args[a].str());
                        }
                    } else {
                        size_t u = subTypeName.find('_');
                        if (u != std::string::npos) {
                            std::string targ = subTypeName.substr(u + 1);
                            if (!actualPayloadTypes.empty()) {
                                actualPayloadTypes[0] = toEirType(targ);
                            }
                        }
                    }
                }
                size_t payOffset = 8;
                for (size_t b = 0; b < arm.bindings.size() && b < actualPayloadTypes.size(); ++b) {
                    TypePtr bTy = actualPayloadTypes[b];
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
    if (sname.find('<') != std::string::npos) {
        sname = mangleTemplate(typeParamOverride.empty() ? sname : substType(sname, typeParamOverride));
    }
    TypePtr stype = toEirType(sname);
    size_t al = getTypeAlign(stype);
    if (al < 1) al = 1;
    std::string tempSlot = newVar();
    emit(Instruction{Opcode::Alloca, tempSlot, stype, {Operand::constInt(static_cast<int64_t>(al), Type::getI32())}});

    for (size_t i = 0; i < node->fieldInits.size(); ++i) {
        std::string fname = node->fieldInits[i].first;
        ExprPtr fval = node->fieldInits[i].second;
        if (!fval) continue;

        const StructFieldInfo* fi = nullptr;
        TypePtr ftype = nullptr;
        if (module) {
            auto it = module->structLayouts.find(sname);
            if (it != module->structLayouts.end()) {
                if (fname.empty() && i < it->second.fields.size()) {
                    fname = it->second.fields[i].name;
                }
                fi = it->second.getFieldInfo(fname);
                if (fi) ftype = fi->type;
            }
        }
        if (!ftype) ftype = getExprEirType(fval);

        if (fi && fi->isBitfield) {
            Operand val = evaluateExpr(fval);
            std::string wordPtr = newVar();
            emit(Instruction{Opcode::OffsetPtr, wordPtr, Type::getPtr(fi->storageType),
                             {Operand::var(tempSlot, Type::getPtr(stype)), Operand::constInt(static_cast<int64_t>(fi->storageOffset), Type::getI64())}});
            std::string wordVal = newVar();
            emit(Instruction{Opcode::Load, wordVal, fi->storageType, {Operand::var(wordPtr, Type::getPtr(fi->storageType))}});

            uint64_t mask = (fi->bitWidth >= 64) ? ~0ULL : ((1ULL << fi->bitWidth) - 1);
            uint64_t fieldMask = mask << fi->bitOffset;
            std::string cleared = newVar();
            emit(Instruction{Opcode::And, cleared, fi->storageType, {Operand::var(wordVal, fi->storageType), Operand::constInt(static_cast<int64_t>(~fieldMask), fi->storageType)}});

            Operand valToStore = val;
            if (val.type->bitWidth != fi->storageType->bitWidth) {
                std::string conv = newVar();
                if (val.type->bitWidth > fi->storageType->bitWidth) {
                    emit(Instruction{Opcode::Trunc, conv, fi->storageType, {val}});
                } else {
                    emit(Instruction{fi->isSigned ? Opcode::SExt : Opcode::ZExt, conv, fi->storageType, {val}});
                }
                valToStore = Operand::var(conv, fi->storageType);
            }

            std::string vMasked = newVar();
            emit(Instruction{Opcode::And, vMasked, fi->storageType, {valToStore, Operand::constInt(static_cast<int64_t>(mask), fi->storageType)}});
            Operand vShifted = Operand::var(vMasked, fi->storageType);
            if (fi->bitOffset > 0) {
                std::string s = newVar();
                emit(Instruction{Opcode::Shl, s, fi->storageType, {vShifted, Operand::constInt(static_cast<int64_t>(fi->bitOffset), fi->storageType)}});
                vShifted = Operand::var(s, fi->storageType);
            }

            std::string newWord = newVar();
            emit(Instruction{Opcode::Or, newWord, fi->storageType, {Operand::var(cleared, fi->storageType), vShifted}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(newWord, fi->storageType), Operand::var(wordPtr, Type::getPtr(fi->storageType))}});
            continue;
        }

        std::string fptr = newVar();
        emit(Instruction{Opcode::FieldPtr, fptr, Type::getPtr(ftype), {
            Operand::var(sname, nullptr),
            Operand::var(tempSlot, Type::getPtr(stype)),
            Operand::constString(fname)
        }});

        Operand val = evalForType(fval, ftype ? ftype->nominalName() : "");
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

void EIRBuilder::visit(TemplateCallExpr* node) {
    if (node->templateName == "va_arg" && node->args.size() == 1 && node->typeArgs.size() == 1) {
        Operand ap = evaluateLValue(node->args[0]);
        std::string t = typeParamOverride.empty() ? node->typeArgs[0] : substType(node->typeArgs[0], typeParamOverride);
        TypePtr resTy = toEirType(t);
        std::string dst = newVar();
        emit(Instruction{Opcode::Call, dst, resTy, {
            Operand::global("va_arg", Type::getPtr()),
            ap
        }});
        lastValue = Operand::var(dst, resTy);
        return;
    }

    std::string fnName = node->templateName;
    std::vector<std::string> substitutedArgs;
    for (const auto& a : node->typeArgs) {
        substitutedArgs.push_back(typeParamOverride.empty() ? a : substType(a, typeParamOverride));
    }

    auto templ = funcTemplateDecls.find(node->templateName);
    if (templ != funcTemplateDecls.end()) {
        FunctionDecl* fd = templ->second;
        std::map<std::string, std::string> subs;
        for (size_t i = 0; i < fd->typeParams.size() && i < substitutedArgs.size(); ++i) {
            subs[fd->typeParams[i]] = substitutedArgs[i];
        }
        std::string mangledName = node->templateName;
        for (const auto& t : substitutedArgs) mangledName += "_" + mangleTemplate(t);
        Function* func = instantiateFnTemplate(fd, mangledName, subs);
        if (func) fnName = func->name;
    } else if (module && !module->getFunction(fnName)) {
        std::string mangled = node->templateName;
        for (const auto& t : substitutedArgs) {
            mangled += "_" + mangleTemplate(t);
        }
        if (module->getFunction(mangled)) {
            fnName = mangled;
        }
    }

    // Check if callee is an ADT variant constructor
    auto it = adtVariants.find(node->templateName);
    if (it != adtVariants.end()) {
        auto& vinfo = it->second;
        TypePtr enumTy = Type::getStruct(vinfo.enumName, {});
        std::string slot = newVar() + ".adt";
        emit(Instruction{Opcode::Alloca, slot, enumTy, {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {
            Operand::constInt(vinfo.variantIndex, Type::getI32()),
            Operand::var(slot, Type::getPtr(Type::getI32()))
        }});

        size_t payOffset = 8;
        for (size_t p = 0; p < node->args.size() && p < vinfo.payloadTypes.size(); ++p) {
            TypePtr pt = (p < substitutedArgs.size()) ? toEirType(substitutedArgs[p]) : vinfo.payloadTypes[p];
            size_t pAlign = getTypeAlign(pt);
            payOffset = (payOffset + pAlign - 1) & ~(pAlign - 1);
            std::string payPtr = newVar() + ".pay";
            emit(Instruction{Opcode::OffsetPtr, payPtr, Type::getPtr(pt), {
                Operand::var(slot, Type::getPtr()), Operand::constInt(static_cast<int64_t>(payOffset), Type::getI64())
            }});
            std::string targetNominal = pt ? pt->nominalName() : "";
            Operand argVal = evalForType(node->args[p], targetNominal);
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {argVal, Operand::var(payPtr, Type::getPtr(pt))}});
            payOffset += getTypeSize(pt);
        }
        lastLValue = Operand::var(slot, Type::getPtr(enumTy));
        lastValue = Operand::var(slot, enumTy);
        return;
    }

    std::vector<Operand> args;
    args.push_back(Operand::global(fnName, Type::getPtr()));
    Function* targetFn = module ? module->getFunction(fnName) : nullptr;
    for (size_t i = 0; i < node->args.size(); ++i) {
        TypePtr paramType = (targetFn && i < targetFn->params.size()) ? targetFn->params[i].second : nullptr;
        args.push_back(evalForType(node->args[i], paramType));
    }

    TypePtr retTy = nullptr;
    if (module && !fnName.empty()) {
        std::string lookupName = fnName;
        if (lookupName[0] == '@') lookupName = lookupName.substr(1);
        if (Function* fn = module->getFunction(lookupName)) {
            retTy = fn->returnType;
        }
    }
    if (!retTy) retTy = getExprEirType(node);
    std::string dst = retTy->isVoid() ? "" : newVar();

    emit(Instruction{Opcode::Call, dst, retTy, args});
    lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
    lastLValue = (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isInterface() || retTy->isClosure() || retTy->isSlice())) ? Operand::var(dst, Type::getPtr(retTy)) : lastValue;
}

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
        emit(Instruction{Opcode::Alloca, envSlot, Type::getArray(Type::getU8(), totalEnvSize), {Operand::constInt(static_cast<int64_t>(maxAlign), Type::getI32())}});
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
    TypePtr lambdaRet = toEirType(node->returnType);
    emit(Instruction{Opcode::MakeClosure, closureSlot, Type::getClosure(lambdaRet), {
        Operand::global(lambdaName, Type::getPtr()), envPtr
    }});
    Operand callerClosure = Operand::var(closureSlot, Type::getClosure(lambdaRet));

    // Compile lambda function body
    Function* prevFn = currentFunction;
    BasicBlock* prevBB = currentBlock;
    int prevVarId = nextVarId;
    int prevBlockId = nextBlockId;
    auto prevBreakStack = breakStack;
    auto prevContinueStack = continueStack;
    auto prevCleanups = std::move(cleanupScopes);
    auto prevLoops = std::move(loopContextStack);
    cleanupScopes.clear();
    cleanupScopes.emplace_back(); // root lambda cleanup frame
    loopContextStack.clear();

    std::vector<std::pair<std::string, TypePtr>> fnParams;
    fnParams.emplace_back("env", Type::getPtr());
    for (const auto& p : node->params) {
        fnParams.emplace_back(p.second, toEirType(p.first));
    }
    Function* lambdaFn = module->createFunction(lambdaName, lambdaRet, fnParams);
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

    cleanupScopes = std::move(prevCleanups);
    loopContextStack = std::move(prevLoops);

    currentFunction = prevFn;
    setBlock(prevBB);
    nextVarId = prevVarId;
    nextBlockId = prevBlockId;
    breakStack = prevBreakStack;
    continueStack = prevContinueStack;

    lastValue = callerClosure;
    lastLValue = callerClosure;
}

void EIRBuilder::visit(AsmStmt* node) {
    std::vector<Operand> operands;
    std::string constraints;
    auto addConstraint = [&](const std::string& c) {
        if (!constraints.empty()) constraints += ",";
        constraints += c;
    };

    struct RegOut { Operand addr; TypePtr ty; bool vol; };
    std::vector<RegOut> regOuts;
    struct Tied { std::string constraint; Operand val; };
    std::vector<Tied> tied;

    for (size_t i = 0; i < node->outputs.size(); ++i) {
        const std::string& c = node->outputs[i].first;
        const ExprPtr& e = node->outputs[i].second;
        bool rw = !c.empty() && c[0] == '+';
        std::string body = c.substr(1);
        TypePtr ty = getExprEirType(e);
        Operand addr = evaluateLValue(e);
        bool vol = volatileRooted(e.get());
        if (body == "m") {
            addConstraint("=*m");
            operands.push_back(addr);
            if (rw) tied.push_back({"*m", addr});
        } else {
            addConstraint("=" + body);
            regOuts.push_back({addr, ty, vol});
            if (rw) {
                std::string cur = newVar();
                Instruction ld{Opcode::Load, cur, ty, {addr}};
                ld.isVolatile = vol;
                emit(ld);
                tied.push_back({std::to_string(i), Operand::var(cur, ty)});
            }
        }
    }

    for (auto& [constraint, expr] : node->inputs) {
        operands.push_back(evaluateExpr(expr));
        addConstraint(constraint);
    }
    for (auto& t : tied) {
        operands.push_back(t.val);
        addConstraint(t.constraint);
    }
    for (const auto& clob : node->clobbers) {
        addConstraint("~{" + clob + "}");
    }

    Instruction inst;
    inst.op = Opcode::InlineAsm;
    inst.asmString = node->asmString;
    inst.asmConstraints = constraints;
    inst.clobbers = node->clobbers;
    inst.operands = operands;

    if (regOuts.size() == 1) {
        inst.type = regOuts[0].ty;
        inst.resultVar = newVar();
        emit(inst);
        Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(inst.resultVar, inst.type), regOuts[0].addr}};
        st.isVolatile = regOuts[0].vol;
        emit(st);
    } else if (regOuts.size() > 1) {
        std::vector<TypePtr> retFields;
        for (auto& r : regOuts) retFields.push_back(r.ty);
        inst.type = Type::getStruct("asm_out", retFields);
        inst.resultVar = newVar();
        emit(inst);
        for (size_t i = 0; i < regOuts.size(); ++i) {
            std::string ev = newVar();
            emit(Instruction{Opcode::ExtractFat, ev, regOuts[i].ty, {Operand::var(inst.resultVar, inst.type), Operand::constInt((int64_t)i, Type::getI32())}});
            Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(ev, regOuts[i].ty), regOuts[i].addr}};
            st.isVolatile = regOuts[i].vol;
            emit(st);
        }
    } else {
        inst.type = Type::getVoid();
        emit(inst);
    }
}

void EIRBuilder::visit(UnionDecl*) {}

void EIRBuilder::visit(SizeofExpr* node) {
    TypePtr ty = nullptr;
    if (!node->typeName.empty()) {
        std::string actualType = node->typeName;
        if (!typeParamOverride.empty()) {
            actualType = substType(actualType, typeParamOverride);
        }
        ty = toEirType(actualType);
    } else if (node->operand) {
        ty = getExprEirType(node->operand);
    }
    size_t sz = 0;
    if (ty) {
        sz = getTypeSize(ty, module.get());
        if (sz == 0 && ty->isStruct() && module) {
            sz = module->getStructSize(ty->nominalName());
        }
    }
    if (sz == 0) sz = 8;
    lastValue = Operand::constInt(static_cast<int64_t>(sz), Type::getI64());
    lastLValue = lastValue;
}

void EIRBuilder::visit(ThreadCreateExpr* node) {
    Operand worker = evaluateExpr(node->worker);
    std::string handleVar = newVar() + ".thr_handle";
    emit(Instruction{Opcode::Call, handleVar, Type::getPtr(), {Operand::global("__eir_thread_create", Type::getPtr()), worker}});
    lastValue = Operand::var(handleVar, Type::getPtr());
    lastLValue = lastValue;
}

void EIRBuilder::visit(FreeClosureExpr* node) {
    Operand closureOp = evaluateExpr(node->closure);
    std::string envPtr = newVar() + ".env";
    emit(Instruction{Opcode::ExtractFat, envPtr, Type::getPtr(), {closureOp, Operand::constInt(1, Type::getI32())}});
    emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("free", Type::getPtr()), Operand::var(envPtr, Type::getPtr())}});
    lastValue = Operand::var("", Type::getVoid());
}

void EIRBuilder::visit(AwaitExpr*) {}

void EIRBuilder::visit(ThreadJoinStmt* node) {
    Operand tid = evaluateExpr(node->tid);
    emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("__eir_thread_join", Type::getVoid()), tid}});
}
void EIRBuilder::visit(ThrowStmt*) {}
void EIRBuilder::visit(TryStmt* node) {
    if (node->finally) {
        cleanupScopes.emplace_back();
        cleanupScopes.back().push_back({node->finally, false});
    }
    if (node->body) {
        node->body->accept(this);
    }
    if (node->finally) {
        if (currentBlock && !currentBlock->hasTerminator()) {
            runCleanupsToDepth(cleanupScopes.size() - 1, false);
        }
        cleanupScopes.pop_back();
    }
}

void EIRBuilder::visit(DeferStmt* node) {
    if (node->body && !cleanupScopes.empty()) {
        cleanupScopes.back().push_back({node->body, node->isErr});
    }
}

} // namespace eir
