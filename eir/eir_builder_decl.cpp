// eir_builder_decl.cpp: the module build, functions, variables and locals (split from eir_builder.cpp).
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

std::unique_ptr<Module> EIRBuilder::build(Program* program) {
    volatileVars.clear();
    cleanupScopes.clear();
    loopContextStack.clear();
    templateDecls.clear();
    funcTemplateDecls.clear();
    templateInstanceArgs.clear();
    typeParamOverride.clear();
    if (semaInstanceArgs) {
        for (const auto& kv : *semaInstanceArgs) {
            templateInstanceArgs.emplace(kv.first, kv.second);
        }
    }
    module = std::make_unique<Module>();
    module->name = "main";
    module->targetTriple = targetTriple;

    if (!program) return std::move(module);

    // Phase 0: Type Aliases
    for (auto& decl : program->declarations) {
        if (auto ta = nodeCast<TypeAliasDecl>(decl.get())) {
            typeAliases[ta->name] = ta->aliased;
            templateTypeAliases()[ta->name] = ta->aliased;
        }
    }

    // Phase 1: Structs, Enums, and Interfaces
    plainStructs.clear();
    unionDecls.clear();
    for (auto& decl : program->declarations) {
        if (auto iface = nodeCast<InterfaceDecl>(decl.get())) {
            InterfaceInfo info;
            info.name = iface->name;
            for (const auto& m : iface->methods) {
                info.methodNames.push_back(m.name);
                info.methodReturnTypes.push_back(m.returnType);
                std::vector<std::string> pts;
                for (const auto& p : m.params) pts.push_back(p.first);
                info.methodParamTypes.push_back(pts);
            }
            interfaces[iface->name] = info;
        } else if (auto e = nodeCast<EnumDecl>(decl.get())) {
            if (!e->typeParams.empty()) {
                genericEnums[e->name] = e;
                for (size_t i = 0; i < e->members.size(); ++i)
                    adtVariants[e->members[i].first] = {e->name, static_cast<int>(i), {}};
            } else if (e->isADT()) {
                adtEnums[e->name] = e;
            } else {
                plainEnums[e->name] = e;
                int64_t nextVal = 0;
                for (size_t i = 0; i < e->members.size(); ++i) {
                    const std::string& mname = e->members[i].first;
                    if (i < e->valueExprs.size() && e->valueExprs[i]) {
                        nextVal = foldConstInt(e->valueExprs[i]);
                    } else if (i < e->members.size()) {
                        nextVal = e->members[i].second;
                    }
                    enumConstants[mname] = nextVal;
                    nextVal++;
                }
            }
        } else if (auto s = nodeCast<StructDecl>(decl.get())) {
            if (!s->typeParams.empty()) {
                templateDecls[s->name] = s;
            } else {
                plainStructs[s->name] = s;
            }
        } else if (auto u = nodeCast<UnionDecl>(decl.get())) {
            unionDecls[u->name] = u;
        } else if (auto f = nodeCast<FunctionDecl>(decl.get())) {
            if (!f->typeParams.empty()) {
                if (!funcTemplateDecls.count(f->name) || f->body) funcTemplateDecls[f->name] = f;   // the definition, not a prototype
            }
        } else if (auto v = nodeCast<VarDecl>(decl.get())) {
            if (v->isExtern) {
                continue;
            }
            if (v->isVolatile) {
                volatileVars.insert(v->name);
            }
            TypePtr type = toEirType(v->type);
            Operand initOp = Operand::constInt(0, type);
            if (v->initializer) {
                if (auto u = nodeCast<UnaryExpr>(v->initializer.get())) {
                    if (u->op == "&") {
                        if (auto id = nodeCast<IdentExpr>(u->operand.get())) {
                            initOp = Operand::global(id->name, type);
                        }
                    }
                }
                if (initOp.kind != OperandKind::GlobalRef) {
                    if (type && type->isInt()) {
                        if (ty::CInt folded; foldCInt(v->initializer, folded)) {
                            int64_t cv = truncConst(folded.v, type);
                            if (v->isConst) {
                                enumConstants[v->name] = cv;
                                constTypes[v->name] = type;
                            }
                            initOp = Operand::constInt(cv, type);
                        } else {
                            initOp = Operand::constInt(0, type);
                        }
                    } else if (type && type->isFloat()) {
                        double fv = 0.0;
                        if (auto lit = nodeCast<LiteralExpr>(v->initializer.get())) {
                            fv = std::strtod(lit->value.c_str(), nullptr);
                        }
                        initOp = Operand::constFloat(fv, type);
                    } else {
                        initOp = Operand::constNull();
                    }
                }
            }
            module->globals.push_back(GlobalVar{v->name, type, initOp, v->isConst});
        }
    }

    for (auto& decl : program->declarations) {
        auto e = nodeCast<EnumDecl>(decl.get());
        if (e && adtEnums.count(e->name) && !module->structLayouts.count(e->name))
            layoutAdt(e->name, adtEnums[e->name], {});
    }
    for (auto& decl : program->declarations) {
        if (auto s = nodeCast<StructDecl>(decl.get()); s && s->typeParams.empty())
            ensureStructLayout(s->name);
    }
    for (auto& decl : program->declarations) {
        if (auto u = nodeCast<UnionDecl>(decl.get())) ensureStructLayout(u->name);
    }

    // Phase 2: Function prototypes
    definedFunctions.clear();
    for (auto& decl : program->declarations)
        if (auto f = nodeCast<FunctionDecl>(decl.get()); f && f->typeParams.empty() && f->body)
            definedFunctions.insert(f->name);
    for (auto& decl : program->declarations) {
        if (auto f = nodeCast<FunctionDecl>(decl.get())) {
            if (!f->typeParams.empty()) continue;
            std::vector<std::pair<std::string, TypePtr>> params;
            bool hasVarargs = false;
            for (const auto& p : f->params) {
                if (p.first == "...") {
                    hasVarargs = true;
                    break;
                }
                params.emplace_back(p.second, toEirType(p.first));
            }
            if (Function* have = module->getFunction(f->name)) {
                if (f->isExport && f->body) have->isExport = true;   // a prototype came first
                continue;
            }
            auto fn = module->createFunction(f->name, toEirType(f->returnType), params);
            fn->isVariadic = hasVarargs;
            fn->isExport = f->isExport && f->body;
        } else if (auto ext = nodeCast<ExternDecl>(decl.get())) {
            if (module->getFunction(ext->name)) continue;
            std::vector<std::pair<std::string, TypePtr>> params;
            bool hasVarargs = false;
            for (const auto& p : ext->params) {
                if (p.first == "...") {
                    hasVarargs = true;
                    continue;
                }
                params.emplace_back(p.second, toEirType(p.first));
            }
            auto fn = module->createFunction(ext->name, toEirType(ext->returnType), params);
            fn->isExtern = true;
            fn->isVariadic = hasVarargs;
        } else if (auto s = nodeCast<StructDecl>(decl.get())) {
            if (!s->typeParams.empty()) continue;
            for (const auto& m : s->methods) {
                if (auto mf = nodeCast<FunctionDecl>(m.get())) {
                    std::vector<std::pair<std::string, TypePtr>> params;
                    params.emplace_back("self", Type::getPtr(Type::getStruct(s->name, {})));
                    for (const auto& p : mf->params) {
                        params.emplace_back(p.second, toEirType(p.first));
                    }
                    if (module->getFunction(s->name + "_" + mf->name)) continue;
                    module->createFunction(s->name + "_" + mf->name, toEirType(mf->returnType), params);
                }
            }
        }
    }

    bool hasGlobalInits = false;
    for (const auto& decl : program->declarations) {
        if (auto v = nodeCast<VarDecl>(decl.get())) {
            if (!v->isExtern && (v->initializer || defaultExprFor(v->type))) {
                hasGlobalInits = true;
                break;
            }
        }
    }
    if (hasGlobalInits) {
        module->createFunction("__eir_global_init", Type::getVoid(), {});
        module->globals.push_back(GlobalVar{"__eir_inited", Type::getI1(), Operand::constInt(0, Type::getI1()), false});
    }

    // Phase 3: Function bodies
    program->accept(this);

    // Phase 4: Emit global initializers
    if (hasGlobalInits) {
        Function* initFn = module->getFunction("__eir_global_init");
        currentFunction = initFn;
        nextVarId = 0;
        nextBlockId = 0;
        BasicBlock* entryBB = initFn->createBlock("entry");
        setBlock(entryBB);

        std::string flagVal = newVar();
        emit(Instruction{Opcode::Load, flagVal, Type::getI1(), {Operand::global("__eir_inited", Type::getPtr(Type::getI1()))}});
        BasicBlock* doInitBB = initFn->createBlock("do_init");
        BasicBlock* exitBB = initFn->createBlock("already_inited");
        emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(flagVal, Type::getI1()), Operand::block(exitBB->name), Operand::block(doInitBB->name)}});

        setBlock(doInitBB);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(1, Type::getI1()), Operand::global("__eir_inited", Type::getPtr(Type::getI1()))}});

        for (const auto& decl : program->declarations) {
            if (auto v = nodeCast<VarDecl>(decl.get())) {
                ExprPtr init = v->isExtern ? nullptr : (v->initializer ? v->initializer : defaultExprFor(v->type));
                if (init) {
                    TypePtr type = toEirType(v->type);
                    Operand target = Operand::global(v->name, Type::getPtr(type));
                    if (auto arrLit = nodeCast<ArrayLitExpr>(init.get())) {
                        emitArrayInitInto(target, arrLit, type);
                    } else {
                        Operand val = evalForType(init, v->type);
                        Instruction st{Opcode::Store, "", Type::getVoid(),
                            {(type->isStruct() || type->isArray()) ? Operand::var(val.str(), type) : val,
                             target}};
                        st.isVolatile = v->isVolatile;
                        emit(st);
                    }
                }
            }
        }
        emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(exitBB->name)}});

        setBlock(exitBB);
        emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
        currentFunction = nullptr;
        currentBlock = nullptr;
    }

    return std::move(module);
}

