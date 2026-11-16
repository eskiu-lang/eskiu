#include "eir_builder.h"
#include "../sema/type_checker.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "../template_utils.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <functional>

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
                // A dimension is an integer constant expression (`N * 2`, `sizeof(S) / 4`).
                long long v = 0;
                auto name = [&](const std::string& nm, long long& out) {
                    if (nm.rfind("sizeof(", 0) == 0 && nm.back() == ')') {
                        out = static_cast<long long>(sizeOfType(nm.substr(7, nm.size() - 8)));
                        return out > 0;
                    }
                    auto it = enumConstants.find(nm);
                    if (it == enumConstants.end()) return false;
                    out = it->second;
                    return true;
                };
                if (ty::foldDim(t.dim, name, v) && v > 0) n = static_cast<uint64_t>(v);
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
                ensureStructLayout(sname);   // every struct the module names has a layout
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
    // Inside a generic instance the shared AST carries no per-instance types: a local's
    // type is the one its declaration took for this instance.
    if (!typeParamOverride.empty()) {
        if (auto id = nodeCast<const IdentExpr>(expr)) {
            TypePtr vt = lookupVarType(id->name);
            if (vt && !vt->isVoid()) return vt;
        }
    }
    // In a generic instance, the type the checker gave this expression in this instance.
    if (!typeParamOverride.empty() && instanceExprTypes) {
        auto it = instanceExprTypes->find(const_cast<Expr*>(expr));
        if (it != instanceExprTypes->end()) {
            for (const auto& [subs, t] : it->second) {
                if (subs == typeParamOverride) return toEirType(t);
            }
        }
    }
    if (resolvedExprTypes) {
        auto it = resolvedExprTypes->find(const_cast<Expr*>(expr));
        if (it != resolvedExprTypes->end()) {
            std::string t = it->second;
            if (!typeParamOverride.empty()) t = substType(t, typeParamOverride);
            return toEirType(t);
        }
    }
    if (auto id = nodeCast<const IdentExpr>(expr)) {
        TypePtr vt = lookupVarType(id->name);
        if (vt && !vt->isVoid()) return vt;
    }
    if (auto c = nodeCast<const CastExpr>(expr)) {
        std::string targetTy = c->targetType;
        if (!typeParamOverride.empty()) targetTy = substType(targetTy, typeParamOverride);
        return toEirType(targetTy);
    }
    if (auto mem = nodeCast<const MemberExpr>(expr)) {
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
    if (auto idx = nodeCast<const IndexExpr>(expr)) {
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
    if (auto u = nodeCast<const UnaryExpr>(expr)) {
        if (u->op == "*") {
            TypePtr opty = getExprEirType(u->operand);
            if (opty && opty->isPointer() && opty->elemType) return opty->elemType;
        } else if (u->op == "&") {
            TypePtr opty = getExprEirType(u->operand);
            if (opty) return Type::getPtr(opty);
        }
    }
    if (auto tc = nodeCast<const TemplateCallExpr>(expr)) {
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
    if (auto ce = nodeCast<const CallExpr>(expr)) {
        TypePtr cTy = getExprEirType(ce->callee.get());
        if (cTy && cTy->isClosure() && cTy->returnType) {
            return cTy->returnType;
        }
        if (auto id = nodeCast<const IdentExpr>(ce->callee.get())) {
            if (module) {
                if (Function* fn = module->getFunction(id->name)) return fn->returnType;
            }
        }
        if (auto mem = nodeCast<const MemberExpr>(ce->callee.get())) {
            TypePtr bty = getExprEirType(mem->base);
            std::string sname;
            if (bty && bty->isStruct()) sname = bty->nominalName();
            else if (bty && bty->isPointer() && bty->elemType && bty->elemType->isStruct()) sname = bty->elemType->nominalName();
            if (!sname.empty() && module) {
                if (Function* fn = module->getFunction(sname + "_" + mem->member)) return fn->returnType;
            }
        }
    }
    if (auto b = nodeCast<const BinaryExpr>(expr)) {
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

// An integer constant expression folded with C's rules (ty::CInt: promotions, usual
// arithmetic conversions, wraparound), as sema and codegen fold. False when it is not one.
bool EIRBuilder::foldCInt(const ExprPtr& expr, ty::CInt& out) {
    if (!expr) return false;
    if (auto lit = nodeCast<LiteralExpr>(expr.get())) {
        if (lit->kind == LiteralExpr::Kind::INT) {
            try {
                if (!lit->value.empty() && lit->value[0] == '-') {
                    out = ty::cintNegLiteral(std::stoull(lit->value.substr(1), nullptr, 0));
                } else {
                    out = ty::cintLiteral(std::stoull(lit->value, nullptr, 0));
                }
            } catch (...) { return false; }
            return true;
        }
        if (lit->kind == LiteralExpr::Kind::CHAR) {
            out = ty::cintMake(lit->value.empty() ? 0 : static_cast<unsigned char>(lit->value[0]), 32, false);
            return true;
        }
        if (lit->kind == LiteralExpr::Kind::BOOL) {
            out = ty::cintMake(lit->value == "true" ? 1 : 0, 32, false);
            return true;
        }
        return false;
    }
    if (auto id = nodeCast<IdentExpr>(expr.get())) {
        auto it = enumConstants.find(id->name);
        if (it == enumConstants.end()) return false;
        auto ct = constTypes.find(id->name);
        TypePtr t = ct != constTypes.end() ? ct->second : Type::getI32();
        if (t->bitWidth < 32) out = ty::cintMake(it->second, 32, false);
        else out = ty::cintMake(it->second, t->bitWidth > 32 ? 64 : 32, !t->isSigned);
        return true;
    }
    if (auto u = nodeCast<UnaryExpr>(expr.get())) {
        ty::CInt x;
        return foldCInt(u->operand, x) && ty::cintUnary(u->op, x, out);
    }
    if (auto b = nodeCast<BinaryExpr>(expr.get())) {
        ty::CInt l, r;
        return foldCInt(b->left, l) && foldCInt(b->right, r) && ty::cintBinary(b->op, l, r, out);
    }
    if (auto c = nodeCast<CastExpr>(expr.get())) {
        ty::CInt x;
        std::string t = c->targetType;
        auto al = typeAliases.find(t);
        if (al != typeAliases.end()) t = al->second;
        return foldCInt(c->expr, x) && ty::cintCast(t, x, out);
    }
    if (auto sz = nodeCast<SizeofExpr>(expr.get())) {
        uint64_t n = 0;
        if (!sz->typeName.empty() && lookupVar(sz->typeName)) n = getTypeSize(lookupVarType(sz->typeName), module.get());
        else if (!sz->typeName.empty()) n = sizeOfType(sz->typeName);
        else if (sz->operand) n = getTypeSize(getExprEirType(sz->operand), module.get());
        if (n == 0) return false;
        out = ty::cintMake(static_cast<long long>(n), 64, true);   // uint64 in constant folds, as sema's
        return true;
    }
    if (auto t = nodeCast<TernaryExpr>(expr.get())) {
        ty::CInt c, a, b;
        if (!foldCInt(t->condition, c) || !foldCInt(t->thenExpr, a) || !foldCInt(t->elseExpr, b)) return false;
        out = c.v ? a : b;
        return true;
    }
    return false;
}

// The size of a type spelled in the source, laid out for the target.
uint64_t EIRBuilder::sizeOfType(const std::string& spelling) {
    std::string t = typeParamOverride.empty() ? spelling : substType(spelling, typeParamOverride);
    TypePtr ty = toEirType(t);
    if (ty && ty->isStruct()) ensureStructLayout(ty->nominalName());
    size_t sz = getTypeSize(ty, module.get());
    if (ty && ty->isStruct()) {
        size_t ss = module->getStructSize(ty->nominalName());
        if (ss) sz = ss;
    }
    return sz;
}

int64_t EIRBuilder::foldConstInt(const ExprPtr& expr) {
    ty::CInt v;
    return foldCInt(expr, v) ? v.v : 0;
}

void EIRBuilder::emitArrayInitInto(Operand dest, ArrayLitExpr* lit, TypePtr arrTy) {
    if (!lit || !arrTy || !arrTy->isArray()) return;
    for (size_t i = 0; i < lit->elements.size() && i < arrTy->arraySize; ++i) {
        std::string elemPtr = newVar();
        emit(Instruction{Opcode::Gep, elemPtr, Type::getPtr(arrTy->elemType), {
            dest, Operand::constInt(static_cast<int64_t>(i), Type::getI32())
        }});
        if (arrTy->elemType->isArray()) {
            if (auto subLit = nodeCast<ArrayLitExpr>(lit->elements[i].get())) {
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

} // namespace eir
