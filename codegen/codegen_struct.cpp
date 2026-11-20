// codegen_struct.cpp: struct layout and the struct, extern, intrinsic, alias and union declarations (split from codegen_decl.cpp).
#include "codegen.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <map>
#include <functional>
#include "../sema/type.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

void CodeGen::declareStructType(StructDecl* node) {
    if (!node->typeParams.empty()) {
        templateDecls[node->name] = node;
        return;
    }
    if (structTypes.count(node->name)) return; // already created by the pre-pass

    layoutStruct(node->name, node->fields, node->pads, node->isPacked, node->packAlign);
}

void CodeGen::layoutStruct(const std::string& name, const std::vector<StructDecl::Field>& fields,
                           const std::vector<StructDecl::Pad>& pads, bool isPacked, int packAlign) {
    bool hasBitfields = !pads.empty();
    for (const auto& f : fields) if (f.bitWidth > 0) hasBitfields = true;
    // An unnamed bitfield of nonzero width, here or in a field's type, keeps the struct
    // from being a homogeneous FP aggregate for the C ABI (as in clang).
    bool nonHomog = false;
    for (const auto& p : pads) if (p.bitWidth > 0) nonHomog = true;
    for (const auto& f : fields) {
        llvm::Type* ft = getTypeFromString(f.type);
        while (auto* at = llvm::dyn_cast<llvm::ArrayType>(ft)) ft = at->getElementType();
        if (auto* fst = llvm::dyn_cast<llvm::StructType>(ft); fst && notHomogeneous.count(fst)) nonHomog = true;
    }
    auto note = [&](llvm::StructType* st) { if (nonHomog) notHomogeneous.insert(st); };

    if (!hasBitfields) {
        // #pragma pack(N>=2), or a field whose C alignment is not LLVM's (a pack(N)
        // struct, which LLVM sees as packed): manual layout (padding + physical-index
        // remap), a packed LLVM type whose C alignment is recorded in cAlignOverride.
        bool manual = packAlign >= 2;
        if (!isPacked && !manual) {
            const llvm::DataLayout& DL = module->getDataLayout();
            for (const auto& f : fields) {
                llvm::Type* ft = getTypeFromString(f.type);
                if (cAlignOf(ft) != DL.getABITypeAlign(ft).value()) manual = true;
            }
        }
        if (manual) {
            std::vector<llvm::Type*> phys;
            std::map<std::string, BitfieldSlot> slots;
            uint64_t align = 1;
            buildPackedLayout(fields, packAlign >= 2 ? (unsigned)packAlign : 0, phys, slots, align);
            auto* st = llvm::StructType::create(*context, phys, name, /*isPacked=*/true);
            structTypes[name]  = st;
            structFields[name] = fields;
            structLayout[name] = slots;
            if (align > 1) cAlignOverride[st] = align;
            note(st);
            return;
        }
        std::vector<llvm::Type*> fieldTypes;
        for (const auto& field : fields)
            fieldTypes.push_back(getTypeFromString(field.type));
        auto* st = llvm::StructType::create(*context, fieldTypes, name, isPacked);
        structTypes[name] = st;
        structFields[name] = fields;
        note(st);
        return;
    }

    std::vector<llvm::Type*> phys;
    std::map<std::string, BitfieldSlot> slots;
    bool llvmPacked = isPacked;
    uint64_t align = 1;
    std::vector<std::pair<uint64_t, uint64_t>> unnamedData;
    layoutBitfieldStruct(layoutFields(fields, pads), isPacked, (unsigned)std::max(packAlign, 0),
                         phys, slots, llvmPacked, align, unnamedData);
    auto* st = llvm::StructType::create(*context, phys, name, llvmPacked);
    structTypes[name]  = st;
    structFields[name] = fields;
    structLayout[name] = slots;
    if (align > module->getDataLayout().getABITypeAlign(st).value()) cAlignOverride[st] = align;
    if (!pads.empty()) unnamedBitData[st] = unnamedData;
    note(st);
}

// An enum bitfield with no negative member reads back zero-extended (clang and GCC give
// such an enum an unsigned underlying type; MS rules keep it signed).
bool CodeGen::enumBitfieldUnsigned(const std::string& type) {
    auto it = plainEnumDecls.find(expandAlias(tyq::strip(type)));
    if (it == plainEnumDecls.end() || !it->second->typeParams.empty()) return false;
    for (const auto& m : it->second->members) if (m.second < 0) return false;
    return true;
}

