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

static std::string intSpelling(const TypePtr& t);

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
        if (auto id = dynamic_cast<const IdentExpr*>(expr)) {
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

// An integer constant expression folded with C's rules (ty::CInt: promotions, usual
// arithmetic conversions, wraparound), as sema and codegen fold. False when it is not one.
bool EIRBuilder::foldCInt(const ExprPtr& expr, ty::CInt& out) {
    if (!expr) return false;
    if (auto lit = dynamic_cast<LiteralExpr*>(expr.get())) {
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
    if (auto id = dynamic_cast<IdentExpr*>(expr.get())) {
        auto it = enumConstants.find(id->name);
        if (it == enumConstants.end()) return false;
        auto ct = constTypes.find(id->name);
        TypePtr t = ct != constTypes.end() ? ct->second : Type::getI32();
        if (t->bitWidth < 32) out = ty::cintMake(it->second, 32, false);
        else out = ty::cintMake(it->second, t->bitWidth > 32 ? 64 : 32, !t->isSigned);
        return true;
    }
    if (auto u = dynamic_cast<UnaryExpr*>(expr.get())) {
        ty::CInt x;
        return foldCInt(u->operand, x) && ty::cintUnary(u->op, x, out);
    }
    if (auto b = dynamic_cast<BinaryExpr*>(expr.get())) {
        ty::CInt l, r;
        return foldCInt(b->left, l) && foldCInt(b->right, r) && ty::cintBinary(b->op, l, r, out);
    }
    if (auto c = dynamic_cast<CastExpr*>(expr.get())) {
        ty::CInt x;
        std::string t = c->targetType;
        auto al = typeAliases.find(t);
        if (al != typeAliases.end()) t = al->second;
        return foldCInt(c->expr, x) && ty::cintCast(t, x, out);
    }
    if (auto sz = dynamic_cast<SizeofExpr*>(expr.get())) {
        uint64_t n = 0;
        if (!sz->typeName.empty() && lookupVar(sz->typeName)) n = getTypeSize(lookupVarType(sz->typeName), module.get());
        else if (!sz->typeName.empty()) n = sizeOfType(sz->typeName);
        else if (sz->operand) n = getTypeSize(getExprEirType(sz->operand), module.get());
        if (n == 0) return false;
        out = ty::cintMake(static_cast<long long>(n), 64, true);   // uint64 in constant folds, as sema's
        return true;
    }
    if (auto t = dynamic_cast<TernaryExpr*>(expr.get())) {
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
            auto savedScopes = scopeStack;
            auto savedTypes = varTypeStack;
            if (!frame[j].scopes.empty()) {
                scopeStack = frame[j].scopes;
                varTypeStack = frame[j].varTypes;
            }
            frame[j].body->accept(this);
            scopeStack = std::move(savedScopes);
            varTypeStack = std::move(savedTypes);
        }
    }
}

// An unbraced body (of an if, else, loop, case or match arm) is a scope of its own: its
// defers run at the end of it, and only if it was reached.
void EIRBuilder::visitBody(const StmtPtr& body) {
    if (!body) return;
    if (dynamic_cast<BlockStmt*>(body.get())) {
        body->accept(this);
        return;
    }
    pushScope();
    cleanupScopes.emplace_back();
    size_t depth = cleanupScopes.size() - 1;
    body->accept(this);
    if (currentBlock && !currentBlock->hasTerminator()) runCleanupsToDepth(depth, false);
    cleanupScopes.pop_back();
    popScope();
}

// Lay out a struct or union as C does on the target: natural alignment capped by
// `#pragma pack`, bitfields by the SysV/AAPCS rules (sharing an aligned unit of their
// type) or the MS ones on Windows (a new word when the type's size changes), unnamed
// bitfields only moving the position. The rules are TypeChecker::bitfieldLayout's (which
// computes sizeof); this also records where each field lives.
void EIRBuilder::layoutRecord(const std::string& name, const std::vector<StructDecl::Field>& declFields,
                              const std::vector<StructDecl::Pad>& pads, bool isUnion, int packAlign,
                              const std::map<std::string, std::string>& subs) {
    auto up = [](uint64_t x, uint64_t a) { return a ? (x + a - 1) / a * a : x; };
    TargetLayoutInfo L = targetLayoutInfo(targetTriple);
    bool packed = packAlign == 1;
    uint64_t packN = packAlign >= 2 ? static_cast<uint64_t>(packAlign) : 0;
    bool contiguous = packed || packN;

    StructInfo sinfo;
    sinfo.name = name;
    std::vector<TypePtr> fieldTypes;
    uint64_t structAlign = 1;

    struct Item { StructDecl::Field f; TypePtr ty; uint64_t size, align, natural; };
    std::vector<Item> items;
    for (const auto& f : layoutFields(declFields, pads)) {
        std::string tstr = subs.empty() ? f.type : substType(f.type, subs);
        TypePtr ft = toEirType(tstr);
        ensureLayoutOf(ft);
        uint64_t fs = getTypeSize(ft, module.get());
        if (ft && ft->isStruct()) { size_t ss = module->getStructSize(ft->nominalName()); if (ss) fs = ss; }
        uint64_t fa = getTypeAlign(ft, module.get());
        if (fa < 1) fa = 1;
        uint64_t natural = fa;
        if (packed) fa = 1;
        if (packN) fa = std::min(fa, packN);
        items.push_back({f, ft, fs, fa, natural});
        if (!f.unnamed) fieldTypes.push_back(ft);
    }

    auto unsignedUnit = [](uint64_t bytes) {
        return bytes >= 8 ? Type::getU64() : bytes >= 4 ? Type::getU32() : bytes >= 2 ? Type::getU16() : Type::getU8();
    };
    auto addField = [&](const Item& it, uint64_t off) {
        StructFieldInfo fi;
        fi.name = it.f.name;
        fi.type = it.ty;
        fi.offset = off;
        fi.size = it.size;
        sinfo.fields.push_back(fi);
    };
    auto addBitfield = [&](const Item& it, uint64_t unitOff, uint64_t unitBytes, uint64_t bitOff) {
        StructFieldInfo fi;
        fi.name = it.f.name;
        fi.type = it.ty;
        fi.offset = unitOff;
        fi.size = unitBytes;
        fi.isBitfield = true;
        fi.bitOffset = static_cast<unsigned>(bitOff);
        fi.bitWidth = static_cast<unsigned>(it.f.bitWidth);
        fi.isSigned = it.ty && it.ty->isInt() && it.ty->isSigned && it.ty->bitWidth > 1;
        // An enum bitfield with no negative member is unsigned on SysV/AAPCS (clang).
        if (auto en = plainEnums.find(tyq::strip(it.f.type)); en != plainEnums.end() && !L.msBitfields) {
            bool negative = false;
            for (const auto& m : en->second->members) {
                auto c = enumConstants.find(m.first);
                if (c != enumConstants.end() && c->second < 0) negative = true;
            }
            if (!negative) {
                fi.isSigned = false;
                fi.type = Type::getU32();
            }
        }
        fi.storageType = unsignedUnit(unitBytes);
        fi.storageOffset = unitOff;
        sinfo.fields.push_back(fi);
    };
    // A bitfield packed back to back is read through the smallest unit covering its bits.
    auto spanUnit = [](uint64_t bitpos, uint64_t w) {
        uint64_t bytes = (bitpos % 8 + w + 7) / 8;
        return bytes <= 1 ? 1 : bytes <= 2 ? 2 : bytes <= 4 ? 4 : 8;
    };

    uint64_t size = 0;
    if (isUnion) {
        for (const auto& it : items) {
            if (it.f.unnamed) {
                uint64_t bytes = L.msBitfields ? it.size : (static_cast<uint64_t>(std::max(it.f.bitWidth, 0)) + 7) / 8;
                size = std::max(size, bytes);
                continue;
            }
            structAlign = std::max(structAlign, it.align);
            if (it.f.bitWidth > 0) addBitfield(it, 0, it.size, 0);
            else addField(it, 0);
            size = std::max(size, it.size);
        }
    } else if (L.msBitfields) {
        uint64_t off = 0, curBits = 0, curOff = 0, wordOff = 0;
        bool open = false, afterBits = false;
        for (const auto& it : items) {
            if (it.f.unnamed && it.f.bitWidth <= 0) {
                if (afterBits) {
                    uint64_t a = L.msvcEnv ? it.align : it.natural;
                    off = up(off, a);
                    structAlign = std::max(structAlign, a);
                }
                open = false;
                afterBits = false;
                continue;
            }
            if (it.f.bitWidth > 0) {
                uint64_t stBits = it.size * 8, w = static_cast<uint64_t>(it.f.bitWidth);
                if (!open || curBits != stBits || curOff + w > stBits) {
                    wordOff = up(off, it.align);
                    off = wordOff + it.size;
                    structAlign = std::max(structAlign, it.align);
                    open = true;
                    curBits = stBits;
                    curOff = 0;
                }
                if (!it.f.unnamed) addBitfield(it, wordOff, it.size, curOff);
                curOff += w;
                afterBits = true;
            } else {
                open = false;
                afterBits = false;
                off = up(off, it.align);
                addField(it, off);
                off += it.size;
                structAlign = std::max(structAlign, it.align);
            }
        }
        size = off;
    } else {
        uint64_t bitpos = 0;
        for (const auto& it : items) {
            if (it.f.unnamed && it.f.bitWidth <= 0) {
                bitpos = up(bitpos, it.natural * 8);
                if (L.unnamedBitfieldsAlign) structAlign = std::max(structAlign, it.natural);
                continue;
            }
            if (!it.f.unnamed || L.unnamedBitfieldsAlign) structAlign = std::max(structAlign, it.align);
            if (it.f.bitWidth > 0) {
                uint64_t w = static_cast<uint64_t>(it.f.bitWidth), unitBits = it.size * 8;
                if (!contiguous && it.align < it.size) {
                    if (bitpos % (it.align * 8) + w > unitBits) bitpos = up(bitpos, it.align * 8);
                } else if (!contiguous && unitBits && bitpos / unitBits != (bitpos + w - 1) / unitBits) {
                    bitpos = up(bitpos, unitBits);
                }
                if (!it.f.unnamed) {
                    if (contiguous) {
                        uint64_t unit = spanUnit(bitpos, w);
                        addBitfield(it, bitpos / 8, unit, bitpos % 8);
                    } else {
                        uint64_t unitOff = bitpos / (it.align * 8) * it.align;
                        addBitfield(it, unitOff, it.size, bitpos - unitOff * 8);
                    }
                }
                bitpos += w;
            } else {
                uint64_t off = up((bitpos + 7) / 8, it.align);
                addField(it, off);
                bitpos = (off + it.size) * 8;
            }
        }
        size = (bitpos + 7) / 8;
    }
    sinfo.alignment = structAlign;
    sinfo.totalSize = up(size, structAlign);
    sinfo.isUnion = isUnion;
    sinfo.packAlign = packAlign;
    for (const auto& pd : pads)
        sinfo.pads.push_back({toEirType(subs.empty() ? pd.type : substType(pd.type, subs)), pd.bitWidth, pd.before});
    module->structLayouts[name] = sinfo;
    module->structTypes.push_back(Type::getStruct(name, fieldTypes));
}

// The variant `variant` of the sum type `instName` (an instance of a generic enum has its
// own payload types), else the variant by name.
const EIRBuilder::AdtVariantInfo* EIRBuilder::variantFor(const std::string& variant, const std::string& instName) {
    if (!instName.empty()) {
        ensureStructLayout(instName);
        auto it = adtInstanceVariants.find(instName + "::" + variant);
        if (it != adtInstanceVariants.end()) return &it->second;
    }
    auto it = adtVariants.find(variant);
    return it != adtVariants.end() ? &it->second : nullptr;
}

// The instance `G<args>` of the generic enum declaring `variant`, registered for layout.
std::string EIRBuilder::genericVariantInstance(const std::string& variant, const std::vector<std::string>& args) {
    auto it = adtVariants.find(variant);
    if (it == adtVariants.end() || !genericEnums.count(it->second.enumName) || args.empty()) return "";
    std::string spelled = it->second.enumName + "<";
    for (size_t i = 0; i < args.size(); ++i) spelled += (i ? "," : "") + args[i];
    spelled += ">";
    std::string inst = mangleTemplate(spelled);
    if (!templateInstanceArgs.count(inst)) templateInstanceArgs[inst] = {it->second.enumName, args};
    return inst;
}

// A sum type as codegen's makeAdtStruct: { i32 tag, [N x i64] payload }, the payload at 8,
// N the largest variant's bytes in i64s (at least one). Registers each variant.
void EIRBuilder::layoutAdt(const std::string& name, EnumDecl* e, const std::map<std::string, std::string>& subs) {
    size_t maxPayloadSize = 0;
    for (size_t i = 0; i < e->members.size(); ++i) {
        std::vector<TypePtr> payloadTypes;
        size_t pOffset = 0, pMaxAlign = 1;
        if (i < e->payloads.size()) {
            for (const auto& ptStr : e->payloads[i]) {
                TypePtr pt = toEirType(subs.empty() ? ptStr : substType(ptStr, subs));
                ensureLayoutOf(pt);
                payloadTypes.push_back(pt);
                size_t al = getTypeAlign(pt, module.get());
                if (al > pMaxAlign) pMaxAlign = al;
                pOffset = (pOffset + al - 1) & ~(al - 1);
                size_t sz = getTypeSize(pt, module.get());
                if (pt && pt->isStruct()) { size_t ss = module->getStructSize(pt->nominalName()); if (ss) sz = ss; }
                pOffset += sz;
            }
        }
        pOffset = (pOffset + pMaxAlign - 1) & ~(pMaxAlign - 1);
        if (pOffset > maxPayloadSize) maxPayloadSize = pOffset;
        if (subs.empty()) adtVariants[e->members[i].first] = {name, static_cast<int>(i), payloadTypes};
        else adtInstanceVariants[name + "::" + e->members[i].first] = {name, static_cast<int>(i), payloadTypes};
    }
    size_t words = std::max<size_t>(1, (maxPayloadSize + 7) / 8);
    StructInfo sinfo;
    sinfo.name = name;
    sinfo.fields.push_back({"tag", Type::getI32(), 0, 4});
    sinfo.totalSize = 8 + 8 * words;
    sinfo.alignment = 8;
    module->structLayouts[name] = sinfo;
    module->structTypes.push_back(Type::getStruct(name, {Type::getI32()}));
}

// Lay out the struct a type holds by value (itself, or an array's element).
void EIRBuilder::ensureLayoutOf(const TypePtr& t) {
    TypePtr e = t;
    while (e && e->isArray()) e = e->elemType;
    if (e && e->isStruct()) ensureStructLayout(e->nominalName());
}

void EIRBuilder::ensureStructLayout(const std::string& name) {
    if (name.empty() || module->structLayouts.count(name) || layoutsInProgress.count(name)) return;
    layoutsInProgress.insert(name);
    struct Done {
        std::set<std::string>& s; std::string n;
        ~Done() { s.erase(n); }
    } done{layoutsInProgress, name};
    auto tit = templateInstanceArgs.find(name);
    if (tit != templateInstanceArgs.end()) {
        auto geIt = genericEnums.find(tit->second.first);
        if (geIt != genericEnums.end()) {
            EnumDecl* ge = geIt->second;
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < ge->typeParams.size() && i < tit->second.second.size(); ++i)
                subs[ge->typeParams[i]] = tit->second.second[i];
            layoutAdt(name, ge, subs);
            return;
        }
        auto tdIt = templateDecls.find(tit->second.first);
        if (tdIt != templateDecls.end()) {
            StructDecl* td = tdIt->second;
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < td->typeParams.size() && i < tit->second.second.size(); ++i)
                subs[td->typeParams[i]] = tit->second.second[i];
            layoutRecord(name, td->fields, td->pads, false, td->isPacked ? 1 : td->packAlign, subs);
            return;
        }
    }
    auto sit = plainStructs.find(name);
    if (sit != plainStructs.end()) {
        StructDecl* sd = sit->second;
        layoutRecord(name, sd->fields, sd->pads, false, sd->isPacked ? 1 : sd->packAlign, {});
        return;
    }
    auto ait = adtEnums.find(name);
    if (ait != adtEnums.end()) {
        layoutAdt(name, ait->second, {});
        return;
    }
    auto uit = unionDecls.find(name);
    if (uit != unionDecls.end()) {
        layoutRecord(name, uit->second->fields, uit->second->pads, true, uit->second->packAlign, {});
    }
}

