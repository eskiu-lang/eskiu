#include "codegen.h"
#include "../ast/type_qual.h"

// Template type-name utilities (mangleTemplate / splitTemplateType / substType)
// are shared with the type checker; see template_utils.h.
#include "../template_utils.h"

// CodeGen — algebraic enums (construction + monomorphization), struct
// initialization, and interface boxing.
// Part of the codegen_expr.cpp split; see codegen.h.

// Create the tagged-union LLVM type for an ADT enum given each variant's payload
// field types: { i32 tag; [N x i64] payload }, N sized to the largest variant. The
// i64 array forces 8-byte alignment (enough for scalars/pointers/doubles).
static llvm::StructType* makeAdtStruct(llvm::LLVMContext& ctx, const llvm::DataLayout& DL,
                                       const std::string& name,
                                       const std::vector<std::vector<llvm::Type*>>& variantFields) {
    uint64_t maxBytes = 0;
    for (const auto& fields : variantFields) {
        uint64_t bytes = 0;
        for (llvm::Type* t : fields) {
            uint64_t al = DL.getABITypeAlign(t).value();
            bytes = ((bytes + al - 1) / al) * al + DL.getTypeAllocSize(t);
        }
        if (bytes > maxBytes) maxBytes = bytes;
    }
    uint64_t n = (maxBytes + 7) / 8; if (n == 0) n = 1;
    std::vector<llvm::Type*> f = {
        llvm::Type::getInt32Ty(ctx),
        llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx), n)
    };
    return llvm::StructType::create(ctx, f, name);
}

void CodeGen::visit(EnumDecl* node) {
    enumTypes.insert(node->name);
    if (!node->isADT()) {
        for (const auto& m : node->members) enumConstants[m.first] = m.second;
        plainEnumDecls[node->name] = node;   // classic int enum: `match` lowers to a value switch
        return;
    }
    if (!node->typeParams.empty()) {
        // Generic ADT enum: a template; instances are built on demand (ensureEnumInst).
        genericEnumDecls[node->name] = node;
        for (size_t v = 0; v < node->members.size(); ++v)
            genericVariants[node->members[v].first] = {node->name, (int)v};
        return;
    }
    adtEnumDecls[node->name] = node;
    std::vector<std::vector<llvm::Type*>> vf;
    for (size_t v = 0; v < node->members.size(); ++v) {
        adtVariants[node->members[v].first] = {node->name, (int)v};
        std::vector<llvm::Type*> fields;
        for (const auto& ft : node->payloads[v]) fields.push_back(getTypeFromString(ft));
        vf.push_back(fields);
    }
    structTypes[node->name] = makeAdtStruct(*context, module->getDataLayout(), node->name, vf);
}

// Monomorphize a generic enum for `typeArgs` (build its struct + record the args).
std::string CodeGen::ensureEnumInst(const std::string& genericName,
                                    const std::vector<std::string>& typeArgs) {
    std::string inst = genericName + "<";
    for (size_t i = 0; i < typeArgs.size(); ++i) { if (i) inst += ","; inst += typeArgs[i]; }
    inst += ">";
    std::string mangled = mangleTemplate(inst);
    if (structTypes.count(mangled)) return mangled;
    EnumDecl* ge = genericEnumDecls[genericName];
    std::map<std::string, std::string> subs;
    for (size_t i = 0; i < ge->typeParams.size() && i < typeArgs.size(); ++i)
        subs[ge->typeParams[i]] = typeArgs[i];
    std::vector<std::vector<llvm::Type*>> vf;
    for (size_t v = 0; v < ge->members.size(); ++v) {
        std::vector<llvm::Type*> fields;
        for (const auto& ft : ge->payloads[v]) fields.push_back(getTypeFromString(substType(ft, subs)));
        vf.push_back(fields);
    }
    structTypes[mangled] = makeAdtStruct(*context, module->getDataLayout(), mangled, vf);
    enumInstanceArgs[mangled] = {genericName, typeArgs};
    return mangled;
}

