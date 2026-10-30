#include "codegen.h"
#include "../ast/type_qual.h"
#include <typeinfo>
#include <cstdlib>
#include <cstdio>
#include <iostream>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

// ============================================================================
// Type System
// ============================================================================

bool CodeGen::resolveArrayDim(const std::string& dim, uint64_t& out) const {
    if (dim.empty()) return false;
    bool digits = true;
    for (char c : dim) if (!std::isdigit((unsigned char)c)) { digits = false; break; }
    if (digits) { out = std::stoull(dim); return true; }
    auto e = enumConstants.find(dim);
    if (e != enumConstants.end()) { out = (uint64_t)e->second; return true; }
    auto c = constInts.find(dim);
    if (c != constInts.end())     { out = (uint64_t)c->second; return true; }
    // An integer constant expression (`(uint8)258`, `N*2`), as the type checker folds it.
    long long v = 0;
    bool sized = false;
    bool folded = ty::foldDim(dim, [&](const std::string& n, long long& r) {
        if (auto en = enumConstants.find(n); en != enumConstants.end()) { r = en->second; return true; }
        if (auto cn = constInts.find(n); cn != constInts.end()) { r = cn->second; return true; }
        if (n.rfind("sizeof(", 0) == 0) {           // `sizeof(T)`, the type's allocation size
            llvm::Type* ty = const_cast<CodeGen*>(this)->getTypeFromString(n.substr(7, n.size() - 8));
            r = (long long)module->getDataLayout().getTypeAllocSize(ty);
            sized = true;
            return true;
        }
        return false;
    }, v);
    if (folded && v > 0) { out = (uint64_t)v; return true; }
    // The type checker leaves a `sizeof` dimension's value to codegen.
    if (folded && sized)
        throw std::runtime_error("array size must be positive, got " + std::to_string(v) + " ('" + dim + "')");
    return false;
}

