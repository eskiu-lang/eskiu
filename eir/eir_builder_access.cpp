// eir_builder_access.cpp: ternaries, members, indexes, struct declarations and conversions (split from eir_builder.cpp).
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
        if (auto id = nodeCast<IdentExpr>(node->base.get())) {
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
        if (auto id = nodeCast<IdentExpr>(node->base.get())) {
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
        if (auto id = nodeCast<IdentExpr>(node->base.get())) {
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
        if (auto mf = nodeCast<FunctionDecl>(m.get())) {
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
        auto* f = nodeCast<FunctionDecl>(m.get());
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

        if (auto* lit = nodeCast<LiteralExpr>(e.get()); lit && lit->kind == LiteralExpr::Kind::NULL_VAL) {
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

} // namespace eir