// The bitfield `m` names (null when it is not one) and the address of its struct.
const StructFieldInfo* EIRBuilder::bitfieldOf(MemberExpr* m) {
    std::string baseType = getExprEirType(m->base)->nominalName();
    auto it = module->structLayouts.find(baseType);
    if (it == module->structLayouts.end()) return nullptr;
    const StructFieldInfo* fi = it->second.getFieldInfo(m->member);
    return fi && fi->isBitfield ? fi : nullptr;
}

Operand EIRBuilder::bitfieldBase(MemberExpr* m) {
    TypePtr baseTy = getExprEirType(m->base);
    return baseTy->isPointer() ? evaluateExpr(m->base) : evaluateLValue(m->base);
}

Operand EIRBuilder::loadBitfield(MemberExpr* m, bool isVolatile) {
    const StructFieldInfo* fi = bitfieldOf(m);
    if (!fi) return Operand::constInt(0, Type::getI32());
    return loadBitfieldAt(bitfieldBase(m), fi, isVolatile);
}

void EIRBuilder::storeBitfield(MemberExpr* m, Operand val, bool isVolatile) {
    const StructFieldInfo* fi = bitfieldOf(m);
    if (!fi) return;
    storeBitfieldAt(bitfieldBase(m), fi, val, isVolatile);
}

Operand EIRBuilder::loadBitfieldAt(const Operand& basePtr, const StructFieldInfo* fi, bool isVolatile) {

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
        // Sign-extend from the field's top bit in a signed type of the unit's width.
        TypePtr sty = fi->type && fi->type->isInt() && fi->type->bitWidth == fi->storageType->bitWidth ? fi->type
                    : fi->storageType->bitWidth == 64 ? Type::getI64() : fi->storageType->bitWidth == 32 ? Type::getI32()
                    : fi->storageType->bitWidth == 16 ? Type::getI16() : Type::getI8();
        unsigned sh = static_cast<unsigned>(fi->storageType->bitWidth) - fi->bitWidth;
        std::string shl = newVar();
        emit(Instruction{Opcode::Shl, shl, fi->storageType, {cur, Operand::constInt(static_cast<int64_t>(sh), fi->storageType)}});
        Operand asSigned = coerceNumeric(Operand::var(shl, fi->storageType), sty);
        std::string ashr = newVar();
        emit(Instruction{Opcode::AShr, ashr, sty, {asSigned, Operand::constInt(static_cast<int64_t>(sh), sty)}});
        cur = Operand::var(ashr, sty);
    }
    return coerceNumeric(coerceNumeric(cur, fi->type), bitfieldReadType(fi));
}

void EIRBuilder::storeBitfieldAt(const Operand& basePtr, const StructFieldInfo* fi, Operand val, bool isVolatile) {

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

    // The value as the field's type, then its bits in the unit's (unsigned) type.
    Operand valToStore = coerceNumeric(coerceNumeric(val, fi->type), fi->storageType);

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
    plainStructs.clear();
    unionDecls.clear();
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
            if (!e->typeParams.empty()) {
                genericEnums[e->name] = e;
                for (size_t i = 0; i < e->members.size(); ++i)
                    adtVariants[e->members[i].first] = {e->name, static_cast<int>(i), {}};
            } else if (e->isADT()) {
                adtEnums[e->name] = e;
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
                plainStructs[s->name] = s;
            }
        } else if (auto u = dynamic_cast<UnionDecl*>(decl.get())) {
            unionDecls[u->name] = u;
        } else if (auto f = dynamic_cast<FunctionDecl*>(decl.get())) {
            if (!f->typeParams.empty()) {
                if (!funcTemplateDecls.count(f->name) || f->body) funcTemplateDecls[f->name] = f;   // the definition, not a prototype
            }
        } else if (auto v = dynamic_cast<VarDecl*>(decl.get())) {
            if (v->isExtern) {
                continue;
            }
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
                        if (ty::CInt folded; foldCInt(v->initializer, folded)) {
                            int64_t cv = truncConst(folded.v, type);
                            if (v->isConst) {
                                enumConstants[v->name] = cv;
                                constTypes[v->name] = type;
                            }
                            initOp = Operand::constInt(cv, type);
                        } else {
                            initOp = Operand::constInt(0, type);
                        }
                    } else if (type && type->isFloat()) {
                        double fv = 0.0;
                        if (auto lit = dynamic_cast<LiteralExpr*>(v->initializer.get())) {
                            fv = std::strtod(lit->value.c_str(), nullptr);
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

    for (auto& decl : program->declarations) {
        auto e = dynamic_cast<EnumDecl*>(decl.get());
        if (e && adtEnums.count(e->name) && !module->structLayouts.count(e->name))
            layoutAdt(e->name, adtEnums[e->name], {});
    }
    for (auto& decl : program->declarations) {
        if (auto s = dynamic_cast<StructDecl*>(decl.get()); s && s->typeParams.empty())
            ensureStructLayout(s->name);
    }
    for (auto& decl : program->declarations) {
        if (auto u = dynamic_cast<UnionDecl*>(decl.get())) ensureStructLayout(u->name);
    }

    // Phase 2: Function prototypes
    definedFunctions.clear();
    for (auto& decl : program->declarations)
        if (auto f = dynamic_cast<FunctionDecl*>(decl.get()); f && f->typeParams.empty() && f->body)
            definedFunctions.insert(f->name);
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
            if (module->getFunction(f->name)) continue;
            auto fn = module->createFunction(f->name, toEirType(f->returnType), params);
            fn->isVariadic = hasVarargs;
        } else if (auto ext = dynamic_cast<ExternDecl*>(decl.get())) {
            if (module->getFunction(ext->name)) continue;
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
                    if (module->getFunction(s->name + "_" + mf->name)) continue;
                    module->createFunction(s->name + "_" + mf->name, toEirType(mf->returnType), params);
                }
            }
        }
    }

    bool hasGlobalInits = false;
    for (const auto& decl : program->declarations) {
        if (auto v = dynamic_cast<VarDecl*>(decl.get())) {
            if (!v->isExtern && (v->initializer || defaultExprFor(v->type))) {
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
                ExprPtr init = v->isExtern ? nullptr : (v->initializer ? v->initializer : defaultExprFor(v->type));
                if (init) {
                    TypePtr type = toEirType(v->type);
                    Operand target = Operand::global(v->name, Type::getPtr(type));
                    if (auto arrLit = dynamic_cast<ArrayLitExpr*>(init.get())) {
                        emitArrayInitInto(target, arrLit, type);
                    } else {
                        Operand val = evalForType(init, v->type);
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
        currentFunction = nullptr;   // a top-level declaration is outside any function
        currentBlock = nullptr;
        decl->accept(this);
    }
}

void EIRBuilder::visit(FunctionDecl* node) {
    if (!node->typeParams.empty()) {
        if (!funcTemplateDecls.count(node->name) || node->body) funcTemplateDecls[node->name] = node;   // the definition, not a prototype
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
        if (currentFunction->blocks.empty() && !definedFunctions.count(node->name)) currentFunction->isExtern = true;
        return;
    }
    if (!currentFunction->blocks.empty()) return;

    currentFunction->isExtern = false;
    // The definition names the parameters: an `extern` or prototype of the same function
    // seen first may have named them differently.
    for (size_t i = 0, k = 0; i < node->params.size() && k < currentFunction->params.size(); ++i) {
        if (node->params[i].first == "...") break;
        currentFunction->params[k++].first = node->params[i].second;
    }

    nextVarId = 0;
    nextBlockId = 0;
    BasicBlock* entryBB = currentFunction->createBlock("entry");
    setBlock(entryBB);

    auto savedCleanups = std::move(cleanupScopes);
    auto savedLoops = std::move(loopContextStack);
    auto savedUnwind = currentUnwindTarget;
    auto savedUnwindStack = std::move(unwindTargetStack);
    size_t savedHandlerDepth = currentHandlerDepth;
    cleanupScopes.clear();
    cleanupScopes.emplace_back(); // root function cleanup frame
    loopContextStack.clear();
    currentUnwindTarget = nullptr;
    unwindTargetStack.clear();
    currentHandlerDepth = 0;

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
    currentUnwindTarget = savedUnwind;
    unwindTargetStack = std::move(savedUnwindStack);
    currentHandlerDepth = savedHandlerDepth;
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

    size_t al = getTypeAlign(type, module.get());
    if (al < 1) al = 1;
    std::string slotName = "%" + node->name + ".addr";
    // A name declared again (a nested block, a second branch) gets its own slot.
    if (lookupVar(node->name) || functionSlots[currentFunction].count(slotName)) slotName = "%" + node->name + "." + std::to_string(nextVarId++) + ".addr";
    functionSlots[currentFunction].insert(slotName);
    // The slot lives in the entry block, so a jump past the declaration (to a later case
    // label) still finds it; the declaration itself sets it, to zero without an initializer.
    entryAlloca(slotName, type, al);

    // The initializer is evaluated before the name is declared: `int64 x = x + 1;` in a
    // nested block reads the outer x. Without one, a struct with field defaults takes them.
    // A variable whose type names a type parameter starts at zero in generic code.
    bool generic = !typeParamOverride.empty() && substType(node->type, typeParamOverride) != node->type;
    ExprPtr init = node->initializer ? node->initializer : (generic ? nullptr : defaultExprFor(node->type));
    if (init) {
        if (auto arrLit = dynamic_cast<ArrayLitExpr*>(init.get())) {
            emitArrayInitInto(Operand::var(slotName, Type::getPtr(type)), arrLit, type);
        } else {
            Operand val = evalForType(init, type);
            Instruction st{Opcode::Store, "", Type::getVoid(), {val, Operand::var(slotName, Type::getPtr(type))}};
            st.isVolatile = node->isVolatile;
            emit(st);
        }
    } else {
        emitZeroFill(Operand::var(slotName, Type::getPtr(type)), type);
    }
    declareVar(node->name, Operand::var(slotName, Type::getPtr(type)), type);
}

// An alloca at the top of the function's entry block.
void EIRBuilder::entryAlloca(const std::string& name, const TypePtr& type, size_t align) {
    BasicBlock* entry = currentFunction->blocks.front().get();
    auto& insts = entry->instructions;
    size_t at = 0;
    while (at < insts.size() && insts[at]->op == Opcode::Alloca) ++at;
    insts.insert(insts.begin() + static_cast<long>(at), std::make_unique<Instruction>(
        Opcode::Alloca, name, type, std::vector<Operand>{Operand::constInt(static_cast<int64_t>(align), Type::getI32())}));
}

// Zero the bytes of a `type` at `ptr`: a scalar store, or 8-byte stores then bytes.
void EIRBuilder::emitZeroFill(const Operand& ptr, const TypePtr& type) {
    if (!type) return;
    if (type->isInt() || type->isFloat() || type->isPointer()) {
        Operand zero = type->isFloat() ? Operand::constFloat(0.0, type) : type->isPointer() ? Operand::constNull() : Operand::constInt(0, type);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {zero, Operand::var(ptr.text, Type::getPtr(type))}});
        return;
    }
    size_t size = getTypeSize(type, module.get());
    if (type->isStruct()) { size_t ss = module->getStructSize(type->nominalName()); if (ss) size = ss; }
    if (size <= 256) {
        size_t off = 0;
        for (; off + 8 <= size; off += 8) {
            std::string p = newVar();
            emit(Instruction{Opcode::OffsetPtr, p, Type::getPtr(Type::getI64()), {ptr, Operand::constInt(static_cast<int64_t>(off), Type::getI64())}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getI64()), Operand::var(p, Type::getPtr(Type::getI64()))}});
        }
        for (; off < size; ++off) {
            std::string p = newVar();
            emit(Instruction{Opcode::OffsetPtr, p, Type::getPtr(Type::getU8()), {ptr, Operand::constInt(static_cast<int64_t>(off), Type::getI64())}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getU8()), Operand::var(p, Type::getPtr(Type::getU8()))}});
        }
        return;
    }
    // A large block: a loop over its 8-byte words (an aggregate's size is a multiple of 8
    // or padded to its alignment; the tail is zeroed bytewise).
    TypePtr i64 = Type::getI64();
    std::string idx = newVar() + ".z";
    entryAlloca(idx, i64, 8);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, i64), Operand::var(idx, Type::getPtr(i64))}});
    BasicBlock* loopBB = createBlock("zero.loop");
    BasicBlock* bodyBB = createBlock("zero.body");
    BasicBlock* doneBB = createBlock("zero.done");
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});
    setBlock(loopBB);
    std::string iv = newVar(), more = newVar();
    emit(Instruction{Opcode::Load, iv, i64, {Operand::var(idx, Type::getPtr(i64))}});
    emit(Instruction{Opcode::ICmp, more, Type::getI1(), {Operand::var(iv, i64), Operand::constInt(static_cast<int64_t>(size / 8), i64)}, ICmpCond::Slt});
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(more, Type::getI1()), Operand::block(bodyBB->name), Operand::block(doneBB->name)}});
    setBlock(bodyBB);
    std::string p = newVar(), next = newVar();
    emit(Instruction{Opcode::Gep, p, Type::getPtr(i64), {ptr, Operand::var(iv, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, i64), Operand::var(p, Type::getPtr(i64))}});
    emit(Instruction{Opcode::Add, next, i64, {Operand::var(iv, i64), Operand::constInt(1, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(next, i64), Operand::var(idx, Type::getPtr(i64))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});
    setBlock(doneBB);
    for (size_t off = size / 8 * 8; off < size; ++off) {
        std::string b = newVar();
        emit(Instruction{Opcode::OffsetPtr, b, Type::getPtr(Type::getU8()), {ptr, Operand::constInt(static_cast<int64_t>(off), Type::getI64())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getU8()), Operand::var(b, Type::getPtr(Type::getU8()))}});
    }
}

