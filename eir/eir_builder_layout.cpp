// eir_builder_layout.cpp: volatile places, cleanups, record layout, variants and bitfields (split from eir_builder.cpp).
#include "eir_builder.h"
#include "../sema/type_checker.h"
#include "../ast/type_qual.h"
#include "../ast/ast_walk.h"
#include "../template_utils.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <functional>

namespace eir {

bool EIRBuilder::volatileRooted(const Expr* e) const {
    if (volatileVars.empty()) return false;
    while (e) {
        if (auto* id = nodeCast<const IdentExpr>(e)) {
            return volatileVars.count(id->name) > 0;
        }
        if (auto* u = nodeCast<const UnaryExpr>(e)) {
            if (u->op != "*" || !u->opFunc.empty()) break;
            e = u->operand.get();
        } else if (auto* ix = nodeCast<const IndexExpr>(e)) {
            if (!ix->opFunc.empty()) break;
            e = ix->base.get();
        } else if (auto* m = nodeCast<const MemberExpr>(e)) {
            e = m->base.get();
        } else {
            break;
        }
    }
    return false;
}

void EIRBuilder::runCleanupsToDepth(size_t depth, bool errorPath) {
    for (size_t i = cleanupScopes.size(); i-- > depth; ) {
        auto frame = cleanupScopes[i];
        for (size_t j = frame.size(); j-- > 0; ) {
            if (currentBlock && currentBlock->hasTerminator()) return;
            if (frame[j].isErr && !errorPath) continue;
            auto savedScopes = scopeStack;
            auto savedTypes = varTypeStack;
            if (!frame[j].scopes.empty()) {
                scopeStack = frame[j].scopes;
                varTypeStack = frame[j].varTypes;
            }
            frame[j].body->accept(this);
            scopeStack = std::move(savedScopes);
            varTypeStack = std::move(savedTypes);
        }
    }
}

// An unbraced body (of an if, else, loop, case or match arm) is a scope of its own: its
// defers run at the end of it, and only if it was reached.
void EIRBuilder::visitBody(const StmtPtr& body) {
    if (!body) return;
    if (nodeCast<BlockStmt>(body.get())) {
        body->accept(this);
        return;
    }
    pushScope();
    cleanupScopes.emplace_back();
    size_t depth = cleanupScopes.size() - 1;
    body->accept(this);
    if (currentBlock && !currentBlock->hasTerminator()) runCleanupsToDepth(depth, false);
    cleanupScopes.pop_back();
    popScope();
}

// Lay out a struct or union as C does on the target: natural alignment capped by
// `#pragma pack`, bitfields by the SysV/AAPCS rules (sharing an aligned unit of their
// type) or the MS ones on Windows (a new word when the type's size changes), unnamed
// bitfields only moving the position. The rules are TypeChecker::bitfieldLayout's (which
// computes sizeof); this also records where each field lives.
void EIRBuilder::layoutRecord(const std::string& name, const std::vector<StructDecl::Field>& declFields,
                              const std::vector<StructDecl::Pad>& pads, bool isUnion, int packAlign,
                              const std::map<std::string, std::string>& subs) {
    auto up = [](uint64_t x, uint64_t a) { return a ? (x + a - 1) / a * a : x; };
    TargetLayoutInfo L = targetLayoutInfo(targetTriple);
    bool packed = packAlign == 1;
    uint64_t packN = packAlign >= 2 ? static_cast<uint64_t>(packAlign) : 0;
    bool contiguous = packed || packN;

    StructInfo sinfo;
    sinfo.name = name;
    std::vector<TypePtr> fieldTypes;
    uint64_t structAlign = 1;

    struct Item { StructDecl::Field f; TypePtr ty; uint64_t size, align, natural; };
    std::vector<Item> items;
    for (const auto& f : layoutFields(declFields, pads)) {
        std::string tstr = subs.empty() ? f.type : substType(f.type, subs);
        TypePtr ft = toEirType(tstr);
        ensureLayoutOf(ft);
        uint64_t fs = getTypeSize(ft, module.get());
        if (ft && ft->isStruct()) { size_t ss = module->getStructSize(ft->nominalName()); if (ss) fs = ss; }
        uint64_t fa = getTypeAlign(ft, module.get());
        if (fa < 1) fa = 1;
        uint64_t natural = fa;
        if (packed) fa = 1;
        if (packN) fa = std::min(fa, packN);
        items.push_back({f, ft, fs, fa, natural});
        if (!f.unnamed) fieldTypes.push_back(ft);
    }

    auto unsignedUnit = [](uint64_t bytes) {
        return bytes >= 8 ? Type::getU64() : bytes >= 4 ? Type::getU32() : bytes >= 2 ? Type::getU16() : Type::getU8();
    };
    auto addField = [&](const Item& it, uint64_t off) {
        StructFieldInfo fi;
        fi.name = it.f.name;
        fi.type = it.ty;
        fi.offset = off;
        fi.size = it.size;
        sinfo.fields.push_back(fi);
    };
    auto addBitfield = [&](const Item& it, uint64_t unitOff, uint64_t unitBytes, uint64_t bitOff) {
        StructFieldInfo fi;
        fi.name = it.f.name;
        fi.type = it.ty;
        fi.offset = unitOff;
        fi.size = unitBytes;
        fi.isBitfield = true;
        fi.bitOffset = static_cast<unsigned>(bitOff);
        fi.bitWidth = static_cast<unsigned>(it.f.bitWidth);
        fi.isSigned = it.ty && it.ty->isInt() && it.ty->isSigned && it.ty->bitWidth > 1;
        // An enum bitfield with no negative member is unsigned on SysV/AAPCS (clang).
        if (auto en = plainEnums.find(tyq::strip(it.f.type)); en != plainEnums.end() && !L.msBitfields) {
            bool negative = false;
            for (const auto& m : en->second->members) {
                auto c = enumConstants.find(m.first);
                if (c != enumConstants.end() && c->second < 0) negative = true;
            }
            if (!negative) {
                fi.isSigned = false;
                fi.type = Type::getU32();
            }
        }
        fi.storageType = unsignedUnit(unitBytes);
        fi.storageOffset = unitOff;
        sinfo.fields.push_back(fi);
    };
    // A bitfield packed back to back is read through the smallest unit covering its bits.
    auto spanUnit = [](uint64_t bitpos, uint64_t w) {
        uint64_t bytes = (bitpos % 8 + w + 7) / 8;
        return bytes <= 1 ? 1 : bytes <= 2 ? 2 : bytes <= 4 ? 4 : 8;
    };

    uint64_t size = 0;
    if (isUnion) {
        for (const auto& it : items) {
            if (it.f.unnamed) {
                uint64_t bytes = L.msBitfields ? it.size : (static_cast<uint64_t>(std::max(it.f.bitWidth, 0)) + 7) / 8;
                size = std::max(size, bytes);
                continue;
            }
            structAlign = std::max(structAlign, it.align);
            if (it.f.bitWidth > 0) addBitfield(it, 0, it.size, 0);
            else addField(it, 0);
            size = std::max(size, it.size);
        }
    } else if (L.msBitfields) {
        uint64_t off = 0, curBits = 0, curOff = 0, wordOff = 0;
        bool open = false, afterBits = false;
        for (const auto& it : items) {
            if (it.f.unnamed && it.f.bitWidth <= 0) {
                if (afterBits) {
                    uint64_t a = L.msvcEnv ? it.align : it.natural;
                    off = up(off, a);
                    structAlign = std::max(structAlign, a);
                }
                open = false;
                afterBits = false;
                continue;
            }
            if (it.f.bitWidth > 0) {
                uint64_t stBits = it.size * 8, w = static_cast<uint64_t>(it.f.bitWidth);
                if (!open || curBits != stBits || curOff + w > stBits) {
                    wordOff = up(off, it.align);
                    off = wordOff + it.size;
                    structAlign = std::max(structAlign, it.align);
                    open = true;
                    curBits = stBits;
                    curOff = 0;
                }
                if (!it.f.unnamed) addBitfield(it, wordOff, it.size, curOff);
                curOff += w;
                afterBits = true;
            } else {
                open = false;
                afterBits = false;
                off = up(off, it.align);
                addField(it, off);
                off += it.size;
                structAlign = std::max(structAlign, it.align);
            }
        }
        size = off;
    } else {
        uint64_t bitpos = 0;
        for (const auto& it : items) {
            if (it.f.unnamed && it.f.bitWidth <= 0) {
                bitpos = up(bitpos, it.natural * 8);
                if (L.unnamedBitfieldsAlign) structAlign = std::max(structAlign, it.natural);
                continue;
            }
            if (!it.f.unnamed || L.unnamedBitfieldsAlign) structAlign = std::max(structAlign, it.align);
            if (it.f.bitWidth > 0) {
                uint64_t w = static_cast<uint64_t>(it.f.bitWidth), unitBits = it.size * 8;
                if (!contiguous && it.align < it.size) {
                    if (bitpos % (it.align * 8) + w > unitBits) bitpos = up(bitpos, it.align * 8);
                } else if (!contiguous && unitBits && bitpos / unitBits != (bitpos + w - 1) / unitBits) {
                    bitpos = up(bitpos, unitBits);
                }
                if (!it.f.unnamed) {
                    if (contiguous) {
                        uint64_t unit = spanUnit(bitpos, w);
                        addBitfield(it, bitpos / 8, unit, bitpos % 8);
                    } else {
                        uint64_t unitOff = bitpos / (it.align * 8) * it.align;
                        addBitfield(it, unitOff, it.size, bitpos - unitOff * 8);
                    }
                }
                bitpos += w;
            } else {
                uint64_t off = up((bitpos + 7) / 8, it.align);
                addField(it, off);
                bitpos = (off + it.size) * 8;
            }
        }
        size = (bitpos + 7) / 8;
    }
    sinfo.alignment = structAlign;
    sinfo.totalSize = up(size, structAlign);
    sinfo.isUnion = isUnion;
    sinfo.packAlign = packAlign;
    for (const auto& pd : pads)
        sinfo.pads.push_back({toEirType(subs.empty() ? pd.type : substType(pd.type, subs)), pd.bitWidth, pd.before});
    module->structLayouts[name] = sinfo;
    module->structTypes.push_back(Type::getStruct(name, fieldTypes));
}

// The variant `variant` of the sum type `instName` (an instance of a generic enum has its
// own payload types), else the variant by name.
const EIRBuilder::AdtVariantInfo* EIRBuilder::variantFor(const std::string& variant, const std::string& instName) {
    if (!instName.empty()) {
        ensureStructLayout(instName);
        auto it = adtInstanceVariants.find(instName + "::" + variant);
        if (it != adtInstanceVariants.end()) return &it->second;
    }
    auto it = adtVariants.find(variant);
    return it != adtVariants.end() ? &it->second : nullptr;
}

// The instance `G<args>` of the generic enum declaring `variant`, registered for layout.
std::string EIRBuilder::genericVariantInstance(const std::string& variant, const std::vector<std::string>& args) {
    auto it = adtVariants.find(variant);
    if (it == adtVariants.end() || !genericEnums.count(it->second.enumName) || args.empty()) return "";
    std::string spelled = it->second.enumName + "<";
    for (size_t i = 0; i < args.size(); ++i) spelled += (i ? "," : "") + args[i];
    spelled += ">";
    std::string inst = mangleTemplate(spelled);
    if (!templateInstanceArgs.count(inst)) templateInstanceArgs[inst] = {it->second.enumName, args};
    return inst;
}

// A sum type as codegen's makeAdtStruct: { i32 tag, [N x i64] payload }, the payload at 8,
// N the largest variant's bytes in i64s (at least one). Registers each variant.
void EIRBuilder::layoutAdt(const std::string& name, EnumDecl* e, const std::map<std::string, std::string>& subs) {
    size_t maxPayloadSize = 0;
    for (size_t i = 0; i < e->members.size(); ++i) {
        std::vector<TypePtr> payloadTypes;
        size_t pOffset = 0, pMaxAlign = 1;
        if (i < e->payloads.size()) {
            for (const auto& ptStr : e->payloads[i]) {
                TypePtr pt = toEirType(subs.empty() ? ptStr : substType(ptStr, subs));
                ensureLayoutOf(pt);
                payloadTypes.push_back(pt);
                size_t al = getTypeAlign(pt, module.get());
                if (al > pMaxAlign) pMaxAlign = al;
                pOffset = (pOffset + al - 1) & ~(al - 1);
                size_t sz = getTypeSize(pt, module.get());
                if (pt && pt->isStruct()) { size_t ss = module->getStructSize(pt->nominalName()); if (ss) sz = ss; }
                pOffset += sz;
            }
        }
        pOffset = (pOffset + pMaxAlign - 1) & ~(pMaxAlign - 1);
        if (pOffset > maxPayloadSize) maxPayloadSize = pOffset;
        if (subs.empty()) adtVariants[e->members[i].first] = {name, static_cast<int>(i), payloadTypes};
        else adtInstanceVariants[name + "::" + e->members[i].first] = {name, static_cast<int>(i), payloadTypes};
    }
    size_t words = std::max<size_t>(1, (maxPayloadSize + 7) / 8);
    StructInfo sinfo;
    sinfo.name = name;
    sinfo.fields.push_back({"tag", Type::getI32(), 0, 4});
    sinfo.totalSize = 8 + 8 * words;
    sinfo.alignment = 8;
    module->structLayouts[name] = sinfo;
    module->structTypes.push_back(Type::getStruct(name, {Type::getI32()}));
}

// Lay out the struct a type holds by value (itself, or an array's element).
void EIRBuilder::ensureLayoutOf(const TypePtr& t) {
    TypePtr e = t;
    while (e && e->isArray()) e = e->elemType;
    if (e && e->isStruct()) ensureStructLayout(e->nominalName());
}

void EIRBuilder::ensureStructLayout(const std::string& name) {
    if (name.empty() || module->structLayouts.count(name) || layoutsInProgress.count(name)) return;
    layoutsInProgress.insert(name);
    struct Done {
        std::set<std::string>& s; std::string n;
        ~Done() { s.erase(n); }
    } done{layoutsInProgress, name};
    auto tit = templateInstanceArgs.find(name);
    if (tit != templateInstanceArgs.end()) {
        auto geIt = genericEnums.find(tit->second.first);
        if (geIt != genericEnums.end()) {
            EnumDecl* ge = geIt->second;
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < ge->typeParams.size() && i < tit->second.second.size(); ++i)
                subs[ge->typeParams[i]] = tit->second.second[i];
            layoutAdt(name, ge, subs);
            return;
        }
        auto tdIt = templateDecls.find(tit->second.first);
        if (tdIt != templateDecls.end()) {
            StructDecl* td = tdIt->second;
            std::map<std::string, std::string> subs;
            for (size_t i = 0; i < td->typeParams.size() && i < tit->second.second.size(); ++i)
                subs[td->typeParams[i]] = tit->second.second[i];
            layoutRecord(name, td->fields, td->pads, false, td->isPacked ? 1 : td->packAlign, subs);
            return;
        }
    }
    auto sit = plainStructs.find(name);
    if (sit != plainStructs.end()) {
        StructDecl* sd = sit->second;
        layoutRecord(name, sd->fields, sd->pads, false, sd->isPacked ? 1 : sd->packAlign, {});
        return;
    }
    auto ait = adtEnums.find(name);
    if (ait != adtEnums.end()) {
        layoutAdt(name, ait->second, {});
        return;
    }
    auto uit = unionDecls.find(name);
    if (uit != unionDecls.end()) {
        layoutRecord(name, uit->second->fields, uit->second->pads, true, uit->second->packAlign, {});
    }
}

