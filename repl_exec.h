// repl_exec.h: result types, symbol versions, running and committing an input (internal to repl.cpp, which includes it once, in order).
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>
#include "repl.h"
#include "codegen/codegen.h"
#include "sema/type_checker.h"

namespace {

// The type of the input's final expression statement, and whether to print it (not an
// assignment, an increment or a call to a variadic C function such as printf).
bool Repl::typeOfLast(const Attempt& a, const TypeChecker& tc, std::string& semaType,
                      bool& printable) const {
    printable = false;
    if (!a.wrapper) return false;
    auto* blk = nodeCast<BlockStmt>(a.wrapper->body.get());
    if (!blk || blk->items.empty()) return false;
    auto* sp = std::get_if<StmtPtr>(&blk->items.back());
    auto* es = sp ? nodeCast<ExprStmt>(sp->get()) : nullptr;
    if (!es || !es->expr) return false;
    const auto& types = tc.expressionTypeMap();
    auto it = types.find(es->expr.get());
    if (it == types.end() || it->second.empty() || it->second == "void" ||
        it->second == "unknown")
        return false;
    semaType = it->second;
    printable = true;
    if (auto* be = nodeCast<BinaryExpr>(es->expr.get()); be && be->op == "=") printable = false;
    if (nodeCast<IncDecExpr>(es->expr.get())) printable = false;
    if (auto* ce = nodeCast<CallExpr>(es->expr.get())) {
        if (auto* id = nodeCast<IdentExpr>(ce->callee.get()))
            for (const auto& d : a.program->declarations)
                if (auto* ex = nodeCast<ExternDecl>(d.get()); ex && ex->name == id->name)
                    for (const auto& p : ex->params)
                        if (p.first == "...") printable = false;
    }
    return true;
}

bool Repl::isStale(const std::string& sym, bool isFunction) const {
    auto de = defEpoch.find(sym);
    int defined = de == defEpoch.end() ? 0 : de->second;
    if (auto d = declEpoch.find(sym); d != declEpoch.end() && d->second > defined) return true;
    for (const auto& p : prefixEpoch)
        if (p.second > defined && sym.compare(0, p.first.size(), p.first) == 0 &&
            (isFunction || !p.first.empty()))
            return true;
    return false;
}

void Repl::processModule(llvm::Module& m, std::map<std::string, int>& ver,
                         std::map<std::string, int>& defEp) {
    for (llvm::GlobalValue& gv : m.global_values()) {
        if (!gv.hasName() || gv.hasLocalLinkage() || gv.hasAppendingLinkage()) continue;
        if (!llvm::isa<llvm::Function>(gv) && !llvm::isa<llvm::GlobalVariable>(gv)) continue;
        std::string base = gv.getName().str();
        if (base.rfind("llvm.", 0) == 0) continue;
        auto vit = version.find(base);
        if (gv.isDeclaration()) {
            if (vit != version.end()) gv.setName(versioned(base, vit->second));
            continue;
        }
        if (vit == version.end()) {
            ver[base] = 1;
            defEp[base] = epoch + 1;
            continue;
        }
        if (isStale(base, llvm::isa<llvm::Function>(gv))) {
            int v = vit->second + 1;
            gv.setName(versioned(base, v));
            ver[base] = v;
            defEp[base] = epoch + 1;
            continue;
        }
        // Already in the JIT: refer to that definition.
        if (auto* go = llvm::dyn_cast<llvm::GlobalObject>(&gv)) go->setComdat(nullptr);
        if (auto* f = llvm::dyn_cast<llvm::Function>(&gv)) {
            f->deleteBody();
        } else if (auto* g = llvm::dyn_cast<llvm::GlobalVariable>(&gv)) {
            g->setInitializer(nullptr);
        }
        gv.setLinkage(llvm::GlobalValue::ExternalLinkage);
        gv.setDSOLocal(false);
        gv.setName(versioned(base, vit->second));
    }
}

bool Repl::compileAndRun(Attempt& a, const std::string& valueSemaType,
                         const std::string& valueDisplay) {
    Program* program = a.program.get();
    TypeChecker tc;
    tc.sourceFile = kReplFile;
    if (!tc.check(program)) return false;
    AsyncTransform(&tc.expressionTypeMap(), &tc.instanceArgsMap(), &tc.instanceExprTypeMap())
        .run(program);
    TypeChecker postTc;
    postTc.sourceFile = kReplFile;
    if (!postTc.check(program)) {
        std::cerr << "error: internal: the async lowering produced a program that does not "
                     "type-check" << std::endl;
        return false;
    }
    std::unique_ptr<llvm::LLVMContext> llctx;
    std::unique_ptr<llvm::Module> mod;
    std::shared_ptr<Layout> layout;
    {
        CodeGen cg;
        cg.resolvedExprTypes = &postTc.expressionTypeMap();
        cg.semaInstanceArgs = &postTc.instanceArgsMap();
        try {
            if (!cg.generateCode(a.program)) {
                std::cerr << "error: code generation failed" << std::endl;
                return false;
            }
            if (!valueSemaType.empty()) {
                layout = cg.replLayout(valueSemaType);
                if (layout->kind == Layout::Opaque || layout->kind == Layout::Struct)
                    layout->name = valueDisplay;
            }
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << std::endl;
            return false;
        }
        mod = cg.takeModule(llctx);
    }

    std::map<std::string, int> ver = version, defEp = defEpoch;
    std::map<std::string, int> savedDecl = declEpoch, savedPrefix = prefixEpoch;
    int cur = epoch + 1;
    // The names this input declares: their earlier symbols become stale.
    for (const auto& d : a.inputDecls) {
        if (d->fromImport) continue;
        bool isType = nodeCast<StructDecl>(d.get()) || nodeCast<EnumDecl>(d.get()) ||
                      nodeCast<UnionDecl>(d.get()) || nodeCast<TypeAliasDecl>(d.get()) ||
                      nodeCast<InterfaceDecl>(d.get());
        // A redefined type may change its layout: every function is compiled again, and
        // the variables whose type names it start over (zeroed) in the new layout.
        if (isType && declEpoch.count(d->name)) {
            prefixEpoch[""] = cur;
            std::regex word("\\b" + d->name + "\\b");
            for (const auto& g : program->declarations)
                if (auto* vd = nodeCast<VarDecl>(g.get()))
                    if (!vd->fromImport && std::regex_search(vd->type, word)) declEpoch[vd->name] = cur;
        }
        declEpoch[d->name] = cur;
        if (auto* fd = nodeCast<FunctionDecl>(d.get()); fd && !fd->typeParams.empty())
            prefixEpoch[d->name + "_"] = cur;
        if (nodeCast<StructDecl>(d.get()) || nodeCast<EnumDecl>(d.get()))
            prefixEpoch[d->name + "_"] = cur;
    }
    // A variable declared again with the same type keeps its storage (the input assigns
    // it); with another type it is a new variable.
    for (const auto& g : a.newGlobals) {
        bool same = false;
        for (const auto& old : globals)
            if (old.name == g.name && old.type == g.type) same = true;
        if (!same) declEpoch[g.name] = cur;
    }
    processModule(*mod, ver, defEp);
    mod->setDataLayout(jit->getDataLayout());
    mod->setTargetTriple(jit->getTargetTriple());
    {
        std::string err;
        llvm::raw_string_ostream es(err);
        if (llvm::verifyModule(*mod, &es)) {
            std::cerr << "error: internal: REPL module does not verify:\n" << err << std::endl;
            declEpoch = savedDecl;
            prefixEpoch = savedPrefix;
            return false;
        }
    }
    loadLibs(program->linkLibs);
    if (auto err = jit->addIRModule(llvm::orc::ThreadSafeModule(std::move(mod), std::move(llctx)))) {
        std::cerr << "error: " << llvm::toString(std::move(err)) << std::endl;
        declEpoch = savedDecl;
        prefixEpoch = savedPrefix;
        return false;
    }
    version = ver;
    defEpoch = defEp;

    if (!a.wrapper) return true;
    auto sym = jit->lookup(a.entry);
    if (!sym) {
        std::cerr << "error: " << llvm::toString(sym.takeError()) << std::endl;
        return true;
    }
    auto fn = sym->toPtr<void (*)()>();
    fflush(stdout);
    bool threw = false;
    try {
        if (int sig = callGuarded(fn)) {
            threw = true;
            fflush(stdout);
#ifdef _WIN32
            std::cerr << "error: the input crashed (signal " << sig << ")"
#else
            std::cerr << "error: the input crashed (" << strsignal(sig)
#endif
                      << "); the session may be unstable" << std::endl;
        }
    } catch (void*& ex) {
        // The thrown object is the Eskiu exception itself (typed void*, its first word).
        threw = true;
        fflush(stdout);
        std::string d = describeException((void*)&ex);
        std::cerr << "error: uncaught exception" << (d.empty() ? "" : ": " + d) << std::endl;
    } catch (...) {
        threw = true;
        fflush(stdout);
        std::cerr << "error: uncaught exception" << std::endl;
    }
    fflush(stdout);
    if (!threw && a.usesAwait) {
        auto ok = jit->lookup("__repl_await_ok");
        if (!ok) {
            llvm::consumeError(ok.takeError());
        } else if (*ok->toPtr<const int*>() == 0) {
            std::cerr << "error: the awaited future did not complete" << std::endl;
            threw = true;
        }
    }
    if (!threw && layout) {
        std::string vname = "__repl_val_" + std::to_string(cur);
        auto vs = jit->lookup(vname);
        if (!vs) {
            llvm::consumeError(vs.takeError());
        } else {
            std::string text = formatValue(*layout, vs->toPtr<const char*>(), 0);
            std::fputs((text + "\n").c_str(), stdout);
            fflush(stdout);
        }
    }
    return true;
}

void Repl::commit(const Plan& plan, Attempt& a) {
    int cur = ++epoch;
    if (a.asyncSupport) asyncSupport = true;
    if (plan.mode == Mode::Decls) {
        chunks.push_back({plan.text, plan.file, cur});
        return;
    }
    for (auto g : a.newGlobals) {
        globals.erase(std::remove_if(globals.begin(), globals.end(),
                                     [&](const ReplGlobal& r) { return r.name == g.name; }),
                      globals.end());
        g.epoch = cur;
        globals.push_back(g);
    }
}

// Type each untyped `let x = E;`, in order, from a check of the input cut at that let.
bool Repl::inferLets(Plan& plan, const std::vector<std::pair<int, int>>& letPos) {
    for (size_t li = 0; li < letPos.size(); ++li) {
        Plan probe = plan;
        probe.probeLet = (long)li;
        Attempt a;
        std::string errs;
        bool built;
        {
            CerrCapture cap;
            built = build(probe, a);
            errs = cap.text();
        }
        if (!built) { std::cerr << errs; return false; }
        TypeChecker tc;
        tc.sourceFile = kReplFile;
        bool ok;
        {
            CerrCapture cap;
            ok = tc.check(a.program.get());
            errs = cap.text();
        }
        std::string t;
        bool printable;
        if (!typeOfLast(a, tc, t, printable)) {
            if (!ok) std::cerr << errs;
            else std::cerr << "error: " << kReplFile << ": cannot infer a type for this `let`"
                           << std::endl;
            return false;
        }
        std::string spelled = tc.displayType(t);
        ParseCtx ctx;
        std::vector<Token> tt;
        if (!lexInto(ctx, spelled, kReplFile, tt) || tt.empty()) return false;
        // A diagnostic about the written-in type points at the variable's name.
        for (auto& tok : tt) { tok.line = letPos[li].first; tok.column = letPos[li].second; }
        plan.letTypes[li] = tt;
    }
    return true;
}

}  // namespace
