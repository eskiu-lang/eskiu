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

// ============================================================================
// Visitor Methods
// ============================================================================

// The C library functions whose size_t / ssize_t positions an `int64` spelling also
// stands for (code written for 64-bit targets): the kind of each parameter, then '.'
// and the result's, as in externCInts ('z' size_t, 's' ssize_t, '-' other).
static const std::map<std::string, std::string>& knownCSizes() {
    static const std::map<std::string, std::string> t = {
        {"memcpy", "--z.-"}, {"memmove", "--z.-"}, {"memset", "--z.-"}, {"memcmp", "--z.-"},
        {"memchr", "--z.-"}, {"memmem", "-z-z.-"}, {"strlen", "-.z"}, {"strnlen", "-z.z"},
        {"strncmp", "--z.-"}, {"strncpy", "--z.-"}, {"strncat", "--z.-"},
        {"strncasecmp", "--z.-"}, {"strspn", "--.z"}, {"strcspn", "--.z"},
        {"strxfrm", "--z.z"}, {"malloc", "z.-"}, {"calloc", "zz.-"}, {"realloc", "-z.-"},
        {"aligned_alloc", "zz.-"}, {"qsort", "-zz-.-"}, {"bsearch", "--zz-.-"},
        {"snprintf", "-z-.-"}, {"vsnprintf", "-z--.-"}, {"strftime", "-z--.z"},
        {"fread", "-zz-.z"}, {"fwrite", "-zz-.z"}, {"getcwd", "-z.-"},
        {"read", "--z.s"}, {"write", "--z.s"}, {"send", "--z-.s"}, {"recv", "--z-.s"},
        {"mmap", "-z----.-"}, {"munmap", "-z.-"},
    };
    return t;
}