// Core builder: alloca the enum struct, zero it (the payload area past this variant's
// fields holds no garbage), store the tag, write payload fields (viewed as the
// variant's struct, coerced to the field types), then load the value.
llvm::Value* CodeGen::buildEnumValue(llvm::StructType* et, int tag,
        const std::vector<std::string>& fieldEsk, const std::vector<ExprPtr>& args) {
    llvm::Value* tmp = entryAlloca(et, nullptr, "variant.tmp");
    if (module->getDataLayout().getTypeAllocSize(et).getFixedValue() > kBulkZeroBytes)
        emitZeroFill(tmp, et);
    else
        builder->CreateStore(llvm::Constant::getNullValue(et), tmp);
    builder->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context), tag),
                         builder->CreateStructGEP(et, tmp, 0));
    std::vector<llvm::Type*> fieldTypes;
    for (const auto& ft : fieldEsk) fieldTypes.push_back(getTypeFromString(ft));
    if (!fieldTypes.empty()) {
        llvm::StructType* vt = llvm::StructType::get(*context, fieldTypes);
        llvm::Value* pay = builder->CreateStructGEP(et, tmp, 1);   // the [N x i64] area
        for (size_t i = 0; i < args.size() && i < fieldTypes.size(); ++i) {
            llvm::Value* fp = builder->CreateStructGEP(vt, pay, i);
            llvm::Value* val = evalForType(args[i], fieldEsk[i]);   // an interface payload boxes
            llvm::Type* ft = fieldTypes[i];
            val = coerceValue(val, ft, eskiuUnsigned(getExprEskiuType(args[i])));  // arg to field type
            builder->CreateStore(val, fp);
        }
    }
    return builder->CreateLoad(et, tmp, "variant.val");
}

llvm::Value* CodeGen::buildVariant(const std::string& variant, const std::vector<ExprPtr>& args) {
    auto& info = adtVariants[variant];
    EnumDecl* ed = adtEnumDecls[info.first];
    return buildEnumValue(structTypes[info.first], info.second, ed->payloads[info.second], args);
}

std::string CodeGen::resolveStructInitName(const std::string& name) {
    if (name.find('<') == std::string::npos) return name;
    // Resolve type args through the enclosing template's substitutions, so a
    // `Pair<A,B>{...}` literal inside a template body instantiates Pair<int,int>,
    // not a bogus Pair_A_B.
    std::string resolved = typeParamOverride.empty() ? name : substType(name, typeParamOverride);
    auto [tn, args] = splitTemplateType(resolved);
    std::string mangled = mangleTemplate(resolved);
    ensureTemplateInstantiated(mangled, tn, args);
    return mangled;
}

void CodeGen::emitZeroFill(llvm::Value* dest, llvm::Type* ty) {
    const llvm::DataLayout& DL = module->getDataLayout();
    emitZeroBytes(dest, llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context),
                                               DL.getTypeAllocSize(ty).getFixedValue()),
                  DL.getABITypeAlign(ty));
}

void CodeGen::emitZeroBytes(llvm::Value* dest, llvm::Value* size, llvm::Align align) {
    llvm::Type* i64 = llvm::Type::getInt64Ty(*context);
    if (!freestanding) {
        builder->CreateMemSet(dest, builder->getInt8(0), size, align);
        return;
    }
    llvm::Function* fn = builder->GetInsertBlock()->getParent();
    llvm::BasicBlock* pre = builder->GetInsertBlock();
    llvm::BasicBlock* head = llvm::BasicBlock::Create(*context, "zero.head", fn);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(*context, "zero.body", fn);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(*context, "zero.done", fn);
    builder->CreateBr(head);
    builder->SetInsertPoint(head);
    llvm::PHINode* i = builder->CreatePHI(i64, 2, "zero.i");
    i->addIncoming(llvm::ConstantInt::get(i64, 0), pre);
    builder->CreateCondBr(builder->CreateICmpULT(i, size), body, done);
    builder->SetInsertPoint(body);
    builder->CreateStore(builder->getInt8(0), builder->CreateGEP(builder->getInt8Ty(), dest, i));
    i->addIncoming(builder->CreateAdd(i, llvm::ConstantInt::get(i64, 1)), body);
    builder->CreateBr(head);
    builder->SetInsertPoint(done);
}

