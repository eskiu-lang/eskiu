// typecheck_layout.cpp: bitfield layout, sizeof, array dimensions, interface conformance and constraints (split from typecheck_type.cpp).
#include "type_checker.h"
#include <set>
#include <algorithm>
#include <cctype>

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with codegen; see template_utils.h.
#include "../template_utils.h"
#include "../ast/type_qual.h"

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