void CodeGen::visit(Program* node) {
    // Three-phase lowering so that any declaration may reference any other
    // regardless of source order (forward references, mutual recursion):
    //   1. type shells (structs/unions) + extern declarations
    //   2. function prototypes (free functions and struct methods)
    //   3. bodies / globals
    // Pre-pass: fold top-level `const` ints so they can be used as array sizes
    // in struct fields / globals declared anywhere (resolved during phase 1).
    for (auto& decl : node->declarations)
        if (auto* v = nodeCast<VarDecl>(decl.get())) foldConstDecl(v);
    // Does the program throw or catch anywhere? Only then do defers get landingpads.
    for (auto& decl : node->declarations) {
        if (auto* f = nodeCast<FunctionDecl>(decl.get())) {
            if (astwalk::containsEH(f->body.get())) programUsesEH = true;
        } else if (auto* s = nodeCast<StructDecl>(decl.get())) {
            for (auto& m : s->methods)
                if (auto* mf = nodeCast<FunctionDecl>(m.get()); mf && astwalk::containsEH(mf->body.get()))
                    programUsesEH = true;
        } else if (auto* v = nodeCast<VarDecl>(decl.get())) {
            if (astwalk::containsEH(v->initializer.get())) programUsesEH = true;
        }
    }
    // Type declarations in dependency order: a type a struct/union/enum holds BY VALUE
    // is laid out first wherever it is declared (`struct S { T t; } struct T {...}`), as
    // a C compiler sees every complete type before its use. A pointer needs no layout.
    std::map<std::string, Decl*> typeDeclOf;
    for (auto& decl : node->declarations) {
        Decl* d = decl.get();
        bool isType = nodeCast<StructDecl>(d) || nodeCast<UnionDecl>(d) ||
                      nodeCast<InterfaceDecl>(d) || nodeCast<EnumDecl>(d) ||
                      nodeCast<TypeAliasDecl>(d);
        if (isType && !typeDeclOf.count(d->name)) typeDeclOf[d->name] = d;
    }
    std::set<Decl*> typeDone, typeVisiting;
    std::function<void(Decl*)> declareTypeDecl;
    std::function<void(const ty::Type&)> needType = [&](const ty::Type& t) {
        switch (t.kind) {
            case ty::Type::Kind::Pointer: case ty::Type::Kind::Fn: case ty::Type::Kind::Slice:
                return;
            case ty::Type::Kind::Array:
                if (t.elem) needType(*t.elem);
                return;
            default: break;
        }
        for (const auto& a : t.args) needType(a);
        auto it = typeDeclOf.find(t.name);
        if (it != typeDeclOf.end()) declareTypeDecl(it->second);
    };
    auto needStr = [&](const std::string& ts) { needType(ty::Type::parse(ts)); };
    declareTypeDecl = [&](Decl* d) {
        if (typeDone.count(d) || typeVisiting.count(d)) return;
        typeVisiting.insert(d);
        if (auto* s = nodeCast<StructDecl>(d)) {
            if (s->typeParams.empty()) for (const auto& f : s->fields) needStr(f.type);
        } else if (auto* u = nodeCast<UnionDecl>(d)) {
            for (const auto& f : u->fields) needStr(f.type);
        } else if (auto* e = nodeCast<EnumDecl>(d)) {
            if (e->typeParams.empty())
                for (const auto& pl : e->payloads) for (const auto& ft : pl) needStr(ft);
        } else if (auto* a = nodeCast<TypeAliasDecl>(d)) {
            needStr(a->aliased);
        }
        typeVisiting.erase(d);
        typeDone.insert(d);
        if (auto* s = nodeCast<StructDecl>(d)) declareStructType(s);  // registers templates too
        else d->accept(this);
    };
    // Generic templates first: a concrete type may hold an instance of one declared later.
    for (auto& decl : node->declarations) {
        if (auto* s = nodeCast<StructDecl>(decl.get()); s && !s->typeParams.empty()) declareTypeDecl(s);
        if (auto* e = nodeCast<EnumDecl>(decl.get()); e && !e->typeParams.empty()) declareTypeDecl(e);
    }
    for (auto& decl : node->declarations) {
        if (nodeCast<StructDecl>(decl.get()) || nodeCast<UnionDecl>(decl.get()) ||
            nodeCast<InterfaceDecl>(decl.get()) || nodeCast<EnumDecl>(decl.get()) ||
            nodeCast<TypeAliasDecl>(decl.get())) {
            declareTypeDecl(decl.get());
        } else if (nodeCast<IntrinsicDecl>(decl.get())) {
            decl->accept(this);
        }
    }
    // Externs after every type shell: a by-value struct parameter declared later in
    // the source must already have its layout for the C-ABI signature.
    for (auto& decl : node->declarations)
        if (auto* f = nodeCast<FunctionDecl>(decl.get()))
            if (f->body) definedFunctionNames.insert(f->name);
    for (auto& decl : node->declarations) {
        auto* e = nodeCast<ExternDecl>(decl.get());
        if (!e) continue;
        std::vector<char>& k = externCInts[e->name];
        size_t n = 0;
        while (n < e->params.size() && e->params[n].first != "...") ++n;
        if (k.size() < n + 1) k.resize(n + 1, 0);
        for (size_t i = 0; i < n; ++i)
            if (char c = cIntKind(e->params[i].first)) k[i] = c;
        if (char c = cIntKind(e->returnType)) k[n] = c;
        // A known C function spelled with int64 where C has size_t or ssize_t.
        auto known = knownCSizes().find(e->name);
        if (known != knownCSizes().end() && known->second.find('.') == n) {
            const std::string& pat = known->second;
            size_t dot = pat.find('.');
            auto wide = [](const std::string& t) { return t == "int64" || t == "uint64"; };
            for (size_t i = 0; i < n && i < dot; ++i)
                if (pat[i] != '-' && !k[i] && wide(expandAlias(e->params[i].first))) k[i] = pat[i];
            if (pat[dot + 1] != '-' && !k[n] && wide(expandAlias(e->returnType))) k[n] = pat[dot + 1];
        }
    }
    for (auto& decl : node->declarations)
        if (nodeCast<ExternDecl>(decl.get())) decl->accept(this);
    for (auto& decl : node->declarations) {
        if (auto* f = nodeCast<FunctionDecl>(decl.get())) {
            if (f->typeParams.empty())
                declareFunction(f->name, f->returnType, f->params);
            else
                f->accept(this);   // register the template up front: a use may precede it
        } else if (auto* s = nodeCast<StructDecl>(decl.get())) {
            if (!s->typeParams.empty()) continue;
            for (auto& method : s->methods) {
                if (auto* mf = nodeCast<FunctionDecl>(method.get())) {
                    std::vector<std::pair<std::string, std::string>> params;
                    params.push_back({"*" + s->name, "self"});
                    for (auto& p : mf->params) params.push_back(p);
                    declareFunction(s->name + "_" + mf->name, mf->returnType, params);
                }
            }
        }
    }
    for (auto& decl : node->declarations) {
        // Externs, unions, interfaces, enums, and aliases were handled in phase 1.
        if (nodeCast<ExternDecl>(decl.get()) ||
            nodeCast<IntrinsicDecl>(decl.get()) ||
            nodeCast<UnionDecl>(decl.get()) ||
            nodeCast<InterfaceDecl>(decl.get()) ||
            nodeCast<EnumDecl>(decl.get()) ||
            nodeCast<TypeAliasDecl>(decl.get()))
            continue;
        decl->accept(this);
    }
}

