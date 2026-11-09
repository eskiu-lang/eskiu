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

// Size and alignment of a struct with bitfields as codegen's layoutBitfieldStruct lays it
// out: the MS rules on Windows, else SysV/AAPCS (clang's Itanium layout).
bool TypeChecker::bitfieldLayout(const StructInfo& si, unsigned long long& size,
                                 unsigned long long& align, int depth) {
    auto up = [](unsigned long long x, unsigned long long a) { return a ? (x + a - 1) / a * a : x; };
    bool packed = si.packAlign == 1;
    // `#pragma pack(N>=2)` caps each alignment at N and packs bitfields back to back.
    unsigned long long packN = si.packAlign >= 2 ? (unsigned long long)si.packAlign : 0;
    unsigned long long structAlign = 1;
    if (layoutInfo.msBitfields) {
        // Each storage word and normal field is an element of the natural (or packed) layout.
        // An unnamed bitfield takes its bits like a named one; a `: 0` right after a bitfield
        // closes the word and aligns the next field to its type, else it is ignored.
        unsigned long long off = 0, curBits = 0, curOff = 0;
        bool open = false, afterBits = false;
        for (const auto& f : layoutFields(si.fields, si.pads)) {
            if (f.bitWidth < 0) return false;
            unsigned long long fs = 0, fa = 1;
            if (!constLayout(f.type, fs, fa, depth + 1)) return false;
            unsigned long long natural = fa;
            if (packed) fa = 1;
            if (packN) fa = std::min(fa, packN);
            if (f.unnamed && f.bitWidth == 0) {
                if (afterBits) {
                    unsigned long long a = layoutInfo.msvcEnv ? fa : natural;
                    off = up(off, a); structAlign = std::max(structAlign, a);
                }
                open = false; afterBits = false;
                continue;
            }
            if (f.bitWidth > 0) {
                unsigned long long stBits = fs * 8, w = (unsigned long long)f.bitWidth;
                if (!open || curBits != stBits || curOff + w > stBits) {
                    off = up(off, fa) + fs; structAlign = std::max(structAlign, fa);
                    open = true; curBits = stBits; curOff = 0;
                }
                curOff += w;
                afterBits = true;
            } else {
                open = false; afterBits = false;
                off = up(off, fa) + fs; structAlign = std::max(structAlign, fa);
            }
        }
        align = structAlign;
        size = up(off, structAlign);
        return true;
    }
    // SysV / AAPCS. An unnamed bitfield takes its bits like a named one; a `: 0` moves the
    // next field to the next boundary of its type's alignment (whatever the packing).
    // Neither raises the struct alignment, except on AAPCS.
    bool contiguous = packed || packN;
    unsigned long long bitpos = 0;
    for (const auto& f : layoutFields(si.fields, si.pads)) {
        if (f.bitWidth < 0) return false;
        unsigned long long fs = 0, fa = 1;
        if (!constLayout(f.type, fs, fa, depth + 1)) return false;
        if (f.unnamed && f.bitWidth == 0) {
            bitpos = up(bitpos, fa * 8);
            if (layoutInfo.unnamedBitfieldsAlign) structAlign = std::max(structAlign, fa);
            continue;
        }
        if (packed) fa = 1;
        if (packN) fa = std::min(fa, packN);
        if (!f.unnamed || layoutInfo.unnamedBitfieldsAlign) structAlign = std::max(structAlign, fa);
        if (f.bitWidth > 0) {
            unsigned long long w = (unsigned long long)f.bitWidth, unitBits = fs * 8;
            if (!contiguous && fa < fs) {
                // Aligned below its size (int64 on 32-bit x86 SysV): may not cross its
                // size from an alignment boundary.
                if (bitpos % (fa * 8) + w > unitBits) bitpos = up(bitpos, fa * 8);
            } else if (!contiguous && unitBits && bitpos / unitBits != (bitpos + w - 1) / unitBits) {
                bitpos = up(bitpos, unitBits);
            }
            bitpos += w;
        } else {
            unsigned long long off = up((bitpos + 7) / 8, fa);
            bitpos = (off + fs) * 8;
        }
    }
    align = structAlign;
    size = up((bitpos + 7) / 8, structAlign);
    return true;
}

long long TypeChecker::constSizeof(const std::string& t) {
    unsigned long long s = 0, a = 1;
    if (!constLayout(t, s, a)) return 0;
    return (long long)s;
}

