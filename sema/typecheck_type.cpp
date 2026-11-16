#include "type_checker.h"
#include <set>
#include <algorithm>
#include <cctype>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include "../ast/type_qual.h"

// ============================================================================

// TypeChecker — the type system: inference, normalization, validation,
// promotion, interface-satisfaction + constraint checking.
// Part of the type_checker.cpp split; see type_checker.h.

// Type inference
std::string TypeChecker::inferBinaryExprType(const std::string& leftIn, const std::string& op,
                                             const std::string& rightIn) {
    // A nullable `?*T` compares/operates like `*T` (the `?` only governs deref-safety),
    // so strip a leading `?` from either operand before inference.
    std::string leftType  = (!leftIn.empty()  && leftIn[0]  == '?') ? leftIn.substr(1)  : leftIn;
    std::string rightType = (!rightIn.empty() && rightIn[0] == '?') ? rightIn.substr(1) : rightIn;
    if (op != "=") {
        leftType = dealiasOperand(leftType);
        rightType = dealiasOperand(rightType);
        if (!leftType.empty() && leftType[0] == '?') leftType = leftType.substr(1);
        if (!rightType.empty() && rightType[0] == '?') rightType = rightType.substr(1);
    }
    if (op == "=") {
        return isValidAssignment(leftType, rightType) ? leftType : "error";
    }
    // A classic enum value is an int in arithmetic, bitwise and comparison operators.
    leftType = plainEnumAsInt(leftType);
    rightType = plainEnumAsInt(rightType);
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        // Operands must be mutually comparable: both numeric, both pointer-like
        // (including `null`), or the same non-aggregate type. Rejecting the rest
        // keeps codegen from emitting an `icmp`/`fcmp` on mismatched types (an
        // assertion / miscompile) or a meaningless comparison of aggregates.
        std::string l = normalizeType(leftType), r = normalizeType(rightType);
        if (l == "error" || r == "error" || l == "unknown" || r == "unknown")
            return "bool";                       // do not cascade a prior error
        if (l == "void" || r == "void") return "error";   // a void call has no value
        auto ptrish = [&](const std::string& t) { return isPointerType(t) || t == "null"; };
        // An interface value compares with `null` (its data pointer is null or set).
        if ((op == "==" || op == "!=") &&
            ((interfaceDecls.count(l) && r == "null") || (l == "null" && interfaceDecls.count(r))))
            return "bool";
        // Aggregates (structs, unions, sum types, interface values, arrays, slices,
        // closures) have no built-in comparison (only a user `operator ==`).
        if (isAggregateValue(l) || isAggregateValue(r)) return "error";
        bool ok = (isNumericType(l) && isNumericType(r)) ||
                  (ptrish(l) && ptrish(r)) ||
                  (l == r);
        return ok ? "bool" : "error";
    }
    if (op == "&&" || op == "||") {
        // The operands are truth values: a scalar (number, bool, pointer), not an aggregate.
        if (!isTruthValue(normalizeType(leftType)) || !isTruthValue(normalizeType(rightType))) return "error";
        if (normalizeType(leftType) == "void" || normalizeType(rightType) == "void") return "error";
        return "bool";
    }
    // Bitwise and shift operators work on integers
    if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
        if (!isIntType(leftType) || !isIntType(rightType)) return "error";
        // A shift has the (promoted) type of its left operand alone, as in C.
        if (op == "<<" || op == ">>") return intPromoted(leftType);
        return promoteType(intPromoted(leftType), intPromoted(rightType));
    }
    // Pointer arithmetic: ptr + int / ptr - int → ptr; ptr - ptr → int64
    if (op == "-" && isPointerType(leftType) && isPointerType(rightType)) return "int64";
    if ((op == "+" || op == "-") && isPointerType(leftType) && isIntType(rightType)) {
        return leftType;
    }
    if (!isNumericType(leftType) || !isNumericType(rightType)) {
        return "error";
    }
    return promoteType(intPromoted(leftType), intPromoted(rightType));
}