llvm::Function* CodeGen::declareFunction(
        const std::string& name, const std::string& returnTypeStr,
        const std::vector<std::pair<std::string, std::string>>& params) {
    // Get parameter types
    std::vector<llvm::Type*> paramTypes;
    for (auto& param : params) {
        if (param.first == "...") continue; // variadic — handled by extern decls
        paramTypes.push_back(getTypeFromString(param.first));
    }

    // Use sret for large struct returns
    llvm::Type* returnType = getTypeFromString(returnTypeStr);
    bool sret = needsSret(returnType);
    if (sret) {
        funcSretTypes[name] = returnType;
        // Prepend hidden sret pointer as first parameter
        paramTypes.insert(paramTypes.begin(), llvm::PointerType::get(*context, 0));
    }

    // Eskiu param types — for interface boxing at call sites
    {
        std::vector<std::string> pts;
        for (auto& p : params)
            if (p.first != "...") pts.push_back(p.first);
        funcEskiuParamTypes[name] = pts;
    }
    funcEskiuReturnType[name] = returnTypeStr;

    // Idempotent: reuse a prototype declared by the pre-pass.
    if (llvm::Function* existing = module->getFunction(name)) return existing;

    bool isVarArg = false;
    for (auto& p : params) if (p.first == "...") isVarArg = true;
    llvm::FunctionType* funcType = llvm::FunctionType::get(
        sret ? llvm::Type::getVoidTy(*context) : returnType, paramTypes, isVarArg);
    llvm::Function* func = llvm::Function::Create(
        funcType, llvm::Function::ExternalLinkage, name, module.get());
    // A narrow integer result is extended by the callee (signext/zeroext), so a C
    // caller (a callback, or C calling an Eskiu function) reads it the C way.
    // Every Eskiu value is initialized (locals start at zero), so a scalar argument or
    // result is never undef or poison: noundef, as clang marks C's.
    auto scalar = [](llvm::Type* t) { return t->isIntegerTy() || t->isFloatingPointTy() || t->isPointerTy(); };
    if (!sret) {
        llvm::Attribute::AttrKind rext = cabiExtAttr(returnTypeStr, returnType);
        if (rext != llvm::Attribute::None) func->addRetAttr(rext);
        if (scalar(returnType)) func->addRetAttr(llvm::Attribute::NoUndef);
    }
    for (unsigned i = sret ? 1 : 0; i < func->arg_size(); ++i)
        if (scalar(func->getArg(i)->getType())) func->addParamAttr(i, llvm::Attribute::NoUndef);

    // Set parameter names (skip index 0 for sret functions — that's the hidden ret ptr)
    size_t paramIdx = 0;
    size_t argIdx   = 0;
    for (auto& arg : func->args()) {
        if (sret && argIdx == 0) {
            arg.setName("sret.ptr");
            argIdx++;
            continue;
        }
        if (paramIdx < params.size() && params[paramIdx].first != "...") {
            arg.setName(params[paramIdx].second);
            paramIdx++;
        }
        argIdx++;
    }
    return func;
}

void CodeGen::failAt(const ASTNode* node, const std::string& msg) const {
    if (!node || node->line <= 0) throw std::runtime_error(msg);
    std::string file = diagFile.empty() ? "<input>" : diagFile;
    throw std::runtime_error(file + ":" + std::to_string(node->line) + ":" + std::to_string(node->col) +
                             ": " + msg);
}