void TypeChecker::checkArrayDim(const std::string& dim, ASTNode* at) {
    long long v = 0;
    bool known = false;
    bool sizedLater = false;   // a `sizeof` term: codegen knows the value
    if (!dim.empty() && (std::isdigit((unsigned char)dim[0]) || dim[0] == '-') &&
        std::all_of(dim.begin() + 1, dim.end(), [](unsigned char c) { return std::isdigit(c); })) {
        try { v = std::stoll(dim); known = true; } catch (...) {}
    } else if (auto ec = enumConstants.find(dim); ec != enumConstants.end()) {
        v = ec->second; known = true;
    } else if (const Symbol* sym = findSymbol(dim); sym && sym->isConst && sym->constInit) {
        known = foldConstInt(sym->constInit, v);
    } else {
        // An integer constant expression (`(uint8)258`, `N*2`), folded like a `const`.
        known = ty::foldDim(dim, [&](const std::string& n, long long& r) {
            if (auto ec = enumConstants.find(n); ec != enumConstants.end()) { r = ec->second; return true; }
            if (n.rfind("sizeof(", 0) == 0) {
                // `sizeof(T)`: T must be a sized type. A fixed-size scalar folds here; any
                // other size is target layout, which codegen folds (and checks positive).
                std::string t = n.substr(7, n.size() - 8);
                size_t before = errors.size();
                validateStructType(normalizeType(t), at);
                if (errors.size() == before && isVoidValueType(t))
                    errorAt(at, "sizeof of 'void': a void value has no size");
                r = errors.size() == before ? constSizeof(t) : 0;
                if (r == 0) { r = 8; sizedLater = true; }
                return true;
            }
            const Symbol* cs = findSymbol(n);
            return cs && cs->isConst && cs->constInit && foldConstInt(cs->constInit, r);
        }, v);
    }
    if (known && v <= 0 && !sizedLater)
        errorAt(at, "array size must be positive, got " + std::to_string(v) +
                    (std::to_string(v) == dim ? "" : " ('" + dim + "')"));
    // No variable-length arrays: the size is fixed at compile time.
    if (!known && !dim.empty())
        errorAt(at, "array size must be a compile-time constant, got '" + dim + "'");
}

// Type checking utilities
// Does `structName` satisfy `iface` (structural typing)? Each required method must exist
// with the interface's signature: same return type and the same parameter types after
// the receiver. Returns "" when satisfied, else the reason (for the diagnostic).
std::string TypeChecker::interfaceMismatch(const std::string& structName, InterfaceDecl* iface) {
    static const std::set<std::string> kScalarPrims = {
        "int","int8","int16","int32","int64","uint","uint8","uint16","uint32",
        "uint64","char","bool","float","double"};
    for (const auto& method : iface->methods) {
        // A struct satisfies via a mangled method `Type_method`. Free-function fallback
        // (lets PRIMITIVES satisfy a constraint): a top-level fn named `method` whose first
        // parameter is the constrained type acts as the receiver-taking implementation, so
        // `int cmp(int,int)` satisfies `Ord` for int. Gated to scalar primitives to match
        // codegen's dispatch (a struct must satisfy via a real method).
        const std::pair<std::string, std::vector<std::string>>* sig = nullptr;
        std::pair<std::string, std::vector<std::string>> freeSig;
        auto mit = functionSignatures.find(structName + "_" + method.name);
        if (mit != functionSignatures.end()) {
            sig = &mit->second;
            // An inline method of a generic instance is checked once it is used, and
            // a vtable slot is a use.
            if (auto gm = genericMethodInsts.find(mit->first); gm != genericMethodInsts.end())
                queueInstance(gm->second.fn, gm->second.owner->typeParams, gm->second.subs,
                              gm->second.owner->name, "." + method.name, mit->first,
                              "*" + structName, gm->second.owner->sourceFile);
        } else if (FunctionDecl* gf = genericFreeMethodFor(structName, method, freeSig)) {
            sig = &freeSig;
            calledFns.insert(gf->name);
        } else if (kScalarPrims.count(structName)) {
            auto fit = functionSignatures.find(method.name);
            if (fit != functionSignatures.end() && !fit->second.second.empty() &&
                ty::Type::parse(fit->second.second[0]).nominalName() == structName)
                sig = &fit->second;
        }
        if (!sig) return "missing method '" + method.name + "'";
        calledFns.insert(mit != functionSignatures.end() ? mit->first : method.name);   // -Wall: used via the interface
        const auto& params = sig->second;
        if (params.size() != method.params.size() + 1)
            return "method '" + method.name + "' takes " + std::to_string(params.empty() ? 0 : params.size() - 1) +
                   " parameter(s), the interface requires " + std::to_string(method.params.size());
        // A type spelled with the interface's own name (`int cmp(Ord* o)`) stands for the
        // implementing type (a Self type), so it is not compared literally.
        auto isSelf = [&](const std::string& t) {
            return ty::Type::parse(tyq::strip(t)).nominalName() == iface->name;
        };
        if (!isSelf(method.returnType) && normalizeType(sig->first) != normalizeType(method.returnType))
            return "method '" + method.name + "' returns '" + sig->first + "', the interface requires '" +
                   method.returnType + "'";
        // A Self parameter is the implementing type (or the interface itself).
        auto isImplType = [&](const std::string& t) {
            std::string raw = ty::Type::parse(tyq::strip(t)).nominalName();
            return raw == iface->name || raw == structName ||
                   ty::Type::parse(normalizeType(tyq::strip(t))).nominalName() == structName;
        };
        for (size_t i = 0; i < method.params.size(); ++i) {
            bool self = isSelf(method.params[i].first);
            if ((self && !isImplType(params[i + 1])) ||
                (!self && normalizeType(params[i + 1]) != normalizeType(method.params[i].first)))
                return "method '" + method.name + "' parameter " + std::to_string(i + 1) + " is '" +
                       params[i + 1] + "', the interface requires '" + method.params[i].first + "'";
        }
    }
    return "";
}