// C integer promotion: an integer type narrower than int (bool, char, int8/16,
// uint8/16) is int in arithmetic; every other type is itself.
std::string TypeChecker::intPromoted(const std::string& raw) {
    std::string t = tyq::strip(raw);
    if (t == "bool" || t == "char" || t == "int8" || t == "uint8" || t == "int16" || t == "uint16")
        return "int32";
    return t;
}

// A classic (payload-less) enum type spelled by name is `int` as an operand.
std::string TypeChecker::plainEnumAsInt(const std::string& type) {
    return plainEnumDecls.count(tyq::strip(type)) ? std::string("int") : type;
}

std::string TypeChecker::dealiasOperand(const std::string& type) {
    std::string cur = tyq::strip(type);
    bool changed = false;
    for (int guard = 0; guard < 64; ++guard) {
        auto it = typeAliases.find(cur);
        if (it == typeAliases.end()) break;
        cur = tyq::strip(it->second);
        changed = true;
    }
    return changed ? cur : type;
}

std::string TypeChecker::inferUnaryExprType(const std::string& op, const std::string& operandIn) {
    // A `?*T` derefs like `*T` (deref-safety is enforced separately by checkNullableDeref).
    std::string operandType = (!operandIn.empty() && operandIn[0] == '?') ? operandIn.substr(1) : operandIn;
    if (op != "&") {
        operandType = dealiasOperand(operandType);
        if (!operandType.empty() && operandType[0] == '?') operandType = operandType.substr(1);
        operandType = plainEnumAsInt(operandType);
    }
    if (op == "!") {
        // Logical not of a scalar (number, bool, pointer). A struct operand is not a
        // truth value: "error" here lets a user `operator !(V)` resolve instead.
        std::string n = normalizeType(operandType);
        if (!isTruthValue(n) || isVoidValueType(n)) return "error";   // `!v()` of a void call
        return "bool";
    }
    if (op == "-" || op == "+") {
        if (isNumericType(operandType)) {
            return intPromoted(operandType);   // C: unary `+` and `-` promote a narrow operand
        }
        return "error";
    }
    if (op == "~") {
        if (isIntType(operandType)) return intPromoted(operandType);
        return "error";
    }
    if (op == "&") {
        // A leading star would bind looser than an array suffix (`*int[3]` is an array of
        // pointers), so the address of an array (or slice) takes the trailing spelling.
        std::string st = tyq::strip(operandType);
        if (!st.empty() && st.back() == ']') return operandType + "*";
        return "*" + operandType;
    }
    if (op == "*") {
        if (tyq::strip(operandType) == "string") return "char";   // a string is a char pointer
        if (isPointerType(operandType)) {
            return getPointeeType(operandType);
        }
        return "error";
    }
    return "error";
}

// A (normalized) value type with no scalar meaning: a struct/union, a sum type, an
// interface value, a fixed array, a slice, or a closure. Such a value is not a truth
// value and has no built-in comparison.
bool TypeChecker::isAggregateValue(const std::string& t) {
    ty::Type pt = ty::Type::parse(t);
    if (pt.kind == ty::Type::Kind::Array || pt.kind == ty::Type::Kind::Slice || pt.isFn()) return true;
    if (pt.kind == ty::Type::Kind::Pointer || isPointerType(t)) return false;
    if (t.rfind("struct:", 0) == 0 || t.rfind("interface:", 0) == 0) return true;
    return adtEnums.count(t) || interfaceDecls.count(t);
}

// A (normalized) type usable as a truth value: a scalar, or an interface value (true when
// it refers to something, false when it is null).
bool TypeChecker::isTruthValue(const std::string& t) {
    return !isAggregateValue(t) || interfaceDecls.count(t);
}

// Type validation
bool TypeChecker::isVoidValueType(const std::string& type) {
    ty::Type t = ty::Type::parse(normalizeType(type));
    while ((t.kind == ty::Type::Kind::Array || t.kind == ty::Type::Kind::Slice) && t.elem) { ty::Type e = *t.elem; t = e; }
    return t.kind == ty::Type::Kind::Void;
}