// The bitfield `m` names (null when it is not one) and the address of its struct.
const StructFieldInfo* EIRBuilder::bitfieldOf(MemberExpr* m) {
    std::string baseType = getExprEirType(m->base)->nominalName();
    auto it = module->structLayouts.find(baseType);
    if (it == module->structLayouts.end()) return nullptr;
    const StructFieldInfo* fi = it->second.getFieldInfo(m->member);
    return fi && fi->isBitfield ? fi : nullptr;
}

Operand EIRBuilder::bitfieldBase(MemberExpr* m) {
    TypePtr baseTy = getExprEirType(m->base);
    return baseTy->isPointer() ? evaluateExpr(m->base) : evaluateLValue(m->base);
}

Operand EIRBuilder::loadBitfield(MemberExpr* m, bool isVolatile) {
    const StructFieldInfo* fi = bitfieldOf(m);
    if (!fi) return Operand::constInt(0, Type::getI32());
    return loadBitfieldAt(bitfieldBase(m), fi, isVolatile);
}

void EIRBuilder::storeBitfield(MemberExpr* m, Operand val, bool isVolatile) {
    const StructFieldInfo* fi = bitfieldOf(m);
    if (!fi) return;
    storeBitfieldAt(bitfieldBase(m), fi, val, isVolatile);
}

