#include "codegen.h"
#include "../ast/type_qual.h"
#include "llvm/TargetParser/Triple.h"
#include <functional>
#include <set>
#include "codegen_cabi_util.h"

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
    if (t.getArch() == llvm::Triple::wasm32) return CAbiTarget::Wasm32;
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
    case CAbiTarget::Wasm32: {
        // clang's WebAssembly C ABI: a struct made of one scalar (through one-field
        // structs and one-element arrays) is that scalar; any other aggregate goes
        // byval at its alignment, and comes back through sret.
        if (llvm::Type* e = x86SingleElement(ty)) {
            r.kind = CAbiArg::Coerce; r.ty = e;
            return r;
        }
        return indirect(CAbiArg::ByVal, align);
    }
    case CAbiTarget::None:
        break;
    }
    return r;
}