void CodeGen::layoutBitfieldStruct(const std::vector<StructDecl::Field>& fields, bool packed,
                                   unsigned packN, std::vector<llvm::Type*>& phys,
                                   std::map<std::string, BitfieldSlot>& slots, bool& llvmPacked,
                                   uint64_t& cAlign,
                                   std::vector<std::pair<uint64_t, uint64_t>>& unnamedData) {
    llvm::Triple triple(module->getTargetTriple());
    if (triple.isOSWindows()) {
        // MS: consecutive bitfields share a storage word of their declared type while the
        // type size stays the same and the next one fits; a normal field closes the word.
        // Each word is its own element, so LLVM's natural layout is the MS one; under
        // #pragma pack(N>=2), or with a field whose C alignment is not LLVM's, the
        // elements are placed by hand at their C alignment (capped at N). An unnamed
        // bitfield takes its bits like a named one. A `: 0` right after a bitfield closes
        // the word and aligns the next field to its type (capped by the packing on MSVC,
        // not on mingw), which needs the layout by hand; anywhere else it is ignored.
        const llvm::DataLayout& DL = module->getDataLayout();
        uint64_t offset = 0, structAlign = 1;
        bool manual = packN >= 2;
        if (!packed)
            for (const auto& f : fields) {
                llvm::Type* ft = getTypeFromString(f.type);
                if (cAlignOf(ft) != DL.getABITypeAlign(ft).value()) manual = true;
            }
        bool afterBits = false;
        for (const auto& f : fields) {
            if (f.unnamed && f.bitWidth == 0) { if (afterBits) manual = true; afterBits = false; }
            else afterBits = f.bitWidth > 0;
        }
        afterBits = false;
        auto addElem = [&](llvm::Type* t) -> unsigned {
            if (manual) {
                uint64_t a = packed ? 1 : cAlignOf(t);
                if (packN >= 2) a = std::min<uint64_t>(a, packN);
                structAlign = std::max(structAlign, a);
                uint64_t at = (offset + a - 1) / a * a;
                if (at > offset) phys.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), at - offset));
                offset = at + DL.getTypeAllocSize(t).getFixedValue();
            }
            phys.push_back(t);
            return (unsigned)phys.size() - 1;
        };
        int curPhys = -1; unsigned curUnitBits = 0, curOffset = 0;
        for (const auto& f : fields) {
            if (f.unnamed && f.bitWidth == 0) {
                if (afterBits) {
                    uint64_t a = cAlignOf(getTypeFromString(f.type));
                    if (triple.isWindowsMSVCEnvironment()) {
                        if (packed) a = 1;
                        if (packN >= 2) a = std::min<uint64_t>(a, packN);
                    }
                    structAlign = std::max(structAlign, a);
                    uint64_t at = (offset + a - 1) / a * a;
                    if (at > offset) phys.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), at - offset));
                    offset = at;
                }
                curPhys = -1; curUnitBits = 0; curOffset = 0; afterBits = false;
                continue;
            }
            afterBits = f.bitWidth > 0;
            if (f.bitWidth > 0) {
                llvm::Type* sty = getTypeFromString(f.type);
                // A bool bitfield's storage unit is its byte, as in C (the value is i1).
                llvm::Type* uty = sty->isIntegerTy(1) ? llvm::Type::getInt8Ty(*context) : sty;
                unsigned stBits = uty->getIntegerBitWidth();
                if (curPhys < 0 || curUnitBits != stBits ||
                    curOffset + (unsigned)f.bitWidth > stBits) {
                    curPhys = (int)addElem(uty);
                    curUnitBits = stBits; curOffset = 0;
                }
                BitfieldSlot s;
                s.isBitfield = true; s.physIndex = (unsigned)curPhys;
                s.bitOffset = curOffset; s.bitWidth = (unsigned)f.bitWidth;
                s.storageType = sty; s.isSigned = !eskiuUnsigned(f.type);
                if (uty != sty) s.accessType = uty;
                if (!f.unnamed) slots[f.name] = s;
                curOffset += (unsigned)f.bitWidth;
            } else {
                curPhys = -1; curUnitBits = 0; curOffset = 0;
                llvm::Type* ft = getTypeFromString(f.type);
                BitfieldSlot s;
                s.isBitfield = false; s.physIndex = addElem(ft);
                s.storageType = ft;
                slots[f.name] = s;
            }
        }
        if (manual) {
            uint64_t total = (offset + structAlign - 1) / structAlign * structAlign;
            if (total > offset) phys.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), total - offset));
            llvmPacked = true;
            cAlign = structAlign;
        }
        return;
    }

    // SysV / AAPCS (clang's Itanium record layout): a bitfield goes at the next free bit
    // unless it would cross a boundary of its declared type's storage unit, then it
    // starts at that boundary. A packed struct (or #pragma pack) packs bitfields
    // back to back. A normal field starts at the next byte, aligned. Every field is
    // then addressed by byte offset; the element list only has to reproduce the C
    // size and alignment (and the integer/FP classes the C ABI lowering reads). An
    // unnamed bitfield takes its bits like a named one but is no storage (it holds no
    // data); a `: 0` moves the next field to the next boundary of its type's alignment,
    // whatever the packing. Neither raises the struct alignment, except on AAPCS.
    bool unnamedAligns = unnamedBitfieldsAlign(triple);
    const llvm::DataLayout& DL = module->getDataLayout();
    llvm::Type* i8 = llvm::Type::getInt8Ty(*context);
    bool contiguous = packed || packN >= 2;
    uint64_t cap = packed ? 1 : (packN >= 2 ? packN : 0);
    auto capAlign = [&](uint64_t a) { return cap ? std::min(a, cap) : a; };
    struct Span { uint64_t off, end; llvm::Type* ty; };
    std::vector<Span> normals, units;
    uint64_t bitpos = 0, structAlign = 1;
    for (const auto& f : fields) {
        llvm::Type* ty = getTypeFromString(f.type);
        uint64_t size = DL.getTypeAllocSize(ty).getFixedValue();
        if (f.unnamed && f.bitWidth == 0) {
            uint64_t a = cAlignOf(ty);
            bitpos = (bitpos + a * 8 - 1) / (a * 8) * (a * 8);
            if (unnamedAligns) structAlign = std::max(structAlign, a);
            unnamedData.push_back({bitpos / 8, (bitpos + size * 8 + 7) / 8});
            continue;
        }
        uint64_t align = capAlign(cAlignOf(ty));
        if (!f.unnamed || unnamedAligns) structAlign = std::max(structAlign, align);
        BitfieldSlot s;
        s.byOffset = true; s.storageType = ty;
        if (f.bitWidth > 0) {
            uint64_t w = (uint64_t)f.bitWidth, unitBits = size * 8;
            s.isBitfield = true; s.bitWidth = (unsigned)w;
            s.isSigned = !eskiuUnsigned(f.type) && !enumBitfieldUnsigned(f.type);
            if (contiguous) {
                s.byteOffset = bitpos / 8;
                s.bitOffset = (unsigned)(bitpos % 8);
                uint64_t span = (s.bitOffset + w + 7) / 8;
                s.accessType = llvm::IntegerType::get(*context, (unsigned)(span * 8));
                s.accessAlign = 1;
                if (!f.unnamed) units.push_back({s.byteOffset, s.byteOffset + span, nullptr});
            } else if (align < size) {
                // A type aligned below its size (int64 on 32-bit x86 SysV): the bitfield
                // may not cross its type's size from an alignment boundary, and its storage
                // is the bytes it spans from that boundary (clang's rule).
                uint64_t alignBits = align * 8;
                if (bitpos % alignBits + w > unitBits)
                    bitpos = (bitpos + alignBits - 1) / alignBits * alignBits;
                s.byteOffset = bitpos / alignBits * align;
                s.bitOffset = (unsigned)(bitpos - s.byteOffset * 8);
                uint64_t span = (s.bitOffset + w + 7) / 8;
                s.accessType = llvm::IntegerType::get(*context, (unsigned)(span * 8));
                s.accessAlign = (unsigned)align;
                if (!f.unnamed) units.push_back({s.byteOffset, s.byteOffset + span, nullptr});
            } else {
                if (bitpos / unitBits != (bitpos + w - 1) / unitBits)
                    bitpos = (bitpos + unitBits - 1) / unitBits * unitBits;
                s.byteOffset = bitpos / unitBits * size;
                s.bitOffset = (unsigned)(bitpos - s.byteOffset * 8);
                // A bool bitfield is read and written as its byte (the value is i1).
                llvm::Type* uty = ty->isIntegerTy(1) ? i8 : ty;
                s.accessType = uty;
                s.accessAlign = (unsigned)DL.getABITypeAlign(uty).value();
                if (!f.unnamed) units.push_back({s.byteOffset, s.byteOffset + size, uty});
            }
            if (f.unnamed) {
                uint64_t first = s.byteOffset * 8 + s.bitOffset;
                unnamedData.push_back({first / 8, (first + size * 8 + 7) / 8});
            }
            bitpos += w;
        } else {
            uint64_t off = ((bitpos + 7) / 8 + align - 1) / align * align;
            s.byteOffset = off;
            normals.push_back({off, off + size, ty});
            bitpos = (off + size) * 8;
        }
        if (!f.unnamed) slots[f.name] = s;
    }
    uint64_t total = ((bitpos + 7) / 8 + structAlign - 1) / structAlign * structAlign;
    // The bytes an unnamed bitfield counts as data for the C ABI (clang's
    // BitsContainNoUserData: from its first bit through its type's size) end at the struct.
    for (auto& d : unnamedData) { d.first = std::min(d.first, total); d.second = std::min(d.second, total); }

    // Elements: the bitfield storage (each maximal storage unit as an integer of its
    // type, which carries the unit's alignment; the byte runs of a packed struct as
    // `[n x i8]`), the normal fields outside it as themselves, and `[n x i8]` for any
    // other byte (padding, or the part of a normal field sharing a unit that sticks out).
    std::vector<Span> elems;
    std::sort(units.begin(), units.end(), [](const Span& a, const Span& b) {
        return a.off != b.off ? a.off < b.off : a.end > b.end;
    });
    for (const auto& u : units) {
        if (!elems.empty() && u.off < elems.back().end) {
            if (contiguous) elems.back().end = std::max(elems.back().end, u.end);
            continue;
        }
        elems.push_back(u);
    }
    std::vector<Span> storage = elems;
    for (const auto& n : normals) {
        uint64_t o = n.off;
        bool overlap = false;
        for (const auto& u : storage) {
            if (u.end <= o || u.off >= n.end) continue;
            overlap = true;
            if (u.off > o) elems.push_back({o, u.off, nullptr});
            o = std::max(o, u.end);
        }
        if (!overlap) elems.push_back(n);
        else if (o < n.end) elems.push_back({o, n.end, nullptr});
    }
    std::sort(elems.begin(), elems.end(), [](const Span& a, const Span& b) { return a.off < b.off; });
    // The elements' natural LLVM layout gives the C one unless it is less aligned than C
    // (a field whose C alignment is above LLVM's): then the type is packed, padded by hand.
    uint64_t llAlign = 1;
    for (const auto& e : elems)
        if (e.ty) llAlign = std::max<uint64_t>(llAlign, DL.getABITypeAlign(e.ty).value());
    bool byHand = contiguous || llAlign < structAlign;
    uint64_t cur = 0;
    for (const auto& e : elems) {
        llvm::Type* t = e.ty ? e.ty : llvm::ArrayType::get(i8, e.end - e.off);
        uint64_t a = byHand ? 1 : DL.getABITypeAlign(t).value();
        uint64_t at = (cur + a - 1) / a * a;
        if (at > e.off) throw std::runtime_error("internal: bitfield layout element misplaced");
        if (at < e.off) phys.push_back(llvm::ArrayType::get(i8, e.off - cur));
        phys.push_back(t);
        cur = e.end;
    }
    // Trailing bytes the natural layout would not add (padding of a packed type, or the
    // bits of a trailing unnamed bitfield).
    uint64_t naturalEnd = byHand ? cur : (cur + llAlign - 1) / llAlign * llAlign;
    if (cur < total && naturalEnd < total) phys.push_back(llvm::ArrayType::get(i8, total - cur));
    llvmPacked = byHand;
    cAlign = structAlign;
}

