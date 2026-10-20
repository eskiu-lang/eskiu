#include "codegen.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

// ============================================================================
// Symbol Table Management
// ============================================================================

void CodeGen::pushScope() {
    scopeStack.push_back(symbolTable);
    varTypeStack.push_back({});
}

void CodeGen::popScope() {
    if (!scopeStack.empty()) {
        symbolTable = scopeStack.back();
        scopeStack.pop_back();
    }
    if (!varTypeStack.empty()) {
        varTypeStack.pop_back();
    }
}

void CodeGen::defineVarType(const std::string& name, const std::string& type) {
    if (!varTypeStack.empty())
        varTypeStack.back()[name] = type;
    else
        globalVarTypes[name] = type;   // top-level / global scope
}

std::string CodeGen::lookupVarType(const std::string& name) const {
    for (auto it = varTypeStack.rbegin(); it != varTypeStack.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
    auto g = globalVarTypes.find(name);
    return g != globalVarTypes.end() ? g->second : "";
}

llvm::Constant* CodeGen::evaluateConstantExpr(const ExprPtr& expr) {
    // A non-capturing lambda is a compile-time-constant closure {fn_ptr, null}:
    // emit its function and fold to the constant fat pointer, so a global/static
    // initializer holds a real callable instead of a null closure. (A capturing
    // lambda needs a live frame, so it can't be a global constant.)
    if (auto* lam = dynamic_cast<LambdaExpr*>(expr.get())) {
        if (!lam->captures.empty()) return nullptr;
        std::string lambdaName = "__lambda" + std::to_string(lambdaSeq++);
        llvm::Function* func = emitLambdaFunction(lam, lambdaName, nullptr);
        auto* fatTy = llvm::cast<llvm::StructType>(getTypeFromString("fn()->void"));
        return llvm::ConstantStruct::get(fatTy,
            {func, llvm::ConstantPointerNull::get(
                       llvm::PointerType::get(*context, 0))});
    }
    return foldViaCodegen(expr, nullptr);
}

llvm::Constant* CodeGen::foldViaCodegen(const ExprPtr& expr, llvm::Type* targetTy,
                                        const std::string& asIface) {
    // The IRBuilder constant-folds an operation on constant operands instead of emitting
    // an instruction, so running the normal expression codegen over a constant tree
    // yields an llvm::Constant. It runs in a throwaway function (so a non-constant
    // operand has somewhere to emit into) that is deleted afterwards.
    llvm::IRBuilderBase::InsertPointGuard guard(*builder);
    llvm::Function* savedFn = currentFunction;
    size_t depth = exprValueStack.size();
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(*context), false);
    auto* scratch = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                           "__eskiu.consteval", module.get());
    builder->SetInsertPoint(llvm::BasicBlock::Create(*context, "entry", scratch));
    currentFunction = scratch;
    ++constEvalDepth;
    llvm::Value* v = nullptr;
    try {
        v = asIface.empty() ? evaluateExpr(expr) : evalForType(expr, asIface);
        if (v && targetTy && asIface.empty()) v = coerceValue(v, targetTy, eskiuUnsigned(getExprEskiuType(expr)));
    } catch (const std::exception&) {
        v = nullptr;
    }
    --constEvalDepth;
    while (exprValueStack.size() > depth) exprValueStack.pop();
    currentFunction = savedFn;
    auto* c = llvm::dyn_cast_or_null<llvm::Constant>(v);
    scratch->eraseFromParent();
    if (c && targetTy && c->getType() != targetTy) return nullptr;
    return c;
}