void CodeGen::emitArrayInitInto(llvm::Value* dest, ArrayLitExpr* lit, const std::string& arrType,
                                bool zeroed) {
    // Peel the outer (leftmost) dimension: `int[2][3]` → dim 2, element `int[3]`. An
    // alias (`type A3 = int[3]`) names the array it stands for.
    ty::Type bt = ty::Type::parse(expandAlias(arrType));
    if (bt.kind != ty::Type::Kind::Array) return;
    std::string elemStr = bt.elem->str();
    uint64_t n = 0;
    if (!resolveArrayDim(bt.dim, n)) n = lit->elements.size();
    llvm::Type* elemTy = getTypeFromString(elemStr);
    llvm::Type* arrTy = llvm::ArrayType::get(elemTy, n);
    llvm::Type* i32 = llvm::Type::getInt32Ty(*context);
    bool elemIsArray = ty::Type::parse(expandAlias(elemStr)).kind == ty::Type::Kind::Array;   // `A3[2]`
    // A large array is zeroed once, then only the given elements are stored.
    if (!zeroed && module->getDataLayout().getTypeAllocSize(arrTy).getFixedValue() > kBulkZeroBytes) {
        emitZeroFill(dest, arrTy);
        zeroed = true;
    }
    if (zeroed) n = std::min<uint64_t>(n, lit->elements.size());

    for (uint64_t i = 0; i < n; ++i) {
        llvm::Value* slot = builder->CreateGEP(arrTy, dest,
            {llvm::ConstantInt::get(i32, 0), llvm::ConstantInt::get(i32, i)});
        if (i < lit->elements.size()) {
            // A nested initializer for a sub-array recurses into the row; a scalar
            // element is evaluated and coerced to the element type.
            if (elemIsArray) {
                if (auto* sub = nodeCast<ArrayLitExpr>(lit->elements[i].get()))
                    emitArrayInitInto(slot, sub, elemStr, zeroed);
                else
                    builder->CreateStore(evaluateExpr(lit->elements[i]), slot);
                continue;
            }
            llvm::Value* val = evalForType(lit->elements[i], elemStr);   // boxes into an interface element
            if (val->getType() != elemTy) {
                val = coerceValue(val, elemTy, eskiuUnsigned(getExprEskiuType(lit->elements[i])));
            }
            builder->CreateStore(val, slot);
        } else {
            builder->CreateStore(llvm::Constant::getNullValue(elemTy), slot);   // zero-fill (C-style)
        }
    }
}

ExprPtr CodeGen::defaultExprFor(const std::string& t0) {
    std::string t = tyq::strip(expandAlias(t0));
    ty::Type pt = ty::Type::parse(t);
    if (pt.kind == ty::Type::Kind::Array && pt.elem) {
        ExprPtr e = defaultExprFor(pt.elem->str());
        if (!e) return nullptr;
        auto* at = llvm::dyn_cast<llvm::ArrayType>(getTypeFromString(t));
        if (!at || at->getNumElements() > 65536) return nullptr;
        return std::make_shared<ArrayLitExpr>(std::vector<ExprPtr>((size_t)at->getNumElements(), e));
    }
    if (pt.kind == ty::Type::Kind::Array || isPointerType(t)) return nullptr;
    std::string sname = stripToStructKey(t);
    auto fit = structFields.find(sname);
    // A generic struct's instance starts at zero (its fields have no defaults, and a
    // field's own defaults apply only outside generic code), as in the self-host.
    if (fit == structFields.end() || unionFields.count(sname) || templateInstanceArgs.count(sname)) return nullptr;
    auto hit = structHasDefaults.find(sname);
    if (hit != structHasDefaults.end()) {
        if (hit->second != 1) return nullptr;
    } else {
        structHasDefaults[sname] = -1;               // a recursive struct reaches itself only by pointer
        bool any = false;
        for (const auto& f : fit->second) if (f.defaultValue || defaultExprFor(f.type)) { any = true; break; }
        structHasDefaults[sname] = any ? 1 : 0;
        if (!any) return nullptr;
    }
    return std::make_shared<StructInitExpr>(sname, std::vector<std::pair<std::string, ExprPtr>>{});
}