// The value a variable of type `t` starts with when its declaration has no initializer:
// `S{}` for a struct with field defaults (directly or in a nested struct), an array
// literal of those for an array of such structs, else null (zero). A generic instance and
// a union have no defaults (codegen's defaultExprFor).
ExprPtr EIRBuilder::defaultExprFor(const std::string& t0) {
    std::string t = tyq::strip(t0);
    auto al = typeAliases.find(t);
    if (al != typeAliases.end()) t = al->second;
    if (!typeParamOverride.empty()) t = substType(t, typeParamOverride);
    ty::Type pt = ty::Type::parse(t);
    if (pt.kind == ty::Type::Kind::Array && pt.elem) {
        ExprPtr e = defaultExprFor(pt.elem->str());
        if (!e) return nullptr;
        TypePtr at = toEirType(t);
        if (!at || !at->isArray() || at->arraySize == 0 || at->arraySize > 65536) return nullptr;
        return std::make_shared<ArrayLitExpr>(std::vector<ExprPtr>(static_cast<size_t>(at->arraySize), e));
    }
    if (pt.kind != ty::Type::Kind::Struct && pt.kind != ty::Type::Kind::Named) return nullptr;
    std::string sname = pt.nominalName();
    auto it = plainStructs.find(sname);
    if (it == plainStructs.end()) return nullptr;
    auto hit = structHasDefaults.find(sname);
    if (hit == structHasDefaults.end()) {
        structHasDefaults[sname] = -1;   // a recursive struct reaches itself only by pointer
        bool any = false;
        for (const auto& f : it->second->fields)
            if (f.defaultValue || defaultExprFor(f.type)) { any = true; break; }
        structHasDefaults[sname] = any ? 1 : 0;
        hit = structHasDefaults.find(sname);
    }
    if (hit->second != 1) return nullptr;
    return std::make_shared<StructInitExpr>(sname, std::vector<std::pair<std::string, ExprPtr>>{});
}

// A struct literal with its omitted fields given their defaults; null when the struct has
// none.
std::shared_ptr<StructInitExpr> EIRBuilder::withDefaults(StructInitExpr* init) {
    std::string sname = init->structName;
    if (!defaultExprFor(sname)) return nullptr;
    const auto& fields = plainStructs[sname]->fields;
    bool named = !init->fieldInits.empty() && !init->fieldInits[0].first.empty();
    std::vector<std::pair<std::string, ExprPtr>> all;
    std::set<std::string> given;
    for (size_t i = 0; i < init->fieldInits.size(); ++i) {
        std::string n = named ? init->fieldInits[i].first : (i < fields.size() ? fields[i].name : "");
        if (n.empty()) continue;
        given.insert(n);
        all.push_back({n, init->fieldInits[i].second});
    }
    for (const auto& f : fields) {
        if (given.count(f.name)) continue;
        ExprPtr d = f.defaultValue ? f.defaultValue : defaultExprFor(f.type);
        if (d) all.push_back({f.name, d});
    }
    auto out = std::make_shared<StructInitExpr>(init->structName, all);
    out->line = init->line;
    out->col = init->col;
    return out;
}

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

// The Eskiu spelling of an integer EIR type.
static std::string intSpelling(const TypePtr& t) {
    if (!t || !t->isInt()) return "";
    switch (t->bitWidth) {
        case 1: return "bool";
        case 8: return t->isSigned ? "int8" : "uint8";
        case 16: return t->isSigned ? "int16" : "uint16";
        case 64: return t->isSigned ? "int64" : "uint64";
        default: return t->isSigned ? "int" : "uint";
    }
}