void CodeGen::visit(FunctionDecl* node) {
    if (!node->typeParams.empty()) {
        // A generic prototype (`T f<T>(T x);`) never replaces its definition.
        FunctionDecl*& slot = funcTemplateDecls[node->name];
        if (!slot || node->body || !slot->body) slot = node;
        return;
    }
    // A body emitted in the middle of an expression (a template instantiation) has its
    // own substitutions: an enclosing chain's memoized operand types do not apply.
    struct MemoScope {
        CodeGen& cg;
        decltype(cg.chainTypeMemo) saved;
        ~MemoScope() { cg.chainTypeMemo = saved; }
    } memoScope{*this, chainTypeMemo};
    chainTypeMemo = nullptr;
    struct FileScope {
        CodeGen& cg;
        std::string saved;
        ~FileScope() { cg.diagFile = saved; }
    } fileScope{*this, diagFile};
    if (!node->sourceFile.empty()) diagFile = node->sourceFile;

    // Declare (or reuse) the prototype, then emit the body.
    llvm::Function* func = declareFunction(node->name, node->returnType, node->params);

    // A body-less declaration (forward declaration) only needs the prototype.
    if (!node->body) return;
    // Defensive: skip if a body was already emitted (e.g. forward decl + definition).
    if (!func->empty()) return;
    if (node->isExport) exportedFns.push_back(node->name);

    llvm::Type* returnType = getTypeFromString(node->returnType);
    bool sret = needsSret(returnType);

    // Create entry block
    llvm::BasicBlock* entryBlock = llvm::BasicBlock::Create(*context, "entry", func);
    builder->SetInsertPoint(entryBlock);

    // Save current function + sret context
    llvm::Function* prevFunc       = currentFunction;
    llvm::Value*    prevSretParam  = currentSretParam;
    currentFunction = func;
    currentSretParam = sret ? &*func->arg_begin() : nullptr;

    // A nested function body (e.g. a template instantiated mid-expression, possibly
    // inside a `try`) is a fresh scope-exit context: never the enclosing function's
    // defers/finally, loops, or landingpad.
    BodyContext bodyCtx(this);
    // Nor does it see the enclosing function's locals: a local `free` there must not
    // hide the function `free` from the instantiated body (sema checks each instance
    // with only the globals in scope).
    struct NameScope {
        CodeGen& cg;
        std::map<std::string, llvm::Value*> symbols;
        std::vector<std::map<std::string, llvm::Value*>> scopes;
        std::vector<std::map<std::string, std::string>> varTypes;
        ~NameScope() {
            cg.symbolTable = std::move(symbols);
            cg.scopeStack = std::move(scopes);
            cg.varTypeStack = std::move(varTypes);
        }
    } nameScope{*this, symbolTable, std::move(scopeStack), std::move(varTypeStack)};
    if (!nameScope.scopes.empty()) symbolTable = nameScope.scopes.front();
    scopeStack.clear();
    varTypeStack.clear();

    // Push scope for function parameters
    pushScope();
    // (Eskiu param types for interface boxing were registered by declareFunction.)

    // Define parameters in symbol table + type map (skip sret hidden param at index 0)
    size_t paramIdx = 0;
    size_t argIdx   = 0;
    for (auto& arg : func->args()) {
        if (sret && argIdx == 0) { argIdx++; continue; }  // skip sret ptr
        if (paramIdx < node->params.size() && node->params[paramIdx].first != "...") {
            // Give every parameter a stack slot: it makes the parameter a mutable
            // lvalue (so the body may reassign it, like a local) and gives
            // struct-by-value params a pointer for MemberExpr GEP. The incoming
            // argument is stored into the slot; reads load from it.
            auto* a = entryAlloca(arg.getType(), nullptr,
                                            node->params[paramIdx].second);
            builder->CreateStore(&arg, a);
            llvm::Value* paramSlot = a;
            defineSymbol(node->params[paramIdx].second, paramSlot);
            std::string ptype = !typeParamOverride.empty()
                ? substType(node->params[paramIdx].first, typeParamOverride)
                : node->params[paramIdx].first;
            // Instantiate the template instance's struct now, so member access on
            // this param (e.g. List<String>* self -> self.data) finds its fields
            // even if no earlier code referenced the type. A `const Box<T>* self`
            // names Box_int.
            if (ptype.find('<') != std::string::npos)
                ptype = instanceSpelling(ptype);
            defineVarType(node->params[paramIdx].second, ptype);
            paramIdx++;
        }
        argIdx++;
    }

    // Generate function body
    if (node->body) {
        node->body->accept(this);
    }

    // Default return if no explicit return emitted
    if (!hasTerminator(builder->GetInsertBlock())) {
        if (sret || returnType->isVoidTy()) {
            builder->CreateRetVoid();
        } else if (returnType->isIntegerTy()) {
            builder->CreateRet(llvm::ConstantInt::get(returnType, 0));
        } else {
            builder->CreateRet(llvm::Constant::getNullValue(returnType));
        }
    }

    // Restore context
    popScope();
    currentFunction  = prevFunc;
    currentSretParam = prevSretParam;
}