// ============================================================================
// Visitors
// ============================================================================

void EIRBuilder::visit(Program* node) {
    for (auto& decl : node->declarations) {
        if (auto iface = nodeCast<InterfaceDecl>(decl.get())) {
            iface->accept(this);
        } else if (auto en = nodeCast<EnumDecl>(decl.get())) {
            en->accept(this);
        }
    }
    for (auto& decl : node->declarations) {
        currentFunction = nullptr;   // a top-level declaration is outside any function
        currentBlock = nullptr;
        decl->accept(this);
    }
}

void EIRBuilder::visit(FunctionDecl* node) {
    if (!node->typeParams.empty()) {
        if (!funcTemplateDecls.count(node->name) || node->body) funcTemplateDecls[node->name] = node;   // the definition, not a prototype
        return;
    }

    currentFunction = module->getFunction(node->name);
    if (!currentFunction) {
        std::vector<std::pair<std::string, TypePtr>> params;
        bool hasVarargs = false;
        for (const auto& p : node->params) {
            if (p.first == "...") {
                hasVarargs = true;
                break;
            }
            params.emplace_back(p.second, toEirType(p.first));
        }
        currentFunction = module->createFunction(node->name, toEirType(node->returnType), params);
        currentFunction->isVariadic = hasVarargs;
    }

    if (!node->body) {
        if (currentFunction->blocks.empty() && !definedFunctions.count(node->name)) currentFunction->isExtern = true;
        return;
    }
    if (!currentFunction->blocks.empty()) return;

    currentFunction->isExtern = false;
    // The definition names the parameters: an `extern` or prototype of the same function
    // seen first may have named them differently.
    for (size_t i = 0, k = 0; i < node->params.size() && k < currentFunction->params.size(); ++i) {
        if (node->params[i].first == "...") break;
        currentFunction->params[k++].first = node->params[i].second;
    }

    nextVarId = 0;
    nextBlockId = 0;
    BasicBlock* entryBB = currentFunction->createBlock("entry");
    setBlock(entryBB);

    auto savedCleanups = std::move(cleanupScopes);
    auto savedLoops = std::move(loopContextStack);
    auto savedUnwind = currentUnwindTarget;
    auto savedUnwindStack = std::move(unwindTargetStack);
    size_t savedHandlerDepth = currentHandlerDepth;
    cleanupScopes.clear();
    cleanupScopes.emplace_back(); // root function cleanup frame
    loopContextStack.clear();
    currentUnwindTarget = nullptr;
    unwindTargetStack.clear();
    currentHandlerDepth = 0;

    pushScope();

    // Allocate stack slots for each parameter
    for (size_t i = 0; i < currentFunction->params.size(); ++i) {
        const auto& p = currentFunction->params[i];
        std::string slotName = "%" + p.first + ".addr";
        emit(Instruction{Opcode::Alloca, slotName, p.second, {Operand::constInt(p.second->bitWidth <= 32 ? 4 : 8, Type::getI32())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var("%" + p.first, p.second), Operand::var(slotName, Type::getPtr(p.second))}});
        declareVar(p.first, Operand::var(slotName, Type::getPtr(p.second)), p.second);
    }

    if (node->name == "main") {
        if (module->getFunction("__eir_global_init")) {
            emit(Instruction{Opcode::Call, "", Type::getVoid(), {Operand::global("__eir_global_init", Type::getVoid())}});
        }
    }

    node->body->accept(this);

    // Ensure terminator and run any remaining cleanups
    if (!currentBlock->hasTerminator()) {
        runCleanupsToDepth(0, false);
        if (!currentBlock->hasTerminator()) {
            if (currentFunction->returnType->isVoid()) {
                emit(Instruction{Opcode::Ret, "", Type::getVoid(), {}});
            } else {
                emit(Instruction{Opcode::Unreachable, "", Type::getVoid(), {}});
            }
        }
    }

    cleanupScopes = std::move(savedCleanups);
    loopContextStack = std::move(savedLoops);
    currentUnwindTarget = savedUnwind;
    unwindTargetStack = std::move(savedUnwindStack);
    currentHandlerDepth = savedHandlerDepth;
    popScope();
    currentFunction = nullptr;
    currentBlock = nullptr;
}

