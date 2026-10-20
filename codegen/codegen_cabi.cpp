#include "codegen.h"
#include "../ast/type_qual.h"
#include "llvm/TargetParser/Triple.h"
#include <functional>
#include <set>

// CodeGen — C ABI lowering for `extern` functions that take or return an aggregate
// (struct/union) by value. Eskiu-to-Eskiu calls pass aggregates as first-class LLVM
// values, which is fine between Eskiu functions but is not what a C compiler expects:
// the C ABI coerces small aggregates into integer/FP registers and passes large ones
// through memory. Each such extern is declared with the lowered C signature, and its
// calls convert the Eskiu-level (logical) values to and from it.
//
// Covered: AArch64 AAPCS64 (Darwin + Linux), x86-64 System V, Windows x64, 32-bit
// ARM AAPCS (soft- and hard-float), 32-bit x86 (cdecl: SysV i386, Darwin, Windows).
// The coerced types mirror clang's for the same C signature. Other targets keep the
// first-class lowering.
// Part of the codegen split; see codegen.h.

CodeGen::CAbiTarget CodeGen::cabiTarget() const {
    llvm::Triple t(module->getTargetTriple());
    if (t.isAArch64()) return CAbiTarget::AArch64;
    if (t.getArch() == llvm::Triple::x86_64)
        return t.isOSWindows() ? CAbiTarget::Win64 : CAbiTarget::SysV;
    if (t.isARM() || t.isThumb()) return CAbiTarget::ARM32;
    if (t.getArch() == llvm::Triple::x86) return CAbiTarget::X86;
    return CAbiTarget::None;
}

// The C extension attribute of a narrow integer argument or result (clang's signext /
// zeroext): the caller extends an argument, the callee a result, to 32 bits, and the
// other side relies on it (AArch64 Darwin, x86-64). `char` follows the target's C
// `char`, unsigned on AArch64 and 32-bit ARM outside Darwin and Windows.
llvm::Attribute::AttrKind CodeGen::cabiExtAttr(const std::string& eskiuType, llvm::Type* llty) const {
    if (!llty->isIntegerTy(1) && !llty->isIntegerTy(8) && !llty->isIntegerTy(16))
        return llvm::Attribute::None;
    // As clang: AAPCS64 outside Darwin leaves the upper bits unspecified (the receiver
    // extends), and Windows x64 extends only a bool.
    {
        llvm::Triple t(module->getTargetTriple());
        if (t.isAArch64() && !t.isOSDarwin()) return llvm::Attribute::None;
        if (t.getArch() == llvm::Triple::x86_64 && t.isOSWindows())
            return llty->isIntegerTy(1) ? llvm::Attribute::ZExt : llvm::Attribute::None;
    }
    std::string s = tyq::strip(expandAlias(eskiuType));
    auto ov = typeParamOverride.find(s);
    if (ov != typeParamOverride.end()) s = tyq::strip(expandAlias(ov->second));
    if (s == "char") {
        llvm::Triple t(module->getTargetTriple());
        bool uns = !t.isOSWindows() && (t.isARM() || t.isThumb() || (t.isAArch64() && !t.isOSDarwin()));
        return uns ? llvm::Attribute::ZExt : llvm::Attribute::SExt;
    }
    return eskiuUnsigned(s) ? llvm::Attribute::ZExt : llvm::Attribute::SExt;
}

// Only named struct types are C aggregates here: user structs and unions (and ADT
// enums). Literal structs are Eskiu fat values (closures, slices) passed as-is, and
// the Eskiu `va_list` keeps its first-class lowering.
static bool isCAggregate(llvm::Type* ty) {
    auto* st = llvm::dyn_cast<llvm::StructType>(ty);
    return st && !st->isLiteral() && st->getName() != "__va_list";
}

// Flatten an aggregate into its scalar leaves (byte offset, type). A union contributes
// every member at its own offset, since its storage type keeps only one of them.
void CodeGen::cabiLeaves(llvm::Type* ty, uint64_t base,
                         std::vector<std::pair<uint64_t, llvm::Type*>>& out) const {
    const llvm::DataLayout& DL = module->getDataLayout();
    if (auto* st = llvm::dyn_cast<llvm::StructType>(ty)) {
        auto uit = unionMemberTypes.find(st);
        if (uit != unionMemberTypes.end()) {
            for (llvm::Type* m : uit->second) cabiLeaves(m, base, out);
            return;
        }
        // A C-layout bitfield struct: its fields, not its storage elements. Like clang
        // (BitsContainNoUserData), a bitfield counts as data from its first bit through
        // its declared type's width; each such byte in the struct is an `i8` leaf.
        auto lit = st->hasName() ? structLayout.find(st->getName().str()) : structLayout.end();
        auto fit = st->hasName() ? structFields.find(st->getName().str()) : structFields.end();
        if (lit != structLayout.end() && fit != structFields.end() && !lit->second.empty()
                && lit->second.begin()->second.byOffset) {
            uint64_t size = DL.getTypeAllocSize(st);
            for (const auto& f : fit->second) {
                const BitfieldSlot& s = lit->second.at(f.name);
                if (!s.isBitfield) { cabiLeaves(s.storageType, base + s.byteOffset, out); continue; }
                uint64_t first = s.byteOffset * 8 + s.bitOffset;
                uint64_t last = first + std::max(8u, s.storageType->getIntegerBitWidth());
                for (uint64_t b = first / 8; b < (last + 7) / 8 && b < size; ++b)
                    out.push_back({base + b, llvm::Type::getInt8Ty(ty->getContext())});
            }
            return;
        }
        const llvm::StructLayout* sl = DL.getStructLayout(st);
        // A struct laid out by hand (#pragma pack(N>=2), or holding one): its fields, not
        // the padding runs between them (padding is no data, as in clang).
        if (lit != structLayout.end() && fit != structFields.end() && !lit->second.empty()
                && std::none_of(lit->second.begin(), lit->second.end(),
                                [](const auto& e) { return e.second.isBitfield; })) {
            for (const auto& f : fit->second) {
                const BitfieldSlot& s = lit->second.at(f.name);
                cabiLeaves(s.storageType, base + sl->getElementOffset(s.physIndex), out);
            }
            return;
        }
        for (unsigned i = 0; i < st->getNumElements(); ++i)
            cabiLeaves(st->getElementType(i), base + sl->getElementOffset(i), out);
        return;
    }
    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(ty)) {
        uint64_t esz = DL.getTypeAllocSize(at->getElementType());
        for (uint64_t i = 0; i < at->getNumElements(); ++i)
            cabiLeaves(at->getElementType(), base + i * esz, out);
        return;
    }
    out.push_back({base, ty});
}

