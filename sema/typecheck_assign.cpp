// typecheck_assign.cpp: numeric types, assignability, operator resolution and type spellings (split from typecheck_type.cpp).
#include "type_checker.h"
#include <set>
#include <algorithm>
#include <cctype>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include "../ast/type_qual.h"

bool TypeChecker::isNumericType(const std::string& type) {
    return isIntType(type) || isFloatType(type);
}

bool TypeChecker::isNarrowingNumeric(const std::string& lhsType, const std::string& rhsType) {
    // C-aligned: integer-width narrowing (int64 -> int, int -> uint8) and float-width
    // narrowing (double -> float) are implicit, as in C. The one conversion Eskiu
    // rejects is float/double -> integer, which silently drops the fractional part
    // (`int x = 3.9` giving 3) -- the case C itself flags under -Wall. An out-of-range
    // integer *literal* is caught separately at the assignment sites.
    return isFloatType(rhsType) && isIntType(lhsType);
}

std::string TypeChecker::assignabilityError(const std::string& targetIn,
                                            const std::string& srcIn, Expr* srcExpr) {
    const std::string targetType = nullableAliasTarget(targetIn), srcType = nullableAliasTarget(srcIn);
    if (srcType == "unknown" || targetType.empty() || targetType == "unknown") return "";
    // Nullable-pointer rules (opt-in null safety): a `?*T` behaves like `*T` for
    // assignment except that assigning a nullable pointer to a non-null one drops the
    // check, so it requires an explicit narrowing (or cast). `*T`/`null` -> `?*T` is fine.
    {
        bool tNull = !targetType.empty() && targetType[0] == '?';
        bool sNull = !srcType.empty()    && srcType[0]    == '?';
        if (tNull || sNull) {
            std::string t2 = tNull ? targetType.substr(1) : targetType;
            std::string s2 = sNull ? srcType.substr(1)    : srcType;
            // An interface value refers to its target through a non-null pointer too.
            if (sNull && !tNull && (isPointerType(t2) || interfaceDecls.count(normalizeType(t2))))
                return "assigning a nullable pointer '" + srcType + "' to non-null '" +
                       targetType + "' requires a null-check (e.g. `if (x != null)`)";
            return assignabilityError(t2, s2, srcExpr);
        }
    }
    // A pointer to a nullable pointer (`*?*T`) keeps the check on what it points to:
    // converting it to `**T` would let a null be read as non-null.
    if (targetType.size() > 1 && srcType.size() > 1 && targetType[0] == '*' && srcType[0] == '*' &&
        srcType[1] == '?' && targetType[1] != '?' && isPointerType(targetType.substr(1)))
        return "conversion drops the nullable pointee of '" + srcType + "' ('" + targetType +
               "' points to a non-null pointer)";
    std::string nt = normalizeType(targetType), ns = normalizeType(srcType);
    if (nt == "unknown" || ns == "unknown") return "";   // an already-reported bad type
    // A type already reported as unknown (`Nope f(Zip z)`) must not cascade into a
    // conversion error at every use.
    if (!unknownTypes.empty() &&
        (unknownTypes.count(ty::Type::parse(nt).nominalName()) || unknownTypes.count(ty::Type::parse(ns).nominalName())))
        return "";
    // An integer literal that provably does not fit the target is rejected even though
    // integer-width narrowing is otherwise implicit (its value is statically known).
    if (isIntType(nt)) {
        if (auto* l = nodeCast<LiteralExpr>(srcExpr);
            l && l->kind == LiteralExpr::Kind::INT && !intLiteralFits(nt, srcExpr))
            return "integer literal " + l->value + " is out of range for '" + targetType + "'";
        // Either arm of a `?:` is the assigned value, so each literal arm must fit too.
        if (auto* t = nodeCast<TernaryExpr>(srcExpr))
            for (Expr* arm : {t->thenExpr.get(), t->elseExpr.get()}) {
                std::string e = assignabilityError(targetType, getExpressionType(arm), arm);
                if (!e.empty() && nodeCast<LiteralExpr>(arm)) return e;
            }
    }
    if (isValidAssignment(targetType, srcType)) return "";
    ty::Type lt = ty::Type::parse(nt);
    ty::Type rt = ty::Type::parse(ns);
    if (lt.isFn() || rt.isFn())
        return "incompatible function type '" + srcType + "' for '" + targetType + "'";
    // The only remaining numeric mismatch is float/double -> integer: it drops the
    // fractional part, so require an explicit cast (integer/float-width narrowing is
    // allowed by isValidAssignment above, C-style).
    if (isNumericType(nt) && isNumericType(ns))
        return "cannot assign a floating-point value ('" + srcType + "') to integer type '" +
               targetType + "' without an explicit cast (it drops the fraction)";
    if (dropsConstQual(targetType, srcType))
        return "conversion discards a const qualifier ('" + srcType + "' to '" + targetType + "')";
    if (interfaceDecls.count(nt) && !isPointerType(ns) && structs.count(rt.nominalName()))
        return "cannot convert struct '" + rt.nominalName() + "' to interface '" + nt +
               "' by value; pass a pointer (&x)";
    if (auto ii = interfaceDecls.find(nt); ii != interfaceDecls.end()) {
        if (isPointerType(ns) && pointerDepth(ns) != 1)
            return "cannot convert '" + srcType + "' to interface '" + targetType +
                   "': only a pointer to a struct (a single '*') converts to an interface";
        std::string m = ifaceConstDrop(srcType, rt.nominalName(), ii->second);
        if (!m.empty())
            return "conversion discards a const qualifier ('" + srcType + "' to '" + targetType +
                   "'): method '" + m + "' of '" + rt.nominalName() + "' takes a mutable self";
        std::string why = interfaceMismatch(rt.nominalName(), ii->second);
        if (!why.empty())
            return "'" + srcType + "' does not satisfy interface '" + targetType + "': " + why;
    }
    return "cannot convert '" + srcType + "' to '" + targetType + "'";
}