void EIRBuilder::visit(VarDecl* node) {
    if (!currentFunction) {
        return; // Globals are processed in Phase 1 of build
    }
    if (node->isVolatile) {
        volatileVars.insert(node->name);
    }
    TypePtr type = toEirType(node->type);

    if (node->isStatic) {
        std::string gname = (currentFunction ? currentFunction->name : "global") + "." + node->name;
        std::string guardName = gname + ".inited";
        bool found = false;
        for (const auto& g : module->globals) {
            if (g.name == gname) { found = true; break; }
        }
        if (!found) {
            module->globals.push_back(GlobalVar{gname, type, Operand::constInt(0, type), false});
            module->globals.push_back(GlobalVar{guardName, Type::getI1(), Operand::constInt(0, Type::getI1()), false});
        }
        declareVar(node->name, Operand::global(gname, Type::getPtr(type)), type);

        if (node->initializer) {
            std::string guardVal = newVar();
            emit(Instruction{Opcode::Load, guardVal, Type::getI1(), {Operand::global(guardName, Type::getPtr(Type::getI1()))}});
            BasicBlock* initBB = createBlock("static.init");
            BasicBlock* contBB = createBlock("static.cont");
            emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(guardVal, Type::getI1()), Operand::block(contBB->name), Operand::block(initBB->name)}});

            setBlock(initBB);
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(1, Type::getI1()), Operand::global(guardName, Type::getPtr(Type::getI1()))}});
            if (auto arrLit = nodeCast<ArrayLitExpr>(node->initializer.get())) {
                emitArrayInitInto(Operand::global(gname, Type::getPtr(type)), arrLit, type);
            } else {
                Operand val = evalForType(node->initializer, node->type);
                Instruction st{Opcode::Store, "", Type::getVoid(),
                    {(type->isStruct() || type->isArray()) ? Operand::var(val.str(), type) : val,
                     Operand::global(gname, Type::getPtr(type))}};
                emit(st);
            }
            emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(contBB->name)}});
            setBlock(contBB);
        }
        return;
    }

    size_t al = getTypeAlign(type, module.get());
    if (al < 1) al = 1;
    std::string slotName = "%" + node->name + ".addr";
    // A name declared again (a nested block, a second branch) gets its own slot.
    if (lookupVar(node->name) || functionSlots[currentFunction].count(slotName)) slotName = "%" + node->name + "." + std::to_string(nextVarId++) + ".addr";
    functionSlots[currentFunction].insert(slotName);
    // The slot lives in the entry block, so a jump past the declaration (to a later case
    // label) still finds it; the declaration itself sets it, to zero without an initializer.
    entryAlloca(slotName, type, al);

    // The initializer is evaluated before the name is declared: `int64 x = x + 1;` in a
    // nested block reads the outer x. Without one, a struct with field defaults takes them.
    // A variable whose type names a type parameter starts at zero in generic code.
    bool generic = !typeParamOverride.empty() && substType(node->type, typeParamOverride) != node->type;
    ExprPtr init = node->initializer ? node->initializer : (generic ? nullptr : defaultExprFor(node->type));
    if (init) {
        if (auto arrLit = nodeCast<ArrayLitExpr>(init.get())) {
            emitArrayInitInto(Operand::var(slotName, Type::getPtr(type)), arrLit, type);
        } else {
            Operand val = evalForType(init, type);
            Instruction st{Opcode::Store, "", Type::getVoid(), {val, Operand::var(slotName, Type::getPtr(type))}};
            st.isVolatile = node->isVolatile;
            emit(st);
        }
    } else {
        emitZeroFill(Operand::var(slotName, Type::getPtr(type)), type);
    }
    declareVar(node->name, Operand::var(slotName, Type::getPtr(type)), type);
}