ExprPtr CodeGen::withDefaults(StructInitExpr* init, const ExprPtr& self) {
    std::string sname = resolveStructInitName(init->structName);
    if (!defaultExprFor(sname)) return self;
    const auto& fields = structFields[sname];
    bool named = !init->fieldInits.empty() && !init->fieldInits[0].first.empty();
    std::vector<std::pair<std::string, ExprPtr>> all;
    std::set<std::string> given;
    for (size_t i = 0; i < init->fieldInits.size(); ++i) {
        std::string n = named ? init->fieldInits[i].first : (i < fields.size() ? fields[i].name : "");
        if (n.empty()) continue;
        given.insert(n);
        all.push_back({n, init->fieldInits[i].second});
    }
    for (const auto& f : fields) {
        if (given.count(f.name)) continue;
        ExprPtr d = f.defaultValue ? f.defaultValue : defaultExprFor(f.type);
        if (d) all.push_back({f.name, d});
    }
    auto out = std::make_shared<StructInitExpr>(init->structName, all);
    out->line = init->line; out->col = init->col;
    return out;
}

void CodeGen::emitStructInitInto(llvm::Value* dest, StructInitExpr* init0) {
    ExprPtr keep = withDefaults(init0, nullptr);
    StructInitExpr* init = keep ? static_cast<StructInitExpr*>(keep.get()) : init0;
    std::string sname = resolveStructInitName(init->structName);
    auto fit = structFields.find(sname);
    if (fit == structFields.end()) return;
    const auto& fields = fit->second;
    llvm::StructType* st = structTypes[sname];
    // Fields the literal omits are zero (C aggregate-initializer semantics), not
    // whatever the stack slot held; write the zero first, then the given fields.
    if (module->getDataLayout().getTypeAllocSize(st).getFixedValue() > kBulkZeroBytes)
        emitZeroFill(dest, st);
    else
        builder->CreateStore(llvm::Constant::getNullValue(st), dest);

    bool named = !init->fieldInits.empty() && !init->fieldInits[0].first.empty();

    auto coerce = [&](llvm::Value* val, llvm::Type* fieldType, bool unsignedSrc) -> llvm::Value* {
        return coerceValue(val, fieldType, unsignedSrc);
    };

    auto storeField = [&](size_t idx, ExprPtr expr) {
        // An array field's `{...}` fills the field in place, like an array variable's.
        if (auto* arr = nodeCast<ArrayLitExpr>(expr.get())) {
            llvm::Value* at = dest;
            auto lit = structLayout.find(sname);
            if (lit != structLayout.end()) at = layoutFieldAddr(sname, dest, lit->second.at(fields[idx].name));
            else if (!unionFields.count(sname)) at = builder->CreateStructGEP(st, dest, idx);
            emitArrayInitInto(at, arr, fields[idx].type, /*zeroed=*/true);
            return;
        }
        llvm::Value* val = evalForType(expr, fields[idx].type);
        bool uns = eskiuUnsigned(getExprEskiuType(expr));
        // Bitfield-layout struct: store via the physical slot.
        auto lit = structLayout.find(sname);
        if (lit != structLayout.end()) {
            const BitfieldSlot& slot = lit->second.at(fields[idx].name);
            llvm::Value* gep = layoutFieldAddr(sname, dest, slot);
            if (slot.isBitfield) { storeBitfieldInto(gep, slot, val, uns); return; }
            if (val) builder->CreateStore(coerce(val, slot.storageType, uns), gep);
            return;
        }
        llvm::Type* fieldType = getTypeFromString(fields[idx].type);
        val = coerce(val, fieldType, uns);
        // A union's members all start at offset 0.
        llvm::Value* gep = unionFields.count(sname) ? dest : builder->CreateStructGEP(st, dest, idx);
        if (val) builder->CreateStore(val, gep);
    };

    if (named) {
        for (const auto& [fname, expr] : init->fieldInits) {
            for (size_t i = 0; i < fields.size(); ++i) {
                if (fields[i].name == fname) { storeField(i, expr); break; }
            }
        }
    } else {
        for (size_t i = 0; i < init->fieldInits.size() && i < fields.size(); ++i) {
            storeField(i, init->fieldInits[i].second);
        }
    }
}