llvm::Type* CodeGen::getTypeFromString(const std::string& typeStr) {
    // const has no ABI/layout meaning — strip it before lowering.
    { std::string s = tyq::strip(typeStr); if (s != typeStr) return getTypeFromString(s); }

    // Apply type parameter override during template function instantiation
    if (!typeParamOverride.empty()) {
        std::string resolved = substType(typeStr, typeParamOverride);
        if (resolved != typeStr) return getTypeFromString(resolved);
    }

    // Resolve a type alias to its underlying type (bare-name aliases only; a
    // decorated spelling like *Alias is not an alias key — it lowers to a pointer).
    if (auto it = typeAliases.find(typeStr); it != typeAliases.end())
        return getTypeFromString(it->second);

    // Structural dispatch via the one grammar parser (`ty::Type::parse`) — the same
    // interpreter the type checker uses, so codegen no longer re-implements the
    // type-string grammar. Registry lookups + instantiation stay here.
    llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
    ty::Type t = ty::Type::parse(typeStr);
    switch (t.kind) {
        case ty::Type::Kind::Pointer:
        case ty::Type::Kind::String:
            return llvm::PointerType::get(*context, 0);   // both lower to an opaque ptr
        case ty::Type::Kind::Void:  return llvm::Type::getVoidTy(*context);
        case ty::Type::Kind::Bool:  return llvm::Type::getInt1Ty(*context);
        case ty::Type::Kind::Char:  return llvm::Type::getInt8Ty(*context);
        case ty::Type::Kind::Float:
            return t.name == "float" ? llvm::Type::getFloatTy(*context)
                                     : llvm::Type::getDoubleTy(*context);
        case ty::Type::Kind::Int: {
            const std::string& s = t.name;
            if (s == "int8"  || s == "uint8")  return llvm::Type::getInt8Ty(*context);
            if (s == "int16" || s == "uint16") return llvm::Type::getInt16Ty(*context);
            if (s == "int64" || s == "uint64") return llvm::Type::getInt64Ty(*context);
            return i32;   // int/int32/uint/uint32
        }
        case ty::Type::Kind::VaList: {
            // {ptr,ptr,ptr,i32,i32} — AArch64 layout (superset of x86-64); one type
            // works for both; the va_start/va_arg machinery reads the target slice.
            auto it = structTypes.find("__va_list");
            if (it != structTypes.end()) return it->second;
            llvm::Type* p = llvm::PointerType::get(*context, 0);
            auto* st = llvm::StructType::create(*context, {p, p, p, i32, i32}, "__va_list");
            structTypes["__va_list"] = st;
            return st;
        }
        case ty::Type::Kind::Fn:
            return llvm::StructType::get(*context, {       // fat pointer {fn_ptr, env_ptr}
                llvm::PointerType::get(*context, 0),
                llvm::PointerType::get(*context, 0),
            });
        case ty::Type::Kind::Struct: {                     // "struct:Name" (normalized)
            auto it = structTypes.find(t.name);
            if (it != structTypes.end()) return it->second;
            return llvm::PointerType::get(*context, 0);     // forward-ref placeholder
        }
        case ty::Type::Kind::Interface:
            // QUIRK preserved: a decorated `interface:X` matched nothing in the old
            // code and fell through to i32. (Bare interface names lower to ptr via
            // the Named branch.) Kept identical; a deliberate fix is a later step.
            return i32;
        case ty::Type::Kind::Template: {                   // "Name<args>" — instantiate
            auto [tname, args] = splitTemplateType(typeStr);
            if (genericEnumDecls.count(tname)) {            // generic ADT enum instance
                std::string mangled = ensureEnumInst(tname, args);
                return structTypes[mangled];
            }
            std::string mangled = mangleTemplate(typeStr);
            ensureTemplateInstantiated(mangled, tname, args);
            auto it = structTypes.find(mangled);
            if (it != structTypes.end()) return it->second;
            break;   // fall through to the i32 fallback below
        }
        case ty::Type::Kind::Array: {                      // "T[N]"
            llvm::Type* elem = getTypeFromString(t.elem->str());
            uint64_t n = 0;
            if (resolveArrayDim(t.dim, n)) return llvm::ArrayType::get(elem, n);
            bool negLit = t.dim.size() > 1 && t.dim[0] == '-';
            for (size_t i = 1; negLit && i < t.dim.size(); ++i)
                if (!std::isdigit((unsigned char)t.dim[i])) negLit = false;
            if (negLit)
                throw std::runtime_error("array size must be a positive constant, got '"
                                         + t.dim + "'");
            return llvm::PointerType::get(*context, 0);     // unsized → pointer
        }
        case ty::Type::Kind::Slice: {                      // "T[]" → fat pointer { data, len }
            return llvm::StructType::get(*context, {
                llvm::PointerType::get(*context, 0),
                llvm::Type::getInt64Ty(*context)});
        }
        case ty::Type::Kind::Named: {                      // bare nominal: enum/struct/iface
            if (enumTypes.count(t.name)) {                 // classic enum → i32; ADT → struct
                auto st = structTypes.find(t.name);
                if (st != structTypes.end()) return st->second;
                return i32;
            }
            auto it = structTypes.find(t.name);            // bare struct name
            if (it != structTypes.end()) return it->second;
            // An interface value is its {data, vtable} fat struct.
            auto fit = ifaceFatPtrTypes.find(t.name);
            if (fit != ifaceFatPtrTypes.end()) return fit->second;
            break;
        }
        default: break;                                    // Null/Unknown/Error
    }

    // Folding a constant (e.g. the pre-pass over top-level consts, which runs before any
    // struct is declared): an unknown type means "not foldable yet", not an i32 size.
    if (constEvalDepth > 0) throw std::runtime_error("unknown type '" + typeStr + "' in a constant");
    std::cerr << "Warning: unknown type '" << typeStr << "', defaulting to i32" << std::endl;
    return i32;
}

bool CodeGen::needsSret(llvm::Type* retType) const {
    if (!retType || retType->isVoidTy() || !retType->isSized()) return false;
    if (!retType->isAggregateType()) return false;
    // arm64: structs > 16 bytes use sret; x86-64: structs > 8 bytes that don't fit
    // in two registers.  Using 16 as a conservative threshold covers both.
    return module->getDataLayout().getTypeAllocSize(retType) > 16;
}