FunctionDecl* TypeChecker::genericFreeMethodFor(const std::string& instName, const InterfaceDecl::MethodSig& m,
                                              std::pair<std::string, std::vector<std::string>>& sig) {
    auto ti = templateInstanceArgs.find(instName);
    if (ti == templateInstanceArgs.end()) return nullptr;
    std::string fnName = ti->second.first + "_" + m.name;
    auto ft = funcTemplateDecls.find(fnName);
    if (ft == funcTemplateDecls.end() || ft->second->params.empty()) return nullptr;
    FunctionDecl* fd = ft->second;
    // A vtable slot passes the receiver by pointer.
    if (!tyq::isPtr(fd->params[0].first) || fd->params.size() != m.params.size() + 1) return nullptr;
    std::set<std::string> tps(fd->typeParams.begin(), fd->typeParams.end());
    std::map<std::string, std::string> subs;
    unifyTypeParam(fd->params[0].first, "*" + instName, tps, subs);
    for (size_t j = 1; j < fd->params.size(); ++j)
        unifyTypeParam(fd->params[j].first, m.params[j - 1].first, tps, subs);
    for (const auto& tp : fd->typeParams) if (!subs.count(tp)) return nullptr;
    sig.first = substType(fd->returnType, subs);
    sig.second.clear();
    for (const auto& p : fd->params) sig.second.push_back(substType(p.first, subs));
    std::string mangled = fnName;
    for (const auto& tpn : fd->typeParams) mangled += "_" + mangleTemplate(subs[tpn]);
    queueInstance(fd, fd->typeParams, subs, fnName, "", mangled, "", fd->sourceFile);
    return fd;
}

// Bounded generics: verify each constrained type parameter's concrete argument
// satisfies its interface constraint(s). `subs` maps type-param name → concrete
// type. Reuses the structural-satisfaction check (the concrete type must define
// every method the interface requires).
void TypeChecker::checkConstraints(ASTNode* node,
        const std::map<std::string, std::vector<std::string>>& constraints,
        const std::map<std::string, std::string>& subs) {
    for (const auto& kv : constraints) {
        auto sit = subs.find(kv.first);
        if (sit == subs.end()) continue;
        std::string concrete = sit->second;
        // Normalize first (List<int> → struct:List_int), then take the bare
        // nominal name used in method mangling. The strip used to be hand-rolled
        // here (and got the struct: ordering wrong once) — now it's the structured
        // Type's nominalName(), which can't be gotten wrong.
        std::string bare = ty::Type::parse(normalizeType(concrete)).nominalName();
        for (const auto& ic : kv.second) {
            auto iit = interfaceDecls.find(ic);
            if (iit == interfaceDecls.end()) {
                if (node) errorAt(node, "unknown constraint interface '" + ic + "'");
                else errorAtCtx("unknown constraint interface '" + ic + "'");
                continue;
            }
            std::string why = interfaceMismatch(bare, iit->second);
            if (!why.empty()) {
                std::string msg = "type '" + concrete + "' does not satisfy constraint '" +
                                  ic + "' (required by a bounded type parameter): " + why;
                if (node) errorAt(node, msg); else errorAtCtx(msg);
            }
        }
    }
}