void EIRBuilder::visit(ForStmt* node) {
    pushScope();
    if (auto* ib = dynamic_cast<BlockStmt*>(node->init.get())) {
        // `for (i in A..B)` inside a generic instance: the shared AST carries the type the
        // checker stamped for some instance, so the bounds' common integer type is derived
        // again for this one (as codegen does).
        if (!typeParamOverride.empty() && ib->items.size() == 2 &&
            std::holds_alternative<DeclPtr>(ib->items[0]) && std::holds_alternative<DeclPtr>(ib->items[1])) {
            auto* lo = dynamic_cast<VarDecl*>(std::get<DeclPtr>(ib->items[0]).get());
            auto* hi = dynamic_cast<VarDecl*>(std::get<DeclPtr>(ib->items[1]).get());
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
        if (auto* rhsOp = dynamic_cast<BinaryExpr*>(node->right.get());
            rhsOp && rhsOp->left.get() == node->left.get() && rhsOp->op != "=") {
            emitCompoundAssign(node, rhsOp);
            return;
        }
        if (auto* mem = dynamic_cast<MemberExpr*>(node->left.get())) {
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

        if (auto* mem = dynamic_cast<MemberExpr*>(node->left.get())) {
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

// `x + 1` / `x - 1` for ++/--: a pointer steps by one element, a float by 1.0, an integer
// at its promoted type, converted back to the operand's type.
Operand EIRBuilder::stepByOne(const Operand& cur, const TypePtr& ty, bool decrement) {
    if (ty && ty->isPointer()) {
        std::string dst = newVar();
        emit(Instruction{Opcode::Gep, dst, ty, {cur, Operand::constInt(decrement ? -1 : 1, Type::getI64())}});
        return Operand::var(dst, ty);
    }
    return compoundOp(decrement ? "-" : "+", cur, Operand::constInt(1, Type::getI32()), ty);
}

void EIRBuilder::visit(IncDecExpr* node) {
    if (auto* mem = dynamic_cast<MemberExpr*>(node->operand.get())) {
        std::string baseType = getExprEirType(mem->base)->nominalName();
        auto it = module->structLayouts.find(baseType);
        if (it != module->structLayouts.end()) {
            const StructFieldInfo* fi = it->second.getFieldInfo(mem->member);
            if (fi && fi->isBitfield) {
                // The new value is what the field holds after the store (wrapped to its
                // width); a postfix result keeps the field's declared type (clang).
                Operand base = bitfieldBase(mem);
                Operand cur = loadBitfieldAt(base, fi, volatileRooted(mem));
                Operand stored = coerceNumeric(stepByOne(cur, fi->type, node->decrement), fi->type);
                storeBitfieldAt(base, fi, stored, volatileRooted(mem));
                if (node->prefix) {
                    lastValue = coerceNumeric(loadBitfieldAt(base, fi, volatileRooted(mem)), cur.type);
                } else {
                    lastValue = coerceNumeric(cur, fi->type);
                }
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

    Operand next = stepByOne(Operand::var(cur, ty), ty, node->decrement);
    Instruction st{Opcode::Store, "", Type::getVoid(), {next, addr}};
    st.isVolatile = volatileRooted(node->operand.get());
    emit(st);

    lastValue = node->prefix ? next : Operand::var(cur, ty);
}

void EIRBuilder::visit(IdentExpr* node) {
    // A local or parameter shadows a constant, an enum member and a variant of that name.
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

    auto ecIt = enumConstants.find(node->name);
    if (ecIt != enumConstants.end()) {
        auto ctIt = constTypes.find(node->name);
        lastValue = Operand::constInt(ecIt->second, ctIt != constTypes.end() ? ctIt->second : Type::getI32());
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

    if (module && module->getFunction(node->name)) {
        TypePtr ty = getExprEirType(node);
        if (ty && ty->isClosure()) {
            std::string dst = newVar() + ".fn_closure";
            emit(Instruction{Opcode::MakeClosure, dst, ty, {closureTarget(Operand::global(node->name, Type::getPtr())), Operand::constNull()}});
            lastValue = Operand::var(dst, ty);
            lastLValue = lastValue;
            return;
        }
        lastLValue = Operand::global(node->name, Type::getPtr());
        lastValue = lastLValue;
        return;
    }

    TypePtr ty = getExprEirType(node);
    lastLValue = Operand::global(node->name, Type::getPtr(ty && !ty->isFunction() ? ty : nullptr));
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
            v = std::strtod(node->value.c_str(), nullptr);   // C: a denormal stays, too large is inf
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
        if (id->name == "exception_text" && node->args.empty() && !lookupVar(id->name) && !module->getFunction(id->name)) {
            // The text belongs to its catch block: formatted into the handler's own buffer.
            if (!catchTextSlots.empty()) {
                Operand buf = Operand::var(catchTextSlots.back(), Type::getPtr(Type::getU8()));
                emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("__exception_text_into", Type::getVoid()), buf}});
                lastValue = buf;
                return;
            }
            std::string resVar = newVar();
            emit(Instruction{Opcode::Call, resVar, Type::getPtr(Type::getU8()), {
                Operand::global("__eir_exception_text", Type::getPtr(Type::getU8()))
            }});
            lastValue = Operand::var(resVar, Type::getPtr(Type::getU8()));
            return;
        }
        if (node->dbgBuiltin && node->args.size() == 1) {
            lastValue = emitDbg(node);
            return;
        }
        if (id->name == "__cxa_free_exception" && node->args.size() == 1 && !lookupVar(id->name)) {
            emit(Instruction{Opcode::Call, "", Type::getVoid(), {
                Operand::global("__eir_free_exception", Type::getVoid()), evaluateExpr(node->args[0])
            }});
            lastValue = Operand::var("", Type::getVoid());
            return;
        }
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
        } else if (!module->getFunction(id->name) && isGlobalName(id->name)) {
            TypePtr gt = getExprEirType(node->callee);   // a global fn value
            if (gt && gt->isClosure()) {
                isClosureCall = true;
                closureOp = evaluateExpr(node->callee);
            }
        }
    } else {
        TypePtr cTy = getExprEirType(node->callee);
        if (cTy && cTy->isClosure()) {
            isClosureCall = true;
            closureOp = evaluateExpr(node->callee);
        }
    }

    if (isClosureCall) {
        std::vector<std::string> paramNominals;
        std::vector<TypePtr> paramTypes;
        if (resolvedExprTypes) {
            std::string calleeTy = semaType(node->callee.get());
            if (!calleeTy.empty()) {
                auto parsed = ty::Type::parse(calleeTy);
                if (parsed.kind == ty::Type::Kind::Fn) {
                    for (const auto& p : parsed.params) {
                        paramNominals.push_back(p.nominalName());
                        paramTypes.push_back(toEirType(p.str()));
                    }
                }
            }
        }
        std::vector<Operand> args;
        args.push_back(closureOp);
        for (size_t i = 0; i < node->args.size(); ++i) {
            std::string pName = (i < paramNominals.size()) ? paramNominals[i] : "";
            Operand a = evalForType(node->args[i], pName);
            if (i < paramTypes.size()) a = coerceNumeric(a, paramTypes[i]);
            args.push_back(a);
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
        lastLValue = (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isInterface() || retTy->isClosure() || retTy->isSlice())) ? Operand::var(dst, Type::getPtr(retTy)) : lastValue;
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
                Operand argVal = evalForType(node->args[p], pt);
                emit(Instruction{Opcode::Store, "", Type::getVoid(), {argVal, Operand::var(payPtr, Type::getPtr(pt))}});
                payOffset += getTypeSize(pt, module.get());
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
                std::string st = semaType(arg.get());
                return st.empty() ? eskiuSpelling(getExprEirType(arg)) : st;
            };
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                if (!tps.count(tyq::strip(fd->params[j].first))) {
                    unifyTypeParam(fd->params[j].first, getArgTypeStr(node->args[j]), tps, subs);
                }
            }
            std::set<std::string> fromComposite;   // bound by `*T`, `List<T>*`: those win
            for (const auto& kv : subs) fromComposite.insert(kv.first);
            for (size_t j = 0; j < fd->params.size() && j < node->args.size(); ++j) {
                std::string tp = tyq::strip(fd->params[j].first);
                if (!tps.count(tp) || fromComposite.count(tp)) continue;
                std::string at = getArgTypeStr(node->args[j]);
                if (at.empty() || at == "null") continue;
                if (!subs.count(tp)) {
                    unifyTypeParam(fd->params[j].first, at, tps, subs);
                } else if (subs[tp] != at) {
                    // By-value `T a, T b` deductions of different integer types meet at
                    // their common type (ty::rangeVarType), as sema infers.
                    std::string common = ty::rangeVarType(tyq::strip(subs[tp]), tyq::strip(at));
                    if (!common.empty()) subs[tp] = common;
                }
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
            if (targetFn && targetFn->isExtern && paramType && paramType->isClosure()) {
                args.push_back(cFnPointer(node->args[i]));
                continue;
            }
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
            std::string bt = semaType(mem->base.get());
            if (!bt.empty()) baseTypeName = ty::Type::parse(tyq::strip(bt)).nominalName();
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
            lastLValue = (retTy && (retTy->isStruct() || retTy->isArray() || retTy->isInterface() || retTy->isClosure() || retTy->isSlice())) ? Operand::var(dst, Type::getPtr(retTy)) : lastValue;
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
                at = semaType(node->args[j - 1].get());
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

        // A constrained call on a primitive (`t.cmp(x)` with T = int) is the free function
        // named like the method, taking the primitive first.
        if (baseTy && (baseTy->isInt() || baseTy->isFloat()) && !module->getFunction(baseTypeName + "_" + mem->member)) {
            if (Function* free = module->getFunction(mem->member)) {
                fnName = free->name;
                args.push_back(Operand::global(fnName, Type::getPtr()));
                args.push_back(free->params.empty() ? evaluateExpr(mem->base) : evalForType(mem->base, free->params[0].second));
                for (size_t i = 0; i < node->args.size(); ++i) {
                    TypePtr pt = i + 1 < free->params.size() ? free->params[i + 1].second : nullptr;
                    args.push_back(evalForType(node->args[i], pt));
                }
                TypePtr retTy = free->returnType;
                std::string dst = retTy->isVoid() ? "" : newVar();
                emit(Instruction{Opcode::Call, dst, retTy, args});
                lastValue = dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, retTy);
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
    TypePtr toTy = toEirType(node->targetType);
    // `(*void)f` of a top-level function is its C function pointer, not a closure.
    if (auto* id = dynamic_cast<IdentExpr*>(node->expr.get());
        id && toTy->isPointer() && !lookupVar(id->name) && module->getFunction(id->name)) {
        lastValue = Operand::global(id->name, toTy);
        return;
    }
    Operand val = evaluateExpr(node->expr);

    if (val.kind == OperandKind::ConstantNull) {
        lastValue = Operand::constNull();
        lastValue.type = toTy;
        return;
    }
    if (toTy->isInt() && toTy->bitWidth == 1) {   // to bool: `!= 0` (C _Bool), a pointer too
        lastValue = truth(val);
        return;
    }
    bool fromNum = val.type && (val.type->isInt() || val.type->isFloat());
    if (fromNum && (toTy->isInt() || toTy->isFloat())) {
        lastValue = coerceNumeric(val, toTy);
        return;
    }

    std::string dst = newVar();
    if (val.type->isPointer() && toTy->isPointer()) {
        emit(Instruction{Opcode::Bitcast, dst, toTy, {val}});
    } else if (val.type->isPointer() && toTy->isInt()) {
        emit(Instruction{Opcode::PtrToInt, dst, toTy, {val}});
    } else if (val.type->isInt() && toTy->isPointer()) {
        emit(Instruction{Opcode::IntToPtr, dst, toTy, {val}});
    } else {
        lastValue = val;
        lastValue.type = toTy;
        return;
    }
    lastValue = Operand::var(dst, toTy);
}

void EIRBuilder::visit(TernaryExpr* node) {
    Operand cond = evaluateExpr(node->condition);
    cond = truth(cond);

    BasicBlock* thenBB = createBlock("ternary.then");
    BasicBlock* elseBB = createBlock("ternary.else");
    BasicBlock* mergeBB = createBlock("ternary.merge");

    TypePtr resTy = getExprEirType(node);
    std::string resultSlot = newVar() + ".ternary";
    size_t al = (resTy->bitWidth <= 32 && !resTy->isInterface() && !resTy->isClosure() && !resTy->isSlice() && !resTy->isStruct()) ? 4 : 8;
    emit(Instruction{Opcode::Alloca, resultSlot, resTy, {Operand::constInt(al, Type::getI32())}});

    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {cond, Operand::block(thenBB->name), Operand::block(elseBB->name)}});

    setBlock(thenBB);
    Operand trueVal = evalForType(node->thenExpr, resTy);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {trueVal, Operand::var(resultSlot, Type::getPtr(resTy))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});

    setBlock(elseBB);
    Operand falseVal = evalForType(node->elseExpr, resTy);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {falseVal, Operand::var(resultSlot, Type::getPtr(resTy))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(mergeBB->name)}});

    setBlock(mergeBB);
    std::string dst = newVar();
    emit(Instruction{Opcode::Load, dst, resTy, {Operand::var(resultSlot, Type::getPtr(resTy))}});
    lastValue = Operand::var(dst, resTy);
    lastLValue = Operand::var(resultSlot, Type::getPtr(resTy));   // `(c ? a : b).x` reads the chosen arm
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
        std::string semaBase = semaType(node->base.get());
        if (!semaBase.empty()) {
            std::string st = tyq::strip(semaBase);
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
    bool variadic = false;
    for (const auto& p : fd->params) {
        std::string subPType = substType(p.first, subs);
        params.emplace_back(subPType, p.second);
        if (p.first == "...") { variadic = true; continue; }
        eirParams.emplace_back(p.second, toEirType(subPType));
    }
    module->createFunction(mangledName, toEirType(retType), eirParams)->isVariadic = variadic;

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
            std::string semaE = semaType(e.get());
            if (!semaE.empty()) {
                std::string st = tyq::strip(semaE);
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
        emit(Instruction{Opcode::MakeClosure, dst, targetType, {closureTarget(val), Operand::constNull()}});
        return Operand::var(dst, targetType);
    }

    return coerceNumeric(evaluateExpr(e), targetType);
}

// A value as a value of type `to`: integers widen by the source's signedness and
// truncate, a constant is folded to the target type, numbers cross between integer and
// float, anything becomes a `bool` by comparing with zero. Non-numeric values pass through.
Operand EIRBuilder::coerceNumeric(const Operand& v, const TypePtr& to) {
    if (!to || !(to->isInt() || to->isFloat())) return v;
    TypePtr from = v.type;
    bool toBool = to->isInt() && to->bitWidth == 1;
    if (v.kind == OperandKind::ConstantNull) {
        if (toBool) return Operand::constInt(0, to);
        return v;
    }
    if (!from || !(from->isInt() || from->isFloat())) {
        if (toBool && from && from->isPointer()) {
            std::string dst = newVar();
            emit(Instruction{Opcode::ICmp, dst, to, {v, Operand::constNull()}, ICmpCond::Ne});
            return Operand::var(dst, to);
        }
        return v;
    }
    if (v.kind == OperandKind::ConstantInt) {
        if (to->isFloat()) {
            double d = from->isSigned ? static_cast<double>(v.intVal) : static_cast<double>(static_cast<uint64_t>(v.intVal));
            return Operand::constFloat(d, to);
        }
        return Operand::constInt(toBool ? (v.intVal != 0) : truncConst(v.intVal, to), to);
    }
    if (v.kind == OperandKind::ConstantFloat) {
        if (to->isFloat()) return Operand::constFloat(to->bitWidth == 32 ? static_cast<double>(static_cast<float>(v.floatVal)) : v.floatVal, to);
        if (toBool) return Operand::constInt(v.floatVal != 0.0, to);
        return Operand::constInt(saturateToInt(v.floatVal, to), to);
    }
    if (from->kind == to->kind && from->bitWidth == to->bitWidth && (to->isFloat() || from->isSigned == to->isSigned)) return v;
    std::string dst = newVar();
    if (toBool) {
        if (from->isFloat()) {
            emit(Instruction{Opcode::FCmp, dst, to, {v, Operand::constFloat(0.0, from)}, ICmpCond::Eq, FCmpCond::One});
        } else {
            emit(Instruction{Opcode::ICmp, dst, to, {v, Operand::constInt(0, from)}, ICmpCond::Ne});
        }
        return Operand::var(dst, to);
    }
    Opcode op;
    if (from->isInt() && to->isFloat()) op = Opcode::SIToFP;
    else if (from->isFloat() && to->isInt()) op = Opcode::FPToSI;
    else if (from->isFloat()) op = to->bitWidth < from->bitWidth ? Opcode::FPTrunc : Opcode::FPExt;
    else if (to->bitWidth < from->bitWidth) op = Opcode::Trunc;
    else if (to->bitWidth > from->bitWidth) op = from->isSigned ? Opcode::SExt : Opcode::ZExt;
    else op = Opcode::Trunc;   // same width, other signedness: re-extended as the target
    emit(Instruction{op, dst, to, {v}});
    return Operand::var(dst, to);
}

// An integer constant as a value of `t` (wrapped to its width, extended by its signedness).
int64_t EIRBuilder::truncConst(int64_t v, const TypePtr& t) {
    if (!t || !t->isInt()) return v;
    switch (t->bitWidth) {
        case 1: return v & 1;
        case 8: return t->isSigned ? static_cast<int64_t>(static_cast<int8_t>(v)) : static_cast<int64_t>(static_cast<uint8_t>(v));
        case 16: return t->isSigned ? static_cast<int64_t>(static_cast<int16_t>(v)) : static_cast<int64_t>(static_cast<uint16_t>(v));
        case 32: return t->isSigned ? static_cast<int64_t>(static_cast<int32_t>(v)) : static_cast<int64_t>(static_cast<uint32_t>(v));
        default: return v;
    }
}

// A float converted to the integer type `t`: saturating, NaN to 0.
int64_t EIRBuilder::saturateToInt(double f, const TypePtr& t) {
    if (f != f) return 0;
    unsigned bw = t->bitWidth <= 1 ? 1 : t->bitWidth;
    double lo = t->isSigned ? -std::ldexp(1.0, bw - 1) : 0.0;
    double hi = t->isSigned ? std::ldexp(1.0, bw - 1) : std::ldexp(1.0, bw);
    if (f <= lo) return t->isSigned ? static_cast<int64_t>(lo) : 0;
    if (f >= hi) return t->isSigned ? static_cast<int64_t>(hi - 1) : static_cast<int64_t>(static_cast<uint64_t>(hi - 1));
    return t->isSigned ? static_cast<int64_t>(f) : static_cast<int64_t>(static_cast<uint64_t>(f));
}

// `cur op rhs` for a compound assignment, computed as the binary operator would (promoted,
// at the common type; a shift at the promoted left type) and converted back to `curTy`.
Operand EIRBuilder::compoundOp(const std::string& baseOp, const Operand& cur, const Operand& rhsIn, const TypePtr& curTy) {
    Operand l = promoteInt(cur);
    Operand r = promoteInt(rhsIn);
    bool numeric = l.type && (l.type->isInt() || l.type->isFloat()) && r.type && (r.type->isInt() || r.type->isFloat());
    if (curTy && curTy->isPointer() && (baseOp == "+" || baseOp == "-")) {
        Operand idx = coerceNumeric(r, Type::getI64());
        if (baseOp == "-") {
            std::string neg = newVar();
            emit(Instruction{Opcode::Sub, neg, Type::getI64(), {Operand::constInt(0, Type::getI64()), idx}});
            idx = Operand::var(neg, Type::getI64());
        }
        std::string dst = newVar();
        emit(Instruction{Opcode::Gep, dst, curTy, {cur, idx}});
        return Operand::var(dst, curTy);
    }
    TypePtr t = (baseOp == "<<" || baseOp == ">>" || !numeric) ? l.type : commonType(l.type, r.type);
    l = coerceNumeric(l, t);
    r = coerceNumeric(r, t);
    bool f = t && t->isFloat(), sg = t && t->isSigned;
    Opcode opc = Opcode::Add;
    if (baseOp == "+") opc = f ? Opcode::FAdd : Opcode::Add;
    else if (baseOp == "-") opc = f ? Opcode::FSub : Opcode::Sub;
    else if (baseOp == "*") opc = f ? Opcode::FMul : Opcode::Mul;
    else if (baseOp == "/") opc = f ? Opcode::FDiv : (sg ? Opcode::SDiv : Opcode::UDiv);
    else if (baseOp == "%") opc = sg ? Opcode::SRem : Opcode::URem;
    else if (baseOp == "&") opc = Opcode::And;
    else if (baseOp == "|") opc = Opcode::Or;
    else if (baseOp == "^") opc = Opcode::Xor;
    else if (baseOp == "<<") opc = Opcode::Shl;
    else if (baseOp == ">>") opc = sg ? Opcode::AShr : Opcode::LShr;
    std::string dst = newVar();
    emit(Instruction{opc, dst, t, {l, r}});
    return coerceNumeric(Operand::var(dst, t), curTy);
}

// A condition as an i1: a number or pointer compared with zero, an interface or closure
// by its first word.
Operand EIRBuilder::truth(const Operand& v) {
    if (v.type && v.type->isInt() && v.type->bitWidth == 1) return v;
    if (v.type && (v.type->isInt() || v.type->isFloat())) return coerceNumeric(v, Type::getI1());
    std::string dst = newVar();
    emit(Instruction{Opcode::ICmp, dst, Type::getI1(), {v, Operand::constNull()}, ICmpCond::Ne});
    return Operand::var(dst, Type::getI1());
}

// The type a bitfield reads as: int when every value fits one (C promotion), else its type.
TypePtr EIRBuilder::bitfieldReadType(const StructFieldInfo* fi) {
    TypePtr t = fi->type;
    if (t && t->isInt() && t->bitWidth >= 32 &&
        ((!t->isSigned && fi->bitWidth < 32) || (t->isSigned && t->bitWidth == 64 && fi->bitWidth <= 32)))
        return Type::getI32();
    return t;
}

// `lv op= v` (as `lv = lv op v` with the lvalue node shared): the lvalue's address (or a
// bitfield's struct) once, then `*p = *p op v` with the operator's usual conversions, or
// the overloaded operator's function.
void EIRBuilder::emitCompoundAssign(BinaryExpr* node, BinaryExpr* rhsOp) {
    const std::string& op = rhsOp->op;
    if (auto* mem = dynamic_cast<MemberExpr*>(node->left.get())) {
        if (const StructFieldInfo* fi = bitfieldOf(mem)) {
            Operand base = bitfieldBase(mem);
            Operand cur = loadBitfieldAt(base, fi, volatileRooted(mem));
            Operand rhs = evaluateExpr(rhsOp->right);
            Operand res = compoundOp(op, cur, rhs, fi->type);
            storeBitfieldAt(base, fi, res, volatileRooted(mem));
            lastValue = coerceNumeric(res, cur.type);
            return;
        }
    }
    Operand lhsAddr = evaluateLValue(node->left);
    TypePtr ty = getExprEirType(node->left);
    std::string curVal = newVar();
    Instruction ld{Opcode::Load, curVal, ty, {lhsAddr}};
    ld.isVolatile = volatileRooted(node->left.get());
    emit(ld);
    Operand cur = Operand::var(curVal, ty);
    Operand res;
    if (!rhsOp->opFunc.empty()) {
        Function* f = module->getFunction(rhsOp->opFunc);
        TypePtr pt = f && f->params.size() > 1 ? f->params[1].second : nullptr;
        Operand rhs = pt ? evalForType(rhsOp->right, pt) : evaluateExpr(rhsOp->right);
        TypePtr rt = f ? f->returnType : ty;
        std::string dst = newVar();
        emit(Instruction{Opcode::Call, dst, rt, {Operand::global(rhsOp->opFunc, Type::getPtr()), cur, rhs}});
        res = coerceNumeric(Operand::var(dst, rt), ty);
    } else {
        res = compoundOp(op, cur, evaluateExpr(rhsOp->right), ty);
    }
    Instruction st{Opcode::Store, "", Type::getVoid(), {res, lhsAddr}};
    st.isVolatile = volatileRooted(node->left.get());
    emit(st);
    lastValue = res;
}

// C's integer promotion: an integer narrower than `int` becomes an `int`.
Operand EIRBuilder::promoteInt(const Operand& v) {
    if (v.type && v.type->isInt() && v.type->bitWidth < 32) return coerceNumeric(v, Type::getI32());
    return v;
}

// The type two promoted numeric operands meet at (C's usual arithmetic conversions):
// a float wins (double over float); else the wider integer, and unsigned at equal width.
TypePtr EIRBuilder::commonType(const TypePtr& a, const TypePtr& b) {
    if (a && a->isFloat() && b && b->isFloat()) return a->bitWidth >= b->bitWidth ? a : b;
    if (a && a->isFloat()) return a;
    if (b && b->isFloat()) return b;
    if (!a || !a->isInt()) return b;
    if (!b || !b->isInt()) return a;
    if (a->bitWidth != b->bitWidth) return a->bitWidth > b->bitWidth ? a : b;
    return a->isSigned ? b : a;
}

// The Eskiu spelling of an EIR type, as an exception's type name: `int`, `double`,
// `string` (a pointer to bytes), a struct's (instance) name.
std::string EIRBuilder::eskiuSpelling(const TypePtr& t) {
    if (!t) return "";
    if (t->isInt()) return intSpelling(t);
    if (t->isFloat()) return t->bitWidth == 32 ? "float" : "double";
    if (t->isPointer()) return t->pointee && t->pointee->isInt() && t->pointee->bitWidth == 8 ? "string" : "*void";
    return t->nominalName();
}

// In a generic instance, the overloaded operator for these operand types (sema resolves
// one per instance but cannot stamp the shared AST), or "" for a built-in operation.
std::string EIRBuilder::instanceOperator(const std::string& op, const std::vector<Operand>& ops) {
    if (typeParamOverride.empty()) return "";
    bool nominal = false;
    std::vector<std::string> tys;
    for (const auto& o : ops) {
        if (!o.type || o.kind == OperandKind::ConstantNull) return "";
        if (!(o.type->isInt() || o.type->isFloat())) nominal = true;
        if (o.type->isPointer()) return "";
        tys.push_back(eskiuSpelling(o.type));
    }
    if (!nominal) return "";
    std::string name = eskiuOpName(op, tys);
    if (module->getFunction(name)) return name;
    // An overload is named from its declared spellings (`Vec<int>`), an operand here by its
    // instance (`Vec_int`): retry with each instance's source form.
    for (auto& t : tys) {
        auto it = templateInstanceArgs.find(t);
        if (it == templateInstanceArgs.end()) continue;
        std::string src = it->second.first + "<";
        for (size_t i = 0; i < it->second.second.size(); ++i) src += (i ? "," : "") + it->second.second[i];
        t = src + ">";
    }
    name = eskiuOpName(op, tys);
    return module->getFunction(name) ? name : "";
}

Operand EIRBuilder::callOperator(const std::string& fn, const std::vector<Operand>& args) {
    Function* f = module->getFunction(fn);
    std::vector<Operand> ops{Operand::global(fn, Type::getPtr())};
    for (size_t i = 0; i < args.size(); ++i)
        ops.push_back(f && i < f->params.size() ? coerceNumeric(args[i], f->params[i].second) : args[i]);
    TypePtr rt = f ? f->returnType : Type::getI32();
    std::string dst = rt->isVoid() ? "" : newVar();
    emit(Instruction{Opcode::Call, dst, rt, ops});
    return dst.empty() ? Operand::var("", Type::getVoid()) : Operand::var(dst, rt);
}

// The checker's type spelling for an expression: in a generic instance the one it had in
// this instance (else the shared record with the type parameters substituted); "" if none.
std::string EIRBuilder::semaType(const Expr* e) {
    if (!e) return "";
    if (!typeParamOverride.empty() && instanceExprTypes) {
        auto it = instanceExprTypes->find(const_cast<Expr*>(e));
        if (it != instanceExprTypes->end()) {
            for (const auto& [subs, t] : it->second) {
                if (subs == typeParamOverride) return t;
            }
        }
    }
    if (resolvedExprTypes) {
        auto it = resolvedExprTypes->find(const_cast<Expr*>(e));
        if (it != resolvedExprTypes->end())
            return typeParamOverride.empty() ? it->second : substType(it->second, typeParamOverride);
    }
    return "";
}

// `c ? a : b` for pointers, through a slot.
Operand EIRBuilder::selectPtr(const Operand& c, const Operand& a, const Operand& b) {
    std::string slot = newVar() + ".sel";
    entryAlloca(slot, Type::getPtr(), 8);
    BasicBlock* yes = createBlock("sel.a");
    BasicBlock* no = createBlock("sel.b");
    BasicBlock* done = createBlock("sel.done");
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {c, Operand::block(yes->name), Operand::block(no->name)}});
    setBlock(yes);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {a, Operand::var(slot, Type::getPtr(Type::getPtr()))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(done->name)}});
    setBlock(no);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {b, Operand::var(slot, Type::getPtr(Type::getPtr()))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(done->name)}});
    setBlock(done);
    std::string v = newVar();
    emit(Instruction{Opcode::Load, v, Type::getPtr(), {Operand::var(slot, Type::getPtr(Type::getPtr()))}});
    return Operand::var(v, Type::getPtr());
}