// Resolve `op` (a binary/unary/index op spelling) over the given operand types to a user
// `operator` overload. A candidate matches when the arity agrees and every operand is
// assignable to the matching param (so `V3 * 2.0` picks `operator *(V3, float)` via the
// usual double->float coercion). Returns the operator function's name and sets `outRet` to
// its return type, or "" if no overload matches.
std::string TypeChecker::resolveOperator(const std::string& op,
                                         const std::vector<std::string>& argTypes,
                                         std::string& outRet) {
    auto it = operatorOverloads.find(op);
    if (it == operatorOverloads.end()) return "";
    for (const auto& c : it->second) {
        if (c.params.size() != argTypes.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < argTypes.size(); ++i)
            if (!assignabilityError(c.params[i], argTypes[i], nullptr).empty()) { ok = false; break; }
        if (ok) { outRet = c.retType; return c.fnName; }
    }
    return "";
}

bool TypeChecker::intLiteralFits(const std::string& targetType, Expr* e) {
    auto* lit = nodeCast<LiteralExpr>(e);
    if (!lit || lit->kind != LiteralExpr::Kind::INT) return false;
    std::string t = tyq::strip(targetType);
    bool neg = !lit->value.empty() && lit->value[0] == '-';
    unsigned long long mag = 0;
    try { mag = std::stoull(neg ? lit->value.substr(1) : lit->value, nullptr, 0); }
    catch (...) { return false; }
    // `mag` parsed, so it is at most 2^64-1: a uint64 holds it, an int64 only up to 2^63-1.
    if (t == "uint64") return !neg;
    if (t == "int64")  return neg ? mag <= 9223372036854775808ULL : mag <= 9223372036854775807ULL;
    // Unsigned targets take no negative literal.
    if (t=="bool")   return !neg && mag <= 1ULL;
    if (t=="uint8"||t=="char") return !neg && mag <= 255ULL;
    if (t=="uint16") return !neg && mag <= 65535ULL;
    if (t=="uint"||t=="uint32") return !neg && mag <= 4294967295ULL;
    if (t=="int8")   return neg ? mag <= 128ULL       : mag <= 127ULL;
    if (t=="int16")  return neg ? mag <= 32768ULL     : mag <= 32767ULL;
    if (t=="int"||t=="int32") return neg ? mag <= 2147483648ULL : mag <= 2147483647ULL;
    return false;
}