void CodeGen::visit(ArrayLitExpr* node) {
    (void)node;
    throw std::runtime_error("an array literal '{...}' is only valid as a variable initializer");
}

void CodeGen::visit(StructInitExpr* node) {
    std::string sname = resolveStructInitName(node->structName);
    auto fit = structFields.find(sname);
    if (fit == structFields.end())
        throw std::runtime_error("Unknown struct: " + node->structName);
    llvm::StructType* st = structTypes[sname];
    // Temporary alloca — filled then loaded so caller can store it anywhere
    llvm::Value* tmp = entryAlloca(st, nullptr, sname + ".init");
    emitStructInitInto(tmp, node);
    exprValueStack.push(builder->CreateLoad(st, tmp));
}

void CodeGen::visit(InterfaceDecl* node) {
    // Build vtable struct type: %I_vtable = type { ptr, ptr, ... }
    std::vector<llvm::Type*> fnPtrs(node->methods.size(),
                                     llvm::PointerType::get(*context, 0));
    std::string vtName = node->name + "_vtable";
    llvm::StructType* vtType = llvm::StructType::create(*context, fnPtrs, vtName);
    ifaceVtableTypes[node->name] = vtType;

    // Method order + return/param type info for typed dispatch
    std::vector<std::string> order;
    std::vector<std::string> retTypes;
    std::vector<std::vector<std::string>> paramTypesList;
    for (const auto& m : node->methods) {
        order.push_back(m.name);
        retTypes.push_back(m.returnType);
        std::vector<std::string> pts;
        for (const auto& p : m.params) pts.push_back(p.first);
        paramTypesList.push_back(pts);
    }
    ifaceMethodOrder[node->name]           = order;
    ifaceMethodReturnTypes[node->name]      = retTypes;
    ifaceMethodParamEskiuTypes[node->name]  = paramTypesList;

    // Fat pointer type: %I_fat = type { ptr, ptr }. An interface value IS this struct,
    // by value (like a closure), so it can live in a local, a field, or a return value
    // without pointing into some caller's stack frame.
    llvm::StructType* fatPtr = llvm::StructType::create(*context,
        {llvm::PointerType::get(*context, 0), llvm::PointerType::get(*context, 0)},
        node->name + "_fat");
    ifaceFatPtrTypes[node->name] = fatPtr;
}