// The format and arguments `dbg` prints a value of type `t` at `addr` with, as codegen's
// dbgFormat: integers, bool, char, floats, strings (quoted, or null), arrays up to 16
// elements, structs by field (4 levels), a plain enum as `E(n)`, a pointer as %p, anything
// else as `<T>`.
void EIRBuilder::dbgFormat(const std::string& t0, const Operand& addr, std::string& fmt, std::vector<Operand>& args, int depth) {
    std::string t = tyq::strip(t0);
    auto al = typeAliases.find(t);
    if (al != typeAliases.end()) t = tyq::strip(al->second);
    std::string key = t.rfind("struct:", 0) == 0 ? t.substr(7) : t;
    auto lit = [&](const std::string& x) { for (char c : x) fmt += c == '%' ? std::string("%%") : std::string(1, c); };
    TypePtr ty = toEirType(t);
    static const std::set<std::string> sints = {"int", "int8", "int16", "int32", "int64"};
    static const std::set<std::string> uints = {"uint", "uint8", "uint16", "uint32", "uint64"};
    ty::Type pt = ty::Type::parse(t);
    auto load = [&](const TypePtr& lt) {
        std::string v = newVar();
        emit(Instruction{Opcode::Load, v, lt, {addr}});
        return Operand::var(v, lt);
    };
    if (sints.count(t) || uints.count(t)) {
        args.push_back(coerceNumeric(load(ty), sints.count(t) ? Type::getI64() : Type::getU64()));
        fmt += sints.count(t) ? "%lld" : "%llu";
    } else if (t == "bool") {
        args.push_back(selectPtr(load(ty), Operand::constString("true"), Operand::constString("false")));
        fmt += "%s";
    } else if (t == "char") {
        args.push_back(coerceNumeric(load(ty), Type::getI32()));
        fmt += "'%c'";
    } else if (t == "float" || t == "double") {
        args.push_back(coerceNumeric(load(ty), Type::getF64()));
        fmt += "%g";
    } else if (t == "string") {
        Operand v = load(ty);
        std::string isNull = newVar();
        emit(Instruction{Opcode::ICmp, isNull, Type::getI1(), {v, Operand::constNull()}, ICmpCond::Eq});
        Operand c = Operand::var(isNull, Type::getI1());
        Operand q = selectPtr(c, Operand::constString(""), Operand::constString("\""));
        args.push_back(q);
        args.push_back(selectPtr(c, Operand::constString("null"), v));
        args.push_back(q);
        fmt += "%s%s%s";
    } else if (pt.kind == ty::Type::Kind::Array && pt.elem && ty && ty->isArray()) {
        fmt += "[";
        for (uint64_t i = 0; i < ty->arraySize && i < 16; ++i) {
            if (i) fmt += ", ";
            std::string ep = newVar();
            emit(Instruction{Opcode::Gep, ep, Type::getPtr(ty->elemType), {addr, Operand::constInt(static_cast<int64_t>(i), Type::getI64())}});
            if (depth < 4) dbgFormat(pt.elem->str(), Operand::var(ep, Type::getPtr(ty->elemType)), fmt, args, depth + 1);
            else fmt += "...";
        }
        if (ty->arraySize > 16) fmt += ", ...";
        fmt += "]";
    } else if (StructDecl* sd = plainStructs.count(key) ? plainStructs[key] : nullptr;
               (sd && !unionDecls.count(key)) || (templateInstanceArgs.count(key) && templateDecls.count(templateInstanceArgs[key].first))) {
        std::map<std::string, std::string> subs;
        if (!sd) {
            const auto& ia = templateInstanceArgs[key];
            sd = templateDecls[ia.first];
            for (size_t i = 0; i < sd->typeParams.size() && i < ia.second.size(); ++i) subs[sd->typeParams[i]] = ia.second[i];
        }
        bool bits = false;
        for (const auto& f : sd->fields) if (f.bitWidth != 0) bits = true;
        if (bits || !sd->pads.empty()) {
            fmt += "<"; lit(key); fmt += ">";
            return;
        }
        ensureStructLayout(key);
        lit(key);
        fmt += "{";
        for (size_t i = 0; i < sd->fields.size(); ++i) {
            if (i) fmt += ", ";
            lit(sd->fields[i].name);
            fmt += ": ";
            std::string fts = subs.empty() ? sd->fields[i].type : substType(sd->fields[i].type, subs);
            TypePtr ft = toEirType(fts);
            std::string fp = newVar();
            emit(Instruction{Opcode::FieldPtr, fp, Type::getPtr(ft), {Operand::var(key, nullptr), addr, Operand::constString(sd->fields[i].name)}});
            if (depth < 4) dbgFormat(fts, Operand::var(fp, Type::getPtr(ft)), fmt, args, depth + 1);
            else fmt += "...";
        }
        fmt += "}";
    } else if (ty && ty->isPointer()) {
        args.push_back(load(ty));
        fmt += "%p";
    } else if (plainEnums.count(key)) {
        lit(key);
        fmt += "(%lld)";
        args.push_back(coerceNumeric(load(Type::getI32()), Type::getI64()));
    } else {
        fmt += "<";
        lit(key);
        fmt += ">";
    }
}