bool TypeChecker::dropsConstQual(const std::string& lhs, const std::string& rhs) {
    if (!tyq::isPtr(lhs) || !tyq::isPtr(rhs)) return false;
    if (!tyq::baseConst(tyq::pointee(rhs)) || tyq::baseConst(tyq::pointee(lhs))) return false;
    // Compare the two pointer shapes with the base normalized (`*P` vs `*struct:P`) and the
    // star count canonical (`P*` vs `*P`).
    auto canon = [&](std::string t) {
        t = tyq::strip(t);
        int stars = 0;
        while (!t.empty() && t.back() == '*')  { t.pop_back(); ++stars; }
        while (!t.empty() && t.front() == '*') { t.erase(0, 1); ++stars; }
        return std::string(stars, '*') + normalizeType(t);
    };
    std::string l = canon(lhs), r = canon(rhs);
    // A pointer-to-const also may not silently become a plain `*void`.
    return l == r || l == "*void";
}

// `int32` is another spelling of `int` (and `uint32` of `uint`): a type written with one
// matches the same type written with the other.
static std::string int32AsInt(const std::string& t) {
    std::string out;
    auto word = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
    for (size_t i = 0; i < t.size();) {
        if (i == 0 || !word(t[i - 1])) {
            if (t.compare(i, 5, "int32") == 0 && (i + 5 >= t.size() || !word(t[i + 5]))) { out += "int"; i += 5; continue; }
            if (t.compare(i, 6, "uint32") == 0 && (i + 6 >= t.size() || !word(t[i + 6]))) { out += "uint"; i += 6; continue; }
        }
        out += t[i++];
    }
    return out;
}

bool TypeChecker::isValidAssignment(const std::string& lhsType, const std::string& rhsType) {
    // const-correctness: reject a conversion that would silently drop a pointee
    // const (`const int*` → `int*`). Adding const (`int*` → `const int*`) is fine.
    if (dropsConstQual(lhsType, rhsType)) return false;

    // Normalize both sides so "Point" == "struct:Point"
    std::string lhs = normalizeType(lhsType);
    std::string rhs = normalizeType(rhsType);

    if (lhs == rhs) return true;
    // An array or slice compares by its element type, whatever the element's spelling
    // (`int*[]` is `*int[]`, `Box_int[]` is `struct:Box_int[]`).
    if (!lhs.empty() && lhs.back() == ']' && !rhs.empty() && rhs.back() == ']')
        return canonElemType(lhs) == canonElemType(rhs);
    // Numeric: widening and same-width (incl. signedness changes) are fine; a
    // narrowing conversion (float->int, or wider->narrower) loses information and
    // must be an explicit cast. A literal small enough for the target is handled at
    // the call site (it stays valid), so this type-level rule can be strict.
    if (isNumericType(lhs) && isNumericType(rhs)) return !isNarrowingNumeric(lhs, rhs);
    if (rhs == "null" && interfaceDecls.count(lhs)) return true;   // the empty interface value
    if (lhs == "null" || rhs == "null") return isPointerType(lhs) || isPointerType(rhs);

    // Function/closure types carry a fixed call ABI (which registers hold the
    // parameters and the return value), so there is no implicit adapter between two
    // different fn signatures. Require an exact match — reinterpreting e.g. an
    // int-returning fn as float-returning is a silent miscompile (wrong even at -O0,
    // 0.0 under -O2). Checked before the generic pointer rule below, which a fn type
    // would otherwise satisfy. (A null fn pointer is handled by the `null` case above.)
    {
        ty::Type lt = ty::Type::parse(lhs), rt = ty::Type::parse(rhs);
        if (lt.isFn() || rt.isFn())
            return lt.isFn() && rt.isFn() && int32AsInt(lt.str()) == int32AsInt(rt.str());
    }

    if (isPointerType(lhs) && isPointerType(rhs)) return pointeesCompatible(lhs, rhs);

    // Interface satisfaction: assigning a POINTER to a struct to an interface type (the
    // interface value refers to the struct; a struct value has no address to refer to).
    auto ifaceIt = interfaceDecls.find(lhs);
    if (ifaceIt != interfaceDecls.end()) {
        if (!isPointerType(rhs) || pointerDepth(rhs) != 1) return false;
        std::string structName = ty::Type::parse(rhs).nominalName();
        if (!ifaceConstDrop(rhsType, structName, ifaceIt->second).empty()) return false;
        if (interfaceMismatch(structName, ifaceIt->second).empty())
            return true;
    }

    return false;
}