void CodeGen::cabiUnnamedData(llvm::Type* ty, uint64_t base,
                              std::vector<std::pair<uint64_t, uint64_t>>& out) const {
    const llvm::DataLayout& DL = module->getDataLayout();
    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(ty)) {
        llvm::Type* et = at->getElementType();
        if (!llvm::isa<llvm::StructType>(et) && !llvm::isa<llvm::ArrayType>(et)) return;
        uint64_t esz = DL.getTypeAllocSize(et);
        for (uint64_t i = 0; i < at->getNumElements(); ++i) cabiUnnamedData(et, base + i * esz, out);
        return;
    }
    auto* st = llvm::dyn_cast<llvm::StructType>(ty);
    if (!st) return;
    auto dit = unnamedBitData.find(st);
    if (dit != unnamedBitData.end()) {
        if (dit->second.empty()) out.push_back({base, base});
        for (const auto& d : dit->second) out.push_back({base + d.first, base + d.second});
    }
    auto uit = unionMemberTypes.find(st);
    if (uit != unionMemberTypes.end()) {
        for (llvm::Type* m : uit->second) cabiUnnamedData(m, base, out);
        return;
    }
    auto lit = st->hasName() ? structLayout.find(st->getName().str()) : structLayout.end();
    auto fit = st->hasName() ? structFields.find(st->getName().str()) : structFields.end();
    const llvm::StructLayout* sl = DL.getStructLayout(st);
    if (lit != structLayout.end() && fit != structFields.end() && !lit->second.empty()) {
        for (const auto& f : fit->second) {
            const BitfieldSlot& s = lit->second.at(f.name);
            if (s.isBitfield) continue;
            cabiUnnamedData(s.storageType, base + (s.byOffset ? s.byteOffset : sl->getElementOffset(s.physIndex)), out);
        }
        return;
    }
    for (unsigned i = 0; i < st->getNumElements(); ++i)
        cabiUnnamedData(st->getElementType(i), base + sl->getElementOffset(i), out);
}

// Homogeneous floating-point aggregate: 1..4 leaves of one FP type, densely packed.
// Returned as `[N x fp]` (asArray) or the literal struct `{ fp, ... }` clang uses for
// HFA results and for 32-bit ARM hard-float arguments.
static llvm::Type* hfaType(const std::vector<std::pair<uint64_t, llvm::Type*>>& leaves,
                           uint64_t size, const llvm::DataLayout& DL, bool asArray) {
    if (leaves.empty()) return nullptr;
    llvm::Type* base = leaves[0].second;
    if (!base->isFloatTy() && !base->isDoubleTy()) return nullptr;
    uint64_t esz = DL.getTypeAllocSize(base);
    std::set<uint64_t> offs;
    for (const auto& l : leaves) {
        if (l.second != base || l.first % esz != 0) return nullptr;
        offs.insert(l.first);
    }
    uint64_t n = offs.size();
    if (n > 4 || size != n * esz) return nullptr;
    if (asArray) return llvm::ArrayType::get(base, n);
    return llvm::StructType::get(base->getContext(), std::vector<llvm::Type*>(n, base));
}

// The C fields of aggregate `ty` for the 32-bit x86 rules: a struct's elements (a
// `#pragma pack` padding run is a byte array there), a union's members. A bitfield
// struct has no such list (clang does not expand one, nor treat it as one element).
bool CodeGen::x86Fields(llvm::Type* ty, std::vector<llvm::Type*>& out) const {
    auto* st = llvm::dyn_cast<llvm::StructType>(ty);
    if (!st) return false;
    auto uit = unionMemberTypes.find(st);
    if (uit != unionMemberTypes.end()) { out = uit->second; return true; }
    auto lit = st->hasName() ? structLayout.find(st->getName().str()) : structLayout.end();
    if (lit != structLayout.end())
        for (const auto& s : lit->second) if (s.second.isBitfield) return false;
    // With unnamed bitfields (padding, no field to clang): the named fields only.
    auto fit = st->hasName() ? structFields.find(st->getName().str()) : structFields.end();
    if (unnamedBitData.count(st) && lit != structLayout.end() && fit != structFields.end()) {
        for (const auto& f : fit->second) out.push_back(lit->second.at(f.name).storageType);
        return true;
    }
    out.assign(st->element_begin(), st->element_end());
    return true;
}