llvm::Value* CodeGen::layoutFieldAddr(const std::string& sname, llvm::Value* base,
                                      const BitfieldSlot& slot, const llvm::Twine& name) {
    if (slot.byOffset)
        return builder->CreateConstInBoundsGEP1_64(llvm::Type::getInt8Ty(*context), base,
                                                   slot.byteOffset, name);
    return builder->CreateStructGEP(structTypes[sname], base, slot.physIndex, name);
}

void CodeGen::visit(StructDecl* node) {
    if (!node->typeParams.empty()) {
        templateDecls[node->name] = node;
        return;
    }

    declareStructType(node);

    // Emit methods as mangled functions: StructName_methodName(self: *Struct, ...)
    for (const auto& method : node->methods) {
        if (auto func = nodeCast<FunctionDecl>(method.get())) {
            std::vector<std::pair<std::string, std::string>> params;
            params.push_back({"*" + node->name, "self"});
            for (const auto& p : func->params) params.push_back(p);

            auto mangled = std::make_shared<FunctionDecl>(
                node->name + "_" + func->name,
                func->returnType, params, func->body);
            mangled->sourceFile = func->sourceFile.empty() ? node->sourceFile : func->sourceFile;
            mangled->accept(this);
        }
    }
}

// A C math function that reads and writes no memory: one that never sets errno, or
// (`errnoFree`, Apple targets, where clang defaults to -fno-math-errno) one that
// only would. Matched by name and an all-double (or, with an `f` suffix, all-float)
// signature, so it can be hoisted, merged and turned into an instruction (`sqrt`).
static bool pureLibm(const std::string& name, llvm::FunctionType* ft, bool errnoFree) {
    static const std::map<std::string, std::pair<unsigned, bool>> fns = {
        {"fabs", {1, false}}, {"floor", {1, false}}, {"ceil", {1, false}},
        {"trunc", {1, false}}, {"round", {1, false}}, {"rint", {1, false}},
        {"nearbyint", {1, false}}, {"fmin", {2, false}}, {"fmax", {2, false}},
        {"copysign", {2, false}},
        {"sqrt", {1, true}}, {"cbrt", {1, true}}, {"exp", {1, true}}, {"exp2", {1, true}},
        {"expm1", {1, true}}, {"log", {1, true}}, {"log2", {1, true}}, {"log10", {1, true}},
        {"log1p", {1, true}}, {"sin", {1, true}}, {"cos", {1, true}}, {"tan", {1, true}},
        {"asin", {1, true}}, {"acos", {1, true}}, {"atan", {1, true}}, {"sinh", {1, true}},
        {"cosh", {1, true}}, {"tanh", {1, true}}, {"asinh", {1, true}}, {"acosh", {1, true}},
        {"atanh", {1, true}}, {"atan2", {2, true}}, {"pow", {2, true}}, {"hypot", {2, true}},
        {"fmod", {2, true}}, {"remainder", {2, true}},
    };
    std::string base = name;
    bool isFloat = false;
    auto it = fns.find(base);
    if (it == fns.end() && !base.empty() && base.back() == 'f') {
        base.pop_back();
        isFloat = true;
        it = fns.find(base);
    }
    if (it == fns.end() || (it->second.second && !errnoFree)) return false;
    if (ft->isVarArg() || ft->getNumParams() != it->second.first) return false;
    auto ok = [&](llvm::Type* t) { return isFloat ? t->isFloatTy() : t->isDoubleTy(); };
    if (!ok(ft->getReturnType())) return false;
    for (llvm::Type* p : ft->params())
        if (!ok(p)) return false;
    return true;
}