bool CodeGen::isPointerType(const std::string& typeStr) const {
    if (typeStr.empty()) return false;
    return typeStr.front() == '*' || typeStr.back() == '*' || typeStr == "string";
}

bool CodeGen::eskiuUnsigned(const std::string& t) const {
    std::string s = tyq::strip(expandAlias(t));
    return s == "uint" || s == "uint8" || s == "uint16" || s == "uint32" ||
           s == "uint64" || s == "char" || s == "bool";
}

llvm::AllocaInst* CodeGen::entryAlloca(llvm::Type* ty, llvm::Value* arrSize,
                                       const llvm::Twine& name) {
    llvm::BasicBlock* entry = &builder->GetInsertBlock()->getParent()->getEntryBlock();
    llvm::IRBuilder<> tmp(entry, entry->begin());
    return tmp.CreateAlloca(ty, arrSize, name);
}

// A value used as a condition: bool as is, int `!= 0`, float `une 0.0` (NaN is
// true, as in C), pointer `!= null`.
llvm::Value* CodeGen::emitTruthy(llvm::Value* val) {
    llvm::Type* t = val->getType();
    if (t->isIntegerTy(1)) return val;
    if (t->isFloatingPointTy())
        return builder->CreateFCmpUNE(val, llvm::ConstantFP::get(t, 0.0));
    if (t->isPointerTy()) return builder->CreateIsNotNull(val);
    if (t->isIntegerTy()) return builder->CreateICmpNE(val, llvm::ConstantInt::get(t, 0));
    // An interface value `{data, vtable}` is true when its data pointer is set.
    if (auto* st = llvm::dyn_cast<llvm::StructType>(t);
        st && st->getNumElements() == 2 && st->getElementType(0)->isPointerTy())
        return builder->CreateIsNotNull(builder->CreateExtractValue(val, 0));
    throw std::runtime_error("value cannot be used as a condition");
}

llvm::Value* CodeGen::coerceInt(llvm::Value* val, llvm::Type* ty, bool unsignedSrc) {
    if (!val || val->getType() == ty) return val;
    if (!val->getType()->isIntegerTy() || !ty->isIntegerTy()) return val;
    unsigned sw = val->getType()->getIntegerBitWidth();
    unsigned dw = ty->getIntegerBitWidth();
    // To bool: `!= 0`, not a truncation (a returned 4 is true, not its low bit).
    if (dw == 1) return builder->CreateICmpNE(val, llvm::ConstantInt::get(val->getType(), 0));
    if (sw < dw) return unsignedSrc ? builder->CreateZExt(val, ty) : builder->CreateSExt(val, ty);
    if (sw > dw) return builder->CreateTrunc(val, ty);
    return val;
}

llvm::Value* CodeGen::intToFloat(llvm::Value* val, llvm::Type* ty, bool unsignedSrc) {
    // The single place integer->float conversion happens. An unsigned source
    // must use UIToFP, not SIToFP: a high-bit-set unsigned value (e.g. uint32
    // 4000000000) would otherwise convert to a negative float.
    return unsignedSrc ? builder->CreateUIToFP(val, ty)
                       : builder->CreateSIToFP(val, ty);
}

llvm::Value* CodeGen::coerceValue(llvm::Value* val, llvm::Type* target, bool unsignedSrc) {
    // The single home for implicit numeric coercion: int/int -> coerceInt,
    // int/float -> intToFloat, float/int -> emitFloatToInt, float/float -> FPCast.
    // This ladder was copy-pasted at every implicit-conversion site (assignment,
    // return, var-decl, ternary, ADT field, template arg); some copies had drifted
    // (the ternary omitted float->int). Non-numeric or already-matching values pass
    // through; explicit `as` casts (pointer conversions) stay in visit(CastExpr).
    if (!val || val->getType() == target) return val;
    llvm::Type* src = val->getType();
    if (src->isIntegerTy() && target->isIntegerTy())             return coerceInt(val, target, unsignedSrc);
    if (src->isIntegerTy() && target->isFloatingPointTy())       return intToFloat(val, target, unsignedSrc);
    if (src->isFloatingPointTy() && target->isIntegerTy(1))
        return builder->CreateFCmpUNE(val, llvm::ConstantFP::get(src, 0.0));
    if (src->isFloatingPointTy() && target->isIntegerTy())       return emitFloatToInt(val, target, false);
    if (src->isFloatingPointTy() && target->isFloatingPointTy()) return builder->CreateFPCast(val, target);
    return val;
}