llvm::Constant* CodeGen::constInitializer(const ExprPtr& expr, llvm::Type* declType) {
    // Array literal `{...}` against a fixed-size array type: fold each element to the
    // array's element type and zero-fill any tail (C semantics: `int[3] = {1}` → {1,0,0}).
    if (auto* arr = dynamic_cast<ArrayLitExpr*>(expr.get())) {
        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(declType);
        if (!arrTy) return nullptr;
        llvm::Type* elemTy = arrTy->getElementType();
        uint64_t n = arrTy->getNumElements();
        std::vector<llvm::Constant*> elems;
        bool split = false;
        for (auto& e : arr->elements) {
            if (elems.size() >= n) break;        // extra elements ignored
            llvm::Constant* c = constInitializer(e, elemTy);   // recurse (nested arrays)
            if (!c) return nullptr;              // a non-constant element: not foldable
            if (c->getType() != elemTy) split = true;
            elems.push_back(c);
        }
        // A long zero tail (or an element already in this form) is a struct of the given
        // elements and one zeroinitializer array for the rest, as clang emits it: the same
        // bytes, without a constant per zero element.
        uint64_t tail = n - elems.size();
        if (split || tail >= kSplitZeroTail) {
            if (tail) elems.push_back(llvm::ConstantAggregateZero::get(llvm::ArrayType::get(elemTy, tail)));
            return llvm::ConstantStruct::getAnon(*context, elems);
        }
        while (elems.size() < n) elems.push_back(llvm::Constant::getNullValue(elemTy));
        return llvm::ConstantArray::get(arrTy, elems);
    }
    // Struct literal `S{ ... }` against a struct type: fold each named/positional field
    // to a constant and zero-fill the rest. A bitfield struct or a union is folded
    // through its byte image (constAggregateImage).
    if (auto* si0 = dynamic_cast<StructInitExpr*>(expr.get())) {
        ExprPtr full = withDefaults(si0, expr);       // omitted fields take their defaults
        auto* si = static_cast<StructInitExpr*>(full.get());
        std::string sname = resolveStructInitName(si->structName);
        auto fit = structFields.find(sname);
        auto stIt = structTypes.find(sname);
        if (fit == structFields.end() || stIt == structTypes.end()) return nullptr;
        if (structLayout.count(sname) || unionFields.count(sname))
            return constAggregateImage(si, sname);
        const auto& fields = fit->second;
        llvm::StructType* st = stIt->second;
        std::vector<llvm::Constant*> vals(fields.size(), nullptr);
        bool named = !si->fieldInits.empty() && !si->fieldInits[0].first.empty();
        auto foldField = [&](size_t i, const ExprPtr& e) -> bool {
            if (i >= fields.size()) return true;
            llvm::Constant* c = constInitializer(e, st->getElementType((unsigned)i));
            if (!c) return false;               // a provided field isn't constant → bail
            vals[i] = c;
            return true;
        };
        if (named) {
            for (const auto& [fname, e] : si->fieldInits)
                if (long fi = fieldIndexOf(sname, fields, fname); fi >= 0 && !foldField((size_t)fi, e)) return nullptr;
        } else {
            for (size_t i = 0; i < si->fieldInits.size() && i < fields.size(); ++i)
                if (!foldField(i, si->fieldInits[i].second)) return nullptr;
        }
        bool split = false;
        for (size_t i = 0; i < vals.size(); ++i) {
            if (!vals[i]) vals[i] = llvm::Constant::getNullValue(st->getElementType((unsigned)i));
            if (vals[i]->getType() != st->getElementType((unsigned)i)) split = true;
        }
        // A field in the split array form (above) makes the struct a literal one with the
        // same layout.
        if (split) return llvm::ConstantStruct::getAnon(*context, vals, st->isPacked());
        return llvm::ConstantStruct::get(st, vals);
    }
    if (dynamic_cast<LambdaExpr*>(expr.get())) {
        llvm::Constant* c = evaluateConstantExpr(expr);
        return (c && c->getType() == declType) ? c : nullptr;
    }
    // An interface value `{data, vtable}`: `null`, or a boxed link-time address.
    for (const auto& [iname, fatTy] : ifaceFatPtrTypes)
        if (fatTy == declType) return foldViaCodegen(expr, declType, iname);
    return foldViaCodegen(expr, declType);
}