void CodeGen::visit(ExternDecl* node) {
    // An `extern` the program also defines is an Eskiu function: its definition
    // declares the prototype (Eskiu convention, not the C ABI lowering below).
    if (definedFunctionNames.count(node->name)) return;
    // Get parameter types
    std::vector<llvm::Type*> paramTypes;
    bool hasVarargs = false;
    // A fn-typed parameter of a C function is a C function pointer, not an Eskiu
    // closure: it is declared `ptr` and each call passes a function's C address.
    std::vector<bool> fnPtr;
    std::vector<bool> vaList;

    for (auto& param : node->params) {
        if (param.first == "...") {
            hasVarargs = true;
            break;
        }
        ty::Type pt = ty::Type::parse(expandAlias(param.first));
        bool isFn = pt.isFn();
        // A va_list goes to C the way the target's C `va_list` does (see evalCVaList).
        bool isVa = pt.kind == ty::Type::Kind::VaList;
        fnPtr.push_back(isFn);
        vaList.push_back(isVa);
        paramTypes.push_back(isFn || isVa ? llvm::PointerType::get(*context, 0) : getTypeFromString(param.first));
    }
    if (std::find(fnPtr.begin(), fnPtr.end(), true) != fnPtr.end()) externFnPtrParams[node->name] = fnPtr;
    if (std::find(vaList.begin(), vaList.end(), true) != vaList.end()) externVaListParams[node->name] = vaList;

    // size_t, ssize_t and c_long are int64 in Eskiu and the target's C types here.
    llvm::Type* returnType = getTypeFromString(node->returnType);
    auto ck = externCInts.find(node->name);
    if (ck != externCInts.end()) {
        const std::vector<char>& k = ck->second;
        for (size_t i = 0; i < paramTypes.size() && i < k.size(); ++i)
            if (k[i]) paramTypes[i] = cIntType(k[i]);
        char rk = k.size() > paramTypes.size() ? k[paramTypes.size()] : 0;
        if (rk) {
            returnType = cIntType(rk);
            if (!returnType->isIntegerTy(64)) externRetExt[node->name] = rk == 'z' ? 'z' : 's';
        }
    }

    // Create function type
    llvm::FunctionType* funcType = llvm::FunctionType::get(returnType, paramTypes, hasVarargs);

    // By-value aggregates: declare the C-ABI-lowered signature (see codegen_cabi.cpp).
    // Narrow integer params/result carry signext/zeroext, as clang declares them.
    std::vector<llvm::Attribute::AttrKind> pext;
    for (size_t i = 0; i < paramTypes.size(); ++i)
        pext.push_back(fnPtr[i] ? llvm::Attribute::None : cabiExtAttr(node->params[i].first, paramTypes[i]));
    llvm::Attribute::AttrKind rext = cabiExtAttr(node->returnType, returnType);
    CAbiSig sig;
    if (buildCAbiSig(funcType, sig)) {
        if (!module->getFunction(node->name)) {
            llvm::Function* fn = declareCAbiExtern(node->name, sig);
            unsigned idx = sig.ret.kind == CAbiArg::Sret ? 1 : 0;
            for (size_t i = 0; i < sig.params.size(); ++i) {
                const CAbiArg& a = sig.params[i];
                if (a.kind == CAbiArg::Direct && pext[i] != llvm::Attribute::None) fn->addParamAttr(idx, pext[i]);
                idx += a.kind == CAbiArg::Expand ? llvm::cast<llvm::StructType>(a.ty)->getNumElements() : 1;
            }
            if (sig.ret.kind == CAbiArg::Direct && rext != llvm::Attribute::None) fn->addRetAttr(rext);
        }
        return;
    }

    // Create external function declaration
    llvm::Function* fn = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage, node->name, module.get());
    cConvFns.insert(fn);
    for (size_t i = 0; i < pext.size(); ++i)
        if (pext[i] != llvm::Attribute::None) fn->addParamAttr((unsigned)i, pext[i]);
    if (rext != llvm::Attribute::None) fn->addRetAttr(rext);
    if (!freestanding && pureLibm(node->name, funcType, module->getTargetTriple().isOSDarwin())) {
        fn->setDoesNotAccessMemory();
        fn->setDoesNotThrow();
        fn->setWillReturn();
    }
}