// clang's shouldReturnTypeInRegister: 1, 2, 4 or 8 bytes, and every non-empty field
// (an array's element) is itself returned in a register. A bitfield struct's fields
// have integer types, so only its size decides.
bool CodeGen::x86RetInRegs(llvm::Type* ty) const {
    const llvm::DataLayout& DL = module->getDataLayout();
    uint64_t size = DL.getTypeAllocSize(ty);
    if (size != 1 && size != 2 && size != 4 && size != 8) return false;
    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(ty)) return x86RetInRegs(at->getElementType());
    auto* st = llvm::dyn_cast<llvm::StructType>(ty);
    if (!st) return true;
    std::vector<llvm::Type*> fields;
    if (!x86Fields(ty, fields)) return true;
    for (llvm::Type* f : fields)
        if (DL.getTypeAllocSize(f) != 0 && !x86RetInRegs(f)) return false;
    return true;
}

// clang's isSingleElementStruct: the one scalar a struct (or union) is made of through
// nested one-field structs and one-element arrays, with no padding around it; null if
// none.
llvm::Type* CodeGen::x86SingleElement(llvm::Type* ty) const {
    const llvm::DataLayout& DL = module->getDataLayout();
    std::vector<llvm::Type*> fields;
    if (!llvm::isa<llvm::StructType>(ty) || !x86Fields(ty, fields)) return nullptr;
    llvm::Type* found = nullptr;
    for (llvm::Type* f : fields) {
        if (DL.getTypeAllocSize(f) == 0) continue;
        if (found) return nullptr;
        while (auto* at = llvm::dyn_cast<llvm::ArrayType>(f)) {
            if (at->getNumElements() != 1) break;
            f = at->getElementType();
        }
        if (llvm::isa<llvm::StructType>(f) || llvm::isa<llvm::ArrayType>(f)) {
            found = x86SingleElement(f);
            if (!found) return nullptr;
        } else {
            found = f;
        }
    }
    if (found && DL.getTypeAllocSize(found) != DL.getTypeAllocSize(ty)) return nullptr;
    return found;
}