std::string CodeGen::expandAlias(const std::string& raw) const {
    // const is checked only by the type checker; codegen works on stripped types.
    std::string t = tyq::strip(raw);
    if (t.empty()) return t;
    if (t.front() == '*') {
        // A pointer to an alias of an array (`*AI`, `type AI = int[3]`) is `int[3]*`: a
        // leading star would bind looser than the array suffix (an array of pointers).
        std::string inner = expandAlias(t.substr(1));
        if (!inner.empty() && inner.back() == ']' && inner != t.substr(1)) return inner + "*";
        return "*" + inner;
    }
    if (t.back()  == '*') return expandAlias(t.substr(0, t.size() - 1)) + "*";
    auto it = typeAliases.find(t);
    if (it != typeAliases.end()) return expandAlias(it->second);
    return t;
}

bool CodeGen::isIntPromotingOp(const std::string& op) {
    static const std::set<std::string> ops = {"+","-","*","/","%","&","|","^","<<",">>",
                                              "==","!=","<",">","<=",">="};
    return ops.count(op) > 0;
}

// The type checker types `uint8 + uint8` as uint8 (so assigning it back needs no
// cast), but the value codegen computes is the C-promoted `int`. Report `int` for such
// an arithmetic/bitwise result so every consumer extends it as the signed int it is.
static std::string promotedResultType(const ExprPtr& expr, const std::string& t) {
    static const std::set<std::string> narrow = {"bool","char","int8","uint8","int16","uint16"};
    if (auto* u = dynamic_cast<UnaryExpr*>(expr.get()))     // `-x` / `~x` / `+x` promote too
        return (u->opFunc.empty() && (u->op == "-" || u->op == "~" || u->op == "+") && narrow.count(t)) ? "int" : t;
    auto* b = dynamic_cast<BinaryExpr*>(expr.get());
    if (!b || !b->opFunc.empty()) return t;
    static const std::set<std::string> arith = {"+","-","*","/","%","&","|","^","<<",">>"};
    if (!arith.count(b->op)) return t;
    return narrow.count(t) ? "int" : t;
}

// The static type of a built-in (non-overloaded) binary operator, mirroring the type
// checker's inferBinaryExprType + promoteType. Codegen needs it where the resolver
// table has no entry, e.g. inside a template instance (the type checker skips generic
// bodies), so `(a + b) / c` over `uint` keeps its signedness there too.
static std::string builtinBinaryType(const std::string& l, const std::string& op,
                                     const std::string& r) {
    static const std::set<std::string> cmp = {"==","!=","<",">","<=",">=","&&","||"};
    if (cmp.count(op)) return "bool";
    if (op == "=") return l;
    auto width = [](const std::string& t) -> int {
        if (t == "int64" || t == "uint64") return 64;
        if (t == "int" || t == "int32" || t == "uint" || t == "uint32") return 32;
        if (t == "int16" || t == "uint16") return 16;
        if (t == "int8" || t == "uint8" || t == "char" || t == "bool") return 8;
        return 0;
    };
    auto isFloat = [](const std::string& t) { return t == "float" || t == "double"; };
    auto isPtr = [](const std::string& t) {
        return !t.empty() && (t.front() == '*' || t.back() == '*' || t == "string");
    };
    auto promoted = [&](const std::string& t) { return width(t) && width(t) < 32 ? "int" : t; };
    if (op == "-" && isPtr(l) && isPtr(r)) return "int64";
    if ((op == "+" || op == "-") && isPtr(l) && width(r)) return l;
    if (op == "+" && isPtr(r) && width(l)) return r;
    if (op == "<<" || op == ">>") return width(l) ? promoted(l) : "";
    bool ln = width(l) || isFloat(l), rn = width(r) || isFloat(r);
    if (!ln || !rn) return l == r ? l : "";
    if (l == "double" || r == "double") return "double";
    if (l == "float" || r == "float") return "float";
    std::string a = promoted(l), b = promoted(r);
    if (a == b) return a;
    auto isUns = [](const std::string& t) { return t == "uint" || t == "uint32" || t == "uint64"; };
    int wa = width(a), wb = width(b);
    bool ua = isUns(a), ub = isUns(b);
    int w; bool u;
    if (ua == ub) { w = std::max(wa, wb); u = ua; }
    else {
        int wu = ua ? wa : wb, ws = ua ? wb : wa;
        u = wu >= ws; w = u ? wu : ws;
    }
    return w == 64 ? (u ? "uint64" : "int64") : (u ? "uint" : "int");
}