namespace {
// The bytes of a constant aggregate being assembled (little-endian targets): `opaque`
// marks bytes held by a constant with no byte form (an address), which only the exact
// element at that offset (`direct`) can carry.
struct ConstImage {
    std::vector<uint8_t> bytes;
    std::vector<bool> opaque;
    std::map<uint64_t, llvm::Constant*> direct;
};

bool writeConstBytes(const llvm::DataLayout& DL, llvm::Constant* c, ConstImage& img, uint64_t off) {
    uint64_t size = DL.getTypeAllocSize(c->getType()).getFixedValue();
    if (off + size > img.bytes.size()) return false;
    if (c->isNullValue()) return true;
    llvm::APInt bits;
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c)) bits = ci->getValue();
    else if (auto* cf = llvm::dyn_cast<llvm::ConstantFP>(c)) bits = cf->getValueAPF().bitcastToAPInt();
    else if (auto* st = llvm::dyn_cast<llvm::StructType>(c->getType())) {
        const llvm::StructLayout* sl = DL.getStructLayout(st);
        bool ok = true;
        for (unsigned i = 0; i < st->getNumElements(); ++i)
            ok &= writeConstBytes(DL, c->getAggregateElement(i), img, off + sl->getElementOffset(i));
        return ok;
    } else if (auto* at = llvm::dyn_cast<llvm::ArrayType>(c->getType())) {
        uint64_t esz = DL.getTypeAllocSize(at->getElementType()).getFixedValue();
        bool ok = true;
        for (uint64_t i = 0; i < at->getNumElements(); ++i)
            ok &= writeConstBytes(DL, c->getAggregateElement((unsigned)i), img, off + i * esz);
        return ok;
    } else {
        for (uint64_t i = 0; i < size; ++i) img.opaque[off + i] = true;
        return false;
    }
    unsigned nbits = bits.getBitWidth();
    for (uint64_t i = 0; i < size && i * 8 < nbits; ++i)
        img.bytes[off + i] = (uint8_t)bits.extractBitsAsZExtValue(std::min(8u, nbits - (unsigned)i * 8), (unsigned)i * 8);
    return true;
}

llvm::Constant* constFromImage(const llvm::DataLayout& DL, llvm::Type* t, const ConstImage& img,
                               uint64_t off) {
    auto dit = img.direct.find(off);
    if (dit != img.direct.end() && dit->second->getType() == t) return dit->second;
    // An address stored where the layout has a pointer-sized integer (a union whose layout
    // member is an int64): the link-time address converts in place.
    if (dit != img.direct.end() && dit->second->getType()->isPointerTy() && t->isIntegerTy() &&
        DL.getTypeAllocSize(t) == DL.getTypeAllocSize(dit->second->getType()))
        return llvm::ConstantExpr::getPtrToInt(dit->second, t);
    uint64_t size = DL.getTypeAllocSize(t).getFixedValue();
    if (off + size > img.bytes.size()) return nullptr;
    if (auto* st = llvm::dyn_cast<llvm::StructType>(t)) {
        const llvm::StructLayout* sl = DL.getStructLayout(st);
        std::vector<llvm::Constant*> vals;
        for (unsigned i = 0; i < st->getNumElements(); ++i) {
            llvm::Constant* e = constFromImage(DL, st->getElementType(i), img, off + sl->getElementOffset(i));
            if (!e) return nullptr;
            vals.push_back(e);
        }
        return llvm::ConstantStruct::get(st, vals);
    }
    if (auto* at = llvm::dyn_cast<llvm::ArrayType>(t)) {
        uint64_t esz = DL.getTypeAllocSize(at->getElementType()).getFixedValue();
        std::vector<llvm::Constant*> vals;
        for (uint64_t i = 0; i < at->getNumElements(); ++i) {
            llvm::Constant* e = constFromImage(DL, at->getElementType(), img, off + i * esz);
            if (!e) return nullptr;
            vals.push_back(e);
        }
        return llvm::ConstantArray::get(at, vals);
    }
    bool zero = true;
    for (uint64_t i = 0; i < size; ++i) {
        if (img.opaque[off + i]) return nullptr;
        if (img.bytes[off + i]) zero = false;
    }
    if (zero) return llvm::Constant::getNullValue(t);
    unsigned nbits = (unsigned)DL.getTypeSizeInBits(t).getFixedValue();
    llvm::APInt bits(nbits, 0);
    for (uint64_t i = 0; i < size && i * 8 < nbits; ++i)
        bits.insertBits((uint64_t)img.bytes[off + i], (unsigned)i * 8, std::min(8u, nbits - (unsigned)i * 8));
    if (t->isIntegerTy()) return llvm::ConstantInt::get(t, bits);
    if (t->isFloatingPointTy()) return llvm::ConstantFP::get(t, llvm::APFloat(t->getFltSemantics(), bits));
    if (t->isPointerTy())      // integer bytes where the layout has a pointer
        return llvm::ConstantExpr::getIntToPtr(llvm::ConstantInt::get(t->getContext(), bits), t);
    return nullptr;
}
}  // namespace