CodeGen::CAbiArg CodeGen::classifyCAbi(llvm::Type* ty, bool isReturn, CAbiTarget tgt,
                                       unsigned& freeInt, unsigned& freeSSE) const {
    const llvm::DataLayout& DL = module->getDataLayout();
    llvm::LLVMContext& ctx = *context;
    CAbiArg r;
    if (!isCAggregate(ty)) {
        // A scalar (or a literal fat value) consumes argument registers on SysV.
        if (tgt == CAbiTarget::SysV && !isReturn) {
            std::vector<std::pair<uint64_t, llvm::Type*>> ls;
            cabiLeaves(ty, 0, ls);
            for (const auto& l : ls) {
                if (l.second->isFloatingPointTy()) { if (freeSSE) --freeSSE; }
                else if (freeInt) --freeInt;
            }
        }
        return r;
    }
    uint64_t size = DL.getTypeAllocSize(ty);
    uint64_t align = cAlignOf(ty);
    if (size == 0) return r;
    std::vector<std::pair<uint64_t, llvm::Type*>> leaves;
    cabiLeaves(ty, 0, leaves);
    auto indirect = [&](CAbiArg::Kind k, uint64_t al) {
        r.kind = isReturn ? CAbiArg::Sret : k; r.ty = ty; r.align = (unsigned)al;
        return r;
    };

    // A struct or union holding an unnamed bitfield of nonzero width is no homogeneous
    // FP aggregate (clang: the bitfield is a non-FP member).
    bool homogeneous = !notHomogeneous.count(llvm::cast<llvm::StructType>(ty));
    switch (tgt) {
    case CAbiTarget::AArch64: {
        if (llvm::Type* h = homogeneous ? hfaType(leaves, size, DL, !isReturn) : nullptr) {
            r.kind = CAbiArg::Coerce; r.ty = h;
            // AAPCS64 (not Darwin) keeps a stack-passed HFA in 8-byte-aligned slots.
            if (!isReturn && !llvm::Triple(module->getTargetTriple()).isOSDarwin())
                r.stackAlign = align >= 16 ? 16 : 8;
            return r;
        }
        if (size > 16) return indirect(CAbiArg::Indirect, align);
        r.kind = CAbiArg::Coerce;
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx);
        if (size > 8) r.ty = llvm::ArrayType::get(i64, 2);
        else r.ty = isReturn ? (llvm::Type*)llvm::IntegerType::get(ctx, (unsigned)size * 8) : i64;
        return r;
    }
    case CAbiTarget::Win64: {
        if (size == 1 || size == 2 || size == 4 || size == 8) {
            r.kind = CAbiArg::Coerce; r.ty = llvm::IntegerType::get(ctx, (unsigned)size * 8);
            return r;
        }
        return indirect(CAbiArg::Indirect, align);
    }
    case CAbiTarget::ARM32: {
        std::string tt = llvm::Triple(module->getTargetTriple()).str();
        bool hard = tt.size() >= 2 && tt.compare(tt.size() - 2, 2, "hf") == 0;
        if (hard) {
            if (llvm::Type* h = homogeneous ? hfaType(leaves, size, DL, false) : nullptr) { r.kind = CAbiArg::Coerce; r.ty = h; return r; }
        }
        if (isReturn) {
            if (size <= 4) { r.kind = CAbiArg::Coerce; r.ty = llvm::Type::getInt32Ty(ctx); return r; }
            return indirect(CAbiArg::Sret, align);
        }
        if (size > 64) return indirect(CAbiArg::ByVal, std::min<uint64_t>(std::max<uint64_t>(align, 4), 8));
        r.kind = CAbiArg::Coerce;
        if (align <= 4) r.ty = llvm::ArrayType::get(llvm::Type::getInt32Ty(ctx), (size + 3) / 4);
        else r.ty = llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx), (size + 7) / 8);
        return r;
    }
    case CAbiTarget::SysV: {
        // Classify each eightbyte INTEGER or SSE (INTEGER wins a merge); anything over
        // 16 bytes or with a misaligned field goes to memory.
        // In memory: a result via sret; an argument byval, except that once the integer
        // registers are exhausted an eightbyte-sized one is passed as an integer (it lands
        // in the same stack slot; clang does the same).
        auto memory = [&]() {
            if (!isReturn && freeInt == 0 && align <= 8 && size <= 8) {
                r.kind = CAbiArg::Coerce; r.ty = llvm::IntegerType::get(ctx, (unsigned)size * 8);
                return r;
            }
            return indirect(CAbiArg::ByVal, isReturn ? align : std::max<uint64_t>(align, 8));
        };
        if (size > 16) return memory();
        enum Cls { None, Int, Sse };
        Cls cls[2] = {None, None};
        for (const auto& l : leaves) {
            uint64_t lsz = DL.getTypeAllocSize(l.second);
            uint64_t lal = DL.getABITypeAlign(l.second).value();
            if (l.first % lal != 0 || l.first / 8 != (l.first + lsz - 1) / 8) return memory();
            Cls c = l.second->isFloatingPointTy() ? Sse : Int;
            Cls& slot = cls[l.first / 8];
            if (slot == None || c == Int) slot = c;
        }
        unsigned nEb = size > 8 ? 2 : 1;
        // Unnamed bitfields are no class (NoClass) but count as data for the integer widths
        // (clang's BitsContainNoUserData). An eightbyte holding no named field takes no
        // register: the other one passes alone (the high one at offset 8).
        std::vector<std::pair<uint64_t, uint64_t>> unnamed;
        cabiUnnamedData(ty, 0, unnamed);
        unsigned skipEb = 2;
        if (!unnamed.empty() && nEb == 2 && (cls[0] == None) != (cls[1] == None))
            skipEb = cls[0] == None ? 0 : 1;
        auto leafAt = [&](uint64_t off) -> llvm::Type* {
            for (const auto& l : leaves) if (l.first == off) return l.second;
            return nullptr;
        };
        // The bytes of eightbyte `o` that hold data: the furthest end of a leaf in it. A
        // union contributes every member, so its widest member decides (clang lowers a
        // union through its largest member).
        auto extentAt = [&](uint64_t o) -> uint64_t {
            uint64_t end = 0;
            for (const auto& l : leaves)
                if (l.first >= o && l.first < o + 8)
                    end = std::max<uint64_t>(end, l.first + DL.getTypeAllocSize(l.second) - o);
            for (const auto& d : unnamed) {
                uint64_t lo = std::max<uint64_t>(d.first, o), hi = std::min<uint64_t>(d.second, o + 8);
                if (lo < hi) end = std::max<uint64_t>(end, hi - o);
            }
            return end;
        };
        auto ebType = [&](unsigned eb) -> llvm::Type* {
            uint64_t o = eb * 8;
            if (cls[eb] == Sse) {
                bool dbl = false;
                for (const auto& l : leaves) if (l.first == o && l.second->isDoubleTy()) dbl = true;
                llvm::Type* t0 = leafAt(o);
                if (!t0 || dbl) return llvm::Type::getDoubleTy(ctx);
                llvm::Type* t1 = (size - o > 4) ? leafAt(o + 4) : nullptr;
                if (t1 && t1->isFloatTy()) return llvm::FixedVectorType::get(t0, 2);
                return t0;
            }
            llvm::Type* t0 = leafAt(o);
            if (t0 && (t0->isPointerTy() || t0->isIntegerTy(64))) return t0;
            uint64_t ext = extentAt(o);
            if (ext == 1 || ext == 2 || ext == 4) return llvm::IntegerType::get(ctx, (unsigned)ext * 8);
            return llvm::IntegerType::get(ctx, (unsigned)std::min<uint64_t>(size - o, 8) * 8);
        };
        unsigned needInt = 0, needSSE = 0;
        for (unsigned eb = 0; eb < nEb; ++eb) {
            if (eb == skipEb) continue;
            if (cls[eb] == Sse) ++needSSE; else ++needInt;
        }
        if (!isReturn) {
            if (needInt > freeInt || needSSE > freeSSE) return memory();
            freeInt -= needInt; freeSSE -= needSSE;
        }
        if (skipEb < 2) {
            r.kind = CAbiArg::Coerce; r.ty = ebType(1 - skipEb); r.offset = skipEb == 0 ? 8 : 0;
            return r;
        }
        llvm::Type* lo = ebType(0);
        if (nEb == 1) { r.kind = CAbiArg::Coerce; r.ty = lo; return r; }
        llvm::Type* hi = ebType(1);
        // The high part must start at offset 8: widen a 4-byte low part.
        uint64_t hiStart = (DL.getTypeAllocSize(lo) + DL.getABITypeAlign(hi).value() - 1)
                         / DL.getABITypeAlign(hi).value() * DL.getABITypeAlign(hi).value();
        if (hiStart != 8)
            lo = lo->isFloatTy() ? llvm::Type::getDoubleTy(ctx) : (llvm::Type*)llvm::Type::getInt64Ty(ctx);
        r.ty = llvm::StructType::get(ctx, {lo, hi});
        r.kind = isReturn ? CAbiArg::Coerce : CAbiArg::Expand;
        return r;
    }
    case CAbiTarget::X86: {
        // cdecl on the stack. A result: Linux (and the other SysV i386 systems) return
        // every aggregate through sret; Darwin, Windows and the BSDs return one of 1, 2,
        // 4 or 8 bytes in registers (EAX:EDX), a single `float`/`double` in ST0 (not
        // under MSVC's rules), a single pointer as itself.
        llvm::Triple t(module->getTargetTriple());
        if (isReturn) {
            bool inRegs = t.isOSDarwin() || t.isOSWindows() || t.isOSFreeBSD() ||
                          t.isOSOpenBSD() || t.isOSDragonFly();
            bool win32Struct = t.isOSWindows() && !t.isOSCygMing();
            if (inRegs && x86RetInRegs(ty)) {
                llvm::Type* e = x86SingleElement(ty);
                r.kind = CAbiArg::Coerce;
                if (e && ((!win32Struct && e->isFloatingPointTy()) || e->isPointerTy())) r.ty = e;
                else r.ty = llvm::IntegerType::get(ctx, (unsigned)size * 8);
                return r;
            }
            return indirect(CAbiArg::Sret, align);
        }
        // An argument of at most 16 bytes made only of 32- and 64-bit scalars with no
        // padding is passed as those scalars (clang's expansion); anything else byval,
        // in a 4-byte-aligned stack slot.
        // A struct or union with an unnamed bitfield is not expanded (clang expands no
        // record with a bitfield).
        std::vector<llvm::Type*> fields;
        if (size <= 16 && !unnamedBitData.count(llvm::cast<llvm::StructType>(ty)) && x86Fields(ty, fields)) {
            uint64_t sum = 0;
            bool scalar = !fields.empty();
            for (llvm::Type* f : fields) {
                uint64_t fs = DL.getTypeAllocSize(f);
                bool basic = f->isPointerTy() || f->isFloatTy() || f->isDoubleTy() ||
                             f->isIntegerTy(32) || f->isIntegerTy(64);
                if (!basic) { scalar = false; break; }
                sum += fs;
            }
            if (scalar && sum == size) {
                r.kind = CAbiArg::Expand;
                r.ty = llvm::StructType::get(ctx, fields);
                return r;
            }
        }
        return indirect(CAbiArg::ByVal, 4);
    }
    case CAbiTarget::None:
        break;
    }
    return r;
}