// An alloca at the top of the function's entry block.
void EIRBuilder::entryAlloca(const std::string& name, const TypePtr& type, size_t align) {
    BasicBlock* entry = currentFunction->blocks.front().get();
    auto& insts = entry->instructions;
    size_t at = 0;
    while (at < insts.size() && insts[at]->op == Opcode::Alloca) ++at;
    insts.insert(insts.begin() + static_cast<long>(at), std::make_unique<Instruction>(
        Opcode::Alloca, name, type, std::vector<Operand>{Operand::constInt(static_cast<int64_t>(align), Type::getI32())}));
}

// Zero the bytes of a `type` at `ptr`: a scalar store, or 8-byte stores then bytes.
void EIRBuilder::emitZeroFill(const Operand& ptr, const TypePtr& type) {
    if (!type) return;
    if (type->isInt() || type->isFloat() || type->isPointer()) {
        Operand zero = type->isFloat() ? Operand::constFloat(0.0, type) : type->isPointer() ? Operand::constNull() : Operand::constInt(0, type);
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {zero, Operand::var(ptr.text, Type::getPtr(type))}});
        return;
    }
    size_t size = getTypeSize(type, module.get());
    if (type->isStruct()) { size_t ss = module->getStructSize(type->nominalName()); if (ss) size = ss; }
    if (size <= 256) {
        size_t off = 0;
        for (; off + 8 <= size; off += 8) {
            std::string p = newVar();
            emit(Instruction{Opcode::OffsetPtr, p, Type::getPtr(Type::getI64()), {ptr, Operand::constInt(static_cast<int64_t>(off), Type::getI64())}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getI64()), Operand::var(p, Type::getPtr(Type::getI64()))}});
        }
        for (; off < size; ++off) {
            std::string p = newVar();
            emit(Instruction{Opcode::OffsetPtr, p, Type::getPtr(Type::getU8()), {ptr, Operand::constInt(static_cast<int64_t>(off), Type::getI64())}});
            emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getU8()), Operand::var(p, Type::getPtr(Type::getU8()))}});
        }
        return;
    }
    // A large block: a loop over its 8-byte words (an aggregate's size is a multiple of 8
    // or padded to its alignment; the tail is zeroed bytewise).
    TypePtr i64 = Type::getI64();
    std::string idx = newVar() + ".z";
    entryAlloca(idx, i64, 8);
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, i64), Operand::var(idx, Type::getPtr(i64))}});
    BasicBlock* loopBB = createBlock("zero.loop");
    BasicBlock* bodyBB = createBlock("zero.body");
    BasicBlock* doneBB = createBlock("zero.done");
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});
    setBlock(loopBB);
    std::string iv = newVar(), more = newVar();
    emit(Instruction{Opcode::Load, iv, i64, {Operand::var(idx, Type::getPtr(i64))}});
    emit(Instruction{Opcode::ICmp, more, Type::getI1(), {Operand::var(iv, i64), Operand::constInt(static_cast<int64_t>(size / 8), i64)}, ICmpCond::Slt});
    emit(Instruction{Opcode::BrCond, "", Type::getVoid(), {Operand::var(more, Type::getI1()), Operand::block(bodyBB->name), Operand::block(doneBB->name)}});
    setBlock(bodyBB);
    std::string p = newVar(), next = newVar();
    emit(Instruction{Opcode::Gep, p, Type::getPtr(i64), {ptr, Operand::var(iv, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, i64), Operand::var(p, Type::getPtr(i64))}});
    emit(Instruction{Opcode::Add, next, i64, {Operand::var(iv, i64), Operand::constInt(1, i64)}});
    emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::var(next, i64), Operand::var(idx, Type::getPtr(i64))}});
    emit(Instruction{Opcode::Br, "", Type::getVoid(), {Operand::block(loopBB->name)}});
    setBlock(doneBB);
    for (size_t off = size / 8 * 8; off < size; ++off) {
        std::string b = newVar();
        emit(Instruction{Opcode::OffsetPtr, b, Type::getPtr(Type::getU8()), {ptr, Operand::constInt(static_cast<int64_t>(off), Type::getI64())}});
        emit(Instruction{Opcode::Store, "", Type::getVoid(), {Operand::constInt(0, Type::getU8()), Operand::var(b, Type::getPtr(Type::getU8()))}});
    }
}