// `dbg(e)`: prints `[dbg L:C] <source of e> = <value>` to stderr and yields e's value.
Operand EIRBuilder::emitDbg(CallExpr* node) {
    std::string t = semaType(node->args[0].get());
    TypePtr ty = toEirType(t);
    Operand v = evalForType(node->args[0], ty);
    std::string slot = newVar() + ".dbg";
    entryAlloca(slot, ty, 8);
    Operand addr = Operand::var(slot, Type::getPtr(ty));
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {v, addr}});
    std::string fmt;
    for (char c : "[dbg " + std::to_string(node->callee->line) + ":" + std::to_string(node->callee->col) + "] " + node->dbgText + " = ")
        fmt += c == '%' ? std::string("%%") : std::string(1, c);
    std::vector<Operand> args;
    dbgFormat(t, addr, fmt, args, 0);
    fmt += "\n";

    std::string triple = targetTriple;
#if defined(__APPLE__)
    if (triple.empty()) triple = "darwin";
#elif defined(_WIN32)
    if (triple.empty()) triple = "windows";
#endif
    Operand err;
    if (triple.find("windows") != std::string::npos || triple.find("mingw") != std::string::npos) {
        std::string e = newVar();
        emit(Instruction{Opcode::Call, e, Type::getPtr(), {Operand::global("__acrt_iob_func", Type::getPtr()), Operand::constInt(2, Type::getI32())}});
        err = Operand::var(e, Type::getPtr());
    } else {
        bool darwin = triple.find("darwin") != std::string::npos || triple.find("apple") != std::string::npos ||
                      triple.find("macos") != std::string::npos || triple.find("ios") != std::string::npos;
        std::string e = newVar();
        emit(Instruction{Opcode::Load, e, Type::getPtr(), {Operand::global(darwin ? "__stderrp" : "stderr", Type::getPtr(Type::getPtr()))}});
        err = Operand::var(e, Type::getPtr());
    }
    if (!module->getFunction("fprintf")) {
        Function* f = module->createFunction("fprintf", Type::getI32(), {{"stream", Type::getPtr()}, {"fmt", Type::getPtr(Type::getU8())}});
        f->isExtern = true;
        f->isVariadic = true;
    }
    std::vector<Operand> callArgs{Operand::global("fprintf", Type::getPtr()), err, Operand::constString(fmt)};
    callArgs.insert(callArgs.end(), args.begin(), args.end());
    emit(Instruction{Opcode::Call, newVar(), Type::getI32(), callArgs});
    std::string r = newVar();
    emit(Instruction{Opcode::Load, r, ty, {addr}});
    return Operand::var(r, ty);
}

bool EIRBuilder::isGlobalName(const std::string& name) const {
    for (const auto& g : module->globals) if (g.name == name) return true;
    return false;
}

// A closure's function always takes the env first. A top-level function used as a
// closure is reached through `__fnptr_<name>`, which drops the env and forwards the rest.
Operand EIRBuilder::closureTarget(const Operand& fn) {
    if (fn.kind != OperandKind::GlobalRef || !module) return fn;
    Function* target = module->getFunction(fn.text);
    if (!target || (!target->params.empty() && target->params[0].first == "env")) return fn;

    std::string thunkName = "__fnptr_" + fn.text;
    if (!module->getFunction(thunkName)) {
        std::vector<std::pair<std::string, TypePtr>> params;
        params.emplace_back("env", Type::getPtr());
        for (const auto& p : target->params) params.push_back(p);
        Function* thunk = module->createFunction(thunkName, target->returnType, params);
        BasicBlock* entry = thunk->createBlock("entry");
        std::vector<Operand> args{Operand::global(target->name, Type::getPtr())};
        for (const auto& p : target->params) args.push_back(Operand::var("%" + p.first, p.second));
        bool isVoid = !target->returnType || target->returnType->isVoid();
        std::string res = isVoid ? "" : "%res";
        entry->instructions.push_back(std::make_unique<Instruction>(Opcode::Call, res, target->returnType, args));
        std::vector<Operand> retOps;
        if (!isVoid) retOps.push_back(Operand::var(res, target->returnType));
        entry->instructions.push_back(std::make_unique<Instruction>(Opcode::Ret, "", target->returnType, retOps));
    }
    return Operand::global(thunkName, Type::getPtr());
}

// An extern's fn-typed parameter is a C function pointer: a top-level function by name
// or `null` (sema rejects anything else).
Operand EIRBuilder::cFnPointer(const ExprPtr& e) {
    if (auto* lit = dynamic_cast<LiteralExpr*>(e.get()); lit && lit->kind == LiteralExpr::Kind::NULL_VAL) {
        return Operand::constNull();
    }
    if (auto* id = dynamic_cast<IdentExpr*>(e.get()); id && module && module->getFunction(id->name) && !lookupVar(id->name)) {
        return Operand::global(id->name, Type::getPtr());
    }
    throw std::runtime_error("EIR: an extern's function parameter takes a top-level function or null");
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
// `for (x in it)` as codegen lowers it: a counted for loop (so `continue` reaches the
// step and the label carries over) over a fixed-size array, a slice, or a List-like struct
// with `data` and `size` fields (also through a pointer). The iterable is evaluated once:
// anything but a stable place is held in a local (a List-like value with an address by a
// pointer to it).
void EIRBuilder::visit(ForInStmt* node) {
    std::string idxName = "__forin_i_" + std::to_string(forInCounter++);
    auto idx = [&]() -> ExprPtr { return std::make_shared<IdentExpr>(idxName); };
    auto intLit = [](const std::string& v) { return std::make_shared<LiteralExpr>(LiteralExpr::Kind::INT, v); };

    std::string itType = node->resolvedIterType;
    if (!typeParamOverride.empty()) itType = substType(itType, typeParamOverride);
    if (!typeParamOverride.empty() || itType.empty()) {
        std::string st = semaType(node->iterable.get());
        if (!st.empty()) itType = st;
    }
    itType = tyq::strip(itType);
    auto al = typeAliases.find(itType);
    if (al != typeAliases.end()) itType = al->second;

    ExprPtr iterable = node->iterable;
    std::shared_ptr<VarDecl> holdDecl;
    ty::Type k = ty::Type::parse(itType);
    if (!astwalk::isStablePlace(node->iterable.get())) {
        std::function<bool(Expr*)> addressable = [&](Expr* e) -> bool {
            if (dynamic_cast<IdentExpr*>(e)) return true;
            if (auto* u = dynamic_cast<UnaryExpr*>(e)) return u->op == "*" && u->opFunc.empty();
            if (auto* ix = dynamic_cast<IndexExpr*>(e)) return ix->opFunc.empty() && !ix->highIndex;
            if (auto* m = dynamic_cast<MemberExpr*>(e)) {
                TypePtr bt = getExprEirType(m->base);
                return (bt && bt->isPointer()) || addressable(m->base.get());
            }
            return false;
        };
        bool byRef = k.kind != ty::Type::Kind::Array && k.kind != ty::Type::Kind::Slice && !k.isPointer()
                     && addressable(node->iterable.get());
        ExprPtr init = byRef ? ExprPtr(std::make_shared<UnaryExpr>("&", node->iterable)) : node->iterable;
        holdDecl = std::make_shared<VarDecl>(idxName + "_v", byRef ? "*" + itType : itType, init);
        iterable = std::make_shared<IdentExpr>(idxName + "_v");
    }

    std::string elemType;
    ExprPtr lengthExpr, elemExpr;
    if (k.kind == ty::Type::Kind::Array && k.elem) {
        elemType = k.elem->str();
        TypePtr at = toEirType(itType);
        lengthExpr = intLit(std::to_string(at && at->isArray() ? at->arraySize : 0));
        elemExpr = std::make_shared<IndexExpr>(iterable, idx());
    } else if (k.kind == ty::Type::Kind::Slice && k.elem) {
        elemType = k.elem->str();
        lengthExpr = std::make_shared<MemberExpr>(iterable, "len");
        elemExpr = std::make_shared<IndexExpr>(iterable, idx());
    } else {
        ty::Type base = k;
        while (base.isPointer() && base.pointee) { ty::Type p = *base.pointee; base = p; }
        std::string sname = base.isTemplate() ? mangleTemplate(base.str()) : base.nominalName();
        if (k.isPointer() && !holdDecl) {
            holdDecl = std::make_shared<VarDecl>(idxName + "_p", itType, node->iterable);
            iterable = std::make_shared<IdentExpr>(idxName + "_p");
        }
        ensureStructLayout(sname);
        std::string dataType;
        auto lit = module->structLayouts.find(sname);
        if (lit != module->structLayouts.end() && lit->second.getFieldInfo("size")) {
            elemType = node->resolvedElemType;
            if (!typeParamOverride.empty()) elemType = substType(elemType, typeParamOverride);
        }
        if (elemType.empty()) throw std::runtime_error("EIR: for-in over unsupported type: " + itType);
        lengthExpr = std::make_shared<MemberExpr>(iterable, "size");
        elemExpr = std::make_shared<IndexExpr>(std::make_shared<MemberExpr>(iterable, "data"), idx());
    }

    auto idxDecl = std::make_shared<VarDecl>(idxName, "int", intLit("0"));
    std::vector<BlockItem> initItems;
    if (holdDecl) initItems.push_back(DeclPtr(holdDecl));
    initItems.push_back(DeclPtr(idxDecl));
    StmtPtr init = std::make_shared<BlockStmt>(initItems);
    ExprPtr cond = std::make_shared<BinaryExpr>(idx(), "<", lengthExpr);
    ExprPtr step = std::make_shared<BinaryExpr>(idx(), "=", std::make_shared<BinaryExpr>(idx(), "+", intLit("1")));
    auto elemDecl = std::make_shared<VarDecl>(node->varName, elemType, elemExpr);
    StmtPtr body = std::make_shared<BlockStmt>(std::vector<BlockItem>{DeclPtr(elemDecl), StmtPtr(node->body)});

    ForStmt loop(init, cond, step, body);
    loop.label = node->label;
    visit(&loop);
}

void EIRBuilder::visit(DoWhileStmt* node) {
    BasicBlock* bodyBB = createBlock("dowhile.body");
    BasicBlock* condBB = createBlock("dowhile.cond");
    BasicBlock* exitBB = createBlock("dowhile.exit");

    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(bodyBB->name)}});

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

    setBlock(condBB);
    Operand cond = evaluateExpr(node->condition);
    cond = truth(cond);
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
    loopContextStack.push_back({endBB, nullptr, cleanupScopes.size(), currentHandlerDepth});
    pushScope();

    for (size_t i = 0; i < node->cases.size(); ++i) {
        setBlock(caseBBs[i]);
        // A declaration after a case label is visible to the rest of the switch (C); the
        // case's defers run at its end.
        cleanupScopes.emplace_back();
        size_t caseDepth = cleanupScopes.size() - 1;
        for (auto& item : node->cases[i].stmts) {
            if (currentBlock->hasTerminator()) break;
            if (std::holds_alternative<DeclPtr>(item)) std::get<DeclPtr>(item)->accept(this);
            else if (std::holds_alternative<StmtPtr>(item)) std::get<StmtPtr>(item)->accept(this);
        }
        if (!currentBlock->hasTerminator()) runCleanupsToDepth(caseDepth, false);
        cleanupScopes.pop_back();
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
            visitBody(node->arms[i].body);
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
    std::string instName;
    if (TypePtr st = getExprEirType(node->subject)) {
        instName = st->nominalName();
        if (!templateInstanceArgs.count(instName)) instName.clear();
    }
    for (size_t i = 0; i < node->arms.size(); ++i) {
        if (!node->arms[i].variant.empty() && node->arms[i].variant != "_") {
            int vidx = 0;
            if (const AdtVariantInfo* v = variantFor(node->arms[i].variant, instName)) vidx = v->variantIndex;
            swInst.switchCases.emplace_back(vidx, armBBs[i]->name);
        }
    }
    emit(std::move(swInst));

    for (size_t i = 0; i < node->arms.size(); ++i) {
        auto& arm = node->arms[i];
        setBlock(armBBs[i]);
        pushScope();

        if (!arm.variant.empty() && arm.variant != "_" && !arm.bindings.empty()) {
            const AdtVariantInfo* vp = variantFor(arm.variant, instName);
            if (vp) {
                std::vector<TypePtr> actualPayloadTypes = vp->payloadTypes;
                size_t payOffset = 8;
                for (size_t b = 0; b < arm.bindings.size() && b < actualPayloadTypes.size(); ++b) {
                    TypePtr bTy = actualPayloadTypes[b];
                    size_t bAlign = getTypeAlign(bTy, module.get());
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

                    payOffset += getTypeSize(bTy, module.get());
                }
            }
        }

        visitBody(arm.body);
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

    // Error path: return the error from the enclosing function. Its Result type may differ
    // in the success type only: then a fresh one (zeroed) gets the `ok` flag and the error.
    // This is an early exit, so the pending defers and errdefers run first.
    setBlock(errBB);
    Operand whole = Operand::var(tempSlot, st);
    TypePtr retTy = currentFunction ? currentFunction->returnType : nullptr;
    if (retTy && retTy->isStruct() && retTy->nominalName() != sname) {
        std::string rname = retTy->nominalName();
        ensureStructLayout(rname);
        std::string out = newVar() + ".q_out";
        emit(Instruction{Opcode::Alloca, out, retTy, {Operand::constInt(8, Type::getI32())}});
        for (const char* fname : {"ok", "error"}) {
            const StructFieldInfo* from = module->structLayouts.count(sname) ? module->structLayouts[sname].getFieldInfo(fname) : nullptr;
            const StructFieldInfo* to = module->structLayouts.count(rname) ? module->structLayouts[rname].getFieldInfo(fname) : nullptr;
            if (!from || !to) continue;
            std::string fp = newVar(), tp = newVar(), v = newVar();
            emit(Instruction{Opcode::FieldPtr, fp, Type::getPtr(from->type), {Operand::var(sname, nullptr), Operand::var(tempSlot, Type::getPtr(st)), Operand::constString(fname)}});
            emit(Instruction{Opcode::Load, v, from->type, {Operand::var(fp, Type::getPtr(from->type))}});
            emit(Instruction{Opcode::FieldPtr, tp, Type::getPtr(to->type), {Operand::var(rname, nullptr), Operand::var(out, Type::getPtr(retTy)), Operand::constString(fname)}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(v, from->type), Operand::var(tp, Type::getPtr(to->type))}});
        }
        whole = Operand::var(out, retTy);
    }
    runCleanupsToDepth(0, true);
    if (currentBlock && !currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Ret, "", whole.type, {whole}});
    }

    setBlock(contBB);
    TypePtr valTy = getExprEirType(node);
    std::string valPtr = newVar() + ".q_val_ptr";
    emit(Instruction{Opcode::FieldPtr, valPtr, Type::getPtr(valTy), {Operand::var(sname, nullptr), Operand::var(tempSlot, Type::getPtr(st)), Operand::constString("value")}});
    std::string unwrapped = newVar() + ".q_unwrapped";
    emit(Instruction{Opcode::Load, unwrapped, valTy, {Operand::var(valPtr, Type::getPtr(valTy))}});
    lastValue = Operand::var(unwrapped, valTy);
}