// The type a field of struct `key` is read as: a bitfield whose values all fit an int
// reads as `int` (C promotion), any other field as its declared type.
std::string CodeGen::bitfieldReadEskiu(const std::string& key, const std::string& member,
                                       const std::string& declType) const {
    auto lit = structLayout.find(key);
    if (lit == structLayout.end()) return declType;
    auto sit = lit->second.find(member);
    if (sit == lit->second.end() || !sit->second.isBitfield) return declType;
    return tyq::bitfieldReadType(expandAlias(declType), (int)sit->second.bitWidth) == "int" ? "int" : declType;
}

// A bitfield value loaded in its storage type, converted to the type it is read as.
llvm::Value* CodeGen::bitfieldReadValue(llvm::Value* v, const std::string& key, const std::string& member) {
    auto fit = structFields.find(key);
    if (fit == structFields.end()) return v;
    for (const auto& f : fit->second) {
        if (f.name != member) continue;
        if (bitfieldReadEskiu(key, member, tyq::strip(f.type)) != "int") return v;
        llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
        return v->getType() == i32 ? v : builder->CreateTrunc(v, i32);   // the value fits
    }
    return v;
}

std::string CodeGen::getExprEskiuType(const ExprPtr& expr) const {
    std::string t = getExprEskiuTypeRaw(expr);
    return promotedResultType(expr, expandAlias(t)) == "int" ? "int" : t;
}

std::string CodeGen::getExprEskiuTypeRaw(const ExprPtr& expr) const {
    // Single resolver: prefer the post-transform type checker's resolved type.
    if (resolvedExprTypes) {
        auto it = resolvedExprTypes->find(expr.get());
        if (it != resolvedExprTypes->end() && it->second != "unknown") {
            // [diagnostic] cross-check: the structural derivation must AGREE with
            // the resolver table (a disagreement is a latent two-evaluator bug, of
            // the class fixed in v0.2.4). Off unless ESKIU_RESOLVER_DEBUG is set.
            if (std::getenv("ESKIU_RESOLVER_DEBUG")) {
                std::string d = deriveExprEskiuType(expr);
                // Normalize away benign representational differences (alias spelling,
                // and the `struct:`/`interface:` tag the resolver carries but
                // derivation doesn't) so only semantically meaningful disagreements
                // — a real two-evaluator divergence — are reported.
                auto norm = [&](std::string s) {
                    s = expandAlias(s);
                    for (const char* tag : {"struct:", "interface:"}) {
                        size_t p;
                        while ((p = s.find(tag)) != std::string::npos)
                            s.erase(p, std::string(tag).size());
                    }
                    return s;
                };
                if (!d.empty() && d != "unknown" && norm(d) != norm(it->second)) {
                    const Expr& node = *expr;
                    fprintf(stderr, "[resolver-disagree] table=%s derive=%s %s\n",
                            it->second.c_str(), d.c_str(), typeid(node).name());
                }
            }
            // Codegen treats a nullable `?*T` exactly like `*T` (same repr); the `?`
            // only governs sema deref-safety, so strip it here. Likewise const (a
            // `const P*` parameter), which has no representation.
            std::string r = tyq::strip(it->second);
            if (!r.empty() && r[0] == '?') r = r.substr(1);
            // An alias is its target for every shape check (a pointer, array, slice, fn or
            // interface alias indexes, derefs, calls and boxes as the target does).
            std::string ex = expandAlias(r);
            if (!ex.empty() && ex[0] == '?') ex = ex.substr(1);
            return ex;
        }
        // A table miss is by design — the resolver doesn't annotate every expr, so
        // the structural derivation below legitimately carries the rest. (Only a
        // table/derivation *disagreement* above is a bug.)
    }
    std::string r = deriveExprEskiuType(expr);
    return (!r.empty() && r[0] == '?') ? r.substr(1) : r;   // `?*T` codegens as `*T`
}