Operand EIRBuilder::loadBitfieldAt(const Operand& basePtr, const StructFieldInfo* fi, bool isVolatile) {

    std::string wordPtr = newVar();
    emit(Instruction{Opcode::OffsetPtr, wordPtr, Type::getPtr(fi->storageType),
                     {basePtr, Operand::constInt(static_cast<int64_t>(fi->storageOffset), Type::getI64())}});

    std::string wordVal = newVar();
    Instruction ld{Opcode::Load, wordVal, fi->storageType, {Operand::var(wordPtr, Type::getPtr(fi->storageType))}};
    ld.isVolatile = isVolatile;
    emit(ld);

    Operand cur = Operand::var(wordVal, fi->storageType);
    if (fi->bitOffset > 0) {
        std::string shifted = newVar();
        emit(Instruction{Opcode::LShr, shifted, fi->storageType, {cur, Operand::constInt(static_cast<int64_t>(fi->bitOffset), fi->storageType)}});
        cur = Operand::var(shifted, fi->storageType);
    }
    uint64_t mask = (fi->bitWidth >= 64) ? ~0ULL : ((1ULL << fi->bitWidth) - 1);
    std::string masked = newVar();
    emit(Instruction{Opcode::And, masked, fi->storageType, {cur, Operand::constInt(static_cast<int64_t>(mask), fi->storageType)}});
    cur = Operand::var(masked, fi->storageType);

    if (fi->isSigned && fi->bitWidth < static_cast<unsigned>(fi->storageType->bitWidth)) {
        // Sign-extend from the field's top bit in a signed type of the unit's width.
        TypePtr sty = fi->type && fi->type->isInt() && fi->type->bitWidth == fi->storageType->bitWidth ? fi->type
                    : fi->storageType->bitWidth == 64 ? Type::getI64() : fi->storageType->bitWidth == 32 ? Type::getI32()
                    : fi->storageType->bitWidth == 16 ? Type::getI16() : Type::getI8();
        unsigned sh = static_cast<unsigned>(fi->storageType->bitWidth) - fi->bitWidth;
        std::string shl = newVar();
        emit(Instruction{Opcode::Shl, shl, fi->storageType, {cur, Operand::constInt(static_cast<int64_t>(sh), fi->storageType)}});
        Operand asSigned = coerceNumeric(Operand::var(shl, fi->storageType), sty);
        std::string ashr = newVar();
        emit(Instruction{Opcode::AShr, ashr, sty, {asSigned, Operand::constInt(static_cast<int64_t>(sh), sty)}});
        cur = Operand::var(ashr, sty);
    }
    return coerceNumeric(coerceNumeric(cur, fi->type), bitfieldReadType(fi));
}