bool CodeGen::buildCAbiSig(llvm::FunctionType* logical, CAbiSig& sig) const {
    CAbiTarget tgt = cabiTarget();
    if (tgt == CAbiTarget::None) return false;
    bool any = isCAggregate(logical->getReturnType());
    for (llvm::Type* p : logical->params()) any = any || isCAggregate(p);
    if (!any) return false;
    sig.logical = logical;
    unsigned freeInt = 6, freeSSE = 8;
    llvm::Type* rt = logical->getReturnType();
    if (!rt->isVoidTy()) sig.ret = classifyCAbi(rt, true, tgt, freeInt, freeSSE);
    if (sig.ret.kind == CAbiArg::Sret && freeInt) --freeInt;
    for (llvm::Type* p : logical->params())
        sig.params.push_back(classifyCAbi(p, false, tgt, freeInt, freeSSE));
    llvm::Type* ptrTy = llvm::PointerType::get(*context, 0);
    std::vector<llvm::Type*> irParams;
    if (sig.ret.kind == CAbiArg::Sret) irParams.push_back(ptrTy);
    for (size_t i = 0; i < sig.params.size(); ++i) {
        const CAbiArg& a = sig.params[i];
        switch (a.kind) {
        case CAbiArg::Direct: irParams.push_back(logical->getParamType((unsigned)i)); break;
        case CAbiArg::Coerce: irParams.push_back(a.ty); break;
        case CAbiArg::Expand:
            for (llvm::Type* e : llvm::cast<llvm::StructType>(a.ty)->elements()) irParams.push_back(e);
            break;
        default: irParams.push_back(ptrTy); break;
        }
    }
    llvm::Type* irRet = sig.ret.kind == CAbiArg::Sret ? llvm::Type::getVoidTy(*context)
                      : sig.ret.kind == CAbiArg::Coerce ? sig.ret.ty : rt;
    sig.lowered = llvm::FunctionType::get(irRet, irParams, logical->isVarArg());
    return true;
}

// The ABI attributes (sret / byval + alignment) at their lowered parameter indices.
void CodeGen::addCAbiAttrs(const CAbiSig& sig,
                           const std::function<void(unsigned, llvm::Attribute)>& add) const {
    llvm::LLVMContext& ctx = *context;
    unsigned idx = 0;
    if (sig.ret.kind == CAbiArg::Sret) {
        add(0, llvm::Attribute::getWithStructRetType(ctx, sig.ret.ty));
        add(0, llvm::Attribute::getWithAlignment(ctx, llvm::Align(sig.ret.align)));
        idx = 1;
    }
    for (const auto& a : sig.params) {
        if (a.stackAlign)
            add(idx, llvm::Attribute::getWithStackAlignment(ctx, llvm::Align(a.stackAlign)));
        if (a.kind == CAbiArg::ByVal) {
            add(idx, llvm::Attribute::getWithByValType(ctx, a.ty));
            add(idx, llvm::Attribute::getWithAlignment(ctx, llvm::Align(a.align)));
        }
        idx += a.kind == CAbiArg::Expand
             ? llvm::cast<llvm::StructType>(a.ty)->getNumElements() : 1;
    }
}