std::string CodeGen::deriveExprEskiuType(const ExprPtr& expr) const {
    if (chainTypeMemo) {
        auto it = chainTypeMemo->find(expr.get());
        if (it != chainTypeMemo->end()) {
            if (!it->second) {
                std::string t = deriveExprEskiuTypeUncached(expr);
                (*chainTypeMemo)[expr.get()] = t;
                return t;
            }
            return *it->second;
        }
    }
    return deriveExprEskiuTypeUncached(expr);
}

std::string CodeGen::deriveExprEskiuTypeUncached(const ExprPtr& expr) const {
    if (auto ident = dynamic_cast<IdentExpr*>(expr.get())) {
        return expandAlias(lookupVarType(ident->name));
    }
    // A cast's static type IS the cast target — important so that pointer
    // arithmetic on e.g. (*uint8)structPtr uses a byte stride, not the struct's.
    if (auto cast = dynamic_cast<CastExpr*>(expr.get())) {
        return expandAlias(cast->targetType);
    }
    if (auto lit = dynamic_cast<LiteralExpr*>(expr.get())) {
        switch (lit->kind) {
            case LiteralExpr::Kind::INT:    return "int";
            case LiteralExpr::Kind::FLOAT:  return "double"; // float literals are double
            case LiteralExpr::Kind::STRING: return "string";
            case LiteralExpr::Kind::CHAR:   return "char";
            case LiteralExpr::Kind::BOOL:   return "bool";
            default: return "";
        }
    }
    if (auto member = dynamic_cast<MemberExpr*>(expr.get())) {
        std::string base = getExprEskiuType(member->base);
        if (member->member == "len" && ty::Type::parse(base).kind == ty::Type::Kind::Slice)
            return "int64";                                   // slice length field
        if (!base.empty() && base.front() == '?') base = base.substr(1);
        while (!base.empty() && base.front() == '*') base = base.substr(1);   // `*struct:T`: the star first
        if (base.size() > 7 && base.substr(0, 7) == "struct:") base = base.substr(7);
        while (!base.empty() && base.back()  == '*') base.pop_back();
        if (base.find('<') != std::string::npos) base = mangleTemplate(base);
        auto it = structFields.find(base);
        if (it != structFields.end()) {
            for (const auto& f : it->second) {
                if (f.name == member->member) return bitfieldReadEskiu(base, f.name, tyq::strip(f.type));
            }
        }
    }
    if (auto bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        if (bin->opFunc.empty() && (bin->op == "&&" || bin->op == "||")) return "bool";
        // Each operand's type is derived once (a deep chain must stay linear).
        std::string lt, rt, fn = bin->opFunc;
        if (fn.empty()) {
            lt = expandAlias(getExprEskiuType(bin->left));
            rt = expandAlias(getExprEskiuType(bin->right));
            fn = resolveOpInTemplateTypes(bin->op, {lt, rt});
        }
        auto it = funcEskiuReturnType.find(fn);
        if (!fn.empty() && it != funcEskiuReturnType.end()) return expandAlias(it->second);
        if (fn.empty()) return builtinBinaryType(lt, bin->op, rt);
    }
    if (auto unary = dynamic_cast<UnaryExpr*>(expr.get())) {
        if (unary->op == "-" || unary->op == "!" || unary->op == "~") {
            std::string t, fn = unary->opFunc;
            if (fn.empty()) {
                t = expandAlias(getExprEskiuType(unary->operand));
                fn = resolveOpInTemplateTypes(unary->op == "-" ? "u-" : unary->op, {t});
            }
            auto it = funcEskiuReturnType.find(fn);
            if (!fn.empty() && it != funcEskiuReturnType.end()) return expandAlias(it->second);
            if (fn.empty()) return unary->op == "!" ? "bool" : builtinBinaryType(t, "+", t);
        }
        if (unary->op == "&") return "*" + getExprEskiuType(unary->operand);
        if (unary->op == "*") {
            std::string t = getExprEskiuType(unary->operand);
            return tyq::isPtr(t) ? tyq::pointee(tyq::strip(t)) : "";
        }
    }
    if (auto index = dynamic_cast<IndexExpr*>(expr.get())) {
        std::string base = getExprEskiuType(index->base);
        ty::Type bt = ty::Type::parse(base);
        std::string elem;
        if (base == "string") elem = "char";
        else if (bt.kind == ty::Type::Kind::Array || bt.kind == ty::Type::Kind::Slice) elem = bt.elem->str();
        else if (!base.empty() && base.front() == '*') elem = base.substr(1);
        else if (!base.empty() && base.back()  == '*') elem = base.substr(0, base.size() - 1);
        if (!elem.empty() && !index->highIndex) return expandAlias(elem);   // `Mat[4]`: a row is an array
        if (!elem.empty()) return elem + "[]";                                // a slice
    }
    // A ternary's static type is the common type of its arms (the type checker's
    // resolver table usually supplies it; this is the structural fallback).
    if (auto tern = dynamic_cast<TernaryExpr*>(expr.get())) {
        std::string tt = getExprEskiuType(tern->thenExpr);
        std::string et = getExprEskiuType(tern->elseExpr);
        if (tt == et || et.empty() || et == "unknown") return tt;
        if (tt.empty() || tt == "unknown") return et;
        return tt;
    }
    // A call's static type is its function's return type — needed so member
    // access on a temporary (e.g. make().x) can resolve the struct.
    if (auto call = dynamic_cast<CallExpr*>(expr.get())) {
        if (auto id = dynamic_cast<IdentExpr*>(call->callee.get())) {
            if (id->name == "exception_text" && call->args.empty() && !funcEskiuReturnType.count("exception_text"))
                return "string";
            auto it = funcEskiuReturnType.find(id->name);
            if (it != funcEskiuReturnType.end()) return expandAlias(it->second);
        } else if (auto m = dynamic_cast<MemberExpr*>(call->callee.get())) {
            std::string bt = getExprEskiuType(m->base);
            if (bt.size() > 7 && bt.substr(0, 7) == "struct:") bt = bt.substr(7);
            if (!bt.empty() && bt.front() == '*') bt = bt.substr(1);
            while (!bt.empty() && bt.back() == '*') bt.pop_back();
            if (bt.find('<') != std::string::npos) bt = mangleTemplate(bt);
            auto it = funcEskiuReturnType.find(bt + "_" + m->member);
            if (it != funcEskiuReturnType.end()) return expandAlias(it->second);
            std::map<std::string, std::string> subs;
            if (FunctionDecl* gm = genericMethod(bt, m->member, &subs))
                return expandAlias(substType(gm->returnType, subs));
            if (FunctionDecl* gf = genericFreeMethod(bt, m->member))
                return expandAlias(substType(gf->returnType,
                                             genericFreeMethodSubs(gf, getExprEskiuType(m->base), call->args)));
        }
        return "";
    }
    if (auto tc = dynamic_cast<TemplateCallExpr*>(expr.get())) {
        auto td = funcTemplateDecls.find(tc->templateName);
        if (td != funcTemplateDecls.end()) {
            auto& tp = td->second->typeParams;
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < tp.size() && i < tc->typeArgs.size(); ++i)
                subs[tp[i]] = tc->typeArgs[i];
            return expandAlias(substType(td->second->returnType, subs));
        }
        return "";
    }
    if (auto si = dynamic_cast<StructInitExpr*>(expr.get())) {
        return si->structName;
    }
    return "";
}