std::string TypeChecker::voidTypeError(const std::string& type) {
    if (isVoidValueType(type)) return "cannot have type 'void'";
    // A generic instance whose type argument makes a field a `void` value (`Box<void>`).
    ty::Type t = ty::Type::parse(normalizeType(type));
    while ((t.kind == ty::Type::Kind::Array || t.kind == ty::Type::Kind::Slice) && t.elem) { ty::Type e = *t.elem; t = e; }
    if (t.kind != ty::Type::Kind::Struct) return "";
    auto ia = templateInstanceArgs.find(t.name);
    if (ia == templateInstanceArgs.end()) return "";
    auto td = templateDecls.find(ia->second.first);
    if (td == templateDecls.end()) return "";
    std::map<std::string, std::string> subs;
    const auto& tp = td->second->typeParams;
    for (size_t i = 0; i < tp.size() && i < ia->second.second.size(); ++i) subs[tp[i]] = ia->second.second[i];
    for (const auto& f : td->second->fields)
        if (isVoidValueType(substType(f.type, subs)))
            return "cannot have type '" + type + "': its field '" + f.name + "' would be 'void'";
    return "";
}

void TypeChecker::validateStructType(const std::string& type, ASTNode* at) {
    // Function pointer types are always valid
    if (type.size() > 3 && type.substr(0, 3) == "fn(") return;
    if (type == "unknown") return;   // an already-reported bad type (e.g. a cyclic alias)
    std::string baseType = type;
    if (!baseType.empty() && baseType.front() == '?') {                                  // nullable `?*T`
        baseType = baseType.substr(1);
        if (!isPointerType(baseType)) {
            std::string msg = "a nullable type must be a pointer ('?*T'), got '" + type + "'";
            if (at) errorAt(at, msg); else error(0, 0, msg);
            return;
        }
    }
    // Strip fixed-size array suffixes (T[N], T[N][M], ...) — the element type is what
    // matters here; each dimension (a literal, enum, or const) is resolved in codegen.
    // Parsed once and walked, so a deep `T[1][1]...` costs time linear in its length.
    if (!baseType.empty() && baseType.back() == ']') {
        ty::Type t = ty::Type::parse(baseType);
        const ty::Type* cur = &t;
        while (cur->kind == ty::Type::Kind::Array || cur->kind == ty::Type::Kind::Slice) {
            if (cur->kind == ty::Type::Kind::Array && at) checkArrayDim(cur->dim, at);
            cur = cur->elem.get();
        }
        if (at && t.kind == ty::Type::Kind::Array) {
            unsigned long long sz = 0, al = 1;
            layoutTooLarge = false;
            constLayout(baseType, sz, al);
            if (layoutTooLarge) errorAt(at, "array is too large: '" + type + "' exceeds the largest object size");
            layoutTooLarge = false;
        }
        if (cur != &t) baseType = cur->str();
    }
    // Strip ALL pointer decorators (*T, T*, **T, etc.)
    bool stripped = true;
    while (stripped && !baseType.empty()) {
        stripped = false;
        if (hasPointerSuffix(baseType)) { baseType = extractBaseType(baseType); stripped = true; }
        else if (baseType.front() == '*' || baseType.front() == '?') { baseType = baseType.substr(1); stripped = true; }
    }
    // A leading-star generic pointee (`*List<int>`) reaches here unnormalized (normalizeType
    // only descends through a trailing star): resolve it now, instantiating the template.
    if (baseType.find('<') != std::string::npos) baseType = normalizeType(baseType);

    // A template instance (Pair<int,float> -> struct:Pair_int_float, Option<T> -> Option_T)
    // must supply exactly the template's type-parameter count, each a known type.
    {
        std::string inst = baseType.rfind("struct:", 0) == 0 ? baseType.substr(7) : baseType;
        auto ti = templateInstanceArgs.find(inst);
        if (ti != templateInstanceArgs.end()) {
            const std::string& tname = ti->second.first;
            const auto& args = ti->second.second;
            size_t want = 0;
            if (auto td = templateDecls.find(tname); td != templateDecls.end()) want = td->second->typeParams.size();
            else if (auto ge = genericEnumDecls.find(tname); ge != genericEnumDecls.end()) want = ge->second->typeParams.size();
            std::string msg;
            if (want && args.size() != want)
                msg = "'" + tname + "' expects " + std::to_string(want) + " type argument(s), got " +
                      std::to_string(args.size());
            if (!msg.empty()) { if (at) errorAt(at, msg); else errorAtCtx(msg); return; }
            for (const auto& a : args) validateStructType(normalizeType(a), at);
            return;
        }
    }

    // Check if it's an explicit struct type (struct: prefix)
    if (baseType.find("struct:") == 0) {
        // Extract struct name (remove "struct:" prefix)
        std::string structName = baseType.substr(7);  // strlen("struct:") = 7

        // Look up struct in registry
        if (structs.find(structName) == structs.end()) {
            unknownTypes.insert(structName);
            std::string hint = didYouMean(structName, 't');
            if (at) errorAt(at, "unknown type '" + structName + "'" + hint);
            else errorAtCtx("unknown type '" + structName + "'" + hint);
        }
    } else if (!isPrimitiveType(baseType) && baseType != "va_list") {
        // Valid if it's a known struct, type alias, or enum type — anything else
        // is an undefined type. (Aliases/enums are resolved later in codegen.)
        if (structs.find(baseType) == structs.end() &&
            typeAliases.find(baseType) == typeAliases.end() &&
            enumTypes.find(baseType) == enumTypes.end() &&
            adtEnums.find(baseType) == adtEnums.end() &&     // incl. generic enum instances
            interfaceDecls.find(baseType) == interfaceDecls.end()) {
            unknownTypes.insert(baseType);
            std::string hint = didYouMean(baseType, 't');
            if (at) errorAt(at, "unknown type '" + baseType + "'" + hint);
            else errorAtCtx("unknown type '" + baseType + "'" + hint);
        }
    }
}