llvm::Function* CodeGen::declareCAbiExtern(const std::string& name, const CAbiSig& sig) {
    llvm::Function* fn = llvm::Function::Create(sig.lowered, llvm::Function::ExternalLinkage,
                                                name, module.get());
    addCAbiAttrs(sig, [&](unsigned i, llvm::Attribute a) { fn->addParamAttr(i, a); });
    externAbi[name] = sig;
    return fn;
}

// Reinterpret `v` as `to` through memory (clang's coerced load/store): store it into a
// slot big and aligned enough for both types, then load the other type back.
llvm::Value* CodeGen::cabiReinterpret(llvm::Value* v, llvm::Type* to) {
    if (v->getType() == to) return v;
    const llvm::DataLayout& DL = module->getDataLayout();
    llvm::Type* from = v->getType();
    llvm::Type* big = DL.getTypeAllocSize(from) >= DL.getTypeAllocSize(to) ? from : to;
    llvm::AllocaInst* slot = entryAlloca(big, nullptr, "cabi.coerce");
    slot->setAlignment(std::max(DL.getABITypeAlign(from), DL.getABITypeAlign(to)));
    if (DL.getTypeAllocSize(from) < DL.getTypeAllocSize(to))
        builder->CreateStore(llvm::Constant::getNullValue(to), slot);
    builder->CreateStore(v, slot);
    return builder->CreateLoad(to, slot);
}

llvm::Value* CodeGen::cabiToCoerced(llvm::Value* v, const CAbiArg& a) {
    if (!a.offset) return cabiReinterpret(v, a.ty);
    llvm::Type* wrap = llvm::StructType::get(*context,
        {llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), a.offset), a.ty}, /*isPacked=*/true);
    return builder->CreateExtractValue(cabiReinterpret(v, wrap), {1});
}

llvm::Value* CodeGen::cabiFromCoerced(llvm::Value* c, const CAbiArg& a, llvm::Type* logical) {
    if (!a.offset) return cabiReinterpret(c, logical);
    llvm::Type* wrap = llvm::StructType::get(*context,
        {llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), a.offset), a.ty}, /*isPacked=*/true);
    return cabiReinterpret(builder->CreateInsertValue(llvm::Constant::getNullValue(wrap), c, {1}), logical);
}

llvm::Value* CodeGen::emitCAbiCall(llvm::Function* fn, const CAbiSig& sig,
                                   const std::vector<llvm::Value*>& args, bool allowInvoke) {
    std::vector<llvm::Value*> ir;
    llvm::AllocaInst* sretSlot = nullptr;
    if (sig.ret.kind == CAbiArg::Sret) {
        sretSlot = entryAlloca(sig.ret.ty, nullptr, "cabi.sret");
        ir.push_back(sretSlot);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        llvm::Value* v = args[i];
        if (i >= sig.params.size()) { ir.push_back(v); continue; }   // variadic tail
        const CAbiArg& a = sig.params[i];
        switch (a.kind) {
        case CAbiArg::Direct: ir.push_back(v); break;
        case CAbiArg::Coerce: ir.push_back(cabiToCoerced(v, a)); break;
        case CAbiArg::Expand: {
            llvm::Value* pair = cabiReinterpret(v, a.ty);
            for (unsigned e = 0; e < llvm::cast<llvm::StructType>(a.ty)->getNumElements(); ++e)
                ir.push_back(builder->CreateExtractValue(pair, {e}));
            break;
        }
        default: {                                   // a caller-owned copy in memory
            llvm::AllocaInst* copy = entryAlloca(v->getType(), nullptr, "cabi.arg");
            copy->setAlignment(std::max(copy->getAlign(), llvm::Align(a.align)));
            builder->CreateStore(v, copy);
            ir.push_back(copy);
            break;
        }
        }
    }
    llvm::Value* call = allowInvoke ? createMaybeInvoke(sig.lowered, fn, ir)
                                    : builder->CreateCall(sig.lowered, fn, ir);
    if (auto* cb = llvm::dyn_cast<llvm::CallBase>(call))
        addCAbiAttrs(sig, [&](unsigned i, llvm::Attribute a) { cb->addParamAttr(i, a); });
    llvm::Type* rt = sig.logical->getReturnType();
    if (sig.ret.kind == CAbiArg::Sret) return builder->CreateLoad(rt, sretSlot);
    if (sig.ret.kind == CAbiArg::Coerce) return cabiFromCoerced(call, sig.ret, rt);
    return call;
}