bool TypeChecker::isIntType(const std::string& rawType) {
    std::string type = tyq::strip(rawType);
    return type == "int"   || type == "int8"  || type == "int16"  ||
           type == "int32" || type == "int64" ||
           type == "uint"  || type == "uint8" || type == "uint16" ||
           type == "uint32"|| type == "uint64"||
           type == "char"  || type == "bool";
}

bool TypeChecker::isFloatType(const std::string& rawType) {
    std::string type = tyq::strip(rawType);
    return type == "float" || type == "double";
}

bool TypeChecker::isPrimitiveType(const std::string& rawType) {
    std::string type = tyq::strip(rawType);
    if (type.size() > 3 && type.substr(0, 3) == "fn(") return true;
    return isNumericType(type) || type == "void" || type == "string";
}

bool TypeChecker::isPointerType(const std::string& rawType) {
    std::string type = tyq::strip(rawType);
    // An array suffix binds tighter than a leading star: `*T[N]` is an array of pointers.
    if (!type.empty() && type.back() == ']') return false;
    return !type.empty() && (type[0] == '*' || type.back() == '*' || type == "string");
}

std::string TypeChecker::getPointeeType(const std::string& pointerType) {
    // Accept both pointer spellings: *T (canonical) and T* (trailing-star).
    // The pointee's own const is preserved (a `const int*` derefs to `const int`).
    if (pointerType.size() >= 6 && pointerType.compare(pointerType.size() - 6, 6, "*const") == 0)
        return pointerType.substr(0, pointerType.size() - 6);
    if (!pointerType.empty() && pointerType.back() == ']') return "";   // `*T[N]`: an array
    if (!pointerType.empty() && pointerType.back() == '*')
        return pointerType.substr(0, pointerType.size() - 1);
    // leading-star spelling: const sits before the star(s), e.g. "const *int"
    if (!pointerType.empty() && pointerType.front() == '*')
        return pointerType.substr(1);
    if (tyq::baseConst(pointerType)) {
        std::string inner = pointerType.substr(6);
        if (!inner.empty() && inner.front() == '*') return "const " + inner.substr(1);
    }
    return "";
}