// The value a variable of type `t` starts with when its declaration has no initializer:
// `S{}` for a struct with field defaults (directly or in a nested struct), an array
// literal of those for an array of such structs, else null (zero). A generic instance and
// a union have no defaults (codegen's defaultExprFor).
ExprPtr EIRBuilder::defaultExprFor(const std::string& t0) {
    std::string t = tyq::strip(t0);
    auto al = typeAliases.find(t);
    if (al != typeAliases.end()) t = al->second;
    if (!typeParamOverride.empty()) t = substType(t, typeParamOverride);
    ty::Type pt = ty::Type::parse(t);
    if (pt.kind == ty::Type::Kind::Array && pt.elem) {
        ExprPtr e = defaultExprFor(pt.elem->str());
        if (!e) return nullptr;
        TypePtr at = toEirType(t);
        if (!at || !at->isArray() || at->arraySize == 0 || at->arraySize > 65536) return nullptr;
        return std::make_shared<ArrayLitExpr>(std::vector<ExprPtr>(static_cast<size_t>(at->arraySize), e));
    }
    if (pt.kind != ty::Type::Kind::Struct && pt.kind != ty::Type::Kind::Named) return nullptr;
    std::string sname = pt.nominalName();
    auto it = plainStructs.find(sname);
    if (it == plainStructs.end()) return nullptr;
    auto hit = structHasDefaults.find(sname);
    if (hit == structHasDefaults.end()) {
        structHasDefaults[sname] = -1;   // a recursive struct reaches itself only by pointer
        bool any = false;
        for (const auto& f : it->second->fields)
            if (f.defaultValue || defaultExprFor(f.type)) { any = true; break; }
        structHasDefaults[sname] = any ? 1 : 0;
        hit = structHasDefaults.find(sname);
    }
    if (hit->second != 1) return nullptr;
    return std::make_shared<StructInitExpr>(sname, std::vector<std::pair<std::string, ExprPtr>>{});
}

// A struct literal with its omitted fields given their defaults; null when the struct has
// none.
std::shared_ptr<StructInitExpr> EIRBuilder::withDefaults(StructInitExpr* init) {
    std::string sname = init->structName;
    if (!defaultExprFor(sname)) return nullptr;
    const auto& fields = plainStructs[sname]->fields;
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
    out->line = init->line;
    out->col = init->col;
    return out;
}

} // namespace eir
