// eir_builder_call.cpp: ++/--, identifiers, literals, calls and casts (split from eir_builder.cpp).
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
    if (auto* mem = nodeCast<MemberExpr>(node->operand.get())) {
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
    if (auto id = nodeCast<IdentExpr>(node->callee.get())) {
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
    if (auto id = nodeCast<IdentExpr>(node->callee.get())) {
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
    if (auto id = nodeCast<IdentExpr>(node->callee.get())) {
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
    if (auto id = nodeCast<IdentExpr>(node->callee.get())) {
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

    if (auto id = nodeCast<IdentExpr>(node->callee.get())) {
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
    } else if (auto mem = nodeCast<MemberExpr>(node->callee.get())) {
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
    if (auto* id = nodeCast<IdentExpr>(node->expr.get());
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

} // namespace eir