// One spelling per type for identity checks: pointers leading-star, each nominal part
// normalized, arrays and slices canonical in their element.
std::string TypeChecker::canonElemType(const std::string& raw) {
    ty::Type t = ty::Type::parse(tyq::strip(raw));
    if ((t.kind == ty::Type::Kind::Array || t.kind == ty::Type::Kind::Slice) && t.elem) {
        t.elem = std::make_shared<ty::Type>(ty::Type::parse(canonElemType(t.elem->str())));
        return t.str();
    }
    if (t.kind == ty::Type::Kind::Pointer && t.pointee && !t.bindingConst)
        return std::string(t.nullable ? "?" : "") + "*" + canonElemType(t.pointee->str());
    return int32AsInt(normalizeType(t.str()));
}

// Two pointer types convert implicitly only when they point at the same type (C): a
// `*void` on either side converts to and from any pointer, and the byte pointers
// (`string`, `*char`, `*int8`, `*uint8`) interconvert. Any other change of pointee
// (`*int` to `*Big`) needs a cast. A pointee the checker cannot resolve yet (a type
// parameter, an unknown name) is not judged.
bool TypeChecker::pointeesCompatible(const std::string& lhs, const std::string& rhs) {
    auto canon = [&](const std::string& t) {
        std::string c = tyq::strip(t);
        if (!c.empty() && c[0] == '?') c.erase(0, 1);
        return c;
    };
    std::string l = canon(lhs), r = canon(rhs);
    auto isBytePtr = [&](const std::string& t) {
        if (t == "string") return true;
        if (pointerDepth(t) != 1) return false;
        std::string p = normalizeType(tyq::strip(getPointeeType(t)));
        return p == "char" || p == "int8" || p == "uint8";
    };
    auto isVoidPtr = [&](const std::string& t) {
        return t != "string" && pointerDepth(t) == 1 && normalizeType(tyq::strip(getPointeeType(t))) == "void";
    };
    if (isVoidPtr(l) || isVoidPtr(r)) return true;
    if (isBytePtr(l) && isBytePtr(r)) return true;
    if (l == "string" || r == "string") return false;
    std::string lp = tyq::strip(getPointeeType(l)), rp = tyq::strip(getPointeeType(r));
    if (!lp.empty() && lp[0] == '?') lp.erase(0, 1);
    if (!rp.empty() && rp[0] == '?') rp.erase(0, 1);
    if (isPointerType(lp) && isPointerType(rp)) return pointeesCompatible(lp, rp);
    std::string ln = int32AsInt(normalizeType(lp)), rn = int32AsInt(normalizeType(rp));
    if (ln == rn) return true;
    auto judged = [&](const std::string& t) {
        if (enumDecls.count(t)) return true;   // a sum type is a named type of its own
        ty::Type k = ty::Type::parse(t);
        return k.kind != ty::Type::Kind::Param && k.kind != ty::Type::Kind::Named &&
               k.kind != ty::Type::Kind::Unknown && k.kind != ty::Type::Kind::Error && t != "unknown";
    };
    if (!judged(ln) || !judged(rn)) return true;
    return false;
}

// Pointer levels of a type spelling (`*X` and `X*` are 1, `**X` and `*X*` are 2).
int TypeChecker::pointerDepth(const std::string& raw) {
    std::string t = tyq::strip(raw);
    if (!t.empty() && t.front() == '?') t.erase(0, 1);
    int n = 0;
    while (!t.empty() && t.front() == '*') { t.erase(0, 1); ++n; }
    while (!t.empty() && t.back() == '*')  { t.pop_back(); ++n; }
    return n;
}

// Boxing a pointer to const into interface `iface`: the name of the first method whose
// implementation takes a mutable `self` (it could modify the value), else "".
std::string TypeChecker::ifaceConstDrop(const std::string& srcType, const std::string& structName,
                                         InterfaceDecl* iface) {
    if (!tyq::isPtr(srcType) || !tyq::baseConst(srcType)) return "";
    for (const auto& m : iface->methods) {
        auto it = functionSignatures.find(structName + "_" + m.name);
        if (it == functionSignatures.end() || it->second.second.empty()) continue;
        if (!tyq::baseConst(it->second.second[0])) return m.name;
    }
    return "";
}

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