// Type normalization
void TypeChecker::unifyTypeParam(std::string pattern, std::string concrete,
                                 const std::set<std::string>& tps,
                                 std::map<std::string, std::string>& subs) {
    auto stripStruct = [](std::string s) {
        return s.rfind("struct:", 0) == 0 ? s.substr(7) : s;
    };
    auto canon = [](std::string t) {           // move trailing '*' to leading
        int stars = 0;
        while (!t.empty() && t.back()  == '*') { t.pop_back();    stars++; }
        while (!t.empty() && t.front() == '*') { t = t.substr(1); stars++; }
        return std::string(stars, '*') + t;
    };
    // const has no bearing on the shape (a `const Box<T>* self` binds T from a
    // `const Box<int>*` receiver), so unify the stripped spellings.
    pattern  = canon(stripStruct(tyq::strip(pattern)));
    concrete = canon(stripStruct(tyq::strip(concrete)));
    size_t pi = 0, ci = 0;
    while (pi < pattern.size() && pattern[pi] == '*' &&
           ci < concrete.size() && concrete[ci] == '*') { pi++; ci++; }
    pattern = pattern.substr(pi);
    concrete = stripStruct(concrete.substr(ci));
    if (pattern.empty() || concrete.empty()) return;

    if (tps.count(pattern)) {                   // bare type parameter
        if (!subs.count(pattern)) subs[pattern] = concrete;
        return;
    }
    auto isSlice = [](const std::string& t) {
        return t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0;
    };
    if (isSlice(pattern)) {                     // `T[]` against a slice binds the element
        if (isSlice(concrete))
            unifyTypeParam(pattern.substr(0, pattern.size() - 2),
                           concrete.substr(0, concrete.size() - 2), tps, subs);
        return;
    }
    if (pattern.find('<') == std::string::npos) return;
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

std::string TypeChecker::normalizeType(const std::string& rawType) {
    // const has no bearing on identity/layout — strip it so the rest of the
    // type machinery is const-agnostic. (const survives only in stored declared
    // types, read back by the const-correctness checks.)
    std::string type = tyq::strip(rawType);
    // A type alias inside a fn type is its target (`fn(int)->IF` is `fn(int)->int`), so
    // both spellings are one type. A template instance (`Box<F>`) keeps its argument
    // spellings for substitution (an array alias keeps its grouping: `*T` with `T = IS`
    // is a pointer to the slice) and is named through mangleTemplate, which dealiases.
    if (!typeAliases.empty() && type.find("fn(") != std::string::npos && ty::Type::parse(type).isFn())
        type = ty::dealiasSpelling(type, typeAliases);
    // Inside a generic instance, the template's type parameters name its concrete
    // arguments. Substituted once, at the outermost call (the arguments are concrete).
    if (inInstance && !substituting && !instSubs.empty() &&
        std::any_of(instSubs.begin(), instSubs.end(),
                    [&](const auto& kv) { return type.find(kv.first) != std::string::npos; })) {
        type = substType(type, instSubs);
        substituting = true;
        std::string r = normalizeType(type);
        substituting = false;
        return r;
    }
    if (hasPointerSuffix(type)) {
        return addPointerSuffix(normalizeType(extractBaseType(type)));
    }
    // An array or slice of a template instance (`Box<int>[4]`, `Box<int>*[4]`): the
    // element is the instance, so normalize it in place instead of mangling the whole
    // spelling (brackets and all) as if it were one template name.
    if (!type.empty() && type.back() == ']' && type.find('<') != std::string::npos) {
        ty::Type t = ty::Type::parse(type);
        if ((t.kind == ty::Type::Kind::Array || t.kind == ty::Type::Kind::Slice) && t.elem) {
            t.elem = std::make_shared<ty::Type>(ty::Type::parse(normalizeType(t.elem->str())));
            return t.str();
        }
    }
    // Resolve a type alias to its underlying type.
    if (auto it = typeAliases.find(type); it != typeAliases.end())
        return normalizeType(it->second);
    // A classic enum is an integer; an algebraic enum is its own value type.
    if (adtEnums.count(type)) return type;
    if (enumTypes.count(type)) return "int";
    if (type.find("struct:") == 0 || type.find("interface:") == 0) {
        return type;
    }
    // A template argument may arrive already resolved (`Future<struct:V>`, from a type
    // the checker produced): it names the same instance as the source spelling.
    if (type.find('<') != std::string::npos) {
        for (const char* tag : {"struct:", "interface:"})
            for (size_t p; (p = type.find(tag)) != std::string::npos; )
                type.erase(p, std::string(tag).size());
    }
    // Generic algebraic enum instance: Option<int> → the value type "Option_int".
    if (type.find('<') != std::string::npos) {
        auto [gname, gargs] = splitTemplateType(type);
        if (genericEnumDecls.count(gname)) {
            std::string mangled = mangleTemplate(type);
            templateInstanceArgs[mangled] = {gname, gargs};   // resolved back for match/construction
            adtEnums.insert(mangled);                          // a distinct value type
            return mangled;
        }
    }
    // Template instantiation: Result<int,string> → struct:Result_int_string
    if (type.find('<') != std::string::npos) {
        auto [tname, args] = splitTemplateType(type);
        auto templ = templateDecls.find(tname);
        if (templ != templateDecls.end()) {
            std::string mangled = mangleTemplate(type);
            templateInstanceArgs[mangled] = {tname, args};  // for type-arg inference
            // Instantiate if not already done
            if (structs.find(mangled) == structs.end()) {
                auto& tp = templ->second->typeParams;
                std::map<std::string, std::string> subs;
                for (size_t i = 0; i < tp.size() && i < args.size(); ++i)
                    subs[tp[i]] = args[i];
                StructInfo info;
                info.name = mangled;
                for (const auto& f : templ->second->fields) {
                    StructDecl::Field inst = f;   // keeps the bit width and position
                    inst.type = substType(f.type, subs);
                    info.fields.push_back(inst);
                }
                info.pads = templ->second->pads;
                info.packAlign = templ->second->isPacked ? 1 : templ->second->packAlign;
                structs[mangled] = info;
                // Inline methods of a generic struct become `Box_int_get(*Box_int self, ...)`
                // per instance. Like codegen, which emits one on its first call, a method's
                // body is checked (with the instance's type arguments) once it is called.
                for (const auto& m : templ->second->methods) {
                    auto* mf = nodeCast<FunctionDecl>(m.get());
                    if (!mf) continue;
                    std::vector<std::string> pts{"*" + mangled};
                    for (const auto& p : mf->params) pts.push_back(substType(p.first, subs));
                    defineFunction(mangled + "_" + mf->name, substType(mf->returnType, subs), pts);
                    genericMethodInsts[mangled + "_" + mf->name] = {mf, templ->second, subs};
                }
                // Bounded generics on a struct template (`Map<K: Hashable, V>`):
                // verify the type args satisfy their constraints, once per instance.
                checkConstraints(nullptr, templ->second->constraints, subs);
                // A bitfield typed by a type parameter is checked with the instance's
                // type argument (the template declaration skips it).
                for (const auto& f : templ->second->fields) {
                    if (f.bitWidth <= 0 || !subs.count(f.type)) continue;
                    StructDecl::Field inst = f;
                    inst.type = subs[f.type];
                    checkBitfield(nullptr, tname, inst);
                }
            }
            return "struct:" + mangled;
        }
        return type; // unknown template — will error elsewhere
    }
    if (structs.find(type) != structs.end()) {
        return "struct:" + type;
    }
    return type;
}

// Type promotion
std::string TypeChecker::promoteType(const std::string& raw1, const std::string& raw2) {
    std::string type1 = tyq::strip(raw1), type2 = tyq::strip(raw2);
    if (type1 == type2) return type1;
    if (type1 == "double"  || type2 == "double")  return "double";
    if (type1 == "float"   || type2 == "float")   return "float";
    // C's usual arithmetic conversions once an operand is int-sized or wider: a narrow
    // operand counts as int, and a signed/unsigned pair is unsigned unless the signed
    // type is wider (int32 + uint32 is uint32, int64 + uint64 is uint64, int64 + uint32
    // is int64). Two narrow operands keep the legacy spelling (codegen reports it as int).
    auto width = [&](const std::string& t) -> int {
        if (t == "int64" || t == "uint64") return 64;
        if (t == "int" || t == "int32" || t == "uint" || t == "uint32") return 32;
        return isIntType(t) ? 16 : 0;
    };
    int w1 = width(type1), w2 = width(type2);
    if (w1 && w2 && (w1 >= 32 || w2 >= 32)) {
        auto isUns = [](const std::string& t) { return t == "uint" || t == "uint32" || t == "uint64"; };
        bool u1 = isUns(type1), u2 = isUns(type2);
        w1 = std::max(w1, 32); w2 = std::max(w2, 32);
        int w; bool u;
        if (u1 == u2) { w = std::max(w1, w2); u = u1; }
        else {
            int wu = u1 ? w1 : w2, ws = u1 ? w2 : w1;
            u = wu >= ws; w = u ? wu : ws;
        }
        return w == 64 ? (u ? "uint64" : "int64") : (u ? "uint32" : "int32");
    }
    if (type1 == "int64"   || type2 == "int64")   return "int64";
    if (type1 == "uint64"  || type2 == "uint64")  return "uint64";
    if (type1 == "int32"   || type2 == "int32")   return "int32";
    if (type1 == "uint32"  || type2 == "uint32")  return "uint32";
    if (type1 == "int16"   || type2 == "int16")   return "int16";
    if (type1 == "uint16"  || type2 == "uint16")  return "uint16";
    return type1;
}

// Pointer type utilities
bool TypeChecker::hasPointerSuffix(const std::string& type) const {
    return !type.empty() && type.back() == '*';
}

std::string TypeChecker::extractBaseType(const std::string& pointerType) const {
    if (hasPointerSuffix(pointerType)) {
        // Remove the trailing '*'
        return pointerType.substr(0, pointerType.length() - 1);
    }
    return pointerType;
}

std::string TypeChecker::addPointerSuffix(const std::string& baseType) const {
    return baseType + "*";
}
