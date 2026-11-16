// eir_builder_ops.cpp: integer conversions, compound assignment, interfaces, enums, for-in and do-while (split from eir_builder.cpp).
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
    if (auto* mem = nodeCast<MemberExpr>(node->left.get())) {
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
    if (auto* lit = nodeCast<LiteralExpr>(e.get()); lit && lit->kind == LiteralExpr::Kind::NULL_VAL) {
        return Operand::constNull();
    }
    if (auto* id = nodeCast<IdentExpr>(e.get()); id && module && module->getFunction(id->name) && !lookupVar(id->name)) {
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
            if (nodeCast<IdentExpr>(e)) return true;
            if (auto* u = nodeCast<UnaryExpr>(e)) return u->op == "*" && u->opFunc.empty();
            if (auto* ix = nodeCast<IndexExpr>(e)) return ix->opFunc.empty() && !ix->highIndex;
            if (auto* m = nodeCast<MemberExpr>(e)) {
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

} // namespace eir