// Create a fat pointer {data_ptr, vtable_ptr} for struct S implementing interface I
llvm::Value* CodeGen::boxAsInterface(const std::string& ifaceName,
                                      const std::string& structName,
                                      llvm::Value* structPtr) {
    auto vtIt = ifaceVtableTypes.find(ifaceName);
    if (vtIt == ifaceVtableTypes.end())
        throw std::runtime_error("Unknown interface: " + ifaceName);

    const auto& methods = ifaceMethodOrder[ifaceName];
    llvm::StructType* vtType = vtIt->second;
    llvm::StructType* fatType = ifaceFatPtrTypes[ifaceName];

    // Build vtable constant: { &S_method1, &S_method2, ... }
    std::string vtGlobName = ifaceName + "_vtable_" + structName;
    llvm::GlobalVariable* vtGlob = module->getGlobalVariable(vtGlobName);
    if (!vtGlob) {
        std::vector<llvm::Constant*> entries;
        for (size_t mi = 0; mi < methods.size(); ++mi) {
            const std::string& mname = methods[mi];
            std::string mangled = structName + "_" + mname;
            llvm::Function* fn = module->getFunction(mangled);
            // A generic instance's slot is its inline method, or a generic free
            // `S_m<T..>(S<T..>* self, ...)` instantiated for it (sema's
            // genericFreeMethodFor binds the same type arguments).
            if (!fn) fn = instantiateGenericMethod(structName, mname);
            if (!fn)
                if (FunctionDecl* gf = genericFreeMethod(structName, mname)) {
                    std::set<std::string> tps(gf->typeParams.begin(), gf->typeParams.end());
                    std::map<std::string, std::string> subs;
                    unifyTypeParam(gf->params[0].first, "*" + structName, tps, subs);
                    const auto& ipts = ifaceMethodParamEskiuTypes[ifaceName][mi];
                    for (size_t j = 1; j < gf->params.size() && j - 1 < ipts.size(); ++j)
                        unifyTypeParam(gf->params[j].first, ipts[j - 1], tps, subs);
                    std::string gname = gf->name;
                    for (const auto& tpn : gf->typeParams) gname += "_" + mangleTemplate(subs[tpn]);
                    fn = instantiateFnTemplate(gf, gname, subs);
                }
            if (!fn) throw std::runtime_error("Method not found: " + mangled);
            entries.push_back(fn);
        }
        llvm::Constant* vtInit = llvm::ConstantStruct::get(vtType, entries);
        vtGlob = new llvm::GlobalVariable(*module, vtType, true,
            llvm::GlobalValue::PrivateLinkage, vtInit, vtGlobName);
    }

    // The fat pointer value { data ptr, vtable ptr }.
    llvm::Value* fat = llvm::UndefValue::get(fatType);
    fat = builder->CreateInsertValue(fat, structPtr, {0});
    fat = builder->CreateInsertValue(fat, vtGlob, {1});
    return fat;
}

std::string CodeGen::interfaceName(const std::string& type) const {
    std::string t = expandAlias(type);   // `type Sh = Shape`: Sh is the interface
    if (t.rfind("interface:", 0) == 0) t = t.substr(10);
    return ifaceFatPtrTypes.count(t) ? t : "";
}

llvm::Value* CodeGen::evalForType(const ExprPtr& e, const std::string& targetType) {
    std::string iface = interfaceName(targetType);
    if (iface.empty()) return evaluateExpr(e);
    // Converting to an interface: an interface value passes through; `null` is the
    // empty {null, null}; a pointer to a struct is boxed with that struct's vtable.
    std::string st = getExprEskiuType(e);
    if (interfaceName(st) == iface) return evaluateExpr(e);
    if (auto* lit = nodeCast<LiteralExpr>(e.get()); lit && lit->kind == LiteralExpr::Kind::NULL_VAL)
        return llvm::ConstantAggregateZero::get(ifaceFatPtrTypes[iface]);
    bool isPtr = !st.empty() && (st.front() == '*' || st.back() == '*');
    if (!isPtr)
        throw std::runtime_error("cannot convert a '" + st + "' value to interface '" + iface +
                                 "'; pass a pointer (&x)");
    std::string sname = st;
    while (!sname.empty() && sname.front() == '*') sname = sname.substr(1);
    while (!sname.empty() && sname.back() == '*') sname.pop_back();
    return boxAsInterface(iface, stripToStructKey(sname), evaluateExpr(e));
}