void EIRBuilder::visit(StructInitExpr* node0) {
    std::shared_ptr<StructInitExpr> full = withDefaults(node0);
    StructInitExpr* node = full ? full.get() : node0;
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

        if (auto* arrLit = dynamic_cast<ArrayLitExpr*>(fval.get()); arrLit && ftype && ftype->isArray()) {
            emitArrayInitInto(Operand::var(fptr, Type::getPtr(ftype)), arrLit, ftype);
            continue;
        }
        Operand val = evalForType(fval, ftype);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {val, Operand::var(fptr, Type::getPtr(ftype))}});
    }

    // An aggregate value is the address of its bytes, typed as the aggregate so a store
    // copies all of it.
    lastLValue = Operand::var(tempSlot, Type::getPtr(stype));
    lastValue = Operand::var(tempSlot, stype);
}

void EIRBuilder::visit(ArrayLitExpr* node) {
    TypePtr arrTy = getExprEirType(node);
    size_t al = getTypeAlign(arrTy);
    if (al < 1) al = 1;
    std::string tempSlot = newVar() + ".arrlit";
    emit(Instruction{Opcode::Alloca, tempSlot, arrTy, {Operand::constInt(static_cast<int64_t>(al), Type::getI32())}});
    emitArrayInitInto(Operand::var(tempSlot, Type::getPtr(arrTy)), node, arrTy);
    lastLValue = Operand::var(tempSlot, Type::getPtr(arrTy));
    lastValue = Operand::var(tempSlot, arrTy);
}

// `alloc_with(&a, T, n)` is `(*T)A_alloc(&a, n * sizeof(T))` (A the allocator's struct,
// or the inline `alloc` of a generic instance), null when the size overflows or is
// negative (or does not fit the size parameter), and the block zeroed when it is not
// null, as alloc<T> returns zeroed memory whatever the allocator.
void EIRBuilder::visit(AllocWithExpr* node) {
    Operand allocPtr = evaluateExpr(node->allocator);
    TypePtr at = getExprEirType(node->allocator);
    std::string an = at ? at->nominalName() : "";
    std::string fnName = an + "_alloc";
    Function* af = module->getFunction(fnName);
    if (!af) af = instantiateGenericMethod(an, "alloc");
    if (!af) throw std::runtime_error("EIR: alloc_with: allocator type '" + an + "' has no alloc method (" + fnName + ")");
    fnName = af->name;

    std::string et = typeParamOverride.empty() ? node->elemType : substType(node->elemType, typeParamOverride);
    TypePtr elemTy = toEirType(et);
    size_t esz = getTypeSize(elemTy, module.get());
    if (elemTy && elemTy->isStruct()) {
        size_t ssz = module->getStructSize(elemTy->nominalName());
        if (ssz) esz = ssz;
    }
    if (esz == 0) esz = 1;
    TypePtr i64 = Type::getI64();
    Operand n = coerceNumeric(evaluateExpr(node->count), i64);

    // bad = n < 0 || n > INT64_MAX / esz (|| the size does not fit a narrower parameter)
    int64_t maxN = INT64_MAX / static_cast<int64_t>(esz);
    TypePtr sizeTy = af->params.size() >= 2 ? af->params[1].second : i64;
    if (sizeTy && sizeTy->isInt() && sizeTy->bitWidth < 64) {
        int64_t maxv = static_cast<int64_t>((uint64_t(1) << (sizeTy->bitWidth - 1)) - 1);
        maxN = std::min<int64_t>(maxN, maxv / static_cast<int64_t>(esz));
    }
    std::string neg = newVar(), big = newVar(), bad = newVar();
    emit(Instruction{Opcode::ICmp, neg, Type::getI1(), {n, Operand::constInt(0, i64)}, ICmpCond::Slt});
    emit(Instruction{Opcode::ICmp, big, Type::getI1(), {n, Operand::constInt(maxN, i64)}, ICmpCond::Sgt});
    emit(Instruction{Opcode::Or, bad, Type::getI1(), {Operand::var(neg, Type::getI1()), Operand::var(big, Type::getI1())}});

    TypePtr resTy = Type::getPtr(elemTy);
    std::string slot = newVar() + ".allocw";
    emit(Instruction{Opcode::Alloca, slot, Type::getPtr(), {Operand::constInt(8, Type::getI32())}});
    BasicBlock* callBB = createBlock("allocw.call");
    BasicBlock* zeroBB = createBlock("allocw.zero");
    BasicBlock* loopBB = createBlock("allocw.loop");
    BasicBlock* bodyBB = createBlock("allocw.body");
    BasicBlock* doneBB = createBlock("allocw.done");
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(bad, Type::getI1()), Operand::block(doneBB->name), Operand::block(callBB->name)}});

    setBlock(callBB);
    std::string total = newVar();
    emit(Instruction{Opcode::Mul, total, i64, {n, Operand::constInt(static_cast<int64_t>(esz), i64)}});
    Operand sizeArg = coerceNumeric(Operand::var(total, i64), sizeTy);
    std::string p = newVar();
    emit(Instruction{Opcode::Call, p, Type::getPtr(), {Operand::global(fnName, Type::getPtr()), allocPtr, sizeArg}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(p, Type::getPtr()), Operand::var(slot, Type::getPtr(Type::getPtr()))}});
    std::string isNull = newVar();
    emit(Instruction{Opcode::ICmp, isNull, Type::getI1(), {Operand::var(p, Type::getPtr()), Operand::constNull()}, ICmpCond::Eq});
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(isNull, Type::getI1()), Operand::block(doneBB->name), Operand::block(zeroBB->name)}});

    // Zero the block a byte at a time.
    setBlock(zeroBB);
    std::string idxSlot = newVar() + ".i";
    emit(Instruction{Opcode::Alloca, idxSlot, i64, {Operand::constInt(8, Type::getI32())}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});
    setBlock(loopBB);
    std::string iv = newVar(), more = newVar();
    emit(Instruction{Opcode::Load, iv, i64, {Operand::var(idxSlot, Type::getPtr(i64))}});
    emit(Instruction{Opcode::ICmp, more, Type::getI1(), {Operand::var(iv, i64), Operand::var(total, i64)}, ICmpCond::Slt});
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(more, Type::getI1()), Operand::block(bodyBB->name), Operand::block(doneBB->name)}});
    setBlock(bodyBB);
    std::string bp = newVar(), next = newVar();
    emit(Instruction{Opcode::OffsetPtr, bp, Type::getPtr(Type::getU8()), {Operand::var(p, Type::getPtr()), Operand::var(iv, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getU8()), Operand::var(bp, Type::getPtr(Type::getU8()))}});
    emit(Instruction{Opcode::Add, next, i64, {Operand::var(iv, i64), Operand::constInt(1, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(next, i64), Operand::var(idxSlot, Type::getPtr(i64))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});

    setBlock(doneBB);
    std::string res = newVar();
    emit(Instruction{Opcode::Load, res, resTy, {Operand::var(slot, Type::getPtr(Type::getPtr()))}});
    lastValue = Operand::var(res, resTy);
}

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
    const AdtVariantInfo* vptr = adtVariants.count(node->templateName)
        ? variantFor(node->templateName, genericVariantInstance(node->templateName, substitutedArgs)) : nullptr;
    if (vptr) {
        const AdtVariantInfo& vinfo = *vptr;
        TypePtr enumTy = Type::getStruct(vinfo.enumName, {});
        std::string slot = newVar() + ".adt";
        emit(Instruction{Opcode::Alloca, slot, enumTy, {Operand::constInt(8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {
            Operand::constInt(vinfo.variantIndex, Type::getI32()),
            Operand::var(slot, Type::getPtr(Type::getI32()))
        }});

        size_t payOffset = 8;
        for (size_t p = 0; p < node->args.size() && p < vinfo.payloadTypes.size(); ++p) {
            TypePtr pt = vinfo.payloadTypes[p];
            size_t pAlign = getTypeAlign(pt, module.get());
            payOffset = (payOffset + pAlign - 1) & ~(pAlign - 1);
            std::string payPtr = newVar() + ".pay";
            emit(Instruction{Opcode::OffsetPtr, payPtr, Type::getPtr(pt), {
                Operand::var(slot, Type::getPtr()), Operand::constInt(static_cast<int64_t>(payOffset), Type::getI64())
            }});
            Operand argVal = evalForType(node->args[p], pt);
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {argVal, Operand::var(payPtr, Type::getPtr(pt))}});
            payOffset += getTypeSize(pt, module.get());
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
        if (node->escapes) {
            emit(Instruction{Opcode::Call, envSlot, Type::getPtr(), {
                Operand::global("malloc", Type::getPtr()), Operand::constInt(static_cast<int64_t>(totalEnvSize), Type::getI64())
            }});
        } else {
            emit(Instruction{Opcode::Alloca, envSlot, Type::getArray(Type::getU8(), totalEnvSize), {Operand::constInt(static_cast<int64_t>(maxAlign), Type::getI32())}});
        }
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
    auto prevUnwind = currentUnwindTarget;
    auto prevUnwindStack = std::move(unwindTargetStack);
    size_t prevHandlerDepth = currentHandlerDepth;
    cleanupScopes.clear();
    cleanupScopes.emplace_back(); // root lambda cleanup frame
    loopContextStack.clear();
    currentUnwindTarget = nullptr;
    unwindTargetStack.clear();
    currentHandlerDepth = 0;

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
    currentUnwindTarget = prevUnwind;
    unwindTargetStack = std::move(prevUnwindStack);
    currentHandlerDepth = prevHandlerDepth;

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
    if (!node->typeName.empty() && lookupVar(node->typeName)) {
        ty = lookupVarType(node->typeName);   // `sizeof(x)` of a variable the parser kept as a name
    } else if (!node->typeName.empty()) {
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
        if (ty->isStruct()) ensureStructLayout(ty->nominalName());
        sz = getTypeSize(ty, module.get());
        if (ty->isStruct() && module) {
            size_t ss = module->getStructSize(ty->nominalName());
            if (ss) sz = ss;
        }
    }
    if (sz == 0) sz = 8;
    lastValue = Operand::constInt(static_cast<int64_t>(sz), Type::getI64());
    lastLValue = lastValue;
}

void EIRBuilder::visit(ThreadCreateExpr* node) {
    Operand worker = evaluateExpr(node->worker);
    std::string handleVar = newVar() + ".thr_handle";
    bool owned = dynamic_cast<LambdaExpr*>(node->worker.get()) != nullptr;
    emit(Instruction{Opcode::Call, handleVar, Type::getPtr(), {
        Operand::global("__eir_thread_create", Type::getPtr()),
        worker,
        Operand::constInt(owned ? 1 : 0, Type::getI32())
    }});
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
std::string EIRBuilder::exceptionTypeName(const std::string& raw) const {
    std::string t = tyq::strip(raw);
    if (!t.empty() && t[0] == '?') t = t.substr(1);
    for (const char* tag : {"struct:", "interface:"}) {
        size_t p;
        while ((p = t.find(tag)) != std::string::npos) t.erase(p, std::string(tag).size());
    }
    if (t.find('<') != std::string::npos) t = mangleTemplate(t);
    if (t == "int32") t = "int";
    if (t == "uint32") t = "uint";
    return t.empty() ? "unknown" : t;
}

void EIRBuilder::emitDeferPad(Stmt* body) {
    if (!currentFunction || !currentBlock) return;
    BasicBlock* savedBB = currentBlock;
    BasicBlock* pad = createBlock("defer.lpad");
    setBlock(pad);

    std::string exVar = newVar();
    emit(Instruction{Opcode::LandingPad, exVar, Type::getPtr(), {}});
    Operand exObj = Operand::var(exVar, Type::getPtr());

    if (body) {
        body->accept(this);
    }

    if (currentBlock && !currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Throw, "", Type::getVoid(), {exObj}});
    }

    setBlock(savedBB);
    currentUnwindTarget = pad;
    currentHandlerDepth++;
    emit(Instruction{Opcode::PushHandler, "", Type::getVoid(), {Operand::block(pad->name)}});
}

void EIRBuilder::visit(ThrowStmt* node) {
    if (!node->value) return;

    Operand val = evaluateExpr(node->value);
    Operand exPtr;

    if (node->rethrowCaptured) {
        exPtr = val;
    } else {
        // In a generic instance the shared AST's stamp is some instance's: the thrown type is
        // this instance's.
        std::string thrownType = typeParamOverride.empty() ? node->valueType : "";
        if (thrownType.empty()) thrownType = eskiuSpelling(val.type ? val.type : getExprEirType(node->value));
        thrownType = exceptionTypeName(thrownType);

        size_t paySize = 8;
        if (val.type) {
            if (val.type->isStruct() && module) {
                paySize = module->getStructSize(val.type->nominalName());
            } else {
                paySize = getTypeSize(val.type, module.get());
            }
        }
        if (paySize == 0) paySize = 8;

        Operand typeStr = Operand::constString(thrownType);
        std::string exVar = newVar();
        emit(Instruction{Opcode::Call, exVar, Type::getPtr(), {
            Operand::global("__eir_alloc_exception", Type::getPtr()),
            Operand::constInt(static_cast<int64_t>(16 + paySize), Type::getI64()),
            typeStr
        }});
        exPtr = Operand::var(exVar, Type::getPtr());

        std::string slotVar = newVar();
        emit(Instruction{Opcode::OffsetPtr, slotVar, Type::getPtr(), {
            exPtr,
            Operand::constInt(16, Type::getI64())
        }});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {
            val,
            Operand::var(slotVar, Type::getPtr())
        }});
    }

    emit(Instruction{Opcode::Throw, "", Type::getVoid(), {exPtr}});
}