// C calls an Eskiu function through a raw pointer (`(*void)f`): the thunk takes the
// lowered C signature, rebuilds the Eskiu-level values and calls `target` with the
// Eskiu convention (first-class aggregates, hidden result pointer for a large struct),
// then hands the result back the C way.
llvm::Function* CodeGen::cabiCallbackThunk(llvm::Function* target) {
    std::string name = target->getName().str();
    if (externAbi.count(name) || target->isVarArg()) return target;
    auto sretIt = funcSretTypes.find(name);
    bool eskSret = sretIt != funcSretTypes.end();
    llvm::FunctionType* fty = target->getFunctionType();
    std::vector<llvm::Type*> lps(fty->param_begin() + (eskSret ? 1 : 0), fty->param_end());
    llvm::Type* lret = eskSret ? sretIt->second : fty->getReturnType();
    // A `va_list` parameter comes from C the way C passes its `va_list` (see evalCVaList).
    std::vector<llvm::Type*> cps = lps;
    bool anyVa = false;
    for (llvm::Type*& p : cps) {
        auto* st = llvm::dyn_cast<llvm::StructType>(p);
        if (st && !st->isLiteral() && st->getName() == "__va_list") {
            p = llvm::PointerType::get(*context, 0);
            anyVa = true;
        }
    }
    llvm::FunctionType* cfty = llvm::FunctionType::get(lret, cps, false);
    CAbiSig sig;
    if (!buildCAbiSig(cfty, sig)) {
        if (!anyVa || cabiTarget() == CAbiTarget::None) return target;
        sig.logical = cfty;
        sig.lowered = cfty;
        sig.params.assign(cps.size(), CAbiArg{});
    }
    std::string tname = "__cabi_" + name;
    if (llvm::Function* have = module->getFunction(tname)) return have;
    llvm::Function* thunk = llvm::Function::Create(sig.lowered, llvm::Function::InternalLinkage,
                                                   tname, module.get());
    addCAbiAttrs(sig, [&](unsigned i, llvm::Attribute a) { thunk->addParamAttr(i, a); });
    if (sig.ret.kind == CAbiArg::Direct && !lret->isVoidTy() && funcEskiuReturnType.count(name)) {
        llvm::Attribute::AttrKind ext = cabiExtAttr(funcEskiuReturnType[name], lret);
        if (ext != llvm::Attribute::None) thunk->addRetAttr(ext);
    }

    llvm::IRBuilderBase::InsertPointGuard guard(*builder);
    builder->SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", thunk));
    auto ai = thunk->arg_begin();
    llvm::Value* sretOut = nullptr;
    if (sig.ret.kind == CAbiArg::Sret) sretOut = &*ai++;
    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < sig.params.size(); ++i) {
        const CAbiArg& a = sig.params[i];
        switch (a.kind) {
        case CAbiArg::Direct:
            args.push_back(cps[i] != lps[i] ? cVaListToEskiu(&*ai++, lps[i]) : &*ai++);
            break;
        case CAbiArg::Coerce: args.push_back(cabiFromCoerced(&*ai++, a, lps[i])); break;
        case CAbiArg::Expand: {
            auto* st = llvm::cast<llvm::StructType>(a.ty);
            llvm::Value* pair = llvm::UndefValue::get(st);
            for (unsigned e = 0; e < st->getNumElements(); ++e)
                pair = builder->CreateInsertValue(pair, &*ai++, {e});
            args.push_back(cabiReinterpret(pair, lps[i]));
            break;
        }
        default: args.push_back(builder->CreateLoad(lps[i], &*ai++)); break;
        }
    }
    llvm::Value* r = nullptr;
    if (eskSret) {
        llvm::AllocaInst* tmp = entryAlloca(lret, nullptr, "cabi.res");
        args.insert(args.begin(), tmp);
        builder->CreateCall(target, args);
        r = builder->CreateLoad(lret, tmp);
    } else {
        r = builder->CreateCall(target, args);
    }
    if (sig.ret.kind == CAbiArg::Sret) {
        builder->CreateStore(r, sretOut);
        builder->CreateRetVoid();
    } else if (lret->isVoidTy()) {
        builder->CreateRetVoid();
    } else {
        builder->CreateRet(sig.ret.kind == CAbiArg::Coerce ? cabiToCoerced(r, sig.ret) : r);
    }
    return thunk;
}

// A `va_list` argument of a C function (vprintf, vsnprintf, ...), passed the way C passes
// its `va_list`: on x86-64 System V and AArch64 outside Darwin (and Windows) it is an
// array/struct type that goes by address; elsewhere (Darwin AArch64, Windows x64, 32-bit
// ARM) it is a `char*`, the pointer `llvm.va_start` stores at offset 0 of the storage.
llvm::Value* CodeGen::evalCVaList(const ExprPtr& arg) {
    llvm::Triple t(module->getTargetTriple());
    bool byAddr = !t.isOSWindows() && (t.getArch() == llvm::Triple::x86_64
                                      || (t.isAArch64() && !t.isOSDarwin()));
    llvm::Type* vaTy = getTypeFromString("va_list");
    if (byAddr) {
        if (auto* id = dynamic_cast<IdentExpr*>(arg.get()))
            if (lookupSymbol(id->name)) return evaluateLValue(arg);
        llvm::AllocaInst* copy = entryAlloca(vaTy, nullptr, "va.arg");
        builder->CreateStore(evaluateExpr(arg), copy);
        return copy;
    }
    if (auto* id = dynamic_cast<IdentExpr*>(arg.get()))
        if (lookupSymbol(id->name))
            return builder->CreateLoad(llvm::PointerType::get(*context, 0), evaluateLValue(arg));
    return builder->CreateExtractValue(evaluateExpr(arg), {0});
}