void EIRBuilder::storeBitfieldAt(const Operand& basePtr, const StructFieldInfo* fi, Operand val, bool isVolatile) {

    std::string wordPtr = newVar();
    emit(Instruction{Opcode::OffsetPtr, wordPtr, Type::getPtr(fi->storageType),
                     {basePtr, Operand::constInt(static_cast<int64_t>(fi->storageOffset), Type::getI64())}});

    std::string wordVal = newVar();
    Instruction ld{Opcode::Load, wordVal, fi->storageType, {Operand::var(wordPtr, Type::getPtr(fi->storageType))}};
    ld.isVolatile = isVolatile;
    emit(ld);

    uint64_t mask = (fi->bitWidth >= 64) ? ~0ULL : ((1ULL << fi->bitWidth) - 1);
    uint64_t fieldMask = mask << fi->bitOffset;
    std::string cleared = newVar();
    emit(Instruction{Opcode::And, cleared, fi->storageType, {Operand::var(wordVal, fi->storageType), Operand::constInt(static_cast<int64_t>(~fieldMask), fi->storageType)}});

    // The value as the field's type, then its bits in the unit's (unsigned) type.
    Operand valToStore = coerceNumeric(coerceNumeric(val, fi->type), fi->storageType);

    std::string vMasked = newVar();
    emit(Instruction{Opcode::And, vMasked, fi->storageType, {valToStore, Operand::constInt(static_cast<int64_t>(mask), fi->storageType)}});
    Operand vShifted = Operand::var(vMasked, fi->storageType);
    if (fi->bitOffset > 0) {
        std::string s = newVar();
        emit(Instruction{Opcode::Shl, s, fi->storageType, {vShifted, Operand::constInt(static_cast<int64_t>(fi->bitOffset), fi->storageType)}});
        vShifted = Operand::var(s, fi->storageType);
    }

    std::string newWord = newVar();
    emit(Instruction{Opcode::Or, newWord, fi->storageType, {Operand::var(cleared, fi->storageType), vShifted}});

    Instruction st{Opcode::Store, "", Type::getVoid(), {Operand::var(newWord, fi->storageType), Operand::var(wordPtr, Type::getPtr(fi->storageType))}};
    st.isVolatile = isVolatile;
    emit(st);
}

} // namespace eir