llvm::Constant* CodeGen::foldConstDecl(VarDecl* v) {
    if (!v->isConst || !v->initializer || v->isExtern) return nullptr;
    static const std::set<std::string> scalars = {
        "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32",
        "uint64", "bool", "char", "float", "double"};
    std::string t = tyq::strip(expandAlias(v->type));
    if (!scalars.count(t) && !enumTypes.count(t)) return nullptr;
    llvm::Constant* c = foldViaCodegen(v->initializer, getTypeFromString(t));
    if (!c) return nullptr;
    if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c))
        constInts[v->name] = eskiuUnsigned(t) ? (long long)ci->getZExtValue() : ci->getSExtValue();
    if (currentFunction == nullptr) constGlobalValues[v->name] = c;
    return c;
}

void CodeGen::visit(VarDecl* node) {
    // A variable of a type with field defaults that nothing initializes starts as the
    // default value (`S{}`, an array of them), not as zero.
    if (!node->initializer && !node->isExtern)
        if (ExprPtr d = defaultExprFor(node->type)) node->initializer = d;
    // A numeric `const` folds to its value: array dimensions, case labels and later
    // constant initializers use it, and reads of the variable yield the constant.
    llvm::Constant* constVal = node->isStatic || node->scriptInit ? nullptr : foldConstDecl(node);

    llvm::Type* declType = getTypeFromString(node->type);

    // Global scope (no active function) → emit as llvm::GlobalVariable
    if (currentFunction == nullptr) {
        // `extern <type> <name>;` — the variable is defined in another translation
        // unit (a C global). Emit an external declaration: external linkage, no
        // initializer. References resolve at link time.
        // Beside its definition in the same program (either order, or another input
        // file), an `extern` names that one variable.
        llvm::GlobalVariable* prior = module->getNamedGlobal(node->name);
        if (node->isExtern) {
            auto* gv = prior ? prior : new llvm::GlobalVariable(
                *module, declType, /*isConstant=*/node->isConst,
                llvm::GlobalValue::ExternalLinkage, /*init=*/nullptr, node->name);
            defineSymbol(node->name, gv);
            defineVarType(node->name, node->type);
            return;
        }
        // A script's global starts at zero; the implicit main assigns its initializer.
        splitUnionTop = declType;
        llvm::Constant* init = node->initializer && !node->scriptInit
            ? constInitializer(node->initializer, declType)
            : nullptr;
        splitUnionTop = nullptr;
        if (!init && node->initializer && !node->scriptInit)
            throw std::runtime_error("initializer of global '" + node->name +
                                     "' is not a compile-time constant");
        if (!init) init = llvm::Constant::getNullValue(declType);

        // A global has external (C) linkage, so a C object can reference it by name. An
        // earlier `extern` declaration of it becomes this definition.
        llvm::GlobalVariable* gv = nullptr;
        // The initializer's type is the declared one, or a struct of the same layout
        // (constInitializer's split form of an array with a long zero tail).
        if (prior && prior->isDeclaration() && prior->getValueType() == init->getType()) {
            gv = prior;
            gv->setInitializer(init);
            gv->setConstant(false);
        } else {
            gv = new llvm::GlobalVariable(
                *module, init->getType(), /*isConstant=*/false,
                llvm::GlobalValue::ExternalLinkage, init, node->name);
            // A literal of another type keeps the declared type's alignment.
            if (init->getType() != declType)
                gv->setAlignment(module->getDataLayout().getABITypeAlign(declType));
            if (prior && prior->isDeclaration()) {
                gv->takeName(prior);
                prior->replaceAllUsesWith(gv);
                prior->eraseFromParent();
            }
        }

        if (constVal && !node->isVolatile) constValueOf[gv] = constVal;
        if (node->isVolatile) volatileVars.insert(node->name);
        defineSymbol(node->name, gv);
        defineVarType(node->name, node->type);
        return;
    }
    // Static local: one instance in module scope, persists across calls.
    if (node->isStatic) {
        llvm::Constant* init = node->initializer
            ? constInitializer(node->initializer, declType)
            : nullptr;
        if (!init && node->initializer)
            throw std::runtime_error("initializer of static '" + node->name +
                                     "' is not a compile-time constant");
        if (!init) init = llvm::Constant::getNullValue(declType);
        std::string gname = currentFunction->getName().str() + "." + node->name;
        auto* gv = new llvm::GlobalVariable(
            *module, init->getType(), /*isConstant=*/false,
            llvm::GlobalValue::PrivateLinkage, init, gname);
        defineSymbol(node->name, gv);
        defineVarType(node->name, node->type);
        return;
    }

    llvm::AllocaInst* alloca = entryAlloca(declType, nullptr, node->name);
    // Resolve type params and mangle template names for varTypeStack,
    // preserving pointer suffixes (e.g. "List<int>*" → "List_int*")
    std::string varType = !typeParamOverride.empty()
                          ? substType(node->type, typeParamOverride)
                          : node->type;
    if (varType.find('<') != std::string::npos)
        varType = instanceSpelling(varType);

    // The name is bound after its initializer: in `{ int64 x = x + 1; }` the `x`
    // on the right is the outer one (sema resolves it that way too).
    if (node->initializer) {
        if (auto structInit = nodeCast<StructInitExpr>(node->initializer.get())) {
            // Fill the alloca directly — no temporary needed
            emitStructInitInto(alloca, structInit);
        } else if (auto arrLit = nodeCast<ArrayLitExpr>(node->initializer.get())) {
            emitArrayInitInto(alloca, arrLit, varType);
        } else {
            llvm::Value* val = evalForType(node->initializer, varType);
            val = coerceValue(val, declType, eskiuUnsigned(getExprEskiuType(node->initializer)));
            if (val) builder->CreateStore(val, alloca)->setVolatile(node->isVolatile);   // an MMIO init
        }
    } else {
        // A local declared without a value starts at zero, as a global, a `static` and
        // alloc<T> do: every declaration (so each loop iteration) gets a fresh zero.
        if (module->getDataLayout().getTypeAllocSize(declType).getFixedValue() > kBulkZeroBytes)
            emitZeroFill(alloca, declType);
        else
            builder->CreateStore(llvm::Constant::getNullValue(declType), alloca)->setVolatile(node->isVolatile);
    }
    if (node->isVolatile) volatileVars.insert(node->name);
    if (constVal && !node->isVolatile) constValueOf[alloca] = constVal;
    defineSymbol(node->name, alloca);
    defineVarType(node->name, varType);
}