llvm::Constant* CodeGen::constAggregateImage(StructInitExpr* si, const std::string& sname) {
    const llvm::DataLayout& DL = module->getDataLayout();
    if (!DL.isLittleEndian()) return nullptr;
    llvm::StructType* st = structTypes[sname];
    const auto& fields = structFields[sname];
    uint64_t size = DL.getTypeAllocSize(st).getFixedValue();
    ConstImage img{std::vector<uint8_t>(size, 0), std::vector<bool>(size, false), {}};
    bool isUnion = unionFields.count(sname) > 0;
    auto lit = structLayout.find(sname);
    bool named = !si->fieldInits.empty() && !si->fieldInits[0].first.empty();
    for (size_t k = 0; k < si->fieldInits.size(); ++k) {
        size_t idx = k;
        if (named) {
            long fi = fieldIndexOf(sname, fields, si->fieldInits[k].first);
            idx = fi >= 0 ? (size_t)fi : fields.size();
        }
        if (idx >= fields.size()) continue;
        const ExprPtr& e = si->fieldInits[k].second;
        if (isUnion) {
            llvm::Constant* c = constInitializer(e, getTypeFromString(fields[idx].type));
            if (!c) return nullptr;
            img.direct[0] = c;
            writeConstBytes(DL, c, img, 0);
            continue;
        }
        const BitfieldSlot& slot = lit->second.at(fields[idx].name);
        uint64_t base = slot.byOffset ? slot.byteOffset
                                      : DL.getStructLayout(st)->getElementOffset(slot.physIndex);
        llvm::Constant* c = constInitializer(e, slot.storageType);
        if (!c) return nullptr;
        if (!slot.isBitfield) {
            img.direct[base] = c;
            writeConstBytes(DL, c, img, base);
            continue;
        }
        auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c);
        if (!ci) return nullptr;
        const llvm::APInt& v = ci->getValue();
        uint64_t bitpos = base * 8 + slot.bitOffset;
        for (unsigned b = 0; b < slot.bitWidth && b < v.getBitWidth(); ++b) {
            uint64_t p = bitpos + b;
            if (p / 8 >= size) return nullptr;
            if (v[b]) img.bytes[p / 8] |= (uint8_t)(1u << (p % 8));
            else img.bytes[p / 8] &= (uint8_t)~(1u << (p % 8));
        }
    }
    llvm::Constant* c = constFromImage(DL, st, img, 0);
    if (c || !isUnion || st != splitUnionTop) return c;
    auto d = img.direct.find(0);
    if (d == img.direct.end() || !d->second->getType()->isPointerTy()) return nullptr;
    uint64_t ps = DL.getTypeAllocSize(d->second->getType()).getFixedValue();
    if (ps >= size) return nullptr;
    for (uint64_t i = ps; i < size; ++i)
        if (img.bytes[i] || img.opaque[i]) return nullptr;
    llvm::Type* pad = llvm::ArrayType::get(llvm::Type::getInt8Ty(*context), size - ps);
    return llvm::ConstantStruct::getAnon(*context, {d->second, llvm::Constant::getNullValue(pad)});
}

llvm::Value* CodeGen::lookupSymbol(const std::string& name) {
    auto it = symbolTable.find(name);
    if (it != symbolTable.end()) {
        return it->second;
    }
    return nullptr;
}

void CodeGen::defineSymbol(const std::string& name, llvm::Value* value) {
    symbolTable[name] = value;
}