// A fixed array's dimension (a number, an enum member, or a `const` int) must be
// positive, as in C: a zero or negative size has no layout.
// `sizeof(t)` for a scalar whose size is the same on every target (0 otherwise).
long long fixedScalarSize(const std::string& t) {
    if (t == "int8" || t == "uint8" || t == "char" || t == "bool") return 1;
    if (t == "int16" || t == "uint16") return 2;
    if (t == "int" || t == "int32" || t == "uint" || t == "uint32" || t == "float") return 4;
    if (t == "int64" || t == "uint64" || t == "double") return 8;
    return 0;
}

bool TypeChecker::constLayout(const std::string& t0, unsigned long long& size,
                              unsigned long long& align, int depth) {
    if (depth > 64) return false;
    if (!haveLayoutInfo) { layoutInfo = targetLayoutInfo(targetTriple); haveLayoutInfo = true; }
    const TargetLayoutInfo& L = layoutInfo;
    auto up = [](unsigned long long x, unsigned long long a) { return a ? (x + a - 1) / a * a : x; };
    std::string t = tyq::strip(normalizeType(t0));
    if (!t.empty() && t[0] == '?') t = t.substr(1);
    ty::Type pt = ty::Type::parse(t);
    using K = ty::Type::Kind;
    switch (pt.kind) {
        case K::Bool: case K::Char: size = 1; align = 1; return true;
        case K::Int: case K::Float: {
            long long s = fixedScalarSize(pt.name);
            if (s == 0) return false;
            size = (unsigned long long)s;
            align = s == 1 ? 1 : s == 2 ? L.i16Align
                  : s == 4 ? (pt.kind == K::Float ? L.f32Align : L.i32Align)
                  : (pt.kind == K::Float ? L.f64Align : L.i64Align);
            return true;
        }
        case K::String: case K::Pointer: case K::Null:
            size = L.ptrSize; align = L.ptrAlign; return true;
        case K::Fn: case K::Interface:           // {fn, env} / {data, vtable}
            size = 2ull * L.ptrSize; align = L.ptrAlign; return true;
        case K::Slice:                           // {ptr, i64}
            align = std::max(L.ptrAlign, L.i64Align);
            size = up(up(L.ptrSize, L.i64Align) + 8, align);
            return true;
        case K::Array: {
            if (!pt.elem) return false;
            long long n = 0;
            bool known = false;
            if (!pt.dim.empty() && std::all_of(pt.dim.begin(), pt.dim.end(), [](unsigned char c) { return std::isdigit(c); })) {
                try { n = std::stoll(pt.dim); known = true; } catch (...) {}
            } else {
                known = ty::foldDim(pt.dim, [&](const std::string& nm, long long& r) {
                    if (auto ec = enumConstants.find(nm); ec != enumConstants.end()) { r = ec->second; return true; }
                    if (nm.rfind("sizeof(", 0) == 0) { r = constSizeof(nm.substr(7, nm.size() - 8)); return r > 0; }
                    const Symbol* cs = findSymbol(nm);
                    return cs && cs->isConst && cs->constInit && foldConstInt(cs->constInit, r);
                }, n);
            }
            if (!known || n <= 0) return false;
            unsigned long long es = 0, ea = 1;
            if (!constLayout(pt.elem->str(), es, ea, depth + 1)) return false;
            unsigned long long limit = L.ptrSize >= 8 ? (1ull << 61) - 1 : 0xffffffffull;
            if (es && (unsigned long long)n > limit / es) { layoutTooLarge = true; return false; }
            size = es * (unsigned long long)n; align = ea;
            return true;
        }
        case K::Struct: case K::Named: {
            std::string nm = pt.name;
            if (nm.rfind("struct:", 0) == 0) nm = nm.substr(7);
            if (interfaceDecls.count(nm)) {        // {data, vtable}
                size = 2ull * L.ptrSize; align = L.ptrAlign; return true;
            }
            if (adtEnums.count(nm)) {
                // { i32 tag, [N x i64] payload }, N the largest variant's bytes in i64s
                // (codegen's makeAdtStruct).
                const EnumDecl* ed = nullptr;
                std::map<std::string, std::string> subs;
                if (auto e = enumDecls.find(nm); e != enumDecls.end()) ed = e->second;
                else if (auto ti = templateInstanceArgs.find(nm); ti != templateInstanceArgs.end()) {
                    auto g = genericEnumDecls.find(ti->second.first);
                    if (g == genericEnumDecls.end()) return false;
                    ed = g->second;
                    for (size_t i = 0; i < ed->typeParams.size() && i < ti->second.second.size(); ++i)
                        subs[ed->typeParams[i]] = ti->second.second[i];
                }
                if (!ed || (!ed->typeParams.empty() && subs.empty())) return false;
                unsigned long long maxBytes = 0;
                for (const auto& pl : ed->payloads) {
                    unsigned long long bytes = 0;
                    for (const auto& ft : pl) {
                        unsigned long long fs = 0, fa = 1;
                        if (!constLayout(subs.empty() ? ft : substType(ft, subs), fs, fa, depth + 1)) return false;
                        bytes = up(bytes, fa) + fs;
                    }
                    maxBytes = std::max(maxBytes, bytes);
                }
                unsigned long long n = std::max(1ull, (maxBytes + 7) / 8);
                align = std::max(L.i32Align, L.i64Align);
                size = up(up(4, L.i64Align) + 8 * n, align);
                return true;
            }
            if (enumDecls.count(nm)) return false;
            auto it = structs.find(nm);
            if (it == structs.end()) return false;
            const StructInfo& si = it->second;
            // `#pragma pack(N>=2)`: each field (a union's member) aligned to min(its
            // alignment, N), the size and the struct's own alignment the largest of those,
            // as C.
            unsigned long long packN = si.packAlign >= 2 ? (unsigned long long)si.packAlign : 0;
            if (!si.pads.empty() ||
                std::any_of(si.fields.begin(), si.fields.end(), [](const StructDecl::Field& f) { return f.bitWidth != 0; }))
                return !si.isUnion && bitfieldLayout(si, size, align, depth);
            unsigned long long off = 0, maxAl = 1;
            for (const auto& f : si.fields) {
                if (f.bitWidth != 0) return false;
                unsigned long long fs = 0, fa = 1;
                if (!constLayout(f.type, fs, fa, depth + 1)) return false;
                if (si.packAlign == 1) fa = 1;
                if (packN) fa = std::min(fa, packN);
                maxAl = std::max(maxAl, fa);
                if (si.isUnion) off = std::max(off, fs);
                else off = up(off, fa) + fs;
            }
            align = maxAl;
            size = up(off, maxAl);
            return true;
        }
        default:
            return false;
    }
}