// The Eskiu `va_list` value of a `va_list` C passed in: on x86-64 System V and AArch64
// outside Darwin and Windows the C `va_list` (24 or 32 bytes) at that address, copied
// into the front of the Eskiu storage; elsewhere the `char*` itself, at offset 0.
llvm::Value* CodeGen::cVaListToEskiu(llvm::Value* c, llvm::Type* vaTy) {
    llvm::Triple t(module->getTargetTriple());
    bool x86 = t.getArch() == llvm::Triple::x86_64;
    bool byAddr = !t.isOSWindows() && (x86 || (t.isAArch64() && !t.isOSDarwin()));
    if (!byAddr) return builder->CreateInsertValue(llvm::Constant::getNullValue(vaTy), c, {0});
    llvm::AllocaInst* slot = entryAlloca(vaTy, nullptr, "va.in");
    builder->CreateStore(llvm::Constant::getNullValue(vaTy), slot);
    builder->CreateMemCpy(slot, llvm::MaybeAlign(8), c, llvm::MaybeAlign(8), x86 ? 24 : 32);
    return builder->CreateLoad(vaTy, slot);
}

// `va_arg<T>(ap)`: the next variadic argument, read from the `va_list` storage at `ap`.
// On AArch64 outside Darwin and Windows the `va_list` is the AAPCS64 struct
// {__stack, __gr_top, __vr_top, __gr_offs, __vr_offs}, which LLVM's `va_arg` does not
// expand, so the read is spelled out as clang does (AArch64ABIInfo::EmitAAPCSVAArg): an
// integer or pointer comes from the general-register save area and a float or double from
// the FP/SIMD one while the offset is negative, then from the stack in 8-byte slots.
// Other targets keep the `va_arg` instruction (a `char*` list, or x86-64 System V, which
// LLVM expands for the scalars `va_arg` accepts).
llvm::Value* CodeGen::emitVaArg(llvm::Value* ap, llvm::Type* ty) {
    llvm::Triple t(module->getTargetTriple());
    bool isFp = ty->isFloatingPointTy();
    if (!t.isAArch64() || t.isOSDarwin() || t.isOSWindows() || (!isFp && !ty->isIntOrPtrTy()))
        return builder->CreateVAArg(ap, ty, "va.arg");
    const llvm::DataLayout& dl = module->getDataLayout();
    uint64_t size = dl.getTypeAllocSize(ty);
    bool be = dl.isBigEndian();
    llvm::Type* i8 = llvm::Type::getInt8Ty(*context);
    llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
    llvm::Type* ptr = llvm::PointerType::get(*context, 0);
    llvm::Type* vaTy = getTypeFromString("va_list");
    unsigned offsField = isFp ? 4 : 3, topField = isFp ? 2 : 1;
    int slot = isFp ? 16 : 8;
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* maybeReg = llvm::BasicBlock::Create(*context, "va.maybe_reg", fn);
    llvm::BasicBlock* inReg = llvm::BasicBlock::Create(*context, "va.in_reg", fn);
    llvm::BasicBlock* onStack = llvm::BasicBlock::Create(*context, "va.on_stack", fn);
    llvm::BasicBlock* end = llvm::BasicBlock::Create(*context, "va.end", fn);
    llvm::Value* offsP = builder->CreateStructGEP(vaTy, ap, offsField, "va.offs_p");
    llvm::Value* offs = builder->CreateLoad(i32, offsP, "va.offs");
    builder->CreateCondBr(builder->CreateICmpSGE(offs, llvm::ConstantInt::get(i32, 0)), onStack, maybeReg);
    builder->SetInsertPoint(maybeReg);
    llvm::Value* next = builder->CreateAdd(offs, llvm::ConstantInt::get(i32, slot), "va.next");
    builder->CreateStore(next, offsP);
    builder->CreateCondBr(builder->CreateICmpSLE(next, llvm::ConstantInt::get(i32, 0)), inReg, onStack);
    builder->SetInsertPoint(inReg);
    llvm::Value* top = builder->CreateLoad(ptr, builder->CreateStructGEP(vaTy, ap, topField), "va.top");
    llvm::Value* regAddr = builder->CreateGEP(i8, top, builder->CreateSExt(offs, llvm::Type::getInt64Ty(*context)), "va.reg_addr");
    if (be && size < (uint64_t)slot)
        regAddr = builder->CreateConstGEP1_64(i8, regAddr, slot - size);
    builder->CreateBr(end);
    builder->SetInsertPoint(onStack);
    llvm::Value* stackP = builder->CreateStructGEP(vaTy, ap, 0, "va.stack_p");
    llvm::Value* stack = builder->CreateLoad(ptr, stackP, "va.stack");
    builder->CreateStore(builder->CreateConstGEP1_64(i8, stack, 8), stackP);
    llvm::Value* stackAddr = stack;
    if (be && size < 8) stackAddr = builder->CreateConstGEP1_64(i8, stack, 8 - size);
    builder->CreateBr(end);
    builder->SetInsertPoint(end);
    llvm::PHINode* addr = builder->CreatePHI(ptr, 2, "va.addr");
    addr->addIncoming(regAddr, inReg);
    addr->addIncoming(stackAddr, onStack);
    return builder->CreateLoad(ty, addr, "va.arg");
}

// An argument for a C function pointer parameter (an extern's fn-typed parameter): the
// C address of the named top-level function (through its C-ABI thunk when needed), or
// null. The type checker rejects anything else (a closure value has an environment).
llvm::Value* CodeGen::evalCFnPointer(const ExprPtr& arg) {
    if (auto* id = dynamic_cast<IdentExpr*>(arg.get()))
        if (!lookupSymbol(id->name))
            if (llvm::Function* fn = module->getFunction(id->name)) return cabiCallbackThunk(fn);
    llvm::Value* v = evaluateExpr(arg);
    if (!v->getType()->isPointerTy())
        throw std::runtime_error("a C function pointer argument must name a top-level function");
    return v;
}