void CodeGen::visit(IntrinsicDecl* node) {
    // No declaration is emitted: a call to an intrinsic lowers to inline IR
    // (see the intrinsic dispatch at the top of visit(CallExpr)). We only record
    // the name so that callsites can be recognised regardless of source order.
    intrinsicNames.insert(node->name);
}

void CodeGen::visit(TypeAliasDecl* node) {
    typeAliases[node->name] = node->aliased;
    templateTypeAliases()[node->name] = node->aliased;
}

void CodeGen::visit(UnionDecl* node) {
    // C layout: size = the largest member rounded up to the strictest member
    // alignment, and the union is as aligned as that member. The storage is
    // `{ <most-aligned member>, [pad x i8] }` so LLVM gives it the C alignment
    // (a bare `[N x i8]` would be 1-aligned and pack wrongly inside a struct).
    // Under `#pragma pack(N)` each member's alignment is capped at N, like a struct's
    // fields; when that lowers the union's alignment the storage type is a packed LLVM
    // struct whose C alignment is recorded in cAlignOverride.
    // An unnamed bitfield is no member: it only adds its bytes (on MS rules its type's
    // size) and, on AAPCS, its type's alignment (a `: 0` only that, uncapped). On MSVC a
    // `: 0` right after a bitfield adds its type's size.
    const llvm::DataLayout& DL = module->getDataLayout();
    llvm::Triple triple(module->getTargetTriple());
    bool ms = triple.isOSWindows(), afterBits = false;
    uint64_t maxSize = 0, maxAlign = 1, natAlign = 1, anchorAlign = 1;
    uint64_t cap = node->packAlign >= 1 ? (uint64_t)node->packAlign : 0;
    llvm::Type* anchor = nullptr;
    std::vector<llvm::Type*> memberTys;
    std::vector<uint64_t> unnamedSizes;
    bool nonHomog = false;
    for (const auto& f : layoutFields(node->fields, node->pads)) {
        llvm::Type* ft = getTypeFromString(f.type);
        if (f.unnamed) unnamedSizes.push_back(DL.getTypeAllocSize(ft).getFixedValue());
        if (f.unnamed && f.bitWidth > 0) nonHomog = true;
        llvm::Type* et = ft;
        while (auto* at = llvm::dyn_cast<llvm::ArrayType>(et)) et = at->getElementType();
        if (auto* est = llvm::dyn_cast<llvm::StructType>(et); est && notHomogeneous.count(est)) nonHomog = true;
        bool wasAfterBits = afterBits;
        afterBits = f.unnamed && f.bitWidth > 0;
        if (f.unnamed) {
            uint64_t al = cAlignOf(ft);
            if (f.bitWidth == 0) {
                if (!ms && unnamedBitfieldsAlign(triple)) { maxAlign = std::max(maxAlign, al); natAlign = std::max(natAlign, al); }
                if (wasAfterBits && triple.isWindowsMSVCEnvironment())
                    maxSize = std::max<uint64_t>(maxSize, DL.getTypeAllocSize(ft).getFixedValue());
                continue;
            }
            maxSize = std::max<uint64_t>(maxSize, ms ? DL.getTypeAllocSize(ft).getFixedValue() : ((uint64_t)f.bitWidth + 7) / 8);
            if (!ms && unnamedBitfieldsAlign(triple)) {
                natAlign = std::max(natAlign, al);
                maxAlign = std::max(maxAlign, cap ? std::min(al, cap) : al);
            }
            continue;
        }
        memberTys.push_back(ft);
        uint64_t sz = DL.getTypeAllocSize(ft);
        uint64_t al = cAlignOf(ft);
        natAlign = std::max(natAlign, al);
        if (cap) al = std::min(al, cap);
        if (sz > maxSize) maxSize = sz;
        maxAlign = std::max(maxAlign, al);
        if (!anchor || al > anchorAlign ||
            (al == anchorAlign && sz > DL.getTypeAllocSize(anchor))) {
            anchor = ft; anchorAlign = al;
        }
    }
    uint64_t total = (maxSize + maxAlign - 1) / maxAlign * maxAlign;
    std::vector<llvm::Type*> body;
    if (anchor) body.push_back(anchor);
    uint64_t used = anchor ? DL.getTypeAllocSize(anchor) : 0;
    if (total == 0) total = 1;
    if (total > used)
        body.push_back(llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), total - used));

    std::string mangledName = node->name;
    auto* namedTy = llvm::StructType::create(*context, body, mangledName + ".union",
                                             /*isPacked=*/maxAlign < natAlign);
    structTypes[mangledName] = namedTy;
    if (maxAlign > DL.getABITypeAlign(namedTy).value()) cAlignOverride[namedTy] = maxAlign;
    unionMemberTypes[namedTy] = memberTys;
    if (!node->pads.empty()) {
        auto& data = unnamedBitData[namedTy];
        for (uint64_t sz : unnamedSizes) data.push_back({0, std::min(sz, total)});
    }
    if (nonHomog) notHomogeneous.insert(namedTy);

    // Register fields so MemberExpr can resolve them (all at offset 0, typed via cast)
    unionFields[mangledName] = node->fields;
    // Also register in structFields for MemberExpr type lookup
    std::vector<StructDecl::Field> sf;
    for (const auto& f : node->fields) sf.push_back({f.type, f.name});
    structFields[mangledName] = sf;
}