void EIRBuilder::visit(TryStmt* node) {
    if (!currentFunction) return;

    BasicBlock* lpadBB     = createBlock("try.lpad");
    BasicBlock* dispatchBB = createBlock("try.dispatch");
    BasicBlock* finallyBB  = createBlock("try.finally");
    BasicBlock* doneBB     = createBlock("try.done");

    BasicBlock* savedUnwind = currentUnwindTarget;
    currentUnwindTarget = lpadBB;
    size_t savedHandlerDepth = currentHandlerDepth;

    // Track finally in cleanupScopes for early return/break
    cleanupScopes.emplace_back();
    if (node->finally && !node->unwindOnly) {
        cleanupScopes.back().push_back({node->finally, false});
    }

    // Push try handler
    currentHandlerDepth++;
    emit(Instruction{Opcode::PushHandler, "", Type::getVoid(), {Operand::block(lpadBB->name)}});

    if (node->body) {
        node->body->accept(this);
    }

    // Normal exit of try body
    if (currentBlock && !currentBlock->hasTerminator()) {
        while (currentHandlerDepth > savedHandlerDepth) {
            emit(Instruction{Opcode::PopHandler, "", Type::getVoid(), {}});
            currentHandlerDepth--;
        }
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(finallyBB->name)}});
    }

    cleanupScopes.pop_back();
    currentUnwindTarget = savedUnwind;
    currentHandlerDepth = savedHandlerDepth;

    // --- Landing Pad ---
    setBlock(lpadBB);
    std::string exVar = newVar();
    emit(Instruction{Opcode::LandingPad, exVar, Type::getPtr(), {}});
    Operand exObj = Operand::var(exVar, Type::getPtr());
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(dispatchBB->name)}});

    // --- Catch Dispatch ---
    setBlock(dispatchBB);

    std::string typeSlotVar = newVar();
    emit(Instruction{Opcode::OffsetPtr, typeSlotVar, Type::getPtr(), {exObj, Operand::constInt(8, Type::getI64())}});
    std::string exTypeVar = newVar();
    emit(Instruction{Opcode::Load, exTypeVar, Type::getPtr(Type::getU8()), {Operand::var(typeSlotVar, Type::getPtr())}});
    Operand exType = Operand::var(exTypeVar, Type::getPtr(Type::getU8()));

    for (size_t i = 0; i < node->catches.size(); ++i) {
        auto& c = node->catches[i];
        bool isCatchAll = c.captureAll || c.type.empty() || c.type == "_" || c.name == "_";

        BasicBlock* handlerBB = createBlock("catch." + (c.type.empty() ? "any" : c.type));
        BasicBlock* nextClauseBB = isCatchAll ? nullptr : createBlock("catch.next");

        if (isCatchAll) {
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(handlerBB->name)}});
        } else {
            std::string cTypeName = exceptionTypeName(typeParamOverride.empty() ? c.type : substType(c.type, typeParamOverride));
            Operand cTypeStr = Operand::constString(cTypeName);
            std::string cmpVar = newVar();
            emit(Instruction{Opcode::Call, cmpVar, Type::getI32(), {
                Operand::global("strcmp", Type::getI32()),
                exType,
                cTypeStr
            }});
            std::string matchVar = newVar();
            Instruction cmpInst{Opcode::ICmp, matchVar, Type::getI1(), {
                Operand::var(cmpVar, Type::getI32()),
                Operand::constInt(0, Type::getI32())
            }};
            cmpInst.icmpCond = ICmpCond::Eq;
            emit(cmpInst);
            emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {
                Operand::var(matchVar, Type::getI1()),
                Operand::block(handlerBB->name),
                Operand::block(nextClauseBB->name)
            }});
        }

        // --- Handler Block ---
        setBlock(handlerBB);
        pushScope();

        // Register exception as active caught exception for exception_text()
        emit(Instruction{Opcode::Call, "", Type::getVoid(), {
            Operand::global("__eir_push_caught", Type::getVoid()),
            exObj
        }});
        std::string textSlot = newVar() + ".exc_text";
        entryAlloca(textSlot, Type::getArray(Type::getU8(), 512), 8);
        catchTextSlots.push_back(textSlot);
        emit(Instruction{Opcode::Call, "", Type::getVoid(), {
            Operand::global("__eir_clear_exception", Type::getVoid())
        }});

        if (c.captureAll && !c.name.empty()) {
            // The async lowering's capturing catch-all names the exception object itself,
            // which a later rethrow throws again or __cxa_free_exception releases.
            std::string localSlot = newVar();
            emit(Instruction{Opcode::Alloca, localSlot, Type::getPtr(), {}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {exObj, Operand::var(localSlot, Type::getPtr(Type::getPtr()))}});
            declareVar(c.name, Operand::var(localSlot, Type::getPtr(Type::getPtr())), Type::getPtr());
        }
        if (!isCatchAll && !c.name.empty()) {
            TypePtr catchEirTy = toEirType(typeParamOverride.empty() ? c.type : substType(c.type, typeParamOverride));
            std::string paySlotVar = newVar();
            emit(Instruction{Opcode::OffsetPtr, paySlotVar, Type::getPtr(), {exObj, Operand::constInt(16, Type::getI64())}});
            std::string payValVar = newVar();
            emit(Instruction{Opcode::Load, payValVar, catchEirTy, {Operand::var(paySlotVar, Type::getPtr())}});

            std::string localSlot = newVar();
            emit(Instruction{Opcode::Alloca, localSlot, catchEirTy, {}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {
                Operand::var(payValVar, catchEirTy),
                Operand::var(localSlot, Type::getPtr(catchEirTy))
            }});
            declareVar(c.name, Operand::var(localSlot, Type::getPtr(catchEirTy)), catchEirTy);
        }

        // Finally also runs on early return/break from catch body
        cleanupScopes.emplace_back();
        size_t catchStartHandler = currentHandlerDepth;
        BasicBlock* savedCatchUnwind = currentUnwindTarget;
        if (node->finally && !node->unwindOnly) {
            cleanupScopes.back().push_back({node->finally, false});
            emitDeferPad(node->finally.get());
        }

        if (c.body) {
            c.body->accept(this);
        }

        if (currentBlock && !currentBlock->hasTerminator()) {
            while (currentHandlerDepth > catchStartHandler) {
                emit(Instruction{Opcode::PopHandler, "", Type::getVoid(), {}});
                currentHandlerDepth--;
            }
            emit(Instruction{Opcode::Call, "", Type::getVoid(), {
                Operand::global("__eir_pop_caught", Type::getVoid())
            }});
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(finallyBB->name)}});
        }

        currentUnwindTarget = savedCatchUnwind;
        catchTextSlots.pop_back();
        cleanupScopes.pop_back();
        popScope();

        if (nextClauseBB) {
            setBlock(nextClauseBB);
        }
    }

    // --- Unhandled path (no catch matched, or no catch clauses) ---
    if (currentBlock && !currentBlock->hasTerminator()) {
        if (node->finally) {
            node->finally->accept(this);
        }
        if (currentBlock && !currentBlock->hasTerminator()) {
            emit(Instruction{Opcode::Throw, "", Type::getVoid(), {exObj}});
        }
    }

    // --- Finally Block (normal execution path) ---
    setBlock(finallyBB);
    if (node->finally && !node->unwindOnly) {
        node->finally->accept(this);
    }
    if (currentBlock && !currentBlock->hasTerminator()) {
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(doneBB->name)}});
    }

    currentUnwindTarget = savedUnwind;
    currentHandlerDepth = savedHandlerDepth;

    setBlock(doneBB);
}

void EIRBuilder::visit(DeferStmt* node) {
    if (node->body && !cleanupScopes.empty()) {
        // The body names the variables visible here, even when it runs where a later
        // declaration shadows one of them.
        cleanupScopes.back().push_back({node->body, node->isErr, scopeStack, varTypeStack});
        if (!node->isErr && currentFunction) {
            emitDeferPad(node->body.get());
        }
    }
}

} // namespace eir