uint64_t CodeGen::cAlignOf(llvm::Type* t) const {
    while (auto* at = llvm::dyn_cast<llvm::ArrayType>(t)) t = at->getElementType();
    if (auto* st = llvm::dyn_cast<llvm::StructType>(t)) {
        auto it = cAlignOverride.find(st);
        if (it != cAlignOverride.end()) return it->second;
    }
    return module->getDataLayout().getABITypeAlign(t).value();
}

bool CodeGen::buildPackedLayout(const std::vector<StructDecl::Field>& fields, unsigned packN,
                                std::vector<llvm::Type*>& phys,
                                std::map<std::string, BitfieldSlot>& slots, uint64_t& structAlign) {
    const llvm::DataLayout& DL = module->getDataLayout();
    llvm::Type* i8 = llvm::Type::getInt8Ty(*context);
    uint64_t offset = 0;
    structAlign = 1;
    for (const auto& f : fields) {
        if (f.bitWidth > 0) return false;  // pack + bitfields: fall back to the bitfield path
        llvm::Type* ft = getTypeFromString(f.type);
        uint64_t align = cAlignOf(ft);
        if (packN) align = std::min<uint64_t>(align, packN);
        if (align > structAlign) structAlign = align;
        uint64_t aligned = (offset + align - 1) / align * align;
        if (aligned > offset) { phys.push_back(llvm::ArrayType::get(i8, aligned - offset)); offset = aligned; }
        BitfieldSlot s;
        s.isBitfield = false;
        s.physIndex = (unsigned)phys.size();
        s.storageType = ft;
        slots[f.name] = s;
        phys.push_back(ft);
        offset += DL.getTypeAllocSize(ft).getFixedValue();
    }
    // Round the total size up to the struct's alignment (min(maxFieldAlign, N)),
    // so an array element stride matches the C `#pragma pack(N)` ABI.
    uint64_t total = (offset + structAlign - 1) / structAlign * structAlign;
    if (total > offset) phys.push_back(llvm::ArrayType::get(i8, total - offset));
    return true;
}
